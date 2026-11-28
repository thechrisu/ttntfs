// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SD -> Unix mode mapping (docs/ACL-DESIGN.md §2).
 *
 * Pure functions: no I/O, no globals, so the parser is testable offline
 * on synthetic SDs. Every offset/length is validated against the buffer
 * before it is dereferenced; a crafted SD returns an error, never an
 * out-of-bounds read.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "layout.h"
#include "security.h"
#include "volume.h"
#include "inode.h"

/* ---- SIDs --------------------------------------------------------------- */

/* A SID is 8 bytes + 4 per sub-authority. Windows caps the count at 15. */
#define NTFS_SID_MAX_SUB_AUTHORITIES 15

static size_t ntfs_sid_len(const struct ntfs_sid *sid)
{
	return 8 + 4 * sid->sub_authority_count;
}

/*
 * Validate the SID at @off (@len bytes remain in the buffer) and return
 * its length, or 0 when it is malformed. The SID header itself must fit
 * before sub_authority_count is read.
 */
static size_t ntfs_sid_valid(const u8 *buf, size_t off, size_t len)
{
	const struct ntfs_sid *sid;
	size_t need;

	if (off + 8 > len)
		return 0;
	sid = (const struct ntfs_sid *)(buf + off);
	if (sid->revision != 1)
		return 0;
	if (sid->sub_authority_count > NTFS_SID_MAX_SUB_AUTHORITIES)
		return 0;
	need = ntfs_sid_len(sid);
	if (off + need > len)
		return 0;
	return need;
}

static bool ntfs_sid_equal(const u8 *buf, size_t off, size_t len,
			   const struct ntfs_sid *want, size_t want_len)
{
	size_t have_len;

	if (!want)
		return false;
	have_len = ntfs_sid_valid(buf, off, len);
	if (!have_len || have_len != want_len)
		return false;
	return !memcmp(buf + off, want, want_len);
}

/*
 * Build a well-known SID into @out (a 16-byte scratch: revision +
 * authority + up to 2 sub-authorities covers every SID used here) and
 * return its length. Authority is the 48-bit big-endian value in the
 * last bytes of identifier_authority.value[].
 */
static size_t ntfs_sid_known(u8 *out, u32 authority, u32 sub0, u32 sub1,
			     int nsub)
{
	struct ntfs_sid *sid = (struct ntfs_sid *)out;

	sid->revision = 1;
	sid->sub_authority_count = nsub;
	memset(sid->identifier_authority.value, 0, 6);
	sid->identifier_authority.value[5] = authority & 0xff;
	if (nsub > 0)
		sid->sub_authority[0] = cpu_to_le32(sub0);
	if (nsub > 1)
		sid->sub_authority[1] = cpu_to_le32(sub1);
	return 8 + 4 * nsub;
}

/* Classes an ACE's SID can belong to. One ACE can match several: an
 * Everyone ACE grants the mounting user (owner class) and everyone
 * else (other class) at once. */
#define SD_CLS_OWNER		0x01	/* the SD's owner SID / Creator Owner */
#define SD_CLS_GROUP		0x02	/* the SD's group SID / Creator Group */
#define SD_CLS_EVERYONE	0x04	/* S-1-1-0 */
#define SD_CLS_INTERACTIVE	0x08	/* Authenticated Users / Users / Interactive */

static unsigned int ntfs_sid_classify(const u8 *buf, size_t off, size_t len,
				      const struct ntfs_sid *owner,
				      size_t owner_len,
				      const struct ntfs_sid *group,
				      size_t group_len)
{
	unsigned int cls = 0;
	u8 known[16];
	size_t known_len;

	if (ntfs_sid_equal(buf, off, len, owner, owner_len))
		cls |= SD_CLS_OWNER;
	if (ntfs_sid_equal(buf, off, len, group, group_len))
		cls |= SD_CLS_GROUP;

	/* Creator Owner (S-1-3-0) applies to the creator: the owner. */
	known_len = ntfs_sid_known(known, 3, 0, 0, 1);
	if (ntfs_sid_equal(buf, off, len, (struct ntfs_sid *)known, known_len))
		cls |= SD_CLS_OWNER;
	/* Creator Group (S-1-3-1) applies to the creator's group. */
	known_len = ntfs_sid_known(known, 3, 1, 0, 1);
	if (ntfs_sid_equal(buf, off, len, (struct ntfs_sid *)known, known_len))
		cls |= SD_CLS_GROUP;

	/* Everyone (S-1-1-0) is also an interactive principal. */
	known_len = ntfs_sid_known(known, 1, 0, 0, 1);
	if (ntfs_sid_equal(buf, off, len, (struct ntfs_sid *)known, known_len))
		cls |= SD_CLS_EVERYONE | SD_CLS_INTERACTIVE;
	/* Authenticated Users (S-1-5-11). */
	known_len = ntfs_sid_known(known, 5, 11, 0, 1);
	if (ntfs_sid_equal(buf, off, len, (struct ntfs_sid *)known, known_len))
		cls |= SD_CLS_INTERACTIVE;
	/* Users (S-1-5-32-545). */
	known_len = ntfs_sid_known(known, 5, 32, 545, 2);
	if (ntfs_sid_equal(buf, off, len, (struct ntfs_sid *)known, known_len))
		cls |= SD_CLS_INTERACTIVE;
	/* Interactive (S-1-5-4). */
	known_len = ntfs_sid_known(known, 5, 4, 0, 1);
	if (ntfs_sid_equal(buf, off, len, (struct ntfs_sid *)known, known_len))
		cls |= SD_CLS_INTERACTIVE;
	return cls;
}

/* ---- access masks -------------------------------------------------------- */

/*
 * Map a Windows access mask to rwx bits (0-7). Only data-access rights
 * map: READ_CONTROL / WRITE_DAC / WRITE_OWNER / DELETE / SYNCHRONIZE
 * and bare attribute rights do not grant data access, so they map to
 * nothing. FILE_DELETE_CHILD grants write on directories (delete a
 * child) and nothing on files, where it is meaningless.
 */
static unsigned int ntfs_mask_to_rwx(u32 mask, bool is_dir)
{
	unsigned int rwx = 0;

	if (mask & le32_to_cpu(GENERIC_ALL))
		return 7;
	if (mask & (le32_to_cpu(GENERIC_READ) |
		    le32_to_cpu(FILE_READ_DATA)))
		rwx |= 4;
	if (mask & (le32_to_cpu(GENERIC_WRITE) |
		    le32_to_cpu(FILE_WRITE_DATA) |
		    le32_to_cpu(FILE_APPEND_DATA) |
		    le32_to_cpu(FILE_WRITE_EA) |
		    le32_to_cpu(FILE_WRITE_ATTRIBUTES) |
		    (is_dir ? le32_to_cpu(FILE_DELETE_CHILD) : 0)))
		rwx |= 2;
	if (mask & (le32_to_cpu(GENERIC_EXECUTE) |
		    le32_to_cpu(FILE_EXECUTE)))
		rwx |= 1;
	return rwx;
}

/* ---- the DACL walk -------------------------------------------------------- */

int ntfs_sd_dacl_mode(const void *sd, size_t len, bool is_dir,
		      unsigned int *mode_out)
{
	const struct security_descriptor_relative *hdr;
	const struct ntfs_sid *owner_sid = NULL, *group_sid = NULL;
	size_t owner_len = 0, group_len = 0;
	u16 control;
	u32 dacl_off;
	const struct ntfs_acl *dacl;
	u16 acl_size, ace_count;
	size_t pos, acl_end;
	unsigned int owner_allow = 0, owner_deny = 0;
	unsigned int group_allow = 0, group_deny = 0;
	unsigned int other_allow = 0, other_deny = 0;
	bool group_seen = false;
	unsigned int owner, group, other;
	u16 i;

	if (!sd || !mode_out)
		return -EINVAL;
	if (len < sizeof(*hdr))
		return -EINVAL;

	hdr = (const struct security_descriptor_relative *)sd;
	if (hdr->revision != 1)
		return -EINVAL;
	control = le16_to_cpu(hdr->control);
	if (!(control & le16_to_cpu(SE_SELF_RELATIVE)))
		return -EINVAL;

	/* Owner / group SIDs: offset 0 means absent. */
	if (hdr->owner) {
		u32 off = le32_to_cpu(hdr->owner);

		owner_len = ntfs_sid_valid((const u8 *)sd, off, len);
		if (!owner_len)
			return -EINVAL;
		owner_sid = (const struct ntfs_sid *)((const u8 *)sd + off);
	}
	if (hdr->group) {
		u32 off = le32_to_cpu(hdr->group);

		group_len = ntfs_sid_valid((const u8 *)sd, off, len);
		if (!group_len)
			return -EINVAL;
		group_sid = (const struct ntfs_sid *)((const u8 *)sd + off);
	}

	/* No DACL at all, or an explicitly NULL DACL: full access. */
	if (!(control & le16_to_cpu(SE_DACL_PRESENT)) || !hdr->dacl) {
		*mode_out = 0777;
		return 0;
	}
	dacl_off = le32_to_cpu(hdr->dacl);
	if (dacl_off + sizeof(*dacl) > len)
		return -EINVAL;
	dacl = (const struct ntfs_acl *)((const u8 *)sd + dacl_off);
	if (dacl->revision != 2 && dacl->revision != 4)
		return -EINVAL;
	acl_size = le16_to_cpu(dacl->size);
	if (acl_size < sizeof(*dacl) || dacl_off + acl_size > len)
		return -EINVAL;
	ace_count = le16_to_cpu(dacl->ace_count);
	acl_end = dacl_off + acl_size;

	/* An explicitly empty DACL denies everybody. v1 does not report
	 * 0000 for it (docs/ACL-DESIGN.md §2.1): -ENODATA tells the caller
	 * to fall back to today's behaviour instead. */
	if (ace_count == 0)
		return -ENODATA;

	pos = dacl_off + sizeof(*dacl);
	for (i = 0; i < ace_count; i++) {
		const struct ntfs_ace *ace;
		u16 ace_size;
		u32 mask;
		unsigned int cls, rwx;
		size_t sid_off, sid_len;

		if (pos + 4 > acl_end)
			return -EINVAL;
		ace = (const struct ntfs_ace *)((const u8 *)sd + pos);
		ace_size = le16_to_cpu(ace->size);
		/* Header (4) + mask (4) + at least an empty SID. */
		if (ace_size < 8 || pos + ace_size > acl_end)
			return -EINVAL;
		/* ACEs are 4-byte aligned; a size that is not a multiple
		 * of 4 cannot be a well-formed ACE stream. */
		if (ace_size & 3)
			return -EINVAL;

		if (ace->flags & INHERIT_ONLY_ACE)
			goto next;
		if (ace->type != ACCESS_ALLOWED_ACE_TYPE &&
		    ace->type != ACCESS_DENIED_ACE_TYPE)
			goto next; /* audit/alarm/object ACEs: not modelled */

		sid_off = pos + 8;
		sid_len = ntfs_sid_valid((const u8 *)sd, sid_off,
					 pos + ace_size);
		if (!sid_len)
			return -EINVAL;

		mask = le32_to_cpu(ace->mask);
		rwx = ntfs_mask_to_rwx(mask, is_dir);
		if (!rwx)
			goto next;
		cls = ntfs_sid_classify((const u8 *)sd, sid_off,
					pos + ace_size, owner_sid, owner_len,
					group_sid, group_len);
		if (ace->type == ACCESS_ALLOWED_ACE_TYPE) {
			if (cls & (SD_CLS_OWNER | SD_CLS_INTERACTIVE))
				owner_allow |= rwx;
			if (cls & SD_CLS_GROUP) {
				group_allow |= rwx;
				group_seen = true;
			}
			if (cls & SD_CLS_EVERYONE)
				other_allow |= rwx;
		} else {
			if (cls & (SD_CLS_OWNER | SD_CLS_INTERACTIVE))
				owner_deny |= rwx;
			if (cls & SD_CLS_GROUP) {
				group_deny |= rwx;
				group_seen = true;
			}
			if (cls & SD_CLS_EVERYONE)
				other_deny |= rwx;
		}
next:
		pos += ace_size;
	}

	/* Windows evaluates denies against allows regardless of order:
	 * allowed minus denied, per class. */
	owner = owner_allow & ~owner_deny;
	other = other_allow & ~other_deny;
	if (group_seen)
		group = group_allow & ~group_deny;
	else
		group = other; /* nothing said about the group: as other */

	*mode_out = (owner << 6) | (group << 3) | other;
	return 0;
}

/* ---- $SDS lookup + per-volume cache (docs/ACL-DESIGN.md §2.3) ---------- */

/*
 * $SDS is the named $DATA stream of $Secure. Its name in the same idiom
 * as I30 in dir.c. Note ntfs_attr_readall() cannot read it: its 64 KiB
 * cap is below a real $SDS (262 KiB on a fresh mkntfs image, two copies
 * of a handful of entries), so the lookup reads entry by entry with
 * ntfs_inode_attr_pread() instead.
 */
__le16 sds_name[] = { cpu_to_le16('$'), cpu_to_le16('S'),
		      cpu_to_le16('D'), cpu_to_le16('S'), 0 };

/* The second copy of every SD lives 256 KiB into $SDS (docs/ACL-DESIGN.md
 * §1, confirmed against an mkntfs image 2026-11-28: the first two
 * entries reappear byte-identical at +0x40000). */
#define NTFS_SDS_SECOND_COPY	0x40000

/*
 * Scan one copy of $SDS (at stream offset @base) for @security_id.
 * @avi is the open $SDS attribute inode; @data_size its size.
 *
 * Returns 0 with a kvmalloc'd copy of the SD in *@sd_out (length
 * *@sd_len_out), -ENODATA when the copy ends cleanly without the id
 * (zeroed entry, or the stream ends), or -EINVAL when an entry fails
 * validation (the caller then tries the other copy). A short pread is
 * -EIO. The caller frees *@sd_out with kvfree().
 *
 * Entries are 16-byte aligned and sorted by security_id; the scan is
 * linear (v1: typical volumes hold a handful of distinct SDs, and the
 * result is cached per volume -- see ntfs_sd_mode()).
 */
static int ntfs_sds_scan_copy(struct inode *avi, s64 data_size, loff_t base,
			      u32 security_id, u8 **sd_out, size_t *sd_len_out)
{
	loff_t pos = base;

	while (pos + (loff_t)sizeof(struct sds_entry) <= data_size) {
		struct sds_entry hdr;
		u64 step;
		u8 *sd;
		s64 got;
		u32 length;

		got = ntfs_inode_attr_pread(avi, pos, sizeof(hdr), (u8 *)&hdr);
		if (got != (s64)sizeof(hdr))
			return got < 0 ? (int)got : -EIO;
		/* A zeroed entry is the end of the entries in this copy. */
		if (!hdr.hash && !hdr.security_id && !hdr.offset && !hdr.length)
			return -ENODATA;
		length = le32_to_cpu(hdr.length);
		if (length < sizeof(hdr) ||
		    pos + (loff_t)length > data_size ||
		    le64_to_cpu(hdr.offset) != (u64)pos)
			return -EINVAL;
		if (le32_to_cpu(hdr.security_id) != security_id) {
			/* 64-bit step: a u32 (length + 15) could wrap to 0
			 * for a crafted length and spin this loop forever. */
			step = ((u64)length + 15) & ~15ULL;
			pos += (loff_t)step;
			continue;
		}
		sd = kvmalloc(length - sizeof(hdr), GFP_NOFS);
		if (!sd)
			return -ENOMEM;
		got = ntfs_inode_attr_pread(avi, pos + (loff_t)sizeof(hdr),
					    length - sizeof(hdr), sd);
		if (got != (s64)(length - sizeof(hdr))) {
			kvfree(sd);
			return got < 0 ? (int)got : -EIO;
		}
		*sd_out = sd;
		*sd_len_out = length - sizeof(hdr);
		return 0;
	}
	return -ENODATA;
}

/*
 * Find the SD for @security_id in $Secure:$SDS. Tries the first copy,
 * then the second copy at +256 KiB when the first is corrupt or does
 * not hold the id (a torn first copy can zero the entry). Returns 0
 * with a kvmalloc'd SD in *@sd_out, or a negative errno; the caller
 * frees *@sd_out with kvfree().
 */
static int ntfs_sds_lookup(struct ntfs_volume *vol, u32 security_id,
			   u8 **sd_out, size_t *sd_len_out)
{
	struct inode *avi;
	s64 data_size;
	int err;

	if (!vol->secure_ino)
		return -ENODEV;
	avi = ntfs_attr_iget(vol->secure_ino, AT_DATA, sds_name, 4);
	if (IS_ERR(avi))
		return PTR_ERR(avi);
	data_size = NTFS_I(avi)->data_size;
	err = ntfs_sds_scan_copy(avi, data_size, 0, security_id,
				 sd_out, sd_len_out);
	if (err == -EINVAL || err == -ENODATA) {
		if (data_size > NTFS_SDS_SECOND_COPY +
		    (s64)sizeof(struct sds_entry)) {
			int err2 = ntfs_sds_scan_copy(avi, data_size,
						      NTFS_SDS_SECOND_COPY,
						      security_id,
						      sd_out, sd_len_out);
			if (!err2)
				err = 0;
			else if (err == -ENODATA && err2 == -EINVAL)
				err = err2;
		}
	}
	iput(avi);
	return err;
}

int ntfs_sd_mode(struct ntfs_volume *vol, u32 security_id, bool is_dir,
		 unsigned int *mode_out)
{
	unsigned int mode;
	u8 *sd;
	size_t sd_len;
	int err, i;

	if (!vol || !mode_out)
		return -EINVAL;
	if (!security_id)
		return -ENODATA;

	mutex_lock(&vol->sd_cache_lock);
	for (i = 0; i < NTFS_SD_CACHE_SIZE; i++) {
		if (vol->sd_cache[i].security_id == security_id &&
		    vol->sd_cache[i].is_dir == is_dir) {
			*mode_out = vol->sd_cache[i].perm;
			vol->sd_cache_hits++;
			mutex_unlock(&vol->sd_cache_lock);
			return 0;
		}
	}
	vol->sd_cache_misses++;
	mutex_unlock(&vol->sd_cache_lock);

	err = ntfs_sds_lookup(vol, security_id, &sd, &sd_len);
	if (err)
		return err;
	err = ntfs_sd_dacl_mode(sd, sd_len, is_dir, &mode);
	kvfree(sd);
	if (err)
		return err;

	mutex_lock(&vol->sd_cache_lock);
	/* Another load may have inserted this id while the $SDS read was
	 * in flight; re-check under the lock instead of duplicating it. */
	for (i = 0; i < NTFS_SD_CACHE_SIZE; i++) {
		if (vol->sd_cache[i].security_id == security_id &&
		    vol->sd_cache[i].is_dir == is_dir) {
			*mode_out = vol->sd_cache[i].perm;
			mutex_unlock(&vol->sd_cache_lock);
			return 0;
		}
	}
	i = vol->sd_cache_next;
	vol->sd_cache[i].security_id = security_id;
	vol->sd_cache[i].is_dir = is_dir;
	vol->sd_cache[i].perm = mode;
	vol->sd_cache_next = (i + 1) % NTFS_SD_CACHE_SIZE;
	mutex_unlock(&vol->sd_cache_lock);
	*mode_out = mode;
	return 0;
}

void ntfs_sd_cache_stats(struct ntfs_volume *vol, unsigned long *hits,
			 unsigned long *misses)
{
	if (!vol)
		return;
	mutex_lock(&vol->sd_cache_lock);
	if (hits)
		*hits = vol->sd_cache_hits;
	if (misses)
		*misses = vol->sd_cache_misses;
	mutex_unlock(&vol->sd_cache_lock);
}
