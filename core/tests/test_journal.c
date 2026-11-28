// SPDX-License-Identifier: GPL-2.0
/*
 * test_journal.c - the create path journals, and replaying it converges.
 *
 * Phase 2 (docs/JOURNAL-DESIGN.md §5) wires the transaction API into
 * __ntfs_create. These tests drive the public VFS surface — mount, create,
 * sync, snapshot — and check the journal from the outside, the way a
 * crash would see it:
 *
 *   create_journaled   a regular-file create writes exactly one
 *                        transaction to $LogFile (Initialize, five
 *                        CreateAttributes, AddIndexEntryRoot, SetBits,
 *                        Commit, three table dumps, Forget), the
 *                        $FILE_NAME create carries the name, the SetBits
 *                        names the new record number, and both touched
 *                        MFT records carry an MR_LSN at or past the
 *                        record that touched them. The sync's flush
 *                        notification (Phase 4 step 2) appends three
 *                        DeleteDirtyClusters records, and replaying the
 *                        dirty snapshot redoes nothing — the MFT
 *                        records and the bitmap were already flushed
 *                        and retired — and the file is present and
 *                        ntfsck -n is clean afterwards. A second
 *                        replay of the same dirty snapshot redoes the
 *                        same zero records: replay is idempotent.
 *   flush_notify_retires_dirty_pages
 *                        a sync flushes the $MFT folios and the bitmap
 *                        folio, the flush notification retires the
 *                        create's dirty pages, and the next commit's
 *                        checkpoint advances oldest_lsn past the
 *                        retired transaction's records — the walk
 *                        holds only the second create's records.
 *   flush_notify_no_sync_no_retire
 *                        the negative control: the same two creates
 *                        with no sync between them leave the first
 *                        create's records in the walk, and oldest_lsn
 *                        stays at its first data record.
 *   mkdir_unjournaled  a mkdir writes no transaction records at all,
 *                        and the
 *                        directory is still there after a remount.
 *   create_in_root     a create in the volume root writes no transaction
 *                        records either — its entry lands in the root's
 *                        $INDEX_ALLOCATION block, which Phase 2 cannot
 *                        describe — and the create still succeeds and
 *                        the file is still there after a remount. This
 *                        pins the -ENOENT fallback: a mutation the log
 *                        cannot describe must never fail the operation.
 *
 * Phase 3 (docs/JOURNAL-DESIGN.md §5) wires the transaction API into
 * ntfs_vfs_unlink:
 *
 *   unlink_journaled   a regular-file unlink writes exactly one
 *                        transaction to $LogFile (two open records —
 *                        $MFT:$DATA and $MFT:$BITMAP — DeleteIndexEntryRoot,
 *                        DeleteAttribute, DeallocateFileRecordSegment,
 *                        ClearBitsInNonresidentBitMap, Commit, three
 *                        table dumps, Forget); the create's records
 *                        stay live (their pages were never flushed),
 *                        so the walk holds 25. Replaying the snapshot
 *                        redoes the create's two idempotent records
 *                        plus the unlink's ClearBits, skips the
 *                        DeleteAttribute/DeleteIndexEntryRoot on their
 *                        MR_LSN stamps and the Deallocate on its
 *                        effect (the record is already free) — the
 *                        file is gone and ntfsck -n is clean.
 *   unlink_in_root_unjournaled
 *                        an unlink in the root writes no transaction records
 *                        (its entry lives in the root's
 *                        $INDEX_ALLOCATION block) and still succeeds.
 *   rmdir_journaled    an rmdir of an empty driver-made directory
 *                        journals the same four data records as a
 *                        file unlink; replay redoes only the
 *                        ClearBits.
 *
 *   rename_journaled   a same-directory rename writes exactly one
 *                        transaction to $LogFile (one open record —
 *                        $MFT:$DATA — CreateAttribute, AddIndexEntryRoot,
 *                        DeleteIndexEntryRoot, DeleteAttribute, Commit,
 *                        three table dumps, Forget); the create's
 *                        records stay live, so the walk holds 24.
 *                        Replaying the snapshot redoes only the
 *                        create's two idempotent records — the
 *                        rename's records are skipped on their MR_LSN
 *                        stamps — and the file is under its new name
 *                        and ntfsck -n is clean.
 *   rename_in_root_unjournaled
 *                        a rename in the root writes no transaction records
 *                        (both entries live in the root's
 *                        $INDEX_ALLOCATION block) and still succeeds.
 *   rename_over_journaled
 *                        renaming over an existing file journals the
 *                        target's removal (including its record free)
 *                        in the same transaction as the link and the
 *                        old name's removal: 2 open + 8 data + Commit
 *                        + 3 dumps + Forget = 15 records; the walk
 *                        holds 45 with the two creates' live records
 *                        (14 + 16 — the second create's opens are
 *                        still live, the first's precede oldest).
 *   rename_cross_dir_journaled
 *                        a rename across driver-made directories
 *                        journals the same four data records as a
 *                        same-directory rename, split across the two
 *                        parents; the walk holds 24.
 *
 *   xattr_create_journaled
 *                        setting a small resident xattr writes exactly
 *                        one transaction (one open — $MFT:$DATA — the
 *                        $SI ctime record, CreateAttribute for the ADS,
 *                        Commit, three table dumps, Forget); the walk
 *                        holds 22 with the create's 14. Replay redoes
 *                        only the create's two idempotent records —
 *                        the xattr's records are skipped on the
 *                        file record's MR_LSN stamp — and the value
 *                        reads back after a remount.
 *   xattr_replace_journaled
 *                        replacing a resident xattr stages one
 *                        UpdateResidentValue for the whole value, any
 *                        length — the replay's length-change path,
 *                        which the setattr class left unproven. The
 *                        walk holds 30 (create 14 + create-xattr 8 +
 *                        replace 8); the redo/undo carry the new/old
 *                        values whole.
 *   xattr_big_journaled  setting an 8 KiB xattr (non-resident) stages
 *                        CreateAttribute plus SetBits for its clusters,
 *                        replacing it with 16 KiB stages sizes, mapping
 *                        pairs and the bitmap delta, and removing it
 *                        stages DeleteAttribute plus ClearBits. Replay
 *                        redoes the create's records plus the
 *                        non-resident bitmap records; the ADS is gone
 *                        after a remount and ntfsck -n is clean.
 *   xattr_in_root_journaled
 *                        a file in the volume root IS journalable for
 *                        xattr — only its own record and $Bitmap are
 *                        touched, no parent $FILE_NAME — so the walk
 *                        holds just the xattr transaction's 8 records.
 *
 * The snapshot is copied after create+sync while the original is still
 * mounted, so the copy holds the flushed metadata and the still-dirty
 * log: exactly what a crash at that instant would leave. The original
 * is unmounted cleanly afterwards; the copy is never mounted by the
 * driver that wrote it.
 *
 * Without the Phase 2 wiring the create test fails at the walk: the
 * log holds no client records at all (s.n == 0, not 15).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"
#include "ntfs_logfile.h"
#include "ntfs_image.h"
#include "logfile_layout.h"
#include <linux/xattr.h>
#include <linux/fs.h>

/* $EA test hook: the VFS xattr handler table is exported from ea.c
 * (see ea.h); the EA tests drive handlers[0]->set/get directly
 * because the public ntfs_setxattr() stores xattrs as ADS and never
 * reaches ntfs_set_ea(). Declared here rather than including ea.h,
 * whose internal headers collide with ntfscore.h. */
extern const struct xattr_handler *const ntfs_xattr_handlers[];

static int failures, checks;
#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define CHECK_EQ(a, b) do {						\
	checks++;							\
	if ((a) != (b)) {						\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n",	\
			__FILE__, __LINE__, #a,				\
			(long long)(a), (long long)(b));		\
	}								\
} while (0)

#define FAILF(...) do {							\
	checks++; failures++;						\
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);		\
	fprintf(stderr, __VA_ARGS__);					\
} while (0)

/* ---- external tools -------------------------------------------------- */

static char mkntfs_path[1024], ntfsck_path[1024];
static char tmpdir[1024];

static void tool_paths(void)
{
	const char *m = getenv("NTFS_MKNTFS"), *c = getenv("NTFS_NTFSCK");
	const char *t = getenv("TMPDIR");

	snprintf(mkntfs_path, sizeof(mkntfs_path), "%s",
		 m && *m ? m : "tools/.local/sbin/mkntfs");
	snprintf(ntfsck_path, sizeof(ntfsck_path), "%s",
		 c && *c ? c : "tools/.local-plus/sbin/ntfsck");
	snprintf(tmpdir, sizeof(tmpdir), "%s", t && *t ? t : "/tmp");
	if (tmpdir[0] && tmpdir[strlen(tmpdir) - 1] == '/')
		tmpdir[strlen(tmpdir) - 1] = '\0';
}

static void scratch_path(char *out, size_t n, const char *tag)
{
	snprintf(out, n, "%s/ttntfs-journal-%d-%s.img", tmpdir, (int)getpid(), tag);
}

/* Build a fresh 16 MiB NTFS volume at @path. */
static int make_image(const char *path)
{
	char cmd[2600];
	int rc;

	snprintf(cmd, sizeof(cmd),
		 "dd if=/dev/zero of='%s' bs=1048576 count=16 >/dev/null 2>&1 && "
		 "'%s' -Q -F -L JOURNAL '%s' >/dev/null 2>&1",
		 path, mkntfs_path, path);
	rc = system(cmd);
	return (rc == 0) ? 0 : -EIO;
}

static int copy_file(const char *src, const char *dst)
{
	char buf[1 << 16];
	ssize_t n;
	int in, out;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -errno;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { close(in); return -errno; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) { close(in); close(out); return -EIO; }
	close(in);
	if (close(out) != 0)
		return -errno;
	return (n < 0) ? -errno : 0;
}

/* ntfsck -n: report only, never write. Returns 0 when clean. */
static int run_ntfsck_clean(const char *path)
{
	char cmd[2600];
	int rc;

	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >/dev/null 2>&1",
		 ntfsck_path, path);
	rc = system(cmd);
	if (rc == -1)
		return -1;
	if (WIFEXITED(rc))
		return WEXITSTATUS(rc);
	return -1;
}

/* ---- mounting --------------------------------------------------------- */

static struct ntfs_mount_options mopts = {
	.flags = NTFS_MOUNT_RDONLY_FALLBACK,
	.uid = 0, .gid = 0, .fmask = 022, .dmask = 022,
};

static void quiet_logger(int level, const char *msg, void *ctx)
{
	(void)ctx;
	if (getenv("NTFS_JOURNAL_TRACE"))
		fprintf(stderr, "  [drv %d] %s\n", level, msg);
}

/* ---- $LogFile access on a snapshot ------------------------------------- */

/* ntfs_image_io over a bdev (mirrors logfile_repair.c). */
static int img_pread(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct ntfs_bdev *dev = ctx;
	ssize_t n;

	if (!dev->ops || !dev->ops->pread)
		return -ENOTSUP;
	n = dev->ops->pread(dev, buf, len, off);
	if (n < 0)
		return (int)n;
	if ((size_t)n != len)
		return -EIO;
	return 0;
}

static int img_pwrite(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct ntfs_bdev *dev = ctx;
	ssize_t n;

	if (!dev->ops || !dev->ops->pwrite)
		return -ENOTSUP;
	n = dev->ops->pwrite(dev, buf, len, off);
	if (n < 0)
		return (int)n;
	if ((size_t)n != len)
		return -EIO;
	return 0;
}

static int img_sync(void *ctx)
{
	struct ntfs_bdev *dev = ctx;

	if (!dev->ops || !dev->ops->flush)
		return 0;
	return dev->ops->flush(dev);
}

struct rec_seen {
	uint64_t lsn[64];
	uint16_t redo_op[64];
	uint16_t target_attr[64];
	uint64_t target_vcn[64];
	uint16_t record_off[64];
	int n;
	/* $FILE_NAME create: its redo carries the name in UTF-16LE. */
	int fn_name_hits;
	uint64_t fn_lsn;
	/* SetBits: bit and count from the redo payload. */
	uint32_t bits_bit, bits_count;
	uint64_t bits_lsn;
	uint16_t bits_target_attr;
	/* ClearBits: the same, for the clear record. */
	uint32_t clear_bits_bit, clear_bits_count;
	uint64_t clear_bits_lsn;
	uint16_t clear_bits_target_attr;
	/* UpdateResidentValue: the 4-byte redo/undo values (the $SI
	 * flags the truncate stages when the file flips sparse). */
	uint32_t res_redo, res_undo;
	uint64_t res_lsn;
	/* UpdateResidentValue with a 36-byte payload: the $SI times +
	 * flags the setattr stages (crtime +0, mtime +8, ctime +16,
	 * atime +24, flags +32, all little-endian). The last such
	 * record wins. */
	uint64_t si_redo_crtime, si_undo_crtime;
	uint64_t si_redo_mtime, si_undo_mtime;
	uint64_t si_redo_ctime, si_undo_ctime;
	uint64_t si_redo_atime, si_undo_atime;
	uint32_t si_redo_flags, si_undo_flags;
	uint64_t si_lsn;
	/* UpdateResidentValue whose payload is the $EA value: the
	 * $LXMOD entry's 4-byte mode, redo and undo. The last such
	 * record wins. (The $SI 36-byte record and the 8-byte
	 * $EA_INFORMATION record carry no $LXMOD entry, so they
	 * never match.) */
	uint32_t ea_lxmod_redo, ea_lxmod_undo;
	uint64_t ea_lsn;
	/* UpdateFileNameRoot: the duplicated sizes/flags from the
	 * redo/undo dup payloads (alloc at +0x20, data at +0x28,
	 * flags at +0x30). The last such record wins. */
	uint64_t fn_redo_alloc, fn_undo_alloc;
	uint64_t fn_redo_data, fn_undo_data;
	uint32_t fn_redo_flags, fn_undo_flags;
	uint64_t fnupd_lsn;
	/* SetNewAttributeSizes: the $DATA size from redo/undo. */
	uint64_t sizes_redo_data, sizes_undo_data;
	uint64_t sizes_redo_total;
	uint64_t sizes_lsn;
	/* $DATA create: its redo carries the ADS name in UTF-16LE.
	 * set_ads_name() picks the name; a test that does not call it
	 * scans nothing. */
	int ads_create_hits;
	uint64_t ads_create_lsn;
	/* UpdateResidentValue whose payload is an ADS value: the redo
	 * and undo bytes, up to 64 each (a resident replace). The last
	 * such record wins. The $SI 36-byte record, the 4-byte $SI
	 * flags record and $LXMOD records never match. */
	uint8_t ads_value_redo[64], ads_value_undo[64];
	uint16_t ads_value_redo_len, ads_value_undo_len;
	uint64_t ads_value_lsn;
	int sizes_count;
	/* CreateAttribute for $EA_INFORMATION / $EA: the redo's first
	 * 4 bytes are the attribute type (0xD0 / 0xE0). */
	int ea_info_create_hits, ea_create_hits;
	uint64_t ea_info_create_lsn, ea_create_lsn;
	/* DeleteAttribute for $EA_INFORMATION / $EA: the undo's first
	 * 4 bytes are the attribute type. */
	int ea_info_delete_hits, ea_delete_hits;
	uint64_t ea_info_delete_lsn, ea_delete_lsn;
	/* UpdateResidentValue whose payload is the $EA stream: the
	 * redo and undo bytes, up to 256 each. Matched when either
	 * side contains the EA name (set_ea_name picks it); the last
	 * such record wins. The 8-byte $EA_INFORMATION record never
	 * matches (it carries no name). */
	uint8_t ea_value_redo[256], ea_value_undo[256];
	uint16_t ea_value_redo_len, ea_value_undo_len;
	uint64_t ea_value_lsn;
	/* UpdateResidentValue with an 8-byte payload: the
	 * $EA_INFORMATION value (ea_length +0, need_ea_count +2,
	 * ea_query_length +4, all little-endian). The last such
	 * record wins. */
	uint16_t ea_info_redo_len, ea_info_undo_len;
	uint32_t ea_info_redo_query, ea_info_undo_query;
	uint64_t ea_info_lsn;
};

/* $FILE_NAME create: its redo carries the name in UTF-16LE. The
 * pattern defaults to "j.txt"; a test that creates another name
 * calls set_fn_name() before walk_log(). walk_log() resets the
 * pattern to the default after each walk, so a test that does not
 * call it always scans for "j.txt". */
static uint8_t fn_name_pat[2 * 32];
static size_t fn_name_len;

static void set_fn_name(const char *name)
{
	size_t i, n = strlen(name);

	if (n > 32)
		n = 32;
	for (i = 0; i < n; i++) {
		fn_name_pat[2 * i] = (uint8_t)name[i];
		fn_name_pat[2 * i + 1] = 0;
	}
	fn_name_len = 2 * n;
}

/* The ADS name pattern, mirroring set_fn_name(): the UTF-16LE
 * bytes of the name, for scanning CreateAttribute redo payloads.
 * Defaults to empty (no scan); a test calls set_ads_name() before
 * walk_log(). */
static uint8_t ads_name_pat[2 * 32];
static size_t ads_name_len;

static void set_ads_name(const char *name)
{
	size_t i, n = strlen(name);

	if (n > 32)
		n = 32;
	for (i = 0; i < n; i++) {
		ads_name_pat[2 * i] = (uint8_t)name[i];
		ads_name_pat[2 * i + 1] = 0;
	}
	ads_name_len = 2 * n;
}

/* The EA name pattern, mirroring set_ads_name() — but an EA name in
 * the $EA stream is plain bytes, not UTF-16LE. Defaults to empty
 * (no scan); a test calls set_ea_name() before walk_log(). */
static uint8_t ea_name_pat[32];
static size_t ea_name_len;

static void set_ea_name(const char *name)
{
	size_t n = strlen(name);

	if (n > 32)
		n = 32;
	memcpy(ea_name_pat, name, n);
	ea_name_len = n;
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
		((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
		((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
		((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* Find the $LXMOD entry in a packed $EA value and return its 4-byte
 * mode. An EA entry is: next_entry_offset (u32), flags (u8),
 * name_length (u8), value_length (u16), name (name_length bytes
 * plus a NUL), value. Entries are 4-byte aligned; the last has
 * next_entry_offset 0. */
static bool find_lxmod(const uint8_t *p, size_t len, uint32_t *mode_out)
{
	size_t off = 0;

	while (off + 8 <= len) {
		uint32_t next = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
			((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
		uint8_t name_len = p[off + 5];
		uint16_t value_len = (uint16_t)p[off + 6] |
			((uint16_t)p[off + 7] << 8);
		size_t value_off;

		if (name_len == 6 && off + 8 + 6 + 1 <= len &&
		    !memcmp(p + off + 8, "$LXMOD", 6)) {
			value_off = off + 8 + name_len + 1;
			if (value_len != 4 || value_off + 4 > len)
				return false;
			*mode_out = (uint32_t)p[value_off] |
				((uint32_t)p[value_off + 1] << 8) |
				((uint32_t)p[value_off + 2] << 16) |
				((uint32_t)p[value_off + 3] << 24);
			return true;
		}
		if (next == 0 || next < 8 || off + next > len)
			return false;
		off += next;
	}
	return false;
}

/* Unix seconds/nanoseconds to an NTFS timestamp: 100ns intervals
 * since 1601-01-01. The same arithmetic utc2ntfs does. */
static uint64_t ntfs_time(int64_t sec, int32_t nsec)
{
	return (uint64_t)(sec + 11644473600LL) * 10000000uLL +
		(uint64_t)(nsec / 100);
}

static int collect_rec(const struct ntfs_log_record *r, void *ctx)
{
	struct rec_seen *s = ctx;
	size_t i;

	if (r->record_type != LFS_RECORD_TYPE_CLIENT || s->n >= 64)
		return 0;
	s->lsn[s->n] = r->lsn;
	s->redo_op[s->n] = r->redo_op;
	s->target_attr[s->n] = r->target_attr;
	s->target_vcn[s->n] = r->target_vcn;
	s->record_off[s->n] = r->record_off;
	if (fn_name_len == 0)
		set_fn_name("j.txt");
	if (r->redo_op == LOP_CreateAttribute && r->redo && r->redo_len >= fn_name_len) {
		const uint8_t *p = r->redo;
		for (i = 0; i + fn_name_len <= r->redo_len; i++) {
			if (!memcmp(p + i, fn_name_pat, fn_name_len)) {
				s->fn_name_hits++;
				s->fn_lsn = r->lsn;
				break;
			}
		}
	}
	if (r->redo_op == LOP_CreateAttribute && r->redo && ads_name_len &&
	    r->redo_len >= ads_name_len) {
		const uint8_t *p = r->redo;
		for (i = 0; i + ads_name_len <= r->redo_len; i++) {
			if (!memcmp(p + i, ads_name_pat, ads_name_len)) {
				s->ads_create_hits++;
				s->ads_create_lsn = r->lsn;
				break;
			}
		}
	}
	/* The ADS value: an UpdateResidentValue whose payload is neither
	 * the 36-byte $SI nor the 4-byte $SI flags nor an $LXMOD $EA
	 * value. Stores the whole redo/undo bytes (up to 64 each). */
	if (r->redo_op == LOP_UpdateResidentValue && r->redo && r->undo &&
	    r->redo_len <= 64 && r->undo_len <= 64 &&
	    r->redo_len != 36 && r->undo_len != 36 &&
	    r->redo_len != 4 && r->undo_len != 4) {
		uint32_t dummy;

		if (!find_lxmod(r->redo, r->redo_len, &dummy) &&
		    !find_lxmod(r->undo, r->undo_len, &dummy)) {
			memcpy(s->ads_value_redo, r->redo, r->redo_len);
			memcpy(s->ads_value_undo, r->undo, r->undo_len);
			s->ads_value_redo_len = (uint16_t)r->redo_len;
			s->ads_value_undo_len = (uint16_t)r->undo_len;
			s->ads_value_lsn = r->lsn;
		}
	}
	/* The $EA pair's CreateAttribute: the redo's first 4 bytes
	 * are the attribute type (0xD0 $EA_INFORMATION, 0xE0 $EA). */
	if (r->redo_op == LOP_CreateAttribute && r->redo &&
	    r->redo_len >= 4) {
		const uint8_t *pr = r->redo;
		uint32_t t = (uint32_t)pr[0] |
			((uint32_t)pr[1] << 8) |
			((uint32_t)pr[2] << 16) |
			((uint32_t)pr[3] << 24);

		if (t == 0xD0) {
			s->ea_info_create_hits++;
			s->ea_info_create_lsn = r->lsn;
		}
		if (t == 0xE0) {
			s->ea_create_hits++;
			s->ea_create_lsn = r->lsn;
		}
	}
	/* The $EA pair's DeleteAttribute: the undo carries the
	 * attribute bytes, so the type is read from there. */
	if (r->redo_op == LOP_DeleteAttribute && r->undo &&
	    r->undo_len >= 4) {
		const uint8_t *pu = r->undo;
		uint32_t t = (uint32_t)pu[0] |
			((uint32_t)pu[1] << 8) |
			((uint32_t)pu[2] << 16) |
			((uint32_t)pu[3] << 24);

		if (t == 0xD0) {
			s->ea_info_delete_hits++;
			s->ea_info_delete_lsn = r->lsn;
		}
		if (t == 0xE0) {
			s->ea_delete_hits++;
			s->ea_delete_lsn = r->lsn;
		}
	}
	/* The $EA_INFORMATION value: an UpdateResidentValue with an
	 * 8-byte payload (ea_length +0, need_ea_count +2,
	 * ea_query_length +4). */
	if (r->redo_op == LOP_UpdateResidentValue && r->redo && r->undo &&
	    r->redo_len == 8 && r->undo_len == 8) {
		const uint8_t *pr = r->redo, *pu = r->undo;

		s->ea_info_redo_len = (uint16_t)pr[0] |
			((uint16_t)pr[1] << 8);
		s->ea_info_undo_len = (uint16_t)pu[0] |
			((uint16_t)pu[1] << 8);
		s->ea_info_redo_query = (uint32_t)pr[4] |
			((uint32_t)pr[5] << 8) |
			((uint32_t)pr[6] << 16) |
			((uint32_t)pr[7] << 24);
		s->ea_info_undo_query = (uint32_t)pu[4] |
			((uint32_t)pu[5] << 8) |
			((uint32_t)pu[6] << 16) |
			((uint32_t)pu[7] << 24);
		s->ea_info_lsn = r->lsn;
	}
	/* The $EA stream value: an UpdateResidentValue whose redo or
	 * undo carries the EA name. Stores the whole redo/undo
	 * bytes (up to 256 each); the last such record wins. */
	if (r->redo_op == LOP_UpdateResidentValue && r->redo && r->undo &&
	    ea_name_len && r->redo_len <= 256 && r->undo_len <= 256) {
		bool hit = false;

		for (i = 0; i + ea_name_len <= r->redo_len; i++) {
			if (!memcmp(r->redo + i, ea_name_pat,
				    ea_name_len)) {
				hit = true;
				break;
			}
		}
		if (!hit) {
			for (i = 0; i + ea_name_len <= r->undo_len; i++) {
				if (!memcmp(r->undo + i, ea_name_pat,
					    ea_name_len)) {
					hit = true;
					break;
				}
			}
		}
		if (hit) {
			memcpy(s->ea_value_redo, r->redo, r->redo_len);
			memcpy(s->ea_value_undo, r->undo, r->undo_len);
			s->ea_value_redo_len = (uint16_t)r->redo_len;
			s->ea_value_undo_len = (uint16_t)r->undo_len;
			s->ea_value_lsn = r->lsn;
		}
	}
	if (r->redo_op == LOP_SetBitsInNonresidentBitMap && r->redo && r->redo_len >= BR_SIZE) {
		const uint8_t *p = r->redo;
		s->bits_target_attr = r->target_attr;
		s->bits_bit = (uint32_t)p[BR_BITMAP_OFF] |
			((uint32_t)p[BR_BITMAP_OFF + 1] << 8) |
			((uint32_t)p[BR_BITMAP_OFF + 2] << 16) |
			((uint32_t)p[BR_BITMAP_OFF + 3] << 24);
		s->bits_count = (uint32_t)p[BR_BITS] |
			((uint32_t)p[BR_BITS + 1] << 8) |
			((uint32_t)p[BR_BITS + 2] << 16) |
			((uint32_t)p[BR_BITS + 3] << 24);
		s->bits_lsn = r->lsn;
	}
	if (r->redo_op == LOP_ClearBitsInNonresidentBitMap && r->redo && r->redo_len >= BR_SIZE) {
		const uint8_t *p = r->redo;
		s->clear_bits_target_attr = r->target_attr;
		s->clear_bits_bit = (uint32_t)p[BR_BITMAP_OFF] |
			((uint32_t)p[BR_BITMAP_OFF + 1] << 8) |
			((uint32_t)p[BR_BITMAP_OFF + 2] << 16) |
			((uint32_t)p[BR_BITMAP_OFF + 3] << 24);
		s->clear_bits_count = (uint32_t)p[BR_BITS] |
			((uint32_t)p[BR_BITS + 1] << 8) |
			((uint32_t)p[BR_BITS + 2] << 16) |
			((uint32_t)p[BR_BITS + 3] << 24);
		s->clear_bits_lsn = r->lsn;
	}
	if (r->redo_op == LOP_UpdateResidentValue && r->redo && r->redo_len == 4 &&
	    r->undo && r->undo_len == 4) {
		const uint8_t *pr = r->redo, *pu = r->undo;
		s->res_redo = (uint32_t)pr[0] | ((uint32_t)pr[1] << 8) |
			((uint32_t)pr[2] << 16) | ((uint32_t)pr[3] << 24);
		s->res_undo = (uint32_t)pu[0] | ((uint32_t)pu[1] << 8) |
			((uint32_t)pu[2] << 16) | ((uint32_t)pu[3] << 24);
		s->res_lsn = r->lsn;
	}
	if (r->redo_op == LOP_UpdateResidentValue && r->redo && r->redo_len == 36 &&
	    r->undo && r->undo_len == 36) {
		const uint8_t *pr = r->redo, *pu = r->undo;
		/* $SI layout: crtime +0, mtime +8, ctime +16, atime
		 * +24, flags +32. Read the u64s the same shifted way
		 * as the dup capture below. */
		s->si_redo_crtime = rd64(pr + 0);
		s->si_undo_crtime = rd64(pu + 0);
		s->si_redo_mtime = rd64(pr + 8);
		s->si_undo_mtime = rd64(pu + 8);
		s->si_redo_ctime = rd64(pr + 16);
		s->si_undo_ctime = rd64(pu + 16);
		s->si_redo_atime = rd64(pr + 24);
		s->si_undo_atime = rd64(pu + 24);
		s->si_redo_flags = (uint32_t)pr[32] | ((uint32_t)pr[33] << 8) |
			((uint32_t)pr[34] << 16) | ((uint32_t)pr[35] << 24);
		s->si_undo_flags = (uint32_t)pu[32] | ((uint32_t)pu[33] << 8) |
			((uint32_t)pu[34] << 16) | ((uint32_t)pu[35] << 24);
		s->si_lsn = r->lsn;
	}
	if (r->redo_op == LOP_UpdateResidentValue && r->redo && r->undo &&
	    r->redo_len == r->undo_len) {
		uint32_t redo_mode, undo_mode;

		if (find_lxmod(r->redo, r->redo_len, &redo_mode) &&
		    find_lxmod(r->undo, r->undo_len, &undo_mode)) {
			s->ea_lxmod_redo = redo_mode;
			s->ea_lxmod_undo = undo_mode;
			s->ea_lsn = r->lsn;
		}
	}
	if (r->redo_op == LOP_UpdateFileNameRoot && r->redo &&
	    r->redo_len == FN_DUP_SIZE && r->undo &&
	    r->undo_len == FN_DUP_SIZE) {
		const uint8_t *pr = r->redo, *pu = r->undo;
		/* Dup payload: allocated_size at +0x20, data_size at
		 * +0x28, flags at +0x30. */
		s->fn_redo_alloc = (uint64_t)pr[0x20] |
			((uint64_t)pr[0x21] << 8) |
			((uint64_t)pr[0x22] << 16) |
			((uint64_t)pr[0x23] << 24) |
			((uint64_t)pr[0x24] << 32) |
			((uint64_t)pr[0x25] << 40) |
			((uint64_t)pr[0x26] << 48) |
			((uint64_t)pr[0x27] << 56);
		s->fn_undo_alloc = (uint64_t)pu[0x20] |
			((uint64_t)pu[0x21] << 8) |
			((uint64_t)pu[0x22] << 16) |
			((uint64_t)pu[0x23] << 24) |
			((uint64_t)pu[0x24] << 32) |
			((uint64_t)pu[0x25] << 40) |
			((uint64_t)pu[0x26] << 48) |
			((uint64_t)pu[0x27] << 56);
		s->fn_redo_data = (uint64_t)pr[0x28] |
			((uint64_t)pr[0x29] << 8) |
			((uint64_t)pr[0x2a] << 16) |
			((uint64_t)pr[0x2b] << 24) |
			((uint64_t)pr[0x2c] << 32) |
			((uint64_t)pr[0x2d] << 40) |
			((uint64_t)pr[0x2e] << 48) |
			((uint64_t)pr[0x2f] << 56);
		s->fn_undo_data = (uint64_t)pu[0x28] |
			((uint64_t)pu[0x29] << 8) |
			((uint64_t)pu[0x2a] << 16) |
			((uint64_t)pu[0x2b] << 24) |
			((uint64_t)pu[0x2c] << 32) |
			((uint64_t)pu[0x2d] << 40) |
			((uint64_t)pu[0x2e] << 48) |
			((uint64_t)pu[0x2f] << 56);
		s->fn_redo_flags = (uint32_t)pr[0x30] |
			((uint32_t)pr[0x31] << 8) |
			((uint32_t)pr[0x32] << 16) |
			((uint32_t)pr[0x33] << 24);
		s->fn_undo_flags = (uint32_t)pu[0x30] |
			((uint32_t)pu[0x31] << 8) |
			((uint32_t)pu[0x32] << 16) |
			((uint32_t)pu[0x33] << 24);
		s->fnupd_lsn = r->lsn;
	}
	if (r->redo_op == LOP_SetNewAttributeSizes && r->redo && r->redo_len >= NAS_SIZE &&
	    r->undo && r->undo_len >= NAS_SIZE) {
		const uint8_t *pr = r->redo, *pu = r->undo;
		s->sizes_redo_data = (uint64_t)pr[NAS_DATA] |
			((uint64_t)pr[NAS_DATA + 1] << 8) |
			((uint64_t)pr[NAS_DATA + 2] << 16) |
			((uint64_t)pr[NAS_DATA + 3] << 24) |
			((uint64_t)pr[NAS_DATA + 4] << 32) |
			((uint64_t)pr[NAS_DATA + 5] << 40) |
			((uint64_t)pr[NAS_DATA + 6] << 48) |
			((uint64_t)pr[NAS_DATA + 7] << 56);
		s->sizes_undo_data = (uint64_t)pu[NAS_DATA] |
			((uint64_t)pu[NAS_DATA + 1] << 8) |
			((uint64_t)pu[NAS_DATA + 2] << 16) |
			((uint64_t)pu[NAS_DATA + 3] << 24) |
			((uint64_t)pu[NAS_DATA + 4] << 32) |
			((uint64_t)pu[NAS_DATA + 5] << 40) |
			((uint64_t)pu[NAS_DATA + 6] << 48) |
			((uint64_t)pu[NAS_DATA + 7] << 56);
		s->sizes_redo_total = (uint64_t)pr[NAS_TOTAL] |
			((uint64_t)pr[NAS_TOTAL + 1] << 8) |
			((uint64_t)pr[NAS_TOTAL + 2] << 16) |
			((uint64_t)pr[NAS_TOTAL + 3] << 24) |
			((uint64_t)pr[NAS_TOTAL + 4] << 32) |
			((uint64_t)pr[NAS_TOTAL + 5] << 40) |
			((uint64_t)pr[NAS_TOTAL + 6] << 48) |
			((uint64_t)pr[NAS_TOTAL + 7] << 56);
		s->sizes_lsn = r->lsn;
		s->sizes_count++;
	}
	s->n++;
	return 0;
}

/* Walk the log of the image at @path (read-only). Returns 0 and fills @s. */
static int walk_log(const char *path, struct rec_seen *s)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	struct ntfs_image img;
	struct ntfs_image_io io;
	struct ntfs_log_io log_io;
	struct ntfs_log_geometry geom;
	ntfs_logfile_t *log = NULL;
	int err;

	memset(s, 0, sizeof(*s));
	if (!dev)
		return -EIO;
	io.ctx = dev;
	io.pread = img_pread;
	io.pwrite = NULL;
	io.sync = NULL;
	memset(&img, 0, sizeof(img));
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err)
		goto out_dev;
	ntfs_image_log_io(&img, &log_io);
	ntfs_image_geometry(&img, &geom);
	err = ntfs_logfile_open(&log_io, &geom, &log);
	if (err)
		goto out_img;
	/*
	 * Walk from the oldest LSN, not 0: 0 starts at the current
	 * checkpoint, which the commit's own checkpoint moved past the
	 * data records — the walk would see only the dumps and the
	 * Forget. The oldest LSN is the restart area's client oldest,
	 * the transaction's first record (the first Open record).
	 */
	{
		struct ntfs_logfile_info info;

		ntfs_logfile_get_info(log, &info);
		err = ntfs_logfile_walk(log, info.oldest_lsn, collect_rec, s);
	}
	ntfs_logfile_close(log);
out_img:
	ntfs_image_close(&img);
out_dev:
	ntfs_bdev_close(dev);
	/* Back to the default pattern for the next test. */
	set_fn_name("j.txt");
	return err;
}

/* The log's oldest LSN (the restart area's client oldest) for the
 * image at @path — the LSN a replay walks from. A test uses it to
 * prove a checkpoint advanced past a retired transaction. */
static int log_oldest_lsn(const char *path, uint64_t *lsn_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	struct ntfs_image img;
	struct ntfs_image_io io;
	struct ntfs_log_io log_io;
	struct ntfs_log_geometry geom;
	ntfs_logfile_t *log = NULL;
	int err;

	if (!dev)
		return -EIO;
	io.ctx = dev;
	io.pread = img_pread;
	io.pwrite = NULL;
	io.sync = NULL;
	memset(&img, 0, sizeof(img));
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err)
		goto out_dev;
	ntfs_image_log_io(&img, &log_io);
	ntfs_image_geometry(&img, &geom);
	err = ntfs_logfile_open(&log_io, &geom, &log);
	if (err)
		goto out_img;
	{
		struct ntfs_logfile_info info;

		ntfs_logfile_get_info(log, &info);
		*lsn_out = info.oldest_lsn;
	}
	ntfs_logfile_close(log);
out_img:
	ntfs_image_close(&img);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Read the MR_LSN (offset 8) of MFT record @mft_no from the image at @path. */
static int read_mft_lsn(const char *path, uint64_t mft_no, uint64_t *lsn_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	struct ntfs_image img;
	struct ntfs_image_io io;
	struct ntfs_log_apply ap;
	uint8_t *buf;
	int err;

	*lsn_out = 0;
	if (!dev)
		return -EIO;
	io.ctx = dev;
	io.pread = img_pread;
	io.pwrite = NULL;
	io.sync = NULL;
	memset(&img, 0, sizeof(img));
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err)
		goto out_dev;
	buf = malloc(img.mft_record_size);
	if (!buf) {
		err = -ENOMEM;
		goto out_img;
	}
	ntfs_image_apply(&img, &ap);
	err = ap.read_mft_record(ap.ctx, mft_no, buf);
	if (!err) {
		/* mft_record: magic 0-3, usa_ofs 4-5, usa_count 6-7, lsn 8-15. */
		*lsn_out = (uint64_t)buf[8] |
			((uint64_t)buf[9] << 8) | ((uint64_t)buf[10] << 16) |
			((uint64_t)buf[11] << 24) | ((uint64_t)buf[12] << 32) |
			((uint64_t)buf[13] << 40) | ((uint64_t)buf[14] << 48) |
			((uint64_t)buf[15] << 56);
	}
	free(buf);
out_img:
	ntfs_image_close(&img);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Read record 0's unnamed $DATA attribute from the image at @path:
 * its offset in the record, its allocated/data sizes, and the image's
 * MFT record size. */
static int read_mft0_data_attr(const char *path, uint16_t *off_out,
			       uint64_t *alloc_out, uint64_t *data_out,
			       uint32_t *rec_size_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	struct ntfs_image img;
	struct ntfs_image_io io;
	struct ntfs_log_apply ap;
	uint8_t *buf;
	int err;

	*off_out = 0;
	*alloc_out = 0;
	*data_out = 0;
	*rec_size_out = 0;
	if (!dev)
		return -EIO;
	io.ctx = dev;
	io.pread = img_pread;
	io.pwrite = NULL;
	io.sync = NULL;
	memset(&img, 0, sizeof(img));
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err)
		goto out_dev;
	*rec_size_out = img.mft_record_size;
	buf = malloc(img.mft_record_size);
	if (!buf) {
		err = -ENOMEM;
		goto out_img;
	}
	ntfs_image_apply(&img, &ap);
	err = ap.read_mft_record(ap.ctx, 0, buf);
	if (!err) {
		uint16_t off = (uint16_t)buf[0x14] | ((uint16_t)buf[0x15] << 8);
		uint32_t used = (uint32_t)buf[0x18] | ((uint32_t)buf[0x19] << 8) |
			((uint32_t)buf[0x1a] << 16) | ((uint32_t)buf[0x1b] << 24);
		err = -ENOENT;
		while (off + 8 <= used) {
			uint32_t type = (uint32_t)buf[off] |
				((uint32_t)buf[off + 1] << 8) |
				((uint32_t)buf[off + 2] << 16) |
				((uint32_t)buf[off + 3] << 24);
			uint32_t len = (uint32_t)buf[off + 4] |
				((uint32_t)buf[off + 5] << 8) |
				((uint32_t)buf[off + 6] << 16) |
				((uint32_t)buf[off + 7] << 24);
			if (type == 0xffffffff)
				break;
			if (len < 8 || off + len > used) {
				err = -EIO;
				break;
			}
			/* Unnamed $DATA: type 0x80, non-resident, name_length 0. */
			if (type == 0x80 && buf[off + 8] && buf[off + 9] == 0) {
				uint64_t a = 0, d = 0;
				int i;
				for (i = 0; i < 8; i++) {
					a |= (uint64_t)buf[off + 0x28 + i] << (8 * i);
					d |= (uint64_t)buf[off + 0x30 + i] << (8 * i);
				}
				*off_out = off;
				*alloc_out = a;
				*data_out = d;
				err = 0;
				break;
			}
			off += (uint16_t)len;
		}
	}
	free(buf);
out_img:
	ntfs_image_close(&img);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Replay the journal of the image at @path. Fills @out; returns 0 on success. */
static int replay_path(const char *path, struct ntfs_logfile_analysis *out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_logfile_replay_device(dev, out);
	ntfs_bdev_close(dev);
	return err;
}

/* Mount @path, look @name up under @parent (NULL: the root), unmount. */
static int lookup_nested_after_mount(const char *path, const char *parent,
				     const char *name)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	err = ntfs_lookup(d, name, &f);
	if (!err && f)
		ntfs_inode_put(f);
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Mount @path, look @name up under the root, unmount. Returns the lookup err. */
static int lookup_after_mount(const char *path, const char *name)
{
	return lookup_nested_after_mount(path, NULL, name);
}

/* Mount @path, look @name up under @parent (NULL: the root), read its
 * size, unmount. Returns the lookup err. */
static int size_after_mount(const char *path, const char *parent,
			    const char *name, u64 *size_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct ntfs_attr at;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	err = ntfs_lookup(d, name, &f);
	if (err)
		goto out_d;
	err = ntfs_getattr(f, &at);
	if (!err)
		*size_out = at.size;
	ntfs_inode_put(f);
out_d:
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Mount @path, look @name up under @parent (NULL: the root), read the
 * first @len bytes at @off into @buf, unmount. The same shape as
 * size_after_mount, for the compressed-write tests' post-replay
 * content checks. Returns the lookup/read err, or -EIO on a short
 * read. */
static int content_after_mount(const char *path, const char *parent,
			       const char *name, void *buf, size_t len,
			       loff_t off)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	ssize_t r;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	err = ntfs_lookup(d, name, &f);
	if (err)
		goto out_d;
	r = ntfs_read(f, buf, len, off);
	err = r == (ssize_t)len ? 0 : (r < 0 ? (int)r : -EIO);
	ntfs_inode_put(f);
out_d:
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Mount @path, look @name up under @parent (NULL: the root), read its
 * full attributes (mode, times, flags), unmount. The same shape as
 * size_after_mount, for the setattr tests' post-replay checks. */
/* Two-level variants of size_after_mount/content_after_mount: the
 * compressed-write convert test's file lives at
 * /compressed/inherit/j.txt, one level deeper than the single-parent
 * helpers reach. */
static int size_after_mount2(const char *path, const char *parent1,
			     const char *parent2, const char *name,
			     u64 *size_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d1 = NULL, *d2 = NULL, *f = NULL;
	struct ntfs_attr at;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	err = ntfs_lookup(root, parent1, &d1);
	if (err)
		goto out_vol;
	err = ntfs_lookup(d1, parent2, &d2);
	if (err)
		goto out_d1;
	err = ntfs_lookup(d2, name, &f);
	if (err)
		goto out_d2;
	err = ntfs_getattr(f, &at);
	if (!err)
		*size_out = at.size;
	ntfs_inode_put(f);
out_d2:
	if (d2)
		ntfs_inode_put(d2);
out_d1:
	if (d1)
		ntfs_inode_put(d1);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

static int content_after_mount2(const char *path, const char *parent1,
				const char *parent2, const char *name,
				void *buf, size_t len, loff_t off)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d1 = NULL, *d2 = NULL, *f = NULL;
	ssize_t r;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	err = ntfs_lookup(root, parent1, &d1);
	if (err)
		goto out_vol;
	err = ntfs_lookup(d1, parent2, &d2);
	if (err)
		goto out_d1;
	err = ntfs_lookup(d2, name, &f);
	if (err)
		goto out_d2;
	r = ntfs_read(f, buf, len, off);
	err = r == (ssize_t)len ? 0 : (r < 0 ? (int)r : -EIO);
	ntfs_inode_put(f);
out_d2:
	if (d2)
		ntfs_inode_put(d2);
out_d1:
	if (d1)
		ntfs_inode_put(d1);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

static int attr_after_mount(const char *path, const char *parent,
			    const char *name, struct ntfs_attr *at_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	err = ntfs_lookup(d, name, &f);
	if (err)
		goto out_d;
	err = ntfs_getattr(f, at_out);
	ntfs_inode_put(f);
out_d:
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Mount @path, look @name up under @parent (NULL: the root), read the
 * xattr @xname into @buf (@bufsz bytes, @len_out the length read),
 * unmount. The same shape as attr_after_mount, for the xattr tests'
 * post-replay checks. A missing xattr returns -ENOATTR. */
static int xattr_after_mount(const char *path, const char *parent,
			    const char *name, const char *xname,
			    void *buf, size_t bufsz, size_t *len_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	err = ntfs_lookup(d, name, &f);
	if (err)
		goto out_d;
	err = ntfs_getxattr(f, xname, buf, bufsz, len_out);
	ntfs_inode_put(f);
out_d:
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* $EA test helpers: drive the VFS xattr handler directly, the
 * test_links.c pattern — the public ntfs_setxattr() stores xattrs
 * as ADS and never reaches ntfs_set_ea(). ea_remove passes
 * XATTR_REPLACE with a NULL value, the handler's remove path. */
static int ea_set(ntfs_inode_t *f, const char *name, const void *val,
		  size_t size)
{
	const struct xattr_handler *h = ntfs_xattr_handlers[0];

	return h->set(h, NULL, NULL, (struct inode *)f, name, val, size, 0);
}

static int ea_set_flags(ntfs_inode_t *f, const char *name, const void *val,
			size_t size, int flags)
{
	const struct xattr_handler *h = ntfs_xattr_handlers[0];

	return h->set(h, NULL, NULL, (struct inode *)f, name, val, size,
		      flags);
}

static int ea_remove(ntfs_inode_t *f, const char *name)
{
	const struct xattr_handler *h = ntfs_xattr_handlers[0];

	return h->set(h, NULL, NULL, (struct inode *)f, name, NULL, 0,
		      XATTR_REPLACE);
}

static int ea_get(ntfs_inode_t *f, const char *name, void *buf, size_t size)
{
	const struct xattr_handler *h = ntfs_xattr_handlers[0];

	return h->get(h, NULL, (struct inode *)f, name, buf, size);
}

/* Mount @path, read the EA @name off @file (@parent NULL and @file
 * NULL means the root directory itself; @parent NULL and @file set
 * means a file in the root) into @buf (@bufsz bytes, @len_out the
 * length read), unmount. The same shape as xattr_after_mount, for
 * the EA tests' post-replay checks. A missing EA returns -ENODATA. */
static int ea_after_mount(const char *path, const char *parent,
			  const char *file, const char *name,
			  void *buf, size_t bufsz, size_t *len_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	if (file) {
		err = ntfs_lookup(d, file, &f);
		if (err)
			goto out_d;
	} else {
		f = d;
	}
	/* ea_get returns the length on success, like getxattr(2);
	 * normalize to 0 so callers read the length from len_out. */
	err = ea_get(f, name, buf, bufsz);
	if (err >= 0) {
		if (len_out)
			*len_out = (size_t)err;
		err = 0;
	}
	if (file)
		ntfs_inode_put(f);
out_d:
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* ---- 1. a regular-file create journals --------------------------------- */

static void test_create_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL, *g = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, sub_ino = 0, mrlsn_file = 0, mrlsn_dir = 0,
		lsn_fn = 0, lsn_index = 0;
	int err, i;

	printf("test_create_journaled\n");
	scratch_path(orig, sizeof(orig), "orig");
	scratch_path(snap, sizeof(snap), "snap");
	scratch_path(snap1, sizeof(snap1), "snap1");
	scratch_path(snap2, sizeof(snap2), "snap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_create_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/*
	 * The file goes in a directory this driver just made, not the
	 * volume root: the mkntfs root already has an $INDEX_ALLOCATION
	 * block and its END entry points down at it, so a create there
	 * lands in the allocation block — which Phase 2 cannot describe
	 * and correctly leaves unjournaled (see test 3). A fresh
	 * directory has no allocation block, so the entry lands in its
	 * $INDEX_ROOT and the create journals.
	 */
	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(f);
	CHECK(ino > 16);
	/* Sanity: the file is visible on the live mount. Put the
	 * created inode first, as the other tests do: the lookup
	 * re-loads it through ntfs_iget. */
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_lookup(d, "j.txt", &g), 0);
	if (g) { ntfs_inode_put(g); g = NULL; }

	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log — what a crash at this instant leaves. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: exactly one transaction, 2 open + 9 data + Commit +
	 * 3 dumps + Forget, in the order the create staged them. The
	 * ninth data record is the $MFT $DATA size extension, staged
	 * first so the redo extends $MFT before it initialises the new
	 * record. The sync then flushes the $MFT folios and the bitmap
	 * folio, and the flush notification (Phase 4 step 2) appends
	 * three DeleteDirtyClusters records after the Forget — one per
	 * flushed cluster (record 0's cluster, the new record + parent
	 * cluster, the $MFT:$BITMAP cluster). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 19);
	if (s.n == 19) {
		static const uint16_t want_op[9] = {
			LOP_SetNewAttributeSizes,
			LOP_InitializeFileRecordSegment, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_AddIndexEntryRoot, LOP_SetBitsInNonresidentBitMap,
		};
		/* The two open records come first, one per attribute the
		 * transaction touches, in first-use order. */
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		for (i = 0; i < 9; i++) {
			CHECK_EQ(s.redo_op[i + 2], want_op[i]);
			/* First eight target $MFT:$DATA (0x18); the
			 * SetBits targets $MFT:$BITMAP (0x40). */
			CHECK_EQ(s.target_attr[i + 2], i < 8 ? 0x18u : 0x40u);
		}
		CHECK_EQ(s.redo_op[11], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[12], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[13], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[14], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[15], LOP_ForgetTransaction);
		/* The sync's flush notification: three
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0 (the record names no
		 * transaction). */
		for (i = 16; i < 19; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
		/* The $FILE_NAME create is the last CreateAttribute, the
		 * record right before the index entry. */
		lsn_fn = s.lsn[8];
		lsn_index = s.lsn[9];
		CHECK_EQ(s.redo_op[8], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[9], LOP_AddIndexEntryRoot);
	}
	/* The name appears in exactly one CreateAttribute's redo: the
	 * $FILE_NAME one, and it is the record before the index entry. */
	CHECK_EQ(s.fn_name_hits, 1);
	CHECK_EQ(s.fn_lsn, lsn_fn);
	/* The SetBits names the new record number, one bit. */
	CHECK_EQ(s.bits_bit, (uint32_t)ino);
	CHECK_EQ(s.bits_count, 1u);
	CHECK_EQ(s.bits_lsn, s.lsn[10]);
	/* The SetBits targets the bitmap's first cluster. */
	CHECK_EQ(s.target_vcn[10], 0u);
	/* The $MFT $DATA size extension: exactly one record, staged
	 * first, its redo the live size, its undo one record less. */
	{
		uint16_t data_off = 0;
		uint64_t alloc = 0, data = 0;
		uint32_t rec_size = 0;
		CHECK_EQ(read_mft0_data_attr(snap, &data_off, &alloc, &data, &rec_size), 0);
		CHECK_EQ(s.sizes_count, 1);
		CHECK_EQ(s.sizes_lsn, s.lsn[2]);
		CHECK_EQ(s.record_off[2], data_off);
		CHECK_EQ(s.target_vcn[2], 0u);
		CHECK_EQ(s.sizes_redo_data, data);
		CHECK_EQ(s.sizes_undo_data, data - rec_size);
		/* Short-form $DATA: no compressed_size field, so the
		 * total is 0 — not the pairs bytes that live at 0x40. */
		CHECK_EQ(s.sizes_redo_total, 0u);
		CHECK(rec_size == 1024u);
	}

	/* Both touched records carry an MR_LSN at or past the record
	 * that touched them: the replay's skip check. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_index);

	/* Replay copy 1: the sync flushed the $MFT folios and the
	 * bitmap folio, so the flush notification retired every dirty
	 * page — the $MFT $DATA size extension, the MFT records and
	 * the SetBits are all skipped (zeroed clusters / MR_LSN) and
	 * the replay redoes nothing. The file is already on the
	 * volume; replay is a no-op. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The file is present on the replayed volume, and it is clean. */
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "j.txt"), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * zero records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 1b. the flush notification retires dirty pages ----------------------
 *
 * Phase 4 step 2: a sync flushes the $MFT folios and the bitmap folio,
 * and the flush notification appends DeleteDirtyClusters records that
 * retire the dirty-page entries the flushed pages carried. The next
 * commit's checkpoint then advances oldest_lsn past the retired
 * transaction's records, so the log can wrap in-session instead of
 * filling until journaling degrades to unjournaled.
 *
 * Positive case: create j.txt, sync (flush + notify), create k.txt,
 * sync. The walk holds only k.txt's 16 records plus the sync's three
 * DeleteDirtyClusters records — j.txt's records are gone, and
 * oldest_lsn is past j.txt's first record.
 *
 * Negative control: create j.txt and k.txt with no sync between
 * them, then sync once. The walk still holds j.txt's records (its
 * pages were dirty at k.txt's commit checkpoint, so nothing retired
 * them yet), and oldest_lsn is still j.txt's first data record. */
static void test_flush_notify_retires_dirty_pages(void)
{
	char orig[1024], snap[1024], snap0[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s, s0;
	uint64_t first_lsn_j = 0, oldest = 0;
	int err, i;

	printf("test_flush_notify_retires_dirty_pages\n");
	scratch_path(orig, sizeof(orig), "fnorig");
	scratch_path(snap, sizeof(snap), "fnsnap");
	scratch_path(snap0, sizeof(snap0), "fnsnap0");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_flush_notify_retires_dirty_pages: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	/* j.txt's first record LSN, from a snapshot taken after the
	 * sync that flushed and notified it. */
	CHECK_EQ(copy_file(orig, snap0), 0);
	CHECK_EQ(walk_log(snap0, &s0), 0);
	CHECK(s0.n >= 1);
	if (s0.n >= 1)
		first_lsn_j = s0.lsn[0];
	CHECK(first_lsn_j != 0);

	/* A second create, then a sync: its commit checkpoints, and
	 * j.txt's entries are already retired, so oldest_lsn must
	 * advance past j.txt's first record. */
	err = ntfs_create(d, "k.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); unlink(snap0); return; }

	/* The walk holds only k.txt's 16 records plus the sync's
	 * three DeleteDirtyClusters records. */
	set_fn_name("k.txt");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 19);
	if (s.n == 19) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[2], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.redo_op[8], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[9], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[15], LOP_ForgetTransaction);
		for (i = 16; i < 19; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
	}
	/* k.txt's $FILE_NAME is the only one in the walk; j.txt's
	 * records are gone. */
	CHECK_EQ(s.fn_name_hits, 1);
	CHECK_EQ(s.fn_lsn, s.lsn[8]);
	/* oldest_lsn advanced past j.txt's first record. */
	CHECK_EQ(log_oldest_lsn(snap, &oldest), 0);
	CHECK(oldest > first_lsn_j);
	CHECK_EQ(oldest, s.lsn[0]);

	/* Both files are present and the volume is clean. */
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "j.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "k.txt"), 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap0);
}

/* ---- 1c. without a sync, dirty pages do not retire -----------------------
 *
 * Negative control for 1b: the same two creates with no sync
 * between them, then one sync. The walk still holds j.txt's
 * records — its pages were dirty at k.txt's commit checkpoint,
 * so nothing retired them yet — and oldest_lsn is still j.txt's
 * first data record. */
static void test_flush_notify_no_sync_no_retire(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	uint64_t oldest = 0;
	int err, i;

	printf("test_flush_notify_no_sync_no_retire\n");
	scratch_path(orig, sizeof(orig), "nnorig");
	scratch_path(snap, sizeof(snap), "nnsnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_flush_notify_no_sync_no_retire: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(f);
	f = NULL;
	/* No sync here: j.txt's pages stay dirty. */
	err = ntfs_create(d, "k.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds j.txt's 14 still-live records (its opens
	 * precede oldest_lsn — oldest sits at its first data record,
	 * the SetNewAttributeSizes), then k.txt's full 16, then the
	 * sync's DeleteDirtyClusters records. */
	set_fn_name("j.txt");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK(s.n >= 30);
	if (s.n >= 30) {
		CHECK_EQ(s.redo_op[0], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.redo_op[13], LOP_ForgetTransaction);
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[29], LOP_ForgetTransaction);
		for (i = 30; i < s.n; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
	}
	/* j.txt's $FILE_NAME is still in the walk. Its records start
	 * at the first data record (the opens precede oldest_lsn),
	 * so the $FILE_NAME is at index 6, not 8. */
	CHECK_EQ(s.fn_name_hits, 1);
	CHECK_EQ(s.fn_lsn, s.lsn[6]);
	/* oldest_lsn is still j.txt's first data record — it did
	 * not advance past the unretired transaction. */
	CHECK_EQ(log_oldest_lsn(snap, &oldest), 0);
	CHECK_EQ(oldest, s.lsn[0]);

	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "j.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "k.txt"), 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
}

/* ---- 2. a mkdir writes no transaction records ------------------------ */

static void test_mkdir_unjournaled(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL;
	struct rec_seen s;
	int err;

	printf("test_mkdir_unjournaled\n");
	scratch_path(orig, sizeof(orig), "morig");
	scratch_path(snap, sizeof(snap), "msnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_mkdir_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "d", 0100755, &d);
	CHECK_EQ(err, 0);
	if (!err) {
		CHECK_EQ(ntfs_volume_sync(vol), 0);
		CHECK_EQ(copy_file(orig, snap), 0);
	}
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* Phase 2 journals regular-file creates only: the log holds no
	 * transaction records at all after a mkdir. The flush
	 * notification (Phase 4 step 2) still appends two
	 * DeleteDirtyClusters records: the aborted mkdir staged
	 * dirty-page entries for the $MFT folio and the bitmap folio
	 * before it aborted, and the sync's writeback notifies them
	 * away (the abort-leak note in JOURNAL-DESIGN.md §4.3). The
	 * records are log-client bookkeeping, not transaction
	 * records, and the reader skips them. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
	}

	/* The directory itself is still there after a remount, and the
	 * volume is clean: unjournaled does not mean lost. */
	CHECK_EQ(lookup_after_mount(snap, "d"), 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
}

/* ---- 3. a mkdir in a driver-made directory journals -------------------- */

static void test_mkdir_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *sub = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, sub_ino = 0, mrlsn_file = 0, mrlsn_dir = 0,
		lsn_fn = 0, lsn_index = 0;
	int err, i;

	printf("test_mkdir_journaled\n");
	scratch_path(orig, sizeof(orig), "morig");
	scratch_path(snap, sizeof(snap), "msnap");
	scratch_path(snap1, sizeof(snap1), "msnap1");
	scratch_path(snap2, sizeof(snap2), "msnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_mkdir_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* "sub" goes in the root and so stays unjournaled (test 2); the
	 * mkdir under test goes in "sub", whose $INDEX_ROOT is resident
	 * and describable. A mkdir is a create with an $INDEX_ROOT
	 * instead of a $DATA attribute: the same 16 records, the same
	 * order, the $INDEX_ROOT CreateAttribute sitting exactly where
	 * the file's $DATA CreateAttribute sits. */
	err = ntfs_mkdir(root, "sub", 0100755, &sub);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(sub);

	err = ntfs_mkdir(sub, "jdir", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(d);
	CHECK(ino > 16);
	ntfs_inode_put(d);
	d = NULL;
	CHECK_EQ(ntfs_lookup(sub, "jdir", &f), 0);
	if (f) { ntfs_inode_put(f); f = NULL; }

	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log — what a crash at this instant leaves. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (sub) { ntfs_inode_put(sub); sub = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: exactly one transaction, 2 open + 9 data + Commit +
	 * 3 dumps + Forget, in the order the create staged them. The
	 * five CreateAttributes are $SECURITY_DESCRIPTOR, $INDEX_ROOT,
	 * $EA_INFORMATION, $EA and $FILE_NAME — the $INDEX_ROOT in the
	 * slot the file's $DATA occupies. The sync then flushes the
	 * $MFT folios and the bitmap folio, and the flush notification
	 * (Phase 4 step 2) appends three DeleteDirtyClusters records
	 * after the Forget. */
	/* The $FILE_NAME scan looks for the created name. */
	set_fn_name("jdir");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 19);
	if (s.n == 19) {
		static const uint16_t want_op[9] = {
			LOP_SetNewAttributeSizes,
			LOP_InitializeFileRecordSegment, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_AddIndexEntryRoot, LOP_SetBitsInNonresidentBitMap,
		};
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		for (i = 0; i < 9; i++) {
			CHECK_EQ(s.redo_op[i + 2], want_op[i]);
			CHECK_EQ(s.target_attr[i + 2], i < 8 ? 0x18u : 0x40u);
		}
		CHECK_EQ(s.redo_op[11], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[12], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[13], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[14], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[15], LOP_ForgetTransaction);
		/* The sync's flush notification: three
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		for (i = 16; i < 19; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
		/* The $FILE_NAME create is the last CreateAttribute, the
		 * record right before the index entry. */
		lsn_fn = s.lsn[8];
		lsn_index = s.lsn[9];
		CHECK_EQ(s.redo_op[8], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[9], LOP_AddIndexEntryRoot);
	}
	/* The name appears in exactly one CreateAttribute's redo: the
	 * $FILE_NAME one, and it is the record before the index entry. */
	CHECK_EQ(s.fn_name_hits, 1);
	CHECK_EQ(s.fn_lsn, lsn_fn);
	/* The SetBits names the new record number, one bit. */
	CHECK_EQ(s.bits_bit, (uint32_t)ino);
	CHECK_EQ(s.bits_count, 1u);
	CHECK_EQ(s.bits_lsn, s.lsn[10]);
	/* The SetBits targets the bitmap's first cluster. */
	CHECK_EQ(s.target_vcn[10], 0u);
	/* The $MFT $DATA size extension: exactly one record, staged
	 * first, its redo the live size, its undo one record less. */
	{
		uint16_t data_off = 0;
		uint64_t alloc = 0, data = 0;
		uint32_t rec_size = 0;
		CHECK_EQ(read_mft0_data_attr(snap, &data_off, &alloc, &data, &rec_size), 0);
		CHECK_EQ(s.sizes_count, 1);
		CHECK_EQ(s.sizes_lsn, s.lsn[2]);
		CHECK_EQ(s.record_off[2], data_off);
		CHECK_EQ(s.target_vcn[2], 0u);
		CHECK_EQ(s.sizes_redo_data, data);
		CHECK_EQ(s.sizes_undo_data, data - rec_size);
		CHECK_EQ(s.sizes_redo_total, 0u);
		CHECK(rec_size == 1024u);
	}

	/* Both touched records carry an MR_LSN at or past the record
	 * that touched them: the replay's skip check. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_index);

	/* Replay copy 1: the sync flushed the $MFT folios and the
	 * bitmap folio, so the flush notification retired every dirty
	 * page and the replay redoes nothing. The directory is already
	 * on the volume; replay is a no-op. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The directory is present on the replayed volume, and a file
	 * can be created inside it: that is what proves the replayed
	 * record carries a usable $INDEX_ROOT, not just the name. */
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "jdir"), 0);
	{
		struct ntfs_bdev *dev1;
		ntfs_volume_t *vol1 = NULL;
		ntfs_inode_t *root1 = NULL, *sub1 = NULL, *d1 = NULL,
			*f1 = NULL;

		dev1 = ntfs_bdev_open_path(snap1, false);
		CHECK(dev1 != NULL);
		if (dev1) {
			err = ntfs_mount(dev1, &mopts, &vol1);
			CHECK_EQ(err, 0);
			if (!err) {
				CHECK_EQ(ntfs_volume_root(vol1, &root1), 0);
				if (root1) {
					CHECK_EQ(ntfs_lookup(root1, "sub", &sub1), 0);
					if (sub1) {
						CHECK_EQ(ntfs_lookup(sub1, "jdir", &d1), 0);
						if (d1) {
							CHECK_EQ(ntfs_create(d1, "x.txt", 0100644, &f1), 0);
							if (f1) { ntfs_inode_put(f1); f1 = NULL; }
							ntfs_inode_put(d1);
							d1 = NULL;
						}
						ntfs_inode_put(sub1);
						sub1 = NULL;
					}
					ntfs_inode_put(root1);
					root1 = NULL;
				}
				CHECK_EQ(ntfs_volume_sync(vol1), 0);
				ntfs_unmount(vol1);
			}
			ntfs_bdev_close(dev1);
		}
	}
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * zero records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 3. a create in the volume root writes no transaction records -- */

/* ---- 4. a create in the root falls back to unjournaled -------------- */

static void test_create_in_root_unjournaled(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct rec_seen s;
	int err;

	printf("test_create_in_root_unjournaled\n");
	scratch_path(orig, sizeof(orig), "rorig");
	scratch_path(snap, sizeof(snap), "rsnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_create_in_root_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* The create itself must succeed: the entry goes to the root's
	 * $INDEX_ALLOCATION block, which the journal cannot describe,
	 * so the transaction aborts and the create proceeds unjournaled
	 * instead of failing with -ENOENT. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (!err) {
		CHECK_EQ(ntfs_volume_sync(vol), 0);
		CHECK_EQ(copy_file(orig, snap), 0);
	}
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The create proceeds unjournaled, so the log holds no
	 * transaction records — but the flush notification (Phase 4
	 * step 2) appends two DeleteDirtyClusters records for the
	 * stale dirty-page entries the aborted transaction staged
	 * (the abort-leak note in JOURNAL-DESIGN.md §4.3). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
	}

	CHECK_EQ(lookup_after_mount(snap, "r.txt"), 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
}

/* ---- 5. a regular-file unlink journals --------------------------------- */

static void test_unlink_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, sub_ino = 0, mrlsn_file = 0, mrlsn_dir = 0,
		lsn_del_idx = 0, lsn_del_attr = 0;
	int err, i;

	printf("test_unlink_journaled\n");
	scratch_path(orig, sizeof(orig), "uorig");
	scratch_path(snap, sizeof(snap), "usnap");
	scratch_path(snap1, sizeof(snap1), "usnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_unlink_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(f);
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* The unlink: ntfs_unlink looks the file up, deletes the name,
	 * and iputs it — the evict then deallocates the record, clears
	 * the $MFT bitmap bit and frees the clusters, unjournaled. */
	CHECK_EQ(ntfs_unlink(d, "j.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk from the oldest LSN holds 13 records: the unlink's
	 * 11, plus the sync's two DeleteDirtyClusters records. The
	 * create's records are gone: the sync after the create
	 * flushed the $MFT folios and the bitmap folio, the flush
	 * notification (Phase 4 step 2) retired the create's dirty
	 * pages, and the unlink's commit checkpoint advanced
	 * oldest_lsn past them. That is the point of the
	 * notification — a crash after the sync needs none of the
	 * create's redo records, because the pages are already on
	 * disk. The unlink transaction is 2 open + 4 data + Commit +
	 * 3 dumps + Forget: it touches two attributes, so it opens
	 * both ($MFT:$DATA for the index, the $FILE_NAME and the
	 * Deallocate; $MFT:$BITMAP for the ClearBits), and its data
	 * records are DeleteIndexEntryRoot, DeleteAttribute,
	 * DeallocateFileRecordSegment and ClearBitsInNonresidentBitMap
	 * — the free is part of the unlink transaction, not a
	 * separate evict one. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 13);
	if (s.n == 13) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[3], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[4], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[5], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[6], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[7], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[8], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[9], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[10], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[11], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[11], 0u);
		CHECK_EQ(s.redo_op[12], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[12], 0u);
		/* The index, $FILE_NAME and Deallocate records target
		 * $MFT:$DATA (0x18): the index entry lives in the
		 * parent's record, the $FILE_NAME and the free in the
		 * file's own. The ClearBits targets $MFT:$BITMAP
		 * (0x40). */
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.target_attr[4], 0x18u);
		CHECK_EQ(s.target_attr[5], 0x40u);
		/* The file's record is at vcn ino/4 (1024-byte records
		 * in 4096-byte clusters), the parent's at sub_ino/4;
		 * the bitmap bit for the file's record is in cluster
		 * (ino/8)/4096 of $MFT:$BITMAP. */
		CHECK_EQ(s.target_vcn[2], sub_ino / 4);
		CHECK_EQ(s.target_vcn[3], ino / 4);
		CHECK_EQ(s.target_vcn[4], ino / 4);
		CHECK_EQ(s.target_vcn[5], (ino / 8) / 4096);
		lsn_del_idx = s.lsn[2];
		lsn_del_attr = s.lsn[3];
	}

	/* Both touched records carry an MR_LSN at or past the record
	 * that touched them — even though the file's record was
	 * deallocated afterwards by evict, the stamp survives in it. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_del_attr);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_del_idx);

	/* Replay copy 1: the sync flushed the $MFT folios and the
	 * bitmap folio, so the flush notification retired the
	 * create's and the unlink's dirty pages alike — the replay
	 * redoes nothing. The file is already gone on the volume
	 * (the unlink's delete landed on disk), and the replayed
	 * image is ntfsck-clean: the free is in the unlink
	 * transaction, so no SetBits redo resurrects the freed
	 * record's bit.
	 */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);
	CHECK(!an.needs_chkdsk);

	/* The file is gone on the replayed volume, and it is clean. */
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* The file is gone on the live snapshot too, and it is clean. */
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	(void)i;
	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 6. an unlink in the root falls back to unjournaled --------------- */

static void test_unlink_in_root_unjournaled(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct rec_seen s;
	int err;

	printf("test_unlink_in_root_unjournaled\n");
	scratch_path(orig, sizeof(orig), "ruorig");
	scratch_path(snap, sizeof(snap), "rusnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_unlink_in_root_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* Create in the root (unjournaled — see test 3), then unlink
	 * it: the entry lives in the root's $INDEX_ALLOCATION block,
	 * which the unlink transaction cannot describe, so it aborts
	 * and the unlink proceeds unjournaled instead of failing. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(ntfs_unlink(root, "r.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The create and the unlink both proceed unjournaled, so the
	 * log holds no transaction records — but the flush
	 * notification (Phase 4 step 2) appends two
	 * DeleteDirtyClusters records for the stale dirty-page
	 * entries the aborted transactions staged (the abort-leak
	 * note in JOURNAL-DESIGN.md §4.3). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
	}

	CHECK_EQ(lookup_after_mount(snap, "r.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
}

/* ---- 7. an rmdir journals like an unlink ------------------------------ */

static void test_rmdir_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *e = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t sub_ino = 0, sub2_ino = 0;
	int err;

	printf("test_rmdir_journaled\n");
	scratch_path(orig, sizeof(orig), "dorig");
	scratch_path(snap, sizeof(snap), "dsnap");
	scratch_path(snap1, sizeof(snap1), "dsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_rmdir_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);
	err = ntfs_mkdir(d, "sub2", 0100755, &e);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub2_ino = ntfs_inode_number(e);
	ntfs_inode_put(e);
	e = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* rmdir of the empty driver-made directory: the same four
	 * data records as a file unlink (index entry + $FILE_NAME
	 * + Deallocate + ClearBits). */
	CHECK_EQ(ntfs_rmdir(d, "sub2"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (e) { ntfs_inode_put(e); e = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds only the rmdir's 11 records, plus the sync's
	 * two DeleteDirtyClusters records. The setup mkdir's records
	 * are gone: the sync after the mkdir flushed the $MFT folios
	 * and the bitmap folio, the flush notification (Phase 4
	 * step 2) retired the mkdir's dirty pages, and the rmdir's
	 * commit checkpoint advanced oldest_lsn past them. Before
	 * the mkdir class the setup mkdir was unjournaled and the
	 * walk held only the rmdir's 11. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 13);
	if (s.n == 13) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[3], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[4], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[5], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[6], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[7], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[8], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[9], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[10], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[11], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[11], 0u);
		CHECK_EQ(s.redo_op[12], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[12], 0u);
		CHECK_EQ(s.target_vcn[2], sub_ino / 4);
		CHECK_EQ(s.target_vcn[3], sub2_ino / 4);
		CHECK_EQ(s.target_vcn[4], sub2_ino / 4);
		CHECK_EQ(s.target_vcn[5], (sub2_ino / 8) / 4096);
	}

	memset(&an, 0, sizeof(an));
	CHECK_EQ(copy_file(snap, snap1), 0);
	CHECK_EQ(replay_path(snap1, &an), 0);
	/* No records redo: the sync flushed the $MFT folios and the
	 * bitmap folio, so the flush notification retired the mkdir's
	 * and the rmdir's dirty pages alike. The directory is already
	 * gone on the volume. */
	CHECK_EQ(an.records_redone, 0u);
	CHECK(!an.needs_chkdsk);

	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "sub2"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 8. a same-directory rename journals the link + old-name removal */

static void test_rename_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, sub_ino = 0, mrlsn_file = 0, mrlsn_dir = 0,
		lsn_create_attr = 0, lsn_add_idx = 0, lsn_del_idx = 0,
		lsn_del_attr = 0;
	int err;

	printf("test_rename_journaled\n");
	scratch_path(orig, sizeof(orig), "rnorig");
	scratch_path(snap, sizeof(snap), "rnsnap");
	scratch_path(snap1, sizeof(snap1), "rnsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_rename_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(f);
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* The rename: same directory, no target to replace. */
	CHECK_EQ(ntfs_rename(d, "j.txt", d, "k.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds 11 records: the rename's 10, plus the sync's
	 * one DeleteDirtyClusters record. The create's records are
	 * gone: the sync after the create flushed the $MFT folios
	 * and the bitmap folio, the flush notification (Phase 4
	 * step 2) retired the create's dirty pages, and the rename's
	 * commit checkpoint advanced oldest_lsn past them. The
	 * rename is 1 open ($MFT:$DATA: the rename allocates and
	 * frees nothing, so no $MFT:$BITMAP open record) + 4 data +
	 * Commit + 3 dumps + Forget. The rename's data records are
	 * the link half first (CreateAttribute for the new
	 * $FILE_NAME, AddIndexEntryRoot for the new entry) then the
	 * old name's removal (DeleteIndexEntryRoot,
	 * DeleteAttribute), in live order. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 11);
	if (s.n == 11) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[2], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[3], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[4], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[5], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[6], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[7], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[8], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[9], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[10], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[10], 0u);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.target_attr[4], 0x18u);
		/* The new $FILE_NAME and the old one's removal target
		 * the file's record; both index records target the
		 * parent's. */
		CHECK_EQ(s.target_vcn[1], ino / 4);
		CHECK_EQ(s.target_vcn[2], sub_ino / 4);
		CHECK_EQ(s.target_vcn[3], sub_ino / 4);
		CHECK_EQ(s.target_vcn[4], ino / 4);
		lsn_create_attr = s.lsn[1];
		lsn_add_idx = s.lsn[2];
		lsn_del_idx = s.lsn[3];
		lsn_del_attr = s.lsn[4];
	}

	/* Both touched records carry an MR_LSN at or past the record
	 * that touched them. The file's record was touched by the
	 * CreateAttribute and the DeleteAttribute; the parent's by
	 * both index records. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_create_attr);
	CHECK(mrlsn_file >= lsn_del_attr);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_add_idx);
	CHECK(mrlsn_dir >= lsn_del_idx);

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the create's and the rename's dirty pages alike. The file
	 * is under its new name and ntfsck is clean. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);
	CHECK(!an.needs_chkdsk);

	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "k.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "k.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 9. a rename in the root falls back to unjournaled ---------------- */

static void test_rename_in_root_unjournaled(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct rec_seen s;
	int err;

	printf("test_rename_in_root_unjournaled\n");
	scratch_path(orig, sizeof(orig), "rrorig");
	scratch_path(snap, sizeof(snap), "rrsnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_rename_in_root_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* Create in the root (unjournaled — see test 3), then rename
	 * it in the root: both entries live in the root's
	 * $INDEX_ALLOCATION block, which the rename transaction
	 * cannot describe, so it aborts and the rename proceeds
	 * unjournaled instead of failing. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(ntfs_rename(root, "r.txt", root, "s.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The create and the rename both proceed unjournaled, so the
	 * log holds no transaction records — but the flush
	 * notification (Phase 4 step 2) appends three
	 * DeleteDirtyClusters records for the stale dirty-page
	 * entries the aborted transactions staged (the abort-leak
	 * note in JOURNAL-DESIGN.md §4.3). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 3);
	if (s.n == 3) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[2], 0u);
	}

	CHECK_EQ(lookup_after_mount(snap, "s.txt"), 0);
	CHECK_EQ(lookup_after_mount(snap, "r.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
}

/* ---- 10. renaming over an existing file journals the target's removal -- */

static void test_rename_over_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t j_ino = 0, k_ino = 0, sub_ino = 0;
	int err;

	printf("test_rename_over_journaled\n");
	scratch_path(orig, sizeof(orig), "roorig");
	scratch_path(snap, sizeof(snap), "rosnap");
	scratch_path(snap1, sizeof(snap1), "rosnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_rename_over_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	j_ino = ntfs_inode_number(f);
	ntfs_inode_put(f);
	f = NULL;
	err = ntfs_create(d, "k.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	k_ino = ntfs_inode_number(f);
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Rename j.txt over k.txt: the target's removal (index entry
	 * + $FILE_NAME + record free + bitmap bit) is staged in the
	 * same transaction as the link and the old name's removal. */
	CHECK_EQ(ntfs_rename(d, "j.txt", d, "k.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds 45 records: the two creates' still-live
	 * ones (14 for the first, 16 for the second — see below),
	 * plus the rename's 15 — 2 open ($MFT:$DATA and
	 * $MFT:$BITMAP: the target's free touches the bitmap) + 8
	 * data + Commit + 3 dumps + Forget. The rename's data
	 * records are the target's removal first (DeleteIndexEntryRoot,
	 * DeleteAttribute, DeallocateFileRecordSegment,
	 * ClearBitsInNonresidentBitMap), then the link half
	 * (CreateAttribute, AddIndexEntryRoot), then the old name's
	 * removal (DeleteIndexEntryRoot, DeleteAttribute). */
	/* The walk holds 17 records: the rename's 15, plus the sync's
	 * two DeleteDirtyClusters records. Both creates' records are
	 * gone: the sync after the creates flushed the $MFT folios
	 * and the bitmap folio, the flush notification (Phase 4
	 * step 2) retired the creates' dirty pages, and the rename's
	 * commit checkpoint advanced oldest_lsn past them. The
	 * rename is 2 open ($MFT:$DATA and $MFT:$BITMAP: the
	 * target's free touches the bitmap) + 8 data + Commit + 3
	 * dumps + Forget. The rename's data records are the target's
	 * removal first (DeleteIndexEntryRoot, DeleteAttribute,
	 * DeallocateFileRecordSegment,
	 * ClearBitsInNonresidentBitMap), then the link half
	 * (CreateAttribute, AddIndexEntryRoot), then the old name's
	 * removal (DeleteIndexEntryRoot, DeleteAttribute). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 17);
	if (s.n == 17) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[3], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[4], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[5], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[6], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[7], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[8], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[9], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[10], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[11], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[12], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[13], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[14], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[15], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[15], 0u);
		CHECK_EQ(s.redo_op[16], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[16], 0u);
		CHECK_EQ(s.target_vcn[2], sub_ino / 4);
		CHECK_EQ(s.target_vcn[3], k_ino / 4);
		CHECK_EQ(s.target_vcn[4], k_ino / 4);
		CHECK_EQ(s.target_vcn[5], (k_ino / 8) / 4096);
		CHECK_EQ(s.target_vcn[6], j_ino / 4);
		CHECK_EQ(s.target_vcn[7], sub_ino / 4);
		CHECK_EQ(s.target_vcn[8], sub_ino / 4);
		CHECK_EQ(s.target_vcn[9], j_ino / 4);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the creates' and the rename's dirty pages alike. The file
	 * is under the target name with the source's inode, and
	 * ntfsck is clean. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);
	CHECK(!an.needs_chkdsk);

	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "k.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "k.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 11. a rename across driver-made directories ---------------------- */

static void test_rename_cross_dir_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d1 = NULL, *d2 = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, d1_ino = 0, d2_ino = 0, mrlsn_file = 0,
		mrlsn_d1 = 0, mrlsn_d2 = 0, lsn_create_attr = 0,
		lsn_add_idx = 0, lsn_del_idx = 0, lsn_del_attr = 0;
	int err;

	printf("test_rename_cross_dir_journaled\n");
	scratch_path(orig, sizeof(orig), "rxorig");
	scratch_path(snap, sizeof(snap), "rxsnap");
	scratch_path(snap1, sizeof(snap1), "rxsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_rename_cross_dir_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "d1", 0100755, &d1);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	d1_ino = ntfs_inode_number(d1);
	err = ntfs_mkdir(root, "d2", 0100755, &d2);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	d2_ino = ntfs_inode_number(d2);

	err = ntfs_create(d1, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(f);
	ntfs_inode_put(f);
	f = NULL;
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* The rename: d1/j.txt -> d2/j.txt, no target to replace. */
	CHECK_EQ(ntfs_rename(d1, "j.txt", d2, "j.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d2) { ntfs_inode_put(d2); d2 = NULL; }
	if (d1) { ntfs_inode_put(d1); d1 = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds 11 records: the rename's 10, plus the sync's
	 * one DeleteDirtyClusters record. The create's records are
	 * gone: the sync after the create flushed the $MFT folios
	 * and the bitmap folio, the flush notification (Phase 4
	 * step 2) retired the create's dirty pages, and the rename's
	 * commit checkpoint advanced oldest_lsn past them. The
	 * rename's 10 are the same four data records as a
	 * same-directory rename, split across the two parents: the
	 * link half targets d2, the old name's removal targets d1. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 11);
	if (s.n == 11) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[2], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[3], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[4], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[5], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[6], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[7], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[8], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[9], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[10], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[10], 0u);
		CHECK_EQ(s.target_vcn[1], ino / 4);
		CHECK_EQ(s.target_vcn[2], d2_ino / 4);
		CHECK_EQ(s.target_vcn[3], d1_ino / 4);
		CHECK_EQ(s.target_vcn[4], ino / 4);
		lsn_create_attr = s.lsn[1];
		lsn_add_idx = s.lsn[2];
		lsn_del_idx = s.lsn[3];
		lsn_del_attr = s.lsn[4];
	}

	/* All three touched records carry an MR_LSN at or past the
	 * record that touched them. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_create_attr);
	CHECK(mrlsn_file >= lsn_del_attr);
	CHECK_EQ(read_mft_lsn(snap, d2_ino, &mrlsn_d2), 0);
	CHECK(mrlsn_d2 != 0);
	CHECK(mrlsn_d2 >= lsn_add_idx);
	CHECK_EQ(read_mft_lsn(snap, d1_ino, &mrlsn_d1), 0);
	CHECK(mrlsn_d1 != 0);
	CHECK(mrlsn_d1 >= lsn_del_idx);

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the create's and the rename's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);
	CHECK(!an.needs_chkdsk);

	CHECK_EQ(lookup_nested_after_mount(snap1, "d2", "j.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap1, "d1", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	CHECK_EQ(lookup_nested_after_mount(snap, "d2", "j.txt"), 0);
	CHECK_EQ(lookup_nested_after_mount(snap, "d1", "j.txt"), -ENOENT);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 12. a truncate of a non-resident file journals -------------------- */

/* Shared setup for the truncate tests: a file with 256 KiB of data in
 * a driver-made directory, synced — so its $DATA is non-resident and
 * its clusters are allocated and flushed. The volume stays mounted
 * on success: the caller truncates, syncs and snapshots, then puts
 * the inodes and unmounts. Returns 0 on success. */
static int truncate_setup(const char *orig, struct ntfs_bdev **dev_out,
			  ntfs_volume_t **vol_out,
			  ntfs_inode_t **root_out,
			  ntfs_inode_t **d_out, ntfs_inode_t **f_out,
			  uint64_t *ino_out)
{
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	u8 *buf;
	int err;

	dev = ntfs_bdev_open_path(orig, false);
	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err) { ntfs_bdev_close(dev); return err; }
	err = ntfs_volume_root(vol, &root);
	if (err) { ntfs_unmount(vol); ntfs_bdev_close(dev); return err; }
	err = ntfs_mkdir(root, "sub", 0100755, &d);
	if (err)
		goto out;
	err = ntfs_create(d, "j.txt", 0100644, &f);
	if (err)
		goto out;
	buf = malloc(256 * 1024);
	if (!buf) {
		err = -ENOMEM;
		goto out;
	}
	memset(buf, 'z', 256 * 1024);
	if (ntfs_write(f, buf, 256 * 1024, 0) != 256 * 1024)
		err = -EIO;
	free(buf);
	if (err)
		goto out;
	*ino_out = ntfs_inode_number(f);
	err = ntfs_volume_sync(vol);
	if (err)
		goto out;
	*dev_out = dev;
	*vol_out = vol;
	*root_out = root;
	*d_out = d;
	*f_out = f;
	return 0;

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	return err;
}

static void test_truncate_shrink_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, size = 0;
	int err, i;

	printf("test_truncate_shrink_journaled\n");
	scratch_path(orig, sizeof(orig), "tsorig");
	scratch_path(snap, sizeof(snap), "tssnap");
	scratch_path(snap1, sizeof(snap1), "tssnap1");
	scratch_path(snap2, sizeof(snap2), "tssnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_truncate_shrink_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }
	CHECK(ino > 16);
	/* Shrink 256 KiB -> 64 KiB: 48 clusters freed, one run. */
	err = ntfs_truncate(f, 64 * 1024);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the shrink transaction's 13 records, plus the
	 * sync's two DeleteDirtyClusters records. The create's
	 * records are gone: the sync after the create flushed the
	 * $MFT folios and the bitmap folio, the flush notification
	 * (Phase 4 step 2) retired the create's dirty pages, and
	 * the shrink's commit checkpoint advanced oldest_lsn past
	 * them. ntfs_truncate routes through ntfs_setattr, so the
	 * resize joins the setattr transaction: the truncate's 6
	 * (2 open + 4 data) plus the setattr's 2 (the $SI 36 bytes,
	 * then a second UpdateFileNameRoot with the same dup — the
	 * setattr stages its own $FILE_NAME record from its begin
	 * snapshot, it does not dedupe against the truncate's),
	 * plus Commit + 3 dumps + Forget. The four truncate data
	 * records are the $Bitmap ClearBits for the freed run, the
	 * UpdateMappingPairs for the shorter run list, the
	 * SetNewAttributeSizes for the new sizes, and the
	 * UpdateFileNameRoot for the parent's $FILE_NAME — in that
	 * order: clusters first, then the pairs that name them,
	 * then the sizes, then the parent's duplicated sizes. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 15);
	if (s.n != 15) {
		int k;
		for (k = 0; k < s.n; k++)
			fprintf(stderr, "DBG ow op[%d]=%u attr=0x%x lsn=%llu\n",
				k, s.redo_op[k], s.target_attr[k],
				(unsigned long long)s.lsn[k]);
	}
	if (s.n == 15) {
		/* The opens come in first-use order: the truncate
		 * stages the $Bitmap record first, so the volume
		 * $Bitmap ($Bitmap:$DATA) opens before $MFT:$DATA —
		 * the reverse of the create. The ClearBits targets
		 * the attribute the truncate opened first, and that
		 * attribute is NOT the one the create's SetBits used
		 * ($MFT:$BITMAP, MFT-record bits). */
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], s.clear_bits_target_attr);
		CHECK(s.target_attr[0] != s.bits_target_attr);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[2], s.clear_bits_target_attr);
		CHECK(s.clear_bits_target_attr != s.bits_target_attr);
		CHECK_EQ(s.redo_op[3], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[4], 0x18u);
		CHECK_EQ(s.redo_op[5], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[5], 0x18u);
		/* The setattr's two records, staged at finish after
		 * the truncate's: the $SI 36 bytes (mtime/ctime moved
		 * by the resize) and a second UpdateFileNameRoot —
		 * same dup as [5], staged from the setattr's begin
		 * snapshot. Both address the file's record through
		 * $MFT:$DATA (0x18). */
		CHECK_EQ(s.redo_op[6], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[6], 0x18u);
		CHECK_EQ(s.redo_op[7], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[7], 0x18u);
		CHECK_EQ(s.redo_op[8], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[9], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[10], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[11], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[12], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[13], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[13], 0u);
		CHECK_EQ(s.redo_op[14], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[14], 0u);
		/* The $FILE_NAME record names the new sizes (64 KiB)
		 * and undoes to the old ones (256 KiB); the parent's
		 * record is addressed through $MFT:$DATA, like the
		 * file's own. */
		CHECK_EQ(s.fn_redo_data, 64u * 1024u);
		CHECK_EQ(s.fn_undo_data, 256u * 1024u);
		CHECK_EQ(s.fn_redo_alloc, 64u * 1024u);
		CHECK_EQ(s.fn_undo_alloc, 256u * 1024u);
		/* The collector keeps the LAST $FILE_NAME record: the
		 * setattr's dup at [7], identical to the truncate's
		 * at [5]. */
		CHECK_EQ(s.fnupd_lsn, s.lsn[7]);
		/* The ClearBits frees 48 clusters, one run. (The
		 * SetBits fields still hold the create's 1-bit record;
		 * the clear has its own capture.) */
		CHECK_EQ(s.clear_bits_count, 48u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[2]);
		/* The ClearBits targets the VOLUME $Bitmap
		 * ($Bitmap:$DATA, record 6) — not $MFT:$BITMAP
		 * (record 0, type 0xB0), which holds MFT-record
		 * bits: the assertions above pin that by the
		 * in-log ids (the create's SetBits is the
		 * $MFT:$BITMAP one). Staging cluster bits against
		 * $MFT:$BITMAP applied them to the $MFT:$BITMAP's
		 * own cluster on replay and left the freed
		 * clusters marked allocated in the real $Bitmap:
		 * a leak ntfsck flags after a crash (truncate
		 * crash sweep, 2026-11-27). The page LCN — the
		 * $Bitmap's own cluster — is proved by that sweep,
		 * which replays a torn truncate and checks the
		 * bitmap on disk. */
		/* The sizes record names the new data size. */
		CHECK_EQ(s.sizes_redo_data, 64u * 1024u);
		CHECK_EQ(s.sizes_undo_data, 256u * 1024u);
		CHECK_EQ(s.sizes_lsn, s.lsn[4]);
		/* The pairs record is the $DATA attribute, at the
		 * same record offset as the sizes record. */
		CHECK_EQ(s.record_off[3], s.record_off[4]);
	}
	(void)i;

	/* Replay copy 1: nothing redoes — the sync flushed the $MFT
	 * folios and the bitmap folios, so the flush notification
	 * retired the create's and the shrink's dirty pages alike.
	 * The file is already 64 KiB on the volume. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The file is 64 KiB on the replayed volume, and it is clean. */
	CHECK_EQ(size_after_mount(snap1, "sub", "j.txt", &size), 0);
	CHECK_EQ(size, 64u * 1024u);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * five records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 13. an extend of a non-resident file journals ---------------------- */

static void test_truncate_extend_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, size = 0;
	int err;

	printf("test_truncate_extend_journaled\n");
	scratch_path(orig, sizeof(orig), "teorig");
	scratch_path(snap, sizeof(snap), "tesnap");
	scratch_path(snap1, sizeof(snap1), "tesnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_truncate_extend_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }
	CHECK(ino > 16);

	/* Shrink journaled, then extend back to 256 KiB journaled: the
	 * walk then holds the create (14) + shrink transaction (13)
	 * + extend transaction (12) = 39. Each ntfs_truncate routes
	 * through ntfs_setattr, so each resize joins its setattr
	 * transaction and adds the setattr's two records (the $SI
	 * 36 bytes, then a second UpdateFileNameRoot with the same
	 * dup). The extend is what this test reads. It allocates
	 * nothing: the expand takes the hole path (HOLES_OK), so the
	 * run list gains a 48-cluster hole and the file flips sparse
	 * — the pairs record carries the old and new header (flags,
	 * name_off, pairs_off 0x40 -> 0x48) and there is no $Bitmap
	 * record at all, so the extend has a single open ($DATA).
	 * The flip also sets the sparse bit in the $STANDARD_
	 * INFORMATION flags, which the truncate stages as a fourth
	 * data record (UpdateResidentValue) so the replay keeps the
	 * $DATA and $SI in agreement — and a fifth, the parent's
	 * $FILE_NAME, so the replay keeps the duplicated sizes in
	 * agreement too. */
	err = ntfs_truncate(f, 64 * 1024);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	err = ntfs_truncate(f, 256 * 1024);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	CHECK_EQ(ntfs_volume_sync(vol), 0);

out:
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 25);
	if (s.n == 25) {
		/* The shrink's records sit at [0..10] — its two opens
		 * precede oldest_lsn and are dispensable, so the walk
		 * starts at its ClearBits (its $FILE_NAME at [3], its
		 * setattr's $SI at [4] and second $FILE_NAME at [5],
		 * as in the shrink test). The extend's records sit at
		 * [11..22]: one open ($DATA — no $Bitmap record, so
		 * $BITMAP never opens), then pairs, sizes, the $SI
		 * flags, the parent's $FILE_NAME, then the setattr's
		 * $SI and second $FILE_NAME, Commit, dumps, Forget.
		 * The create's records are gone: the sync after the
		 * create flushed the $MFT folios and the bitmap folio,
		 * the flush notification (Phase 4 step 2) retired the
		 * create's dirty pages, and the shrink's commit
		 * checkpoint advanced oldest_lsn past them. */
		CHECK_EQ(s.redo_op[3], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[4], 0x18u);
		CHECK_EQ(s.redo_op[5], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[5], 0x18u);
		CHECK_EQ(s.redo_op[11], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[11], 0x18u);
		CHECK_EQ(s.redo_op[12], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[12], 0x18u);
		CHECK_EQ(s.redo_op[13], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[13], 0x18u);
		CHECK_EQ(s.redo_op[14], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[15], 0x18u);
		CHECK_EQ(s.redo_op[16], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[16], 0x18u);
		CHECK_EQ(s.redo_op[17], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[17], 0x18u);
		CHECK_EQ(s.redo_op[18], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[19], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[20], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[21], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[22], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[23], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[23], 0u);
		CHECK_EQ(s.redo_op[24], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[24], 0u);
		/* The extend's $FILE_NAME record (the last one the
		 * collector saw — the setattr's second dup at [17])
		 * names the new sizes (256 KiB) and undoes to the old
		 * ones (64 KiB); the sparse flag flips with it (0x200
		 * set, 0x0 undone). */
		CHECK_EQ(s.fn_redo_data, 256u * 1024u);
		CHECK_EQ(s.fn_undo_data, 64u * 1024u);
		CHECK_EQ(s.fn_redo_alloc, 64u * 1024u);
		CHECK_EQ(s.fn_undo_alloc, 64u * 1024u);
		/* The flags flip sparse (0x200) with no ARCHIVE bit:
		 * this driver never sets ARCHIVE on the file, so the
		 * undo is 0x00 — the entry's flags, like the $SI's,
		 * carry only the sparse bit. */
		CHECK_EQ(s.fn_redo_flags, 0x200u);
		CHECK_EQ(s.fn_undo_flags, 0u);
		CHECK_EQ(s.fnupd_lsn, s.lsn[17]);
		/* The $SI record sets the sparse bit (0x200) and
		 * undoes to clear (0x0) — the $DATA flag flips in
		 * step, so the two agree after replay either way. The
		 * 4-byte capture ignores the setattr's 36-byte $SI
		 * record at [16], so this is still the truncate's
		 * flags record at [14]. */
		CHECK_EQ(s.res_redo, 0x200u);
		CHECK_EQ(s.res_undo, 0u);
		CHECK_EQ(s.res_lsn, s.lsn[14]);
		/* No bitmap record in the extend: the ClearBits
		 * fields still hold the shrink's 48-cluster free. */
		CHECK_EQ(s.clear_bits_count, 48u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[0]);
		/* The sizes record names the new data size. */
		CHECK_EQ(s.sizes_redo_data, 256u * 1024u);
		CHECK_EQ(s.sizes_undo_data, 64u * 1024u);
		CHECK_EQ(s.sizes_lsn, s.lsn[13]);
		/* The pairs record is the $DATA attribute, at the
		 * same record offset as the sizes record. */
		CHECK_EQ(s.record_off[12], s.record_off[13]);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folios, so the flush notification retired
	 * the create's, the shrink's and the extend's dirty pages
	 * alike. The file is already 256 KiB on the volume. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	CHECK_EQ(size_after_mount(snap1, "sub", "j.txt", &size), 0);
	CHECK_EQ(size, 256u * 1024u);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 14. a truncate of a resident file writes no transaction records -- */

static void test_truncate_resident_unjournaled(void)
{
	char orig[1024], snap[1024], snap0[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	int err;

	printf("test_truncate_resident_unjournaled\n");
	scratch_path(orig, sizeof(orig), "trorig");
	scratch_path(snap, sizeof(snap), "trsnap");
	scratch_path(snap0, sizeof(snap0), "trsnap0");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_truncate_resident_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* A small file, never written: its $DATA is resident. The
	 * create journals (16 records); the truncate must add
	 * none — walk before and after and compare. The sync's
	 * flush notification (Phase 4 step 2) appends three
	 * DeleteDirtyClusters records after the create's Forget. */
	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	{
		static const char small[] = "hello";
		CHECK_EQ(ntfs_write(f, small, sizeof(small), 0),
			 (ssize_t)sizeof(small));
	}
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	/* Walk a snapshot copy, not the live image: the volume is
	 * still mounted. */
	CHECK_EQ(copy_file(orig, snap0), 0);
	CHECK_EQ(walk_log(snap0, &s), 0);
	CHECK_EQ(s.n, 19);
	if (s.n == 19) {
		CHECK_EQ(s.redo_op[16], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[16], 0u);
		CHECK_EQ(s.redo_op[17], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[17], 0u);
		CHECK_EQ(s.redo_op[18], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[18], 0u);
	}

	/* Truncate the resident file to 2 bytes. */
	err = ntfs_truncate(f, 2);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* Still 19: the truncate added no transaction records — the
	 * three DeleteDirtyClusters records are the create's. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 19);
	if (s.n == 19) {
		CHECK_EQ(s.redo_op[16], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[16], 0u);
		CHECK_EQ(s.redo_op[17], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[17], 0u);
		CHECK_EQ(s.redo_op[18], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[18], 0u);
	}

	{
		uint64_t size = 0;

		CHECK_EQ(size_after_mount(snap, "sub", "j.txt", &size), 0);
		CHECK_EQ(size, 2u);
	}
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap0);
	return;

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	unlink(orig);
	unlink(snap0);
}

/* ---- 15. a chmod + utimens journals ------------------------------------- */

static void test_setattr_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr old, at;
	struct ntfs_attr sa;
	uint64_t ino = 0, size = 0;
	int err, i;

	printf("test_setattr_journaled\n");
	scratch_path(orig, sizeof(orig), "saorig");
	scratch_path(snap, sizeof(snap), "sasnap");
	scratch_path(snap1, sizeof(snap1), "sasnap1");
	scratch_path(snap2, sizeof(snap2), "sasnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_setattr_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }
	CHECK(ino > 16);
	CHECK_EQ(ntfs_getattr(f, &old), 0);
	CHECK_EQ(old.mode, 0100644u);

	/* chmod 0644 -> 0444 and utimens to 2020-01-02 03:04:05 UTC, in
	 * one call. The mode change sets $LXMOD and the READONLY bit
	 * in $SI; the utimens sets mtime/atime. ctime is not touched
	 * by this call (the live driver only bumps ctime on a FLAGS
	 * or SIZE setattr) — the journal reproduces the live bytes,
	 * so the $SI redo and undo agree on ctime and crtime. */
	memset(&sa, 0, sizeof(sa));
	sa.mode = 0100444;
	sa.mtime.sec = 1577934245LL;	/* 2020-01-02 03:04:05 UTC */
	sa.mtime.nsec = 0;
	sa.atime.sec = 1577934245LL;
	sa.atime.nsec = 0;
	err = ntfs_setattr(f, &sa, NTFS_SETATTR_MODE | NTFS_SETATTR_MTIME |
				NTFS_SETATTR_ATIME);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the setattr transaction's 9 records, plus the
	 * sync's one DeleteDirtyClusters record. The create's
	 * records are gone: the sync after the create (in
	 * truncate_setup) flushed the $MFT folios and the bitmap
	 * folio, the flush notification (Phase 4 step 2) retired
	 * the create's dirty pages, and the setattr's commit
	 * checkpoint advanced oldest_lsn past them. The setattr is
	 * one open + $SI + $EA + $FILE_NAME + Commit + 3 dumps +
	 * Forget. No $EA_INFORMATION record: the $LXMOD value changes
	 * but the entry count does not, so its 8 bytes are unchanged.
	 * No $Bitmap record and no second open: everything the
	 * setattr stages lives in the file's own record, addressed
	 * through $MFT:$DATA. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 10);
	if (s.n == 10) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[5], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[6], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[7], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[8], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[9], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[9], 0u);
		/* The $SI record (the 36-byte one, at [1]) names the
		 * new mtime/atime and the READONLY flag, and undoes to
		 * the create's times and flags. crtime and ctime are
		 * untouched by this call, so redo and undo agree. */
		CHECK_EQ(s.si_redo_mtime, ntfs_time(1577934245LL, 0));
		CHECK_EQ(s.si_undo_mtime, ntfs_time(old.mtime.sec, old.mtime.nsec));
		CHECK_EQ(s.si_redo_atime, ntfs_time(1577934245LL, 0));
		CHECK_EQ(s.si_undo_atime, ntfs_time(old.atime.sec, old.atime.nsec));
		CHECK_EQ(s.si_redo_crtime, ntfs_time(old.crtime.sec, old.crtime.nsec));
		CHECK_EQ(s.si_undo_crtime, ntfs_time(old.crtime.sec, old.crtime.nsec));
		CHECK_EQ(s.si_redo_ctime, ntfs_time(old.ctime.sec, old.ctime.nsec));
		CHECK_EQ(s.si_undo_ctime, ntfs_time(old.ctime.sec, old.ctime.nsec));
		CHECK_EQ(s.si_redo_flags, old.file_attributes | 0x1u);
		CHECK_EQ(s.si_undo_flags, old.file_attributes);
		CHECK_EQ(s.si_lsn, s.lsn[1]);
		/* The $EA record (at [2]) carries the new $LXMOD. */
		CHECK_EQ(s.ea_lxmod_redo, 0100444u);
		CHECK_EQ(s.ea_lxmod_undo, 0100644u);
		CHECK_EQ(s.ea_lsn, s.lsn[2]);
		/* The $FILE_NAME record (at [3]) carries the READONLY
		 * flag and undoes to the create's flags; the sizes do
		 * not move. */
		CHECK_EQ(s.fn_redo_flags, old.file_attributes | 0x1u);
		CHECK_EQ(s.fn_undo_flags, old.file_attributes);
		CHECK_EQ(s.fn_redo_data, 256u * 1024u);
		CHECK_EQ(s.fn_undo_data, 256u * 1024u);
		CHECK_EQ(s.fnupd_lsn, s.lsn[3]);
	}
	(void)i;

	/* Replay copy 1: nothing redoes — the sync flushed the $MFT
	 * folios and the bitmap folio, so the flush notification
	 * retired the create's and the setattr's dirty pages alike.
	 * The attributes are already the new ones on the volume. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The attributes are the new ones on the replayed volume. */
	CHECK_EQ(attr_after_mount(snap1, "sub", "j.txt", &at), 0);
	CHECK_EQ(at.mode, 0100444u);
	CHECK_EQ(at.mtime.sec, 1577934245LL);
	CHECK_EQ(at.mtime.nsec, 0);
	CHECK_EQ(at.atime.sec, 1577934245LL);
	CHECK_EQ(at.file_attributes, old.file_attributes | 0x1u);
	CHECK_EQ(at.size, 256u * 1024u);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * zero records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 16. SIZE + MODE in one call is one transaction --------------------- */

static void test_setattr_size_joins(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr old, at;
	struct ntfs_attr sa;
	uint64_t ino = 0, size = 0;
	int err, i, commits = 0;

	printf("test_setattr_size_joins\n");
	scratch_path(orig, sizeof(orig), "sjorig");
	scratch_path(snap, sizeof(snap), "sjsnap");
	scratch_path(snap1, sizeof(snap1), "sjsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_setattr_size_joins: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }
	CHECK(ino > 16);
	CHECK_EQ(ntfs_getattr(f, &old), 0);
	CHECK_EQ(old.mode, 0100644u);

	/* Shrink to 64 KiB and chmod to 0444 in one call: one
	 * transaction must hold the truncate's records and the
	 * setattr's records together. */
	memset(&sa, 0, sizeof(sa));
	sa.size = 64 * 1024;
	sa.mode = 0100444;
	err = ntfs_setattr(f, &sa, NTFS_SETATTR_SIZE | NTFS_SETATTR_MODE);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: one 14-record transaction, plus the sync's two
	 * DeleteDirtyClusters records. The create's records are
	 * gone: the sync after the create (in truncate_setup)
	 * flushed the $MFT folios and the bitmap folio, the flush
	 * notification (Phase 4 step 2) retired the create's dirty
	 * pages, and this transaction's commit checkpoint advanced
	 * oldest_lsn past them. The transaction is the truncate's
	 * 6 (2 open + ClearBits + pairs + sizes + $FILE_NAME), the
	 * setattr's 3 ($SI + $EA + a second $FILE_NAME with the
	 * same dup), and one Commit + 3 dumps + Forget — not two
	 * transactions. The $SI here carries the resize's
	 * mtime/ctime (current_time) as well as the READONLY flag. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 16);
	if (s.n == 16) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[3], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[4], 0x18u);
		CHECK_EQ(s.redo_op[5], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[5], 0x18u);
		CHECK_EQ(s.redo_op[6], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[6], 0x18u);
		CHECK_EQ(s.redo_op[7], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[7], 0x18u);
		CHECK_EQ(s.redo_op[8], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[8], 0x18u);
		CHECK_EQ(s.redo_op[9], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[10], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[11], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[12], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[13], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[14], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[14], 0u);
		CHECK_EQ(s.redo_op[15], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[15], 0u);
		/* Exactly one commit in the walk: this transaction's.
		 * The create's commit retired with its records, so a
		 * second transaction for the resize would show up as
		 * a second commit here — the resize and the setattr
		 * joined one transaction. */
		for (i = 0; i < s.n; i++)
			if (s.redo_op[i] == LOP_CommitTransaction)
				commits++;
		CHECK_EQ(commits, 1);
		/* The setattr's $SI (at [6], the last 36-byte one)
		 * carries the READONLY flag; the resize moved the
		 * times, so redo and undo differ there too. */
		CHECK_EQ(s.si_redo_flags, old.file_attributes | 0x1u);
		CHECK_EQ(s.si_undo_flags, old.file_attributes);
		CHECK(s.si_redo_mtime != s.si_undo_mtime);
		CHECK_EQ(s.si_lsn, s.lsn[6]);
		/* The $EA record (at [7]) carries the new $LXMOD. */
		CHECK_EQ(s.ea_lxmod_redo, 0100444u);
		CHECK_EQ(s.ea_lxmod_undo, 0100644u);
		CHECK_EQ(s.ea_lsn, s.lsn[7]);
		/* The last $FILE_NAME record (the setattr's dup at
		 * [8]) names the new sizes. */
		CHECK_EQ(s.fn_redo_data, 64u * 1024u);
		CHECK_EQ(s.fn_undo_data, 256u * 1024u);
		CHECK_EQ(s.fnupd_lsn, s.lsn[8]);
	}
	(void)i;

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folios, so the flush notification retired
	 * the create's and the joined transaction's dirty pages
	 * alike. The file is already 64 KiB with the new mode. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	CHECK_EQ(attr_after_mount(snap1, "sub", "j.txt", &at), 0);
	CHECK_EQ(at.size, 64u * 1024u);
	CHECK_EQ(at.mode, 0100444u);
	CHECK_EQ(at.file_attributes, old.file_attributes | 0x1u);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 17. a setattr on a root file proceeds unjournaled ------------------ */

static void test_setattr_in_root_unjournaled(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_attr at;
	struct ntfs_attr sa;
	int err;

	printf("test_setattr_in_root_unjournaled\n");
	scratch_path(orig, sizeof(orig), "sruorig");
	scratch_path(snap, sizeof(snap), "srusnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_setattr_in_root_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* The create in root is itself unjournaled (the root's index
	 * lives in $INDEX_ALLOCATION), so the walk holds only the
	 * create's two DeleteDirtyClusters records before the
	 * setattr — the same baseline the root-create test pins. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
	}

	/* chmod + utimens on the root file: the $FILE_NAME stage cannot
	 * find the entry in the root's $INDEX_ROOT (it is in
	 * $INDEX_ALLOCATION), so the transaction aborts and the op
	 * proceeds unjournaled — it must succeed, not fail -ENOENT. */
	memset(&sa, 0, sizeof(sa));
	sa.mode = 0100444;
	sa.mtime.sec = 1577934245LL;	/* 2020-01-02 03:04:05 UTC */
	sa.mtime.nsec = 0;
	err = ntfs_setattr(f, &sa, NTFS_SETATTR_MODE | NTFS_SETATTR_MTIME);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* Still no transaction records: the setattr added one more
	 * DeleteDirtyClusters record (three total) for the stale
	 * dirty-page entry its aborted transaction staged. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 3);
	if (s.n == 3) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[2], 0u);
	}

	/* The op still landed: mode and mtime are the new ones after a
	 * remount, and the volume is clean. */
	CHECK_EQ(attr_after_mount(snap, NULL, "r.txt", &at), 0);
	CHECK_EQ(at.mode, 0100444u);
	CHECK_EQ(at.mtime.sec, 1577934245LL);
	CHECK_EQ(at.mtime.nsec, 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	return;

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	unlink(orig);
	unlink(snap);
}

/* ---- 19. a small resident xattr journals ------------------------------ */

static void test_xattr_create_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_xattr_create_journaled\n");
	scratch_path(orig, sizeof(orig), "xaorig");
	scratch_path(snap, sizeof(snap), "xasnap");
	scratch_path(snap1, sizeof(snap1), "xasnap1");
	scratch_path(snap2, sizeof(snap2), "xasnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_xattr_create_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	/* Set a small resident xattr: one transaction, 8 records. */
	err = ntfs_setxattr(f, "user.a", "abc", 3, 0);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the xattr transaction's 8 records, plus the
	 * sync's one DeleteDirtyClusters record. The create's
	 * records are gone: the sync after the create (in
	 * truncate_setup) flushed the $MFT folios and the bitmap
	 * folio, the flush notification (Phase 4 step 2) retired
	 * the create's dirty pages, and the xattr's commit
	 * checkpoint advanced oldest_lsn past them. The xattr is
	 * one open — $MFT:$DATA — the $SI ctime record, the ADS
	 * CreateAttribute, Commit, three table dumps, Forget. No
	 * $Bitmap record and no second open: a resident create
	 * allocates no clusters. No $FILE_NAME record: ADS sizes are
	 * not in the parent's dup. */
	set_ads_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 9);
	if (s.n == 9) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_CreateAttribute);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[4], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[5], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[6], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[7], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[8], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[8], 0u);
		/* The $SI record (at [1]) names a later ctime than
		 * the create's — the setxattr success path only bumps
		 * ctime — and undoes to the create's ctime. */
		CHECK(s.si_redo_ctime > s.si_undo_ctime);
		CHECK_EQ(s.si_lsn, s.lsn[1]);
		/* The ADS create is the only CreateAttribute carrying
		 * "user.a". The create's $FILE_NAME record retired with
		 * its transaction, so no CreateAttribute here carries
		 * "j.txt". */
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[2]);
		CHECK_EQ(s.fn_name_hits, 0);
	}

	/* Replay copy 1: nothing redoes — the sync flushed the $MFT
	 * folios and the bitmap folio, so the flush notification
	 * retired the create's and the xattr's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The value reads back after a remount. */
	CHECK_EQ(xattr_after_mount(snap1, "sub", "j.txt", "user.a",
				   buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot redoes the same zero
	 * records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 20. replacing a resident xattr journals the whole value ----------- */

static void test_xattr_replace_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_xattr_replace_journaled\n");
	scratch_path(orig, sizeof(orig), "xroorig");
	scratch_path(snap, sizeof(snap), "xrosnap");
	scratch_path(snap1, sizeof(snap1), "xrosnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_xattr_replace_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	err = ntfs_setxattr(f, "user.a", "abc", 3, 0);
	CHECK_EQ(err, 0);
	/* Replace with a longer value: one UpdateResidentValue for
	 * the whole value, any length — the replay's length-change
	 * path, which the setattr class left unproven. */
	err = ntfs_setxattr(f, "user.a", "abcdef", 6, 0);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the create-xattr transaction's 7 still-live
	 * records (its open precedes oldest_lsn and is
	 * dispensable), then the replace transaction's 8 (one open,
	 * the $SI record, the value record, Commit, three dumps,
	 * Forget), plus the sync's one DeleteDirtyClusters record.
	 * The create's records are gone: the sync after the create
	 * (in truncate_setup) flushed the $MFT folios and the
	 * bitmap folio, the flush notification (Phase 4 step 2)
	 * retired the create's dirty pages, and the create-xattr's
	 * commit checkpoint advanced oldest_lsn past them. No sync
	 * runs between the two setxattr calls, so the create-xattr's
	 * records stay live. */
	set_ads_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 16);
	if (s.n == 16) {
		CHECK_EQ(s.redo_op[7], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[7], 0x18u);
		CHECK_EQ(s.redo_op[8], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[8], 0x18u);
		CHECK_EQ(s.redo_op[9], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[9], 0x18u);
		CHECK_EQ(s.redo_op[10], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[11], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[12], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[13], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[14], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[15], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[15], 0u);
		/* The value record (at [9]) carries the new value
		 * whole and undoes to the old value whole. */
		CHECK_EQ(s.ads_value_redo_len, 6u);
		CHECK_EQ(memcmp(s.ads_value_redo, "abcdef", 6), 0);
		CHECK_EQ(s.ads_value_undo_len, 3u);
		CHECK_EQ(memcmp(s.ads_value_undo, "abc", 3), 0);
		CHECK_EQ(s.ads_value_lsn, s.lsn[9]);
		/* The ADS was created once, in the first xattr
		 * transaction. */
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[1]);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the create's and the op's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The new value reads back after a remount. */
	CHECK_EQ(xattr_after_mount(snap1, "sub", "j.txt", "user.a",
				   buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 6u);
	CHECK_EQ(memcmp(buf, "abcdef", 6), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 21. a large non-resident xattr journals its clusters ------------- */

static void test_xattr_big_create_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char big[8 * 1024], buf[8 * 1024];
	size_t len = 0;
	int err;

	printf("test_xattr_big_create_journaled\n");
	scratch_path(orig, sizeof(orig), "xbcorig");
	scratch_path(snap, sizeof(snap), "xbcsnap");
	scratch_path(snap1, sizeof(snap1), "xbcsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_xattr_big_create_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	/* 8 KiB does not fit resident: the ADS goes non-resident. */
	memset(big, 'B', sizeof(big));
	err = ntfs_setxattr(f, "user.big", big, sizeof(big), 0);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the xattr transaction's 10 records, plus the
	 * sync's two DeleteDirtyClusters records. The create's
	 * records are gone: the sync after the create (in
	 * truncate_setup) flushed the $MFT folios and the bitmap
	 * folio, the flush notification (Phase 4 step 2) retired
	 * the create's dirty pages, and the xattr's commit
	 * checkpoint advanced oldest_lsn past them. The xattr is
	 * two opens — $MFT:$DATA then $MFT:$BITMAP, in first-use
	 * order — the $SI record, the ADS CreateAttribute, the
	 * SetBits for its two clusters, Commit, three dumps,
	 * Forget. */
	set_ads_name("user.big");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 12);
	if (s.n == 12) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x68u);
		CHECK_EQ(s.redo_op[2], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_CreateAttribute);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_SetBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[4], 0x68u);
		CHECK_EQ(s.redo_op[5], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[6], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[7], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[8], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[9], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[10], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[10], 0u);
		CHECK_EQ(s.redo_op[11], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[11], 0u);
		/* The SetBits names two clusters: 8 KiB at 4 KiB per
		 * cluster. */
		CHECK_EQ(s.bits_target_attr, 0x68u);
		CHECK_EQ(s.bits_count, 2u);
		CHECK_EQ(s.bits_lsn, s.lsn[4]);
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[3]);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folios, so the flush notification retired
	 * the create's and the xattr's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The value reads back after a remount. */
	CHECK_EQ(xattr_after_mount(snap1, "sub", "j.txt", "user.big",
				   buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, sizeof(big));
	CHECK_EQ(memcmp(buf, big, sizeof(big)), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 22. replacing and removing a non-resident xattr ------------------- */

static void test_xattr_big_replace_remove_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char big[8 * 1024], big2[16 * 1024], buf[16 * 1024];
	size_t len = 0;
	int err;

	printf("test_xattr_big_replace_remove_journaled\n");
	scratch_path(orig, sizeof(orig), "xbrorig");
	scratch_path(snap, sizeof(snap), "xbrsnap");
	scratch_path(snap1, sizeof(snap1), "xbrsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_xattr_big_replace_remove_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	memset(big, 'B', sizeof(big));
	err = ntfs_setxattr(f, "user.big", big, sizeof(big), 0);
	CHECK_EQ(err, 0);
	/* Replace 8 KiB with 16 KiB: the ADS stays non-resident, so
	 * the transaction stages sizes, mapping pairs and the bitmap
	 * delta — the truncate machinery, named. */
	memset(big2, 'C', sizeof(big2));
	err = ntfs_setxattr(f, "user.big", big2, sizeof(big2), 0);
	CHECK_EQ(err, 0);
	/* Remove it: DeleteAttribute plus ClearBits for its clusters. */
	err = ntfs_removexattr(f, "user.big");
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the create-xattr transaction's 7 still-live
	 * records (its two opens precede oldest_lsn and are
	 * dispensable), the replace transaction's 11 (two opens, the
	 * $SI record, sizes, pairs, SetBits, Commit, three dumps,
	 * Forget), then the remove transaction's 10 (two opens, the
	 * $SI record, DeleteAttribute, ClearBits, Commit, three
	 * dumps, Forget), plus the sync's two DeleteDirtyClusters
	 * records. The create's records are gone: the sync after
	 * the create (in truncate_setup) flushed the $MFT folios
	 * and the bitmap folio, the flush notification (Phase 4
	 * step 2) retired the create's dirty pages, and the
	 * create-xattr's commit checkpoint advanced oldest_lsn past
	 * them. No sync runs between the three xattr calls, so the
	 * create-xattr's and the replace's records stay live. */
	set_ads_name("user.big");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 31);
	if (s.n == 31) {
		/* Replace transaction: opens, $SI, sizes, pairs. */
		CHECK_EQ(s.redo_op[8], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[8], 0x18u);
		CHECK_EQ(s.redo_op[9], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[9], 0x68u);
		CHECK_EQ(s.redo_op[10], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[10], 0x18u);
		/* The mapping pairs come before the sizes that name them
		 * (the truncate class stages the same way). */
		CHECK_EQ(s.redo_op[11], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[11], 0x18u);
		CHECK_EQ(s.redo_op[12], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[12], 0x18u);
		CHECK_EQ(s.redo_op[13], LOP_SetBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[13], 0x68u);
		CHECK_EQ(s.redo_op[14], LOP_CommitTransaction);
		/* The sizes record names the 8 KiB -> 16 KiB growth. */
		/* One sizes record in the walk: the replace's (8 KiB
		 * -> 16 KiB). The create's sizes record retired with
		 * its transaction. */
		CHECK_EQ(s.sizes_count, 1);
		CHECK_EQ(s.sizes_redo_data, 16u * 1024u);
		CHECK_EQ(s.sizes_undo_data, 8u * 1024u);
		/* Remove transaction: opens, $SI, DeleteAttribute,
		 * ClearBits. */
		CHECK_EQ(s.redo_op[19], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[19], 0x18u);
		CHECK_EQ(s.redo_op[20], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[20], 0x68u);
		CHECK_EQ(s.redo_op[21], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[21], 0x18u);
		CHECK_EQ(s.redo_op[22], LOP_DeleteAttribute);
		CHECK_EQ(s.target_attr[22], 0x18u);
		CHECK_EQ(s.redo_op[23], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[23], 0x68u);
		CHECK_EQ(s.redo_op[24], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[28], LOP_ForgetTransaction);
		/* The sync's flush notification: two
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[29], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[29], 0u);
		CHECK_EQ(s.redo_op[30], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[30], 0u);
		/* The ClearBits frees the four 16 KiB clusters. */
		CHECK_EQ(s.clear_bits_target_attr, 0x68u);
		CHECK_EQ(s.clear_bits_count, 4u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[23]);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folios, so the flush notification retired
	 * the create's and the xattr transactions' dirty pages
	 * alike. The ADS is already gone on the volume. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	/* No clusters are written back: the replay redoes nothing. */
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The ADS is gone after a remount. */
	CHECK_EQ(xattr_after_mount(snap1, "sub", "j.txt", "user.big",
				   buf, sizeof(buf), &len), -ENOATTR);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 23. an xattr on a file in the volume root journals --------------- */

static void test_xattr_in_root_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_xattr_in_root_journaled\n");
	scratch_path(orig, sizeof(orig), "xirorig");
	scratch_path(snap, sizeof(snap), "xirsnap");
	scratch_path(snap1, sizeof(snap1), "xirsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_xattr_in_root_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* The create in root is itself unjournaled (the root's index
	 * lives in $INDEX_ALLOCATION), so the walk holds only the
	 * create's two DeleteDirtyClusters records before the xattr
	 * — the same baseline the root-create test pins. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
	}

	/* An xattr on the root file journals: only its own record
	 * and $Bitmap are touched, no parent $FILE_NAME. */
	err = ntfs_setxattr(f, "user.a", "abc", 3, 0);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds the xattr transaction's 8 records, plus the
	 * sync's one DeleteDirtyClusters record. The create's two
	 * notifies are gone: the xattr's commit checkpoint advanced
	 * oldest_lsn past them, since the notifies had already
	 * retired the entries they named. */
	set_ads_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 9);
	if (s.n == 9) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_CreateAttribute);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[4], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[5], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[6], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[7], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record after the Forget. */
		CHECK_EQ(s.redo_op[8], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[8], 0u);
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[2]);
	}

	/* Replay: nothing redoes — every record targets the file's
	 * own record and is skipped on its MR_LSN stamp. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The value reads back after a remount. */
	CHECK_EQ(xattr_after_mount(snap1, NULL, "r.txt", "user.a",
				   buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	return;

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	unlink(orig);
	unlink(snap);
}


/* ---- 7. a symlink journals ---------------------------------------------- */

/* Mount @path, look @name up under @parent (NULL: the root), read the
 * link target into @buf (@bufsz bytes, @len_out the length read),
 * unmount. The same shape as xattr_after_mount, for the symlink tests'
 * post-replay checks. */
static int readlink_after_mount(const char *path, const char *parent,
				const char *name, char *buf, size_t bufsz,
				size_t *len_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	int err;

	if (!dev)
		return -EIO;
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		goto out_dev;
	err = ntfs_volume_root(vol, &root);
	if (err)
		goto out_vol;
	d = root;
	if (parent) {
		err = ntfs_lookup(root, parent, &d);
		if (err) {
			ntfs_inode_put(root);
			root = NULL;
			goto out_vol;
		}
	}
	err = ntfs_lookup(d, name, &f);
	if (err)
		goto out_d;
	err = ntfs_readlink(f, buf, bufsz, len_out);
	ntfs_inode_put(f);
out_d:
	if (d && d != root)
		ntfs_inode_put(d);
	if (root)
		ntfs_inode_put(root);
out_vol:
	ntfs_unmount(vol);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* Read the flags word of MFT record @mft_no from the image at @path:
 * magic 0-3, usa 4-7, lsn 8-15, sequence 16-17, link_count 18-19,
 * attrs_offset 20-21, flags 22-23. */
static int read_mft_flags(const char *path, uint64_t mft_no,
			  uint16_t *flags_out)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	struct ntfs_image img;
	struct ntfs_image_io io;
	struct ntfs_log_apply ap;
	uint8_t *buf;
	int err;

	*flags_out = 0;
	if (!dev)
		return -EIO;
	io.ctx = dev;
	io.pread = img_pread;
	io.pwrite = NULL;
	io.sync = NULL;
	memset(&img, 0, sizeof(img));
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err)
		goto out_dev;
	buf = malloc(img.mft_record_size);
	if (!buf) {
		err = -ENOMEM;
		goto out_img;
	}
	ntfs_image_apply(&img, &ap);
	err = ap.read_mft_record(ap.ctx, mft_no, buf);
	if (!err)
		*flags_out = (uint16_t)buf[22] | ((uint16_t)buf[23] << 8);
	free(buf);
out_img:
	ntfs_image_close(&img);
out_dev:
	ntfs_bdev_close(dev);
	return err;
}

/* How many $R entries with reparse tag @tag for record @ino does
 * $Reparse's $R index hold in the image at @path? Reads $Reparse's
 * record (@rp_ino) raw and walks the $R root entries by their on-disk
 * bytes — the driver has no read-side query for the index, and
 * readlink does not consult it, so only this byte check proves the
 * replay landed the entry (a missing entry leaves a link Windows
 * cannot enumerate; a duplicated one leaves two). Returns the count,
 * or -errno on a parse failure. */
static int reparse_index_count_entries(const char *path, uint64_t rp_ino,
				       uint32_t tag, uint64_t ino)
{
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
	struct ntfs_image img;
	struct ntfs_image_io io;
	struct ntfs_log_apply ap;
	uint8_t *buf;
	int err, found = 0;

	if (!dev)
		return -EIO;
	io.ctx = dev;
	io.pread = img_pread;
	io.pwrite = NULL;
	io.sync = NULL;
	memset(&img, 0, sizeof(img));
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err)
		goto out_dev;
	buf = malloc(img.mft_record_size);
	if (!buf) {
		err = -ENOMEM;
		goto out_img;
	}
	ntfs_image_apply(&img, &ap);
	err = ap.read_mft_record(ap.ctx, rp_ino, buf);
	if (err)
		goto out_buf;
	{
		uint16_t off = (uint16_t)buf[0x14] |
			       ((uint16_t)buf[0x15] << 8);
		uint32_t used = (uint32_t)buf[0x18] |
				((uint32_t)buf[0x19] << 8) |
				((uint32_t)buf[0x1a] << 16) |
				((uint32_t)buf[0x1b] << 24);
		uint16_t aoff = 0;
		bool is_r = false;

		err = -ENOENT;
		while (off + 8 <= used) {
			uint32_t type = (uint32_t)buf[off] |
					((uint32_t)buf[off + 1] << 8) |
					((uint32_t)buf[off + 2] << 16) |
					((uint32_t)buf[off + 3] << 24);
			uint32_t len = (uint32_t)buf[off + 4] |
				       ((uint32_t)buf[off + 5] << 8) |
				       ((uint32_t)buf[off + 6] << 16) |
				       ((uint32_t)buf[off + 7] << 24);

			if (type == 0xffffffff)
				break;
			if (len < 8 || off + len > used) {
				err = -EIO;
				goto out_buf;
			}
			/* $INDEX_ROOT, resident, named "$R". */
			if (type == 0x90 && !buf[off + 8] &&
			    buf[off + 9] == 2) {
				uint16_t noff = (uint16_t)buf[off + 10] |
						((uint16_t)buf[off + 11] << 8);

				if (off + noff + 4 <= used &&
				    buf[off + noff] == '$' &&
				    buf[off + noff + 1] == 0 &&
				    buf[off + noff + 2] == 'R' &&
				    buf[off + noff + 3] == 0) {
					aoff = off;
					is_r = true;
					break;
				}
			}
			off += (uint16_t)len;
		}
		if (!is_r)
			goto out_buf;
		/* The $R root: value at value_offset, the index header
		 * 16 bytes into the value, entries at entries_offset
		 * from the header, end at index_length. Entry layout:
		 * 8-byte data union, length u16 @+8, flags u16 @+12,
		 * key (tag u32, file_id u64) @+16. */
		{
			uint16_t voff = (uint16_t)buf[aoff + 20] |
					((uint16_t)buf[aoff + 21] << 8);
			const uint8_t *value = buf + aoff + voff;
			const uint8_t *ih = value + 16;
			uint32_t eoff = (uint32_t)ih[0] |
					((uint32_t)ih[1] << 8) |
					((uint32_t)ih[2] << 16) |
					((uint32_t)ih[3] << 24);
			uint32_t ilen = (uint32_t)ih[4] |
					((uint32_t)ih[5] << 8) |
					((uint32_t)ih[6] << 16) |
					((uint32_t)ih[7] << 24);
			const uint8_t *entries = ih + eoff;
			const uint8_t *end = ih + ilen;
			const uint8_t *e;

			err = -EIO;
			if (entries < value || end > buf + img.mft_record_size)
				goto out_buf;
			for (e = entries; e + 16 <= end;) {
				uint16_t elen = (uint16_t)e[8] |
						((uint16_t)e[9] << 8);
				uint16_t flags = (uint16_t)e[12] |
						 ((uint16_t)e[13] << 8);

				if (elen < 16 || e + elen > end)
					goto out_buf;
				if (flags & 0x02)
					break;
				if (elen >= 32) {
					uint32_t etag = (uint32_t)e[16] |
							((uint32_t)e[17] << 8) |
							((uint32_t)e[18] << 16) |
							((uint32_t)e[19] << 24);
					uint64_t fid = 0;
					int k;

					for (k = 0; k < 8; k++)
						fid |= (uint64_t)e[20 + k] <<
						       (8 * k);
					if (etag == tag &&
					    (fid & 0xffffffffffffULL) == ino)
						found++;
				}
				e += elen;
			}
			err = 0;
		}
	}
out_buf:
	free(buf);
out_img:
	ntfs_image_close(&img);
out_dev:
	ntfs_bdev_close(dev);
	return err ? err : found;
}

/* The walk of one journaled symlink create in a driver-made
 * directory: 2 opens + 10 data + Commit + 3 dumps + Forget, plus
 * the sync's four DeleteDirtyClusters records (the flush
 * notification, Phase 4 step 2). The data records are the $MFT
 * $DATA size extension, the record init, the $SD /
 * $DATA-or-$INDEX_ROOT / $REPARSE_POINT / $FILE_NAME creates,
 * the $R index entry, the $SI flags, the parent index entry and
 * the SetBits — in that order. */
/* The walk checker for a journaled symlink create, shared by the
 * plain, WSL-tag and dir-link cases. @si_redo is the $SI flags value
 * the create stages: FILE_ATTR_REPARSE_POINT (0x400) for a plain or
 * WSL link, plus FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT (0x10000000)
 * for a dir-link — the $SI staging runs after the $FILE_NAME is
 * built, so it carries the final ni->flags, not the reparse set's
 * half-built one. */
static void check_symlink_walk(struct rec_seen *s, const char *snap,
			       uint64_t ino, uint64_t sub_ino,
			       uint64_t rp_ino, uint32_t si_redo)
{
	uint64_t mrlsn_file = 0, mrlsn_dir = 0, mrlsn_rp = 0,
		 lsn_fn = 0, lsn_index = 0, lsn_r = 0;
	int i;

	CHECK_EQ(s->n, 21);
	if (s->n != 21)
		return;
	{
		static const uint16_t want_op[10] = {
			LOP_SetNewAttributeSizes,
			LOP_InitializeFileRecordSegment, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_AddIndexEntryRoot, LOP_CreateAttribute,
			LOP_UpdateResidentValue, LOP_AddIndexEntryRoot,
			LOP_SetBitsInNonresidentBitMap,
		};
		CHECK_EQ(s->redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s->target_attr[0], 0x18u);
		CHECK_EQ(s->redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s->target_attr[1], 0x40u);
		for (i = 0; i < 10; i++) {
			CHECK_EQ(s->redo_op[i + 2], want_op[i]);
			/* First nine target $MFT:$DATA (0x18); the
			 * SetBits targets $MFT:$BITMAP (0x40). */
			CHECK_EQ(s->target_attr[i + 2], i < 9 ? 0x18u : 0x40u);
		}
	}
	CHECK_EQ(s->redo_op[12], LOP_CommitTransaction);
	CHECK_EQ(s->redo_op[13], LOP_TransactionTableDump);
	CHECK_EQ(s->redo_op[14], LOP_DirtyPageTableDump);
	CHECK_EQ(s->redo_op[15], LOP_OpenAttributeTableDump);
	CHECK_EQ(s->redo_op[16], LOP_ForgetTransaction);
	/* The sync's flush notification (Phase 4 step 2): four
	 * DeleteDirtyClusters records after the Forget — the $MFT
	 * folio for record 0's cluster, the folio for the new record
	 * + parent, the $MFT:$BITMAP folio, and the $Reparse folio. */
	for (i = 17; i < 21; i++) {
		CHECK_EQ(s->redo_op[i], LOP_DeleteDirtyClusters);
		CHECK_EQ(s->target_attr[i], 0u);
	}
	/* The $FILE_NAME create is the last CreateAttribute, the
	 * record right before the $SI flags update; the $R entry sits
	 * between the $REPARSE_POINT create and the $FILE_NAME one,
	 * and the $SI flags sit between the $FILE_NAME create and the
	 * parent index entry. */
	lsn_r = s->lsn[7];
	lsn_fn = s->lsn[8];
	lsn_index = s->lsn[10];
	CHECK_EQ(s->redo_op[8], LOP_CreateAttribute);
	CHECK_EQ(s->redo_op[9], LOP_UpdateResidentValue);
	CHECK_EQ(s->redo_op[10], LOP_AddIndexEntryRoot);
	/* The name appears in exactly one CreateAttribute's redo: the
	 * $FILE_NAME one. */
	CHECK_EQ(s->fn_name_hits, 1);
	CHECK_EQ(s->fn_lsn, lsn_fn);
	/* The SetBits names the new record number, one bit. */
	CHECK_EQ(s->bits_bit, (uint32_t)ino);
	CHECK_EQ(s->bits_count, 1u);
	CHECK_EQ(s->bits_lsn, s->lsn[11]);
	CHECK_EQ(s->target_vcn[11], 0u);
	/* The $SI flags record: the final ni->flags, staged after the
	 * $FILE_NAME is built — FILE_ATTR_REPARSE_POINT (0x400) for a
	 * plain or WSL link, plus FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT
	 * for a dir-link — over the as-added $SI, which carried no flags
	 * at all. */
	CHECK_EQ(s->res_redo, si_redo);
	CHECK_EQ(s->res_undo, 0u);
	CHECK_EQ(s->res_lsn, s->lsn[9]);
	/* The $MFT $DATA size extension: exactly one record, staged
	 * first, its redo the live size, its undo one record less. */
	{
		uint16_t data_off = 0;
		uint64_t alloc = 0, data = 0;
		uint32_t rec_size = 0;

		CHECK_EQ(read_mft0_data_attr(snap, &data_off, &alloc, &data,
					     &rec_size), 0);
		CHECK_EQ(s->sizes_count, 1);
		CHECK_EQ(s->sizes_lsn, s->lsn[2]);
		CHECK_EQ(s->record_off[2], data_off);
		CHECK_EQ(s->target_vcn[2], 0u);
		CHECK_EQ(s->sizes_redo_data, data);
		CHECK_EQ(s->sizes_undo_data, data - rec_size);
		CHECK_EQ(s->sizes_redo_total, 0u);
		CHECK(rec_size == 1024u);
	}
	/* All three touched records carry an MR_LSN at or past the
	 * record that touched them: the replay's skip check. The
	 * $Reparse stamp is the new one — without it the replay
	 * would re-apply the $R entry and duplicate it. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_index);
	CHECK_EQ(read_mft_lsn(snap, rp_ino, &mrlsn_rp), 0);
	CHECK(mrlsn_rp != 0);
	CHECK(mrlsn_rp >= lsn_r);
}

/* One journaled symlink create in a driver-made directory, for a
 * given link name, target and reparse tag: the win-tag case
 * ("t.txt", 0xA000000C) and the WSL-tag case ("a:b", 0xA000001D)
 * differ only in the tag the reparse set picks — the journal
 * records are the same shape either way. */
static void symlink_journaled_case(const char *label, const char *lname,
				    const char *target, uint32_t tag)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *l = NULL;
	ntfs_inode_t *ext = NULL, *rp = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr at;
	uint64_t ino = 0, sub_ino = 0, rp_ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("%s\n", label);
	scratch_path(orig, sizeof(orig), "sorig");
	scratch_path(snap, sizeof(snap), "ssnap");
	scratch_path(snap1, sizeof(snap1), "ssnap1");
	scratch_path(snap2, sizeof(snap2), "ssnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP %s: mkntfs unavailable\n", label);
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_symlink(d, lname, target, &l);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(l);
	CHECK(ino > 16);
	ntfs_inode_put(l);
	l = NULL;
	/* $Reparse's record number, for the MR_LSN and $R-entry
	 * checks below: $Extend/$Reparse, by name. */
	err = ntfs_lookup(root, "$Extend", &ext);
	CHECK_EQ(err, 0);
	if (!err) {
		err = ntfs_lookup(ext, "$Reparse", &rp);
		CHECK_EQ(err, 0);
		if (!err)
			rp_ino = ntfs_inode_number(rp);
		ntfs_inode_put(rp);
		rp = NULL;
		ntfs_inode_put(ext);
		ext = NULL;
	}
	CHECK(rp_ino != 0);
	if (err) { goto out; }

	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log — what a crash at this instant leaves. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (l) { ntfs_inode_put(l); l = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: exactly one transaction, 2 open + 10 data + Commit +
	 * 3 dumps + Forget. */
	set_fn_name(lname);
	CHECK_EQ(walk_log(snap, &s), 0);
	check_symlink_walk(&s, snap, ino, sub_ino, rp_ino, 0x400u);

	/* Replay copy 1: the MFT records were already flushed, so only
	 * the two records that carry no LSN to skip on redo — the $MFT
	 * $DATA size extension and the SetBits. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The link reads back after a remount, with the right target,
	 * the reparse flag on its $SI, and its $R entry in place. */
	CHECK_EQ(readlink_after_mount(snap1, "sub", lname,
				      buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, strlen(target));
	CHECK_EQ(memcmp(buf, target, len), 0);
	CHECK_EQ(attr_after_mount(snap1, "sub", lname, &at), 0);
	CHECK(at.file_attributes & 0x400u);
	CHECK_EQ(reparse_index_count_entries(snap1, rp_ino, tag, ino), 1);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * two records. Replay is idempotent — and the $R entry is not
	 * duplicated: it is still exactly one. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(reparse_index_count_entries(snap2, rp_ino, tag, ino), 1);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 23. setting an EA journals the $EA pair -----------------------------
 *
 * A driver-created file already carries the three $LX* entries
 * ($LXUID/$LXGID/$LXMOD, 60-byte stream), so setting a user EA on
 * it is a replace: the $EA_INFORMATION and $EA values are staged
 * whole, the $EA value growing (the xattr-proven length-change
 * path). The walk: the EA transaction's 9 records (one open —
 * $MFT:$DATA — the $SI ctime record, the $EA_INFORMATION record,
 * the $EA record, Commit, three table dumps, Forget), plus the
 * sync's one DeleteDirtyClusters record. The create's records
 * are gone: the sync after the create (in truncate_setup)
 * flushed the $MFT folios and the bitmap folio, the flush
 * notification (Phase 4 step 2) retired the create's dirty
 * pages, and the EA's commit checkpoint advanced oldest_lsn
 * past them. No $Bitmap record: a resident $EA allocates no
 * clusters. No $FILE_NAME record: EA sizes are not in the parent's
 * dup. */

static void test_ea_replace_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_replace_journaled\n");
	scratch_path(orig, sizeof(orig), "eaorig");
	scratch_path(snap, sizeof(snap), "easnap");
	scratch_path(snap1, sizeof(snap1), "easnap1");
	scratch_path(snap2, sizeof(snap2), "easnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_replace_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	/* Set a small EA: the pair exists ($LX*), so this grows the
	 * stream from 60 bytes to 80. */
	err = ea_set(f, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 10);
	if (s.n == 10) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[5], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[6], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[7], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[8], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[9], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[9], 0u);
		/* The $SI record (at [1]) names a later ctime than
		 * the create's — the handler's success path only bumps
		 * ctime — and undoes to the create's ctime. */
		CHECK(s.si_redo_ctime > s.si_undo_ctime);
		CHECK_EQ(s.si_lsn, s.lsn[1]);
		/* The $EA_INFORMATION record (at [2]) carries the
		 * new stream length as its ea_query_length and undoes
		 * to the $LX*-only length; both agree with the $EA
		 * value record's lengths (at [3]). */
		CHECK_EQ(s.ea_info_lsn, s.lsn[2]);
		CHECK_EQ(s.ea_info_redo_query, 80u);
		CHECK_EQ(s.ea_info_undo_query, 60u);
		CHECK_EQ(s.ea_value_lsn, s.lsn[3]);
		CHECK_EQ(s.ea_value_redo_len, 80u);
		CHECK_EQ(s.ea_value_undo_len, 60u);
		CHECK_EQ(s.ea_info_redo_query, (uint32_t)s.ea_value_redo_len);
		CHECK_EQ(s.ea_info_undo_query, (uint32_t)s.ea_value_undo_len);
		/* The $EA value record carries the new stream whole
		 * (with "user.a"/"abc") and undoes to the $LX*-only
		 * stream (no "user.a"). */
		CHECK(memcmp(s.ea_value_redo, s.ea_value_undo, 60) != 0 ||
		      s.ea_value_redo_len != s.ea_value_undo_len);
		{
			bool redo_has = false, undo_has = false;
			size_t i;

			for (i = 0; i + 6 <= s.ea_value_redo_len; i++) {
				if (!memcmp(s.ea_value_redo + i, "user.a", 6))
					redo_has = true;
			}
			for (i = 0; i + 6 <= s.ea_value_undo_len; i++) {
				if (!memcmp(s.ea_value_undo + i, "user.a", 6))
					undo_has = true;
			}
			CHECK(redo_has);
			CHECK(!undo_has);
		}
	}

	/* Replay copy 1: nothing redoes — the sync flushed the $MFT
	 * folios and the bitmap folio, so the flush notification
	 * retired the create's and the EA transaction's dirty pages
	 * alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The value reads back after a remount. */
	CHECK_EQ(ea_after_mount(snap1, "sub", "j.txt", "user.a",
				buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot redoes the same zero
	 * records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 24. replacing an EA grows the stream ------------------------------ */

static void test_ea_replace_grow_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_replace_grow_journaled\n");
	scratch_path(orig, sizeof(orig), "eagorig");
	scratch_path(snap, sizeof(snap), "eagsnap");
	scratch_path(snap1, sizeof(snap1), "eagsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_replace_grow_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	err = ea_set(f, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	/* Replace with a longer value: the stream grows 80 -> 84,
	 * one UpdateResidentValue for the whole value — the length
	 * change the setattr class refused on $EA and the xattr
	 * class proved. */
	err = ea_set(f, "user.a", "abcdef", 6);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the set transaction's 7 still-live records (its
	 * open precedes oldest_lsn and is dispensable), then the
	 * replace transaction's 9, plus the sync's one
	 * DeleteDirtyClusters record. The create's records are gone:
	 * the sync after the create (in truncate_setup) flushed the
	 * $MFT folios and the bitmap folio, the flush notification
	 * (Phase 4 step 2) retired the create's dirty pages, and
	 * the set's commit checkpoint advanced oldest_lsn past
	 * them. No sync runs between the two ea_set calls, so the
	 * set's records stay live. */
	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 18);
	if (s.n == 18) {
		CHECK_EQ(s.redo_op[8], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[9], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[10], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[11], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[12], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[13], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[14], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[15], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[16], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[17], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[17], 0u);
		/* The value record (at [11]) carries the new stream
		 * whole ("abcdef") and undoes to the old stream whole
		 * ("abc"); the lengths differ, the length-change path. */
		CHECK_EQ(s.ea_value_lsn, s.lsn[11]);
		CHECK_EQ(s.ea_value_redo_len, 84u);
		CHECK_EQ(s.ea_value_undo_len, 80u);
		{
			bool redo_has_new = false, undo_has_new = false;
			bool undo_has_old = false;
			size_t i;

			for (i = 0; i + 6 <= s.ea_value_redo_len; i++) {
				if (!memcmp(s.ea_value_redo + i, "abcdef", 6))
					redo_has_new = true;
			}
			for (i = 0; i + 6 <= s.ea_value_undo_len; i++) {
				if (!memcmp(s.ea_value_undo + i, "abcdef", 6))
					undo_has_new = true;
			}
			/* "abc" sits at the very end of the undo stream,
			 * so its scan needs the 3-byte bound, not the
			 * 6-byte one above. */
			for (i = 0; i + 3 <= s.ea_value_undo_len; i++) {
				if (!memcmp(s.ea_value_undo + i, "abc", 3))
					undo_has_old = true;
			}
			CHECK(redo_has_new);
			CHECK(!undo_has_new);
			CHECK(undo_has_old);
		}
		CHECK_EQ(s.ea_info_lsn, s.lsn[10]);
		CHECK_EQ(s.ea_info_redo_query, 84u);
		CHECK_EQ(s.ea_info_undo_query, 80u);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the create's and the op's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The new value reads back after a remount. */
	CHECK_EQ(ea_after_mount(snap1, "sub", "j.txt", "user.a",
				buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 6u);
	CHECK_EQ(memcmp(buf, "abcdef", 6), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 25. removing an EA shrinks the stream, pair remains --------------- */

static void test_ea_remove_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_remove_journaled\n");
	scratch_path(orig, sizeof(orig), "earorig");
	scratch_path(snap, sizeof(snap), "earsnap");
	scratch_path(snap1, sizeof(snap1), "earsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_remove_journaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	err = ea_set(f, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	/* Remove it: the stream shrinks 80 -> 60, back to $LX*
	 * only; the pair itself stays (it is not the last entry). */
	err = ea_remove(f, "user.a");
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the set transaction's 7 still-live records (its
	 * open precedes oldest_lsn and is dispensable), then the
	 * remove transaction's 9, plus the sync's one
	 * DeleteDirtyClusters record. The create's records are gone:
	 * the sync after the create (in truncate_setup) flushed the
	 * $MFT folios and the bitmap folio, the flush notification
	 * (Phase 4 step 2) retired the create's dirty pages, and
	 * the set's commit checkpoint advanced oldest_lsn past
	 * them. No sync runs between the two EA calls, so the set's
	 * records stay live. */
	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 18);
	if (s.n == 18) {
		CHECK_EQ(s.redo_op[8], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[9], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[10], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[11], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[12], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[13], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[14], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[15], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[16], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[17], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[17], 0u);
		/* The value record (at [11]) carries the $LX*-only
		 * stream (no "user.a") and undoes to the stream with
		 * "user.a"; the remove's record matches because its
		 * undo carries the name. */
		CHECK_EQ(s.ea_value_lsn, s.lsn[11]);
		CHECK_EQ(s.ea_value_redo_len, 60u);
		CHECK_EQ(s.ea_value_undo_len, 80u);
		{
			bool redo_has = false, undo_has = false;
			size_t i;

			for (i = 0; i + 6 <= s.ea_value_redo_len; i++) {
				if (!memcmp(s.ea_value_redo + i, "user.a", 6))
					redo_has = true;
			}
			for (i = 0; i + 6 <= s.ea_value_undo_len; i++) {
				if (!memcmp(s.ea_value_undo + i, "user.a", 6))
					undo_has = true;
			}
			CHECK(!redo_has);
			CHECK(undo_has);
		}
		CHECK_EQ(s.ea_info_lsn, s.lsn[10]);
		CHECK_EQ(s.ea_info_redo_query, 60u);
		CHECK_EQ(s.ea_info_undo_query, 80u);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the create's and the op's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The EA is gone after a remount. */
	CHECK_EQ(ea_after_mount(snap1, "sub", "j.txt", "user.a",
				buf, sizeof(buf), &len), -ENODATA);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 26. creating the pair on the root directory -----------------------
 *
 * The root is the only inode a driver-made volume guarantees has
 * no $EA pair (ntfs_create writes the three $LX* entries into
 * every file it makes), so the root is where the CreateAttribute
 * case is tested: setting the first EA stages CreateAttribute
 * for $EA_INFORMATION and for $EA. The walk holds just the EA
 * transaction's 9 records. */

static void test_ea_create_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_create_journaled\n");
	scratch_path(orig, sizeof(orig), "eacorig");
	scratch_path(snap, sizeof(snap), "eacsnap");
	scratch_path(snap1, sizeof(snap1), "eacsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_create_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ea_set(root, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 10);
	if (s.n == 10) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[2], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[3], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[4], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[5], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[6], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[7], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[8], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[9], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[9], 0u);
		/* The two CreateAttributes are the pair, $EA_INFORMATION
		 * first: the redo's first 4 bytes are the type. */
		CHECK_EQ(s.ea_info_create_hits, 1);
		CHECK_EQ(s.ea_info_create_lsn, s.lsn[2]);
		CHECK_EQ(s.ea_create_hits, 1);
		CHECK_EQ(s.ea_create_lsn, s.lsn[3]);
		CHECK(s.si_redo_ctime > s.si_undo_ctime);
		CHECK_EQ(s.si_lsn, s.lsn[1]);
	}

	/* Replay: nothing redoes — every record targets the root's
	 * own record and is skipped on its MR_LSN stamp, and the
	 * flush notification retired the dirty pages besides. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The value reads back after a remount, off the root. */
	CHECK_EQ(ea_after_mount(snap1, NULL, NULL, "user.a",
				buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 27. removing the last EA tears the pair down ----------------------
 *
 * On the root, removing the only EA empties the stream, so
 * ntfs_set_ea removes $EA and then $EA_INFORMATION: the remove
 * transaction stages DeleteAttribute for both, $EA first (the
 * live removal order), from the begin-time bytes. */

static void test_ea_remove_last_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_remove_last_journaled\n");
	scratch_path(orig, sizeof(orig), "ealorig");
	scratch_path(snap, sizeof(snap), "ealsnap");
	scratch_path(snap1, sizeof(snap1), "ealsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_remove_last_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ea_set(root, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	err = ea_remove(root, "user.a");
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the create transaction's 9, then the remove
	 * transaction's 9 — but the walk sees 18, not 19. The
	 * create's Open record precedes the restart area's oldest
	 * LSN (the remove's checkpoint pins oldest at the create's
	 * $SI record, the dirty-page table's oldest for the root's
	 * record), so the walk starts after it; the remove's Open
	 * is after oldest and is visible. The Open is in the log —
	 * the single-transaction create test shows it — and replay
	 * does not need it here: the checkpoint's open-attribute
	 * dump registers $MFT:$DATA. The sync's flush notification
	 * (Phase 4 step 2) appends one DeleteDirtyClusters record
	 * after the remove's Forget. */
	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 18);
	if (s.n == 18) {
		/* The create transaction, minus its Open: $SI, the
		 * two CreateAttributes, Commit, three dumps, Forget. */
		CHECK_EQ(s.redo_op[0], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[1], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[2], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[3], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[4], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[5], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[6], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[7], LOP_ForgetTransaction);
		/* The remove transaction, whole: Open, $SI, the two
		 * DeleteAttributes, Commit, three dumps, Forget. */
		CHECK_EQ(s.redo_op[8], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[9], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[10], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[11], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[12], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[13], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[14], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[15], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[16], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[17], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[17], 0u);
		/* The two DeleteAttributes are the pair, $EA first:
		 * the undo's first 4 bytes are the type. */
		CHECK_EQ(s.ea_delete_hits, 1);
		CHECK_EQ(s.ea_delete_lsn, s.lsn[10]);
		CHECK_EQ(s.ea_info_delete_hits, 1);
		CHECK_EQ(s.ea_info_delete_lsn, s.lsn[11]);
		/* The create transaction made the pair once. */
		CHECK_EQ(s.ea_create_hits, 1);
		CHECK_EQ(s.ea_create_lsn, s.lsn[2]);
		CHECK_EQ(s.ea_info_create_hits, 1);
		CHECK_EQ(s.ea_info_create_lsn, s.lsn[1]);
	}

	/* Replay: nothing redoes. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The EA is gone after a remount, and the pair with it. */
	CHECK_EQ(ea_after_mount(snap1, NULL, NULL, "user.a",
				buf, sizeof(buf), &len), -ENODATA);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 28. an EA on a file in the volume root ---------------------------- */

static void test_ea_in_root_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_in_root_journaled\n");
	scratch_path(orig, sizeof(orig), "eaiorig");
	scratch_path(snap, sizeof(snap), "eaisnap");
	scratch_path(snap1, sizeof(snap1), "eaisnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_in_root_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* The create in root is itself unjournaled (the root's index
	 * lives in $INDEX_ALLOCATION), so the walk holds only the
	 * create's two DeleteDirtyClusters records before the EA set
	 * — the same baseline the root-xattr test pins. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
	}

	/* An EA on the root file journals: only its own record is
	 * touched, no parent $FILE_NAME. */
	err = ea_set(f, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	if (!err)
		CHECK_EQ(ntfs_volume_sync(vol), 0);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds the EA transaction's 9 records, plus the
	 * sync's one DeleteDirtyClusters record. The create's two
	 * notifies are gone: the EA's commit checkpoint advanced
	 * oldest_lsn past them, since the notifies had already
	 * retired the entries they named. */
	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 10);
	if (s.n == 10) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[5], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[6], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[7], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[8], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record after the Forget. */
		CHECK_EQ(s.redo_op[9], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[9], 0u);
		CHECK_EQ(s.ea_value_redo_len, 80u);
		CHECK_EQ(s.ea_value_undo_len, 60u);
		CHECK_EQ(s.ea_info_redo_query, 80u);
		CHECK_EQ(s.ea_info_undo_query, 60u);
	}

	/* Replay: nothing redoes — every record targets the file's
	 * own record and is skipped on its MR_LSN stamp. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The value reads back after a remount. */
	CHECK_EQ(ea_after_mount(snap1, NULL, "r.txt", "user.a",
				buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	return;

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	unlink(orig);
	unlink(snap);
}

/* ---- 29. a failed EA set logs nothing (negative control) --------------- */

static void test_ea_failed_set_unjournaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_ea_failed_set_unjournaled\n");
	scratch_path(orig, sizeof(orig), "eafrorig");
	scratch_path(snap, sizeof(snap), "eafrsnap");
	scratch_path(snap1, sizeof(snap1), "eafrsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_ea_failed_set_unjournaled: mkntfs unavailable\n");
		return;
	}

	err = truncate_setup(orig, &dev, &vol, &root, &d, &f, &ino);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }

	err = ea_set(f, "user.a", "abc", 3);
	CHECK_EQ(err, 0);
	if (err) { unlink(orig); return; }
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	/* XATTR_CREATE on an existing EA fails with -EEXIST: the
	 * transaction aborts and no records are written for it. A
	 * failed set changes nothing, so no second sync is needed. */
	err = ea_set_flags(f, "user.a", "xyz", 3, XATTR_CREATE);
	CHECK_EQ(err, -EEXIST);
	ntfs_inode_put(f);
	f = NULL;
	ntfs_inode_put(d);
	d = NULL;
	ntfs_inode_put(root);
	root = NULL;
	CHECK_EQ(copy_file(orig, snap), 0);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err != -EEXIST) { unlink(orig); unlink(snap); return; }

	/* The walk: the one successful set's 9 records, plus the
	 * sync's one DeleteDirtyClusters record. The create's
	 * records are gone: the sync after the create (in
	 * truncate_setup) flushed the $MFT folios and the bitmap
	 * folio, the flush notification (Phase 4 step 2) retired
	 * the create's dirty pages, and the set's commit checkpoint
	 * advanced oldest_lsn past them. The failed set added
	 * nothing. */
	set_ea_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 10);
	if (s.n == 10) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[1], 0x18u);
		CHECK_EQ(s.redo_op[2], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.redo_op[3], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.redo_op[4], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[5], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[6], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[7], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[8], LOP_ForgetTransaction);
		/* The sync's flush notification: one
		 * DeleteDirtyClusters record, with target attribute 0. */
		CHECK_EQ(s.redo_op[9], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[9], 0u);
	}

	/* Replay: nothing redoes — the sync flushed the $MFT folios
	 * and the bitmap folio, so the flush notification retired
	 * the create's and the op's dirty pages alike. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	/* The old value is untouched after a remount. */
	CHECK_EQ(ea_after_mount(snap1, "sub", "j.txt", "user.a",
				buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

static void test_symlink_journaled(void)
{
	symlink_journaled_case("test_symlink_journaled", "ln.txt",
			       "t.txt", 0xA000000Cu);
}

static void test_symlink_wsl_journaled(void)
{
	symlink_journaled_case("test_symlink_wsl_journaled", "w.txt",
			       "a:b", 0xA000001Du);
}

static void test_symlink_dir_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *rd = NULL, *l = NULL;
	ntfs_inode_t *ext = NULL, *rp = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr at;
	uint64_t ino = 0, sub_ino = 0, rp_ino = 0;
	uint16_t flags = 0;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_symlink_dir_journaled\n");
	scratch_path(orig, sizeof(orig), "sdorig");
	scratch_path(snap, sizeof(snap), "sdsnap");
	scratch_path(snap1, sizeof(snap1), "sdsnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_symlink_dir_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);
	err = ntfs_mkdir(root, "realdir", 0100755, &rd);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ntfs_inode_put(rd);
	rd = NULL;

	/* The target is an existing directory here, so the link is
	 * written as a directory record — Windows' <SYMLINKD>. */
	err = ntfs_symlink(d, "dlink", "../realdir", &l);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(l);
	CHECK(ino > 16);
	ntfs_inode_put(l);
	l = NULL;
	err = ntfs_lookup(root, "$Extend", &ext);
	CHECK_EQ(err, 0);
	if (!err) {
		err = ntfs_lookup(ext, "$Reparse", &rp);
		CHECK_EQ(err, 0);
		if (!err)
			rp_ino = ntfs_inode_number(rp);
		ntfs_inode_put(rp);
		rp = NULL;
		ntfs_inode_put(ext);
		ext = NULL;
	}
	CHECK(rp_ino != 0);
	if (err) { goto out; }

	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (l) { ntfs_inode_put(l); l = NULL; }
	if (rd) { ntfs_inode_put(rd); rd = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk is the same 17-record shape as a file link: the
	 * $INDEX_ROOT create is still one CreateAttribute. */
	set_fn_name("dlink");
	CHECK_EQ(walk_log(snap, &s), 0);
	check_symlink_walk(&s, snap, ino, sub_ino, rp_ino, 0x10000400u);

	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);

	CHECK_EQ(readlink_after_mount(snap1, "sub", "dlink",
				      buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, strlen("../realdir"));
	CHECK_EQ(memcmp(buf, "../realdir", len), 0);
	CHECK_EQ(reparse_index_count_entries(snap1, rp_ino, 0xA000000Cu, ino), 1);
	/* The link's record is a directory record, read from the image. */
	CHECK_EQ(read_mft_flags(snap1, ino, &flags), 0);
	CHECK(flags & 0x0002u);
	/* The $SI flags agree between the live snapshot and the replay:
	 * FILE_ATTR_REPARSE_POINT plus FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT
	 * — the dir-link's final ni->flags, which the old staging (from
	 * ni->flags at the reparse set) missed. */
	CHECK_EQ(attr_after_mount(snap, "sub", "dlink", &at), 0);
	CHECK_EQ(at.file_attributes, 0x10000400u);
	CHECK_EQ(attr_after_mount(snap1, "sub", "dlink", &at), 0);
	CHECK_EQ(at.file_attributes, 0x10000400u);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

static void test_symlink_in_root_unjournaled(void)
{
	char orig[1024], snap[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *l = NULL;
	struct rec_seen s;
	char buf[64];
	size_t len = 0;
	int err;

	printf("test_symlink_in_root_unjournaled\n");
	scratch_path(orig, sizeof(orig), "srorig");
	scratch_path(snap, sizeof(snap), "srsnap");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_symlink_in_root_unjournaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	/* The create itself must succeed: the entry goes to the root's
	 * $INDEX_ALLOCATION block, which the journal cannot describe,
	 * so the transaction aborts — discarding the $REPARSE_POINT,
	 * $R entry and $SI flags records already staged — and the
	 * create proceeds unjournaled instead of failing. */
	err = ntfs_symlink(root, "rl.txt", "t.txt", &l);
	CHECK_EQ(err, 0);
	if (!err) {
		CHECK_EQ(ntfs_volume_sync(vol), 0);
		CHECK_EQ(copy_file(orig, snap), 0);
	}
	if (l) { ntfs_inode_put(l); l = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	if (err) { unlink(orig); unlink(snap); return; }

	/* The create proceeds unjournaled, so the log holds no
	 * transaction records — but the flush notification (Phase 4
	 * step 2) appends three DeleteDirtyClusters records for the
	 * stale dirty-page entries the aborted transaction staged
	 * (the abort-leak note in JOURNAL-DESIGN.md §4.3). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 3);
	if (s.n == 3) {
		CHECK_EQ(s.redo_op[0], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[0], 0u);
		CHECK_EQ(s.redo_op[1], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[1], 0u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[2], 0u);
	}

	CHECK_EQ(readlink_after_mount(snap, NULL, "rl.txt",
				      buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, strlen("t.txt"));
	CHECK_EQ(memcmp(buf, "t.txt", len), 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
}

/* ---- 24. unlinking a symlink journals the $R entry removal ---------
 *
 * A symlink's unlink is one transaction like a plain file's, plus
 * the $R index entry removal in $Reparse: the unlink's data records
 * are DeleteIndexEntryRoot ($I30), DeleteAttribute ($FILE_NAME),
 * DeleteIndexEntryRoot ($R), DeallocateFileRecordSegment and
 * ClearBitsInNonresidentBitMap, in that order — the $R removal is
 * staged in the link-count-0 branch, before ntfs_delete_reparse_index
 * removes the entry, and the commit stamps MR_LSN into $Reparse's
 * record too. Before this class the unlink refused reparse points
 * outright and proceeded unjournaled. The walk below holds the
 * symlink create's 15 still-live records (its 2 opens are already
 * dispensable, as in test_unlink_journaled) plus the unlink's 12.
 */
static void test_unlink_reparse_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *l = NULL;
	ntfs_inode_t *ext = NULL, *rp = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	uint64_t ino = 0, sub_ino = 0, rp_ino = 0;
	uint64_t mrlsn_file = 0, mrlsn_dir = 0, mrlsn_rp = 0,
		 lsn_del_idx = 0, lsn_del_attr = 0, lsn_del_r = 0;
	int err;

	printf("test_unlink_reparse_journaled\n");
	scratch_path(orig, sizeof(orig), "urorig");
	scratch_path(snap, sizeof(snap), "ursnap");
	scratch_path(snap1, sizeof(snap1), "ursnap1");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_unlink_reparse_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_symlink(d, "ln.txt", "t.txt", &l);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(l);
	CHECK(ino > 16);
	ntfs_inode_put(l);
	l = NULL;
	/* $Reparse's record number, for the MR_LSN and $R-entry
	 * checks below: $Extend/$Reparse, by name. */
	err = ntfs_lookup(root, "$Extend", &ext);
	CHECK_EQ(err, 0);
	if (!err) {
		err = ntfs_lookup(ext, "$Reparse", &rp);
		CHECK_EQ(err, 0);
		if (!err)
			rp_ino = ntfs_inode_number(rp);
		ntfs_inode_put(rp);
		rp = NULL;
		ntfs_inode_put(ext);
		ext = NULL;
	}
	CHECK(rp_ino != 0);
	if (err) { goto out; }
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* The unlink: the link, its $R entry and its record all go
	 * in the one transaction. */
	CHECK_EQ(ntfs_unlink(d, "ln.txt"), 0);
	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (l) { ntfs_inode_put(l); l = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 15);
	if (s.n != 15) {
		int k;
		for (k = 0; k < s.n; k++)
			fprintf(stderr, "DBG ow op[%d]=%u attr=0x%x lsn=%llu\n",
				k, s.redo_op[k], s.target_attr[k],
				(unsigned long long)s.lsn[k]);
	}
	if (s.n == 15) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		CHECK_EQ(s.redo_op[2], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[3], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[4], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[5], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[6], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[7], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[8], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[9], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[10], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[11], LOP_ForgetTransaction);
		/* The sync's flush notification: three
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0 — the $MFT folio for the
		 * link's record, the $Reparse folio, and the bitmap
		 * folio. */
		CHECK_EQ(s.redo_op[12], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[12], 0u);
		CHECK_EQ(s.redo_op[13], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[13], 0u);
		CHECK_EQ(s.redo_op[14], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[14], 0u);
		/* Every record targets $MFT:$DATA (0x18) except the
		 * ClearBits, which targets $MFT:$BITMAP (0x40). */
		CHECK_EQ(s.target_attr[2], 0x18u);
		CHECK_EQ(s.target_attr[3], 0x18u);
		CHECK_EQ(s.target_attr[4], 0x18u);
		CHECK_EQ(s.target_attr[5], 0x18u);
		CHECK_EQ(s.target_attr[6], 0x40u);
		/* The $I30 entry lives in the parent's record, the
		 * $FILE_NAME and the free in the file's own, the $R
		 * entry in $Reparse's record (vcn = record/4). */
		CHECK_EQ(s.target_vcn[2], sub_ino / 4);
		CHECK_EQ(s.target_vcn[3], ino / 4);
		CHECK_EQ(s.target_vcn[4], rp_ino / 4);
		CHECK_EQ(s.target_vcn[5], ino / 4);
		CHECK_EQ(s.target_vcn[6], (ino / 8) / 4096);
		lsn_del_idx = s.lsn[2];
		lsn_del_attr = s.lsn[3];
		lsn_del_r = s.lsn[4];
	}
	/* All three touched records carry an MR_LSN at or past the
	 * record that touched them — even though the file's record
	 * was deallocated afterwards by evict, the stamp survives
	 * in it. The $Reparse stamp is the new one: without it the
	 * replay would re-apply the $R removal onto a record that
	 * already lacks the entry, and its offset-based delete
	 * would excise the wrong bytes. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_del_attr);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_del_idx);
	CHECK_EQ(read_mft_lsn(snap, rp_ino, &mrlsn_rp), 0);
	CHECK(mrlsn_rp != 0);
	CHECK(mrlsn_rp >= lsn_del_r);

	/* Replay copy 1: nothing redoes — the sync flushed the $MFT
	 * folios, the $Reparse folio and the bitmap folio, so the
	 * flush notification retired the create's and the unlink's
	 * dirty pages alike. The link is already gone on the volume. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);
	CHECK(!an.needs_chkdsk);

	/* The link is gone and its $R entry with it, on the replayed
	 * volume and on the live snapshot; both are ntfsck-clean. */
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "ln.txt"), -ENOENT);
	CHECK_EQ(reparse_index_count_entries(snap1, rp_ino, 0xA000000Cu, ino), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);
	CHECK_EQ(lookup_nested_after_mount(snap, "sub", "ln.txt"), -ENOENT);
	CHECK_EQ(reparse_index_count_entries(snap, rp_ino, 0xA000000Cu, ino), 0);
	CHECK_EQ(run_ntfsck_clean(snap), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* One journaled mknod in a driver-made directory: a char device
 * "dev" (S_IFCHR|0644), tag IO_REPARSE_TAG_LX_CHR (0x80000025).
 * The device branch of __ntfs_create stages the same three reparse
 * records as the symlink branch — $REPARSE_POINT, $R entry, $SI
 * flags — plus the $EA pair a symlink skips, so the walk is the
 * file create's 16 records plus the three reparse ones. */
static void test_mknod_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	ntfs_inode_t *ext = NULL, *rp = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr at;
	uint64_t ino = 0, sub_ino = 0, rp_ino = 0;
	uint64_t mrlsn_file = 0, mrlsn_dir = 0, mrlsn_rp = 0,
		 lsn_fn = 0, lsn_index = 0, lsn_r = 0;
	int err, i;

	printf("test_mknod_journaled\n");
	scratch_path(orig, sizeof(orig), "mkorig");
	scratch_path(snap, sizeof(snap), "mksnap");
	scratch_path(snap1, sizeof(snap1), "mksnap1");
	scratch_path(snap2, sizeof(snap2), "mksnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_mknod_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	sub_ino = ntfs_inode_number(d);

	err = ntfs_mknod(d, "dev", S_IFCHR | 0644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	ino = ntfs_inode_number(f);
	CHECK(ino > 16);
	ntfs_inode_put(f);
	f = NULL;
	/* $Reparse's record number, for the MR_LSN and $R-entry
	 * checks below: $Extend/$Reparse, by name. */
	err = ntfs_lookup(root, "$Extend", &ext);
	CHECK_EQ(err, 0);
	if (!err) {
		err = ntfs_lookup(ext, "$Reparse", &rp);
		CHECK_EQ(err, 0);
		if (!err)
			rp_ino = ntfs_inode_number(rp);
		ntfs_inode_put(rp);
		rp = NULL;
		ntfs_inode_put(ext);
		ext = NULL;
	}
	CHECK(rp_ino != 0);
	if (err) { goto out; }

	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log — what a crash at this instant leaves. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: exactly one transaction, 2 open + 12 data + Commit +
	 * 3 dumps + Forget, plus the sync's four DeleteDirtyClusters
	 * records (the flush notification, Phase 4 step 2). The data
	 * records, in staging order: the $MFT $DATA size extension,
	 * Initialize, $SD, $DATA, $REPARSE_POINT, $R entry,
	 * $EA_INFORMATION, $EA, $FILE_NAME, $SI flags, $I30 entry,
	 * SetBits. The $SI flags sit after the $FILE_NAME: they are
	 * staged from the final ni->flags, which only exists once
	 * fn->file_attributes is folded back into it.
	 */
	set_fn_name("dev");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 23);
	if (s.n == 23) {
		static const uint16_t want_op[12] = {
			LOP_SetNewAttributeSizes,
			LOP_InitializeFileRecordSegment, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_AddIndexEntryRoot, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_UpdateResidentValue, LOP_AddIndexEntryRoot,
			LOP_SetBitsInNonresidentBitMap,
		};
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		for (i = 0; i < 12; i++) {
			CHECK_EQ(s.redo_op[i + 2], want_op[i]);
			/* First eleven target $MFT:$DATA (0x18); the
			 * SetBits targets $MFT:$BITMAP (0x40). */
			CHECK_EQ(s.target_attr[i + 2], i < 11 ? 0x18u : 0x40u);
		}
		CHECK_EQ(s.redo_op[14], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[15], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[16], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[17], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[18], LOP_ForgetTransaction);
		/* The sync's flush notification: four
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		for (i = 19; i < 23; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
		/* The $R entry sits between the $REPARSE_POINT create
		 * and the $EA ones; the $FILE_NAME create is the last
		 * CreateAttribute but one — the $SI flags update follows
		 * it, and the parent index entry follows that. */
		lsn_r = s.lsn[7];
		lsn_fn = s.lsn[10];
		lsn_index = s.lsn[12];
		CHECK_EQ(s.redo_op[10], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[11], LOP_UpdateResidentValue);
		CHECK_EQ(s.redo_op[12], LOP_AddIndexEntryRoot);
	}
	/* The name appears in exactly one CreateAttribute's redo: the
	 * $FILE_NAME one. */
	CHECK_EQ(s.fn_name_hits, 1);
	CHECK_EQ(s.fn_lsn, lsn_fn);
	/* The SetBits names the new record number, one bit. */
	CHECK_EQ(s.bits_bit, (uint32_t)ino);
	CHECK_EQ(s.bits_count, 1u);
	CHECK_EQ(s.bits_lsn, s.lsn[13]);
	CHECK_EQ(s.target_vcn[13], 0u);
	/* The $SI flags record: the final ni->flags, staged after the
	 * $FILE_NAME is built — FILE_ATTRIBUTE_RECALL_ON_OPEN (0x40000)
	 * and FILE_ATTR_REPARSE_POINT (0x400) over the as-added $SI,
	 * which carried FILE_ATTR_SYSTEM (0x4) — the device branch
	 * overwrites the $SI copy, not the record. Staging from
	 * ni->flags at the reparse set would have replayed 0x40400
	 * (no SYSTEM); the live driver lands 0x40404. */
	CHECK_EQ(s.res_redo, 0x40404u);
	CHECK_EQ(s.res_undo, 0x4u);
	CHECK_EQ(s.res_lsn, s.lsn[11]);
	/* The $MFT $DATA size extension: exactly one record, staged
	 * first, its redo the live size, its undo one record less. */
	{
		uint16_t data_off = 0;
		uint64_t alloc = 0, data = 0;
		uint32_t rec_size = 0;

		CHECK_EQ(read_mft0_data_attr(snap, &data_off, &alloc, &data,
					     &rec_size), 0);
		CHECK_EQ(s.sizes_count, 1);
		CHECK_EQ(s.sizes_lsn, s.lsn[2]);
		CHECK_EQ(s.record_off[2], data_off);
		CHECK_EQ(s.target_vcn[2], 0u);
		CHECK_EQ(s.sizes_redo_data, data);
		CHECK_EQ(s.sizes_undo_data, data - rec_size);
		CHECK_EQ(s.sizes_redo_total, 0u);
		CHECK(rec_size == 1024u);
	}
	/* All three touched records carry an MR_LSN at or past the
	 * record that touched them: the replay's skip check. The
	 * $Reparse stamp is the new one — without it the replay
	 * would re-apply the $R entry and duplicate it. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_index);
	CHECK_EQ(read_mft_lsn(snap, rp_ino, &mrlsn_rp), 0);
	CHECK(mrlsn_rp != 0);
	CHECK(mrlsn_rp >= lsn_r);

	/* Replay copy 1: the MFT records were already flushed, so only
	 * the two records that carry no LSN to skip on redo — the $MFT
	 * $DATA size extension and the SetBits. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The device reads back after a remount, with its mode, the
	 * reparse flag and RECALL_ON_OPEN on its $SI, its LX_CHR tag,
	 * and its $R entry in place. The $SI flags agree between the
	 * live snapshot and the replay — 0x40404 on both — which is
	 * what the old staging got wrong. The live snapshot still has
	 * a dirty $LogFile, so its mount stays read-only and strips
	 * the write bits from the mode (020444); the replayed volume
	 * mounts read-write (020644). */
	CHECK_EQ(attr_after_mount(snap, "sub", "dev", &at), 0);
	CHECK_EQ(at.mode, 020444u);
	CHECK_EQ(at.file_attributes, 0x40404u);
	CHECK_EQ(at.reparse_tag, 0x80000025u);
	CHECK_EQ(attr_after_mount(snap1, "sub", "dev", &at), 0);
	CHECK_EQ(at.mode, 020644u);
	CHECK_EQ(at.file_attributes, 0x40404u);
	CHECK_EQ(at.reparse_tag, 0x80000025u);
	CHECK_EQ(reparse_index_count_entries(snap1, rp_ino, 0x80000025u, ino), 1);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * two records. Replay is idempotent — and the $R entry is not
	 * duplicated: it is still exactly one. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(reparse_index_count_entries(snap2, rp_ino, 0x80000025u, ino), 1);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- compressed-write (Phase 3, last class) --------------------------- */

/* Deterministic incompressible bytes: the encoder must fall back to
 * storing the block raw, which is a different branch of
 * ntfs_write_cb() from the one a compressible block takes. */
static void cwrite_fill_random(unsigned char *p, size_t n, unsigned seed)
{
	unsigned s = seed | 1;

	while (n--) {
		s = s * 1103515245u + 12345u;
		*p++ = (unsigned char)(s >> 16);
	}
}

/* Set the compressed flag on a file's $DATA, through the same
 * system.ntfs_attrib handler a user sets it with. Not journaled:
 * the EA class does not journal the flags path. */
static int cwrite_set_compressed(ntfs_inode_t *h)
{
	uint32_t v = 0x800u; /* FILE_ATTRIBUTE_COMPRESSED */

	return ntfs_xattr_handlers[0]->set(ntfs_xattr_handlers[0], NULL, NULL,
					    (struct inode *)h,
					    "system.ntfs_attrib",
					    &v, sizeof(v), 0);
}

/*
 * Setup shared by the compressed-write tests: a driver-made
 * subdirectory "sub" holding a compressed file "j.txt" with 200 KiB
 * of incompressible bytes (three full 64 KiB compression blocks plus
 * an 8 KiB tail, stored raw). Setting the flag converts the file to
 * non-resident eagerly (ntfs_new_attr_flags calls
 * ntfs_attr_make_non_resident), so the initial write is NOT a
 * conversion: it is an ordinary non-resident compressed write, and
 * it IS journaled as a cwrite transaction of its own (2 opens +
 * SetBits + pairs + sizes + $SI + $FILE_NAME + Commit + 3 dumps +
 * Forget = 12 records). The tests below therefore pin TWO
 * transactions in the walk: the setup write's, then the test op's.
 * Returns 0 with @f the open file, @ino its record number.
 */
static int cwrite_setup(ntfs_volume_t *vol, ntfs_inode_t *root,
			ntfs_inode_t **d_out, ntfs_inode_t **f_out,
			uint64_t *ino_out, uint64_t *sub_ino_out)
{
	ntfs_inode_t *d = NULL, *f = NULL;
	unsigned char *buf;
	ssize_t w;
	int err;

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	if (err)
		return err;
	err = ntfs_create(d, "j.txt", 0100644, &f);
	if (err) {
		ntfs_inode_put(d);
		return err;
	}
	err = cwrite_set_compressed(f);
	if (err) {
		ntfs_inode_put(f);
		ntfs_inode_put(d);
		return err;
	}
	buf = malloc(200 * 1024);
	if (!buf) {
		ntfs_inode_put(f);
		ntfs_inode_put(d);
		return -ENOMEM;
	}
	cwrite_fill_random(buf, 200 * 1024, 12345);
	w = ntfs_write(f, buf, 200 * 1024, 0);
	free(buf);
	if (w != 200 * 1024) {
		ntfs_inode_put(f);
		ntfs_inode_put(d);
		return w < 0 ? (int)w : -EIO;
	}
	*sub_ino_out = ntfs_inode_number(d);
	*ino_out = ntfs_inode_number(f);
	*d_out = d;
	*f_out = f;
	return 0;
}

static void test_cwrite_overwrite_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr at;
	uint64_t ino = 0, sub_ino = 0;
	uint64_t mrlsn_file = 0, mrlsn_dir = 0, lsn_fn = 0;
	unsigned char *bbuf = NULL, *orig_buf = NULL, *got = NULL;
	int err, i;

	printf("test_cwrite_overwrite_journaled\n");
	scratch_path(orig, sizeof(orig), "cworig");
	scratch_path(snap, sizeof(snap), "cwsnap");
	scratch_path(snap1, sizeof(snap1), "cwsnap1");
	scratch_path(snap2, sizeof(snap2), "cwsnap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_cwrite_overwrite_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = cwrite_setup(vol, root, &d, &f, &ino, &sub_ino);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	CHECK(ino > 16);

	/* Overwrite the first 64 KiB block with 'B's: the old block
	 * was stored raw (16 clusters of incompressible bytes), the
	 * new one compresses to a single cluster, so the write moves
	 * the bitmap, the pairs, the sizes (alloc and compressed
	 * total) and the parent's $FILE_NAME dup, all at once — the
	 * one transaction this class exists for. */
	bbuf = malloc(64 * 1024);
	CHECK(bbuf != NULL);
	if (!bbuf) { goto out; }
	memset(bbuf, 'B', 64 * 1024);
	{
		ssize_t w = ntfs_write(f, bbuf, 64 * 1024, 0);

		CHECK_EQ(w, 64 * 1024);
		if (w != 64 * 1024) {
			err = -EIO;
			goto out;
		}
	}
	ntfs_inode_put(f);
	f = NULL;

	CHECK_EQ(ntfs_volume_sync(vol), 0);

	/* Snapshot after sync, original still mounted: flushed metadata
	 * plus the still-dirty log — what a crash at this instant leaves. */
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	free(bbuf);
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the create's 14 still-live records, then the
	 * compressed write's 13 — 2 open + 6 data + Commit + 3 dumps +
	 * Forget. The data records, in staging order: the $Bitmap
	 * delta (ClearBits for the freed old block, SetBits for the
	 * new cluster), UpdateMappingPairs, SetNewAttributeSizes, the
	 * $SI 36 bytes, the parent's $FILE_NAME. The bitmap comes
	 * first and the pairs before the sizes, the truncate class's
	 * order: the pairs must name the runs before the sizes that
	 * count them, and the bitmap must agree with both. The sync's
	 * flush notification (Phase 4 step 2) appends four
	 * DeleteDirtyClusters records after the overwrite's Forget. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 43);
	if (s.n == 43) {
		static const uint16_t want_setup[5] = {
			LOP_SetBitsInNonresidentBitMap,
			LOP_UpdateMappingPairs,
			LOP_SetNewAttributeSizes,
			LOP_UpdateResidentValue,
			LOP_UpdateFileNameRoot,
		};
		static const uint16_t want_op[6] = {
			LOP_ClearBitsInNonresidentBitMap,
			LOP_SetBitsInNonresidentBitMap,
			LOP_UpdateMappingPairs,
			LOP_SetNewAttributeSizes,
			LOP_UpdateResidentValue,
			LOP_UpdateFileNameRoot,
		};
		/* The create's 14 still-live records come first
		 * (its two opens precede oldest_lsn, as in the
		 * truncate test). */
		CHECK_EQ(s.redo_op[0], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.redo_op[13], LOP_ForgetTransaction);
		/* The setup write's 12: 2 opens + SetBits for the
		 * fresh clusters + pairs + sizes (0 -> 200 KiB) +
		 * $SI + $FILE_NAME + Commit + 3 dumps + Forget. No
		 * ClearBits: the file had no clusters to free. */
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x68u);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x18u);
		for (i = 0; i < 5; i++) {
			CHECK_EQ(s.redo_op[i + 16], want_setup[i]);
			CHECK_EQ(s.target_attr[i + 16],
				 i < 1 ? 0x68u : 0x18u);
		}
		CHECK_EQ(s.redo_op[21], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[22], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[23], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[24], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[25], LOP_ForgetTransaction);
		/* The overwrite's 13: 2 opens + the $Bitmap delta
		 * (ClearBits for the freed old block, SetBits for
		 * the new cluster) + pairs + sizes + $SI +
		 * $FILE_NAME + Commit + 3 dumps + Forget. The
		 * bitmap comes first and the pairs before the
		 * sizes, the truncate class's order: the pairs
		 * must name the runs before the sizes that count
		 * them, and the bitmap must agree with both. */
		CHECK_EQ(s.redo_op[26], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[26], 0x68u);
		CHECK_EQ(s.redo_op[27], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[27], 0x18u);
		for (i = 0; i < 6; i++) {
			CHECK_EQ(s.redo_op[i + 28], want_op[i]);
			CHECK_EQ(s.target_attr[i + 28],
				 i < 2 ? 0x68u : 0x18u);
		}
		CHECK_EQ(s.redo_op[34], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[35], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[36], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[37], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[38], LOP_ForgetTransaction);
		/* The sync's flush notification (Phase 4 step 2):
		 * four DeleteDirtyClusters records after the Forget,
		 * one per flushed cluster, all with target attribute
		 * 0. No intermediate sync runs between the create and
		 * the writes, so the create's records stay live. */
		for (i = 39; i < 43; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
		lsn_fn = s.lsn[33];
		/* The freed run is the old raw block, 16 clusters;
		 * the new block is one cluster. */
		CHECK_EQ(s.clear_bits_count, 16u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[28]);
		CHECK_EQ(s.bits_count, 1u);
		CHECK_EQ(s.bits_lsn, s.lsn[29]);
		/* The sizes: data_size is unchanged (an overwrite
		 * within the file), the compressed total drops.
		 * Three sizes records in the walk: the create's
		 * $MFT $DATA extension, the setup write's, and
		 * this overwrite's — the captures hold the last. */
		CHECK_EQ(s.sizes_count, 3);
		CHECK_EQ(s.sizes_lsn, s.lsn[31]);
		CHECK_EQ(s.sizes_redo_data, 200u * 1024);
		CHECK_EQ(s.sizes_undo_data, 200u * 1024);
		CHECK(s.sizes_redo_total != 0u);
		/* The $SI: only the times move — the flags cannot
		 * change on this path, the file was already
		 * compressed. */
		CHECK(s.si_redo_mtime > s.si_undo_mtime);
		CHECK(s.si_redo_ctime > s.si_undo_ctime);
		CHECK_EQ(s.si_redo_flags, s.si_undo_flags);
		CHECK_EQ(s.si_lsn, s.lsn[32]);
		/* The parent's dup: the data size is unchanged, the
		 * allocated size drops with the compressed total.
		 * Both flags are COMPRESSED (0x800): the old dup
		 * takes them from ni->flags at begin, not from the
		 * mapped $SI — the flags handler never writes the
		 * $SI record bytes, so the mapped value is 0x0 and
		 * would stage an undo that clears COMPRESSED from
		 * the entry on replay. */
		CHECK_EQ(s.fn_redo_data, 200u * 1024);
		CHECK_EQ(s.fn_undo_data, 200u * 1024);
		CHECK(s.fn_redo_alloc < s.fn_undo_alloc);
		CHECK_EQ(s.fn_redo_flags, 0x800u);
		CHECK_EQ(s.fn_undo_flags, 0x800u);
		CHECK_EQ(s.fnupd_lsn, lsn_fn);
		/* The $SI flags agree the same way: the 36-byte
		 * record is built from in-memory state at both
		 * ends. */
		CHECK_EQ(s.si_redo_flags, 0x800u);
		CHECK_EQ(s.si_undo_flags, 0x800u);
	}
	/* The file's record carries an MR_LSN at or past the last
	 * data record: the replay's skip check. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);

	/* Replay copy 1: seven records redo — the create's sizes and
	 * SetBits, the setup write's SetBits and $FILE_NAME, and the
	 * overwrite's ClearBits, SetBits and $FILE_NAME. The sizes,
	 * pairs and $SI records of both writes are skipped by the
	 * file record's MR_LSN stamp, as is the create's $FILE_NAME
	 * — but the parent's record carries no stamp to skip on, so
	 * both dups redo. Two MFT records are written (record 0 and
	 * the parent) and two clusters (the $MFT:$BITMAP cluster and
	 * the volume $Bitmap's own cluster). */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The replayed file reads back all-new in the rewritten
	 * block and all-old everywhere else, and its size is
	 * unchanged. */
	CHECK_EQ(size_after_mount(snap1, "sub", "j.txt", &at.size), 0);
	CHECK_EQ(at.size, 200u * 1024);
	orig_buf = malloc(200 * 1024);
	got = malloc(200 * 1024);
	CHECK(orig_buf != NULL && got != NULL);
	if (orig_buf && got) {
		cwrite_fill_random(orig_buf, 200 * 1024, 12345);
		memset(orig_buf, 'B', 64 * 1024);
		CHECK_EQ(content_after_mount(snap1, "sub", "j.txt",
					     got, 200 * 1024, 0), 0);
		CHECK_EQ(memcmp(got, orig_buf, 200 * 1024), 0);
	}
	free(orig_buf);
	free(got);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * seven records, and the content is still the new block.
	 * Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	orig_buf = malloc(200 * 1024);
	got = malloc(200 * 1024);
	if (orig_buf && got) {
		cwrite_fill_random(orig_buf, 200 * 1024, 12345);
		memset(orig_buf, 'B', 64 * 1024);
		CHECK_EQ(content_after_mount(snap2, "sub", "j.txt",
					     got, 200 * 1024, 0), 0);
		CHECK_EQ(memcmp(got, orig_buf, 200 * 1024), 0);
	}
	free(orig_buf);
	free(got);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

static void test_cwrite_append_journaled(void)
{
	char orig[1024], snap[1024], snap1[1024], snap2[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr at;
	uint64_t ino = 0, sub_ino = 0;
	uint64_t mrlsn_file = 0, mrlsn_dir = 0, lsn_fn = 0;
	unsigned char *bbuf = NULL, *orig_buf = NULL, *got = NULL;
	int err, i;

	printf("test_cwrite_append_journaled\n");
	scratch_path(orig, sizeof(orig), "cw2orig");
	scratch_path(snap, sizeof(snap), "cw2snap");
	scratch_path(snap1, sizeof(snap1), "cw2snap1");
	scratch_path(snap2, sizeof(snap2), "cw2snap2");
	if (make_image(orig) != 0) {
		fprintf(stderr, "SKIP test_cwrite_append_journaled: mkntfs unavailable\n");
		return;
	}

	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }

	err = cwrite_setup(vol, root, &d, &f, &ino, &sub_ino);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	CHECK(ino > 16);

	/* Append one 64 KiB block of 'B's at EOF: a fresh block, so
	 * there is no old run to free — only a SetBits — but the
	 * pairs, the sizes (data grows 200 KiB to 264 KiB) and the
	 * parent's dup all move. */
	bbuf = malloc(64 * 1024);
	CHECK(bbuf != NULL);
	if (!bbuf) { goto out; }
	memset(bbuf, 'B', 64 * 1024);
	{
		ssize_t w = ntfs_write(f, bbuf, 64 * 1024, 200 * 1024);

		CHECK_EQ(w, 64 * 1024);
		if (w != 64 * 1024) {
			err = -EIO;
			goto out;
		}
	}
	ntfs_inode_put(f);
	f = NULL;

	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	free(bbuf);
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk: the create's 14 still-live records, the setup
	 * write's 12, then the append's 13 — 2 open + 6 data +
	 * Commit + 3 dumps + Forget. The append DOES free a run:
	 * it starts at 200 KiB, inside block 3, which still holds
	 * the setup's 8 KiB tail (stored raw, 2 clusters), so the
	 * append rewrites that block — allocating fresh clusters
	 * for it and punching the old tail — and writes block 4
	 * fresh. The sync's flush notification (Phase 4 step 2)
	 * appends four DeleteDirtyClusters records after the
	 * append's Forget. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 43);
	if (s.n == 43) {
		static const uint16_t want_setup[5] = {
			LOP_SetBitsInNonresidentBitMap,
			LOP_UpdateMappingPairs,
			LOP_SetNewAttributeSizes,
			LOP_UpdateResidentValue,
			LOP_UpdateFileNameRoot,
		};
		static const uint16_t want_op[6] = {
			LOP_ClearBitsInNonresidentBitMap,
			LOP_SetBitsInNonresidentBitMap,
			LOP_UpdateMappingPairs,
			LOP_SetNewAttributeSizes,
			LOP_UpdateResidentValue,
			LOP_UpdateFileNameRoot,
		};
		CHECK_EQ(s.redo_op[0], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.redo_op[13], LOP_ForgetTransaction);
		/* The setup write's 12, as in the overwrite test. */
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x68u);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x18u);
		for (i = 0; i < 5; i++) {
			CHECK_EQ(s.redo_op[i + 16], want_setup[i]);
			CHECK_EQ(s.target_attr[i + 16],
				 i < 1 ? 0x68u : 0x18u);
		}
		CHECK_EQ(s.redo_op[21], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[22], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[23], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[24], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[25], LOP_ForgetTransaction);
		/* The append's 13, in the same staging order as the
		 * overwrite's. */
		CHECK_EQ(s.redo_op[26], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[26], 0x68u);
		CHECK_EQ(s.redo_op[27], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[27], 0x18u);
		for (i = 0; i < 6; i++) {
			CHECK_EQ(s.redo_op[i + 28], want_op[i]);
			CHECK_EQ(s.target_attr[i + 28],
				 i < 2 ? 0x68u : 0x18u);
		}
		CHECK_EQ(s.redo_op[34], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[35], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[36], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[37], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[38], LOP_ForgetTransaction);
		/* The sync's flush notification (Phase 4 step 2):
		 * four DeleteDirtyClusters records after the Forget,
		 * one per flushed cluster, all with target attribute
		 * 0. No intermediate sync runs between the create and
		 * the writes, so the create's records stay live. */
		for (i = 39; i < 43; i++) {
			CHECK_EQ(s.redo_op[i], LOP_DeleteDirtyClusters);
			CHECK_EQ(s.target_attr[i], 0u);
		}
		lsn_fn = s.lsn[33];
		/* The freed run is the old tail block, 3 clusters:
		 * the 8 KiB of incompressible tail is stored raw
		 * with the unit's sub-block framing. The new
		 * clusters are the rewritten block 3 (8 KiB of
		 * incompressible tail plus 56 KiB of 'B's) and the
		 * fresh block 4. */
		CHECK_EQ(s.clear_bits_count, 3u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[28]);
		CHECK_EQ(s.bits_count, 4u);
		CHECK_EQ(s.bits_lsn, s.lsn[29]);
		/* The sizes: data grows 200 KiB to 264 KiB. Three
		 * sizes records in the walk: the create's, the
		 * setup write's, and this append's. */
		CHECK_EQ(s.sizes_count, 3);
		CHECK_EQ(s.sizes_lsn, s.lsn[31]);
		CHECK_EQ(s.sizes_redo_data, 264u * 1024);
		CHECK_EQ(s.sizes_undo_data, 200u * 1024);
		CHECK(s.sizes_redo_total != 0u);
		/* The $SI times move, the flags do not. */
		CHECK(s.si_redo_mtime > s.si_undo_mtime);
		CHECK_EQ(s.si_redo_flags, s.si_undo_flags);
		CHECK_EQ(s.si_lsn, s.lsn[32]);
		/* The parent's dup: both sizes grow. */
		CHECK_EQ(s.fn_redo_data, 264u * 1024);
		CHECK_EQ(s.fn_undo_data, 200u * 1024);
		CHECK(s.fn_redo_alloc > s.fn_undo_alloc);
		CHECK_EQ(s.fnupd_lsn, lsn_fn);
	}
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);

	/* Replay copy 1: seven records redo — the create's sizes and
	 * SetBits, the setup write's SetBits and $FILE_NAME, and the
	 * append's ClearBits, SetBits and $FILE_NAME. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	CHECK_EQ(size_after_mount(snap1, "sub", "j.txt", &at.size), 0);
	CHECK_EQ(at.size, 264u * 1024);
	orig_buf = malloc(264 * 1024);
	got = malloc(264 * 1024);
	CHECK(orig_buf != NULL && got != NULL);
	if (orig_buf && got) {
		cwrite_fill_random(orig_buf, 200 * 1024, 12345);
		memset(orig_buf + 200 * 1024, 'B', 64 * 1024);
		CHECK_EQ(content_after_mount(snap1, "sub", "j.txt",
					     got, 264 * 1024, 0), 0);
		CHECK_EQ(memcmp(got, orig_buf, 264 * 1024), 0);
	}
	free(orig_buf);
	free(got);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * seven records, and the content is still the appended block. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	orig_buf = malloc(264 * 1024);
	got = malloc(264 * 1024);
	if (orig_buf && got) {
		cwrite_fill_random(orig_buf, 200 * 1024, 12345);
		memset(orig_buf + 200 * 1024, 'B', 64 * 1024);
		CHECK_EQ(content_after_mount(snap2, "sub", "j.txt",
					     got, 264 * 1024, 0), 0);
		CHECK_EQ(memcmp(got, orig_buf, 264 * 1024), 0);
	}
	free(orig_buf);
	free(got);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

static void test_cwrite_convert_unjournaled(void)
{
	char orig[1024], snap[1024], snap1[1024], fx[1024];
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *cdir = NULL, *d = NULL, *f = NULL;
	struct rec_seen s;
	struct ntfs_logfile_analysis an;
	struct ntfs_attr at;
	unsigned char *buf = NULL, *got = NULL;
	const char *idir = getenv("NTFS_IMAGES");
	int err;

	printf("test_cwrite_convert_unjournaled\n");
	snprintf(fx, sizeof(fx), "%s/compressed-sparse.img",
		 idir && *idir ? idir : "tools/images");
	scratch_path(orig, sizeof(orig), "cw3orig");
	scratch_path(snap, sizeof(snap), "cw3snap");
	scratch_path(snap1, sizeof(snap1), "cw3snap1");
	if (copy_file(fx, orig) != 0) {
		fprintf(stderr, "SKIP test_cwrite_convert_unjournaled: fixture %s unavailable\n", fx);
		return;
	}

	/* A resident file that is compressed when its first write
	 * begins: created inside the fixture's /compressed/inherit,
	 * a compressed directory (its $INDEX_ROOT carries
	 * ATTR_IS_COMPRESSED, so the inode loader sets NInoCompressed
	 * on it and __ntfs_create flags the new file's $DATA the same
	 * way — resident, empty, compressed). The first write then
	 * converts the attribute inside do_write, which the log's ops
	 * cannot describe — the attribute changes shape and every
	 * following attribute shifts — so the write proceeds
	 * unjournaled by design.
	 *
	 * The fixture is used because the test cannot flag a
	 * directory itself: the $INDEX_ROOT flag is set by
	 * ntfs_attr_set_compression_flag(), which takes the raw
	 * struct ntfs_inode *, and the test API's ntfs_inode_t is an
	 * opaque handle — the raw inode is only reachable through
	 * NTFS_I() inside the driver. (An earlier version of this
	 * test declared the helper extern and passed the handle: the
	 * call read the VFS inode's bytes at the raw inode's field
	 * offsets and segfaulted in map_mft_record. That was the
	 * test's type error, not a driver bug — the "stale folio"
	 * theory from the debug session was wrong, and no
	 * unmap_mft_record fix is owed.) The setxattr handler cannot
	 * substitute either: for a directory it only updates
	 * ni->flags/$SI, never $INDEX_ROOT, and for a file it converts
	 * to non-resident eagerly — the cwrite_setup path, not a
	 * conversion. The fixture's log is clean (walked at 0 live
	 * records before this test was written), so the walk below
	 * pins exactly the create's records. */
	dev = ntfs_bdev_open_path(orig, false);
	CHECK(dev != NULL);
	if (!dev) { unlink(orig); return; }
	err = ntfs_mount(dev, &mopts, &vol);
	CHECK_EQ(err, 0);
	if (err) { ntfs_bdev_close(dev); unlink(orig); return; }
	CHECK_EQ(ntfs_volume_root(vol, &root), 0);
	if (!root) { ntfs_unmount(vol); ntfs_bdev_close(dev); unlink(orig); return; }
	err = ntfs_lookup(root, "compressed", &cdir);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	err = ntfs_lookup(cdir, "inherit", &d);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	err = ntfs_create(d, "j.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err) { goto out; }
	/* The inheritance must have landed: a resident file with
	 * the compressed flag, or the write below would not take
	 * the compressed branch at all and the test would be
	 * vacuous. */
	CHECK_EQ(ntfs_getattr(f, &at), 0);
	CHECK_EQ(at.compressed, 1);
	buf = malloc(200 * 1024);
	CHECK(buf != NULL);
	if (!buf) { goto out; }
	memset(buf, 'B', 200 * 1024);
	{
		ssize_t w = ntfs_write(f, buf, 200 * 1024, 0);

		CHECK_EQ(w, 200 * 1024);
		if (w != 200 * 1024) {
			err = -EIO;
			goto out;
		}
	}
	/* The file must come out compressed, not merely correct:
	 * 200 KiB of one byte compresses to a few clusters. */
	CHECK_EQ(ntfs_getattr(f, &at), 0);
	CHECK_EQ(at.compressed, 1);
	CHECK(at.alloc_size < at.size);
	ntfs_inode_put(f);
	f = NULL;

	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);

out:
	free(buf);
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (cdir) { ntfs_inode_put(cdir); cdir = NULL; }
	if (root) { ntfs_inode_put(root); root = NULL; }
	if (vol) { ntfs_unmount(vol); vol = NULL; }
	if (dev) { ntfs_bdev_close(dev); dev = NULL; }

	if (err) { unlink(orig); unlink(snap); return; }

	/* The walk holds only the create's records — the conversion
	 * write wrote no transaction records at all. 16 live: 2 opens +
	 * sizes + Initialize + 5x CreateAttribute + AddIndexEntryRoot
	 * + SetBits + Commit + 3 dumps + Forget, plus the sync's four
	 * DeleteDirtyClusters records (the flush notification, Phase 4
	 * step 2). The other two tests pin the same create at 14 live
	 * because on a fresh mkntfs image its two opens precede
	 * oldest_lsn; the fixture's log starts clean, so nothing is
	 * trimmed and the opens count. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 20);
	if (s.n == 20) {
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[2], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.redo_op[3], LOP_InitializeFileRecordSegment);
		CHECK_EQ(s.redo_op[9], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[10], LOP_SetBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[11], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[15], LOP_ForgetTransaction);
		/* The sync's flush notification: four
		 * DeleteDirtyClusters records, one per flushed cluster,
		 * all with target attribute 0. */
		CHECK_EQ(s.redo_op[16], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[16], 0u);
		CHECK_EQ(s.redo_op[17], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[17], 0u);
		CHECK_EQ(s.redo_op[18], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[18], 0u);
		CHECK_EQ(s.redo_op[19], LOP_DeleteDirtyClusters);
		CHECK_EQ(s.target_attr[19], 0u);
	}

	/* Replay copy 1: nothing redoes — the sync flushed the $MFT
	 * folios and the bitmap folio, so the flush notification
	 * retired the create's dirty pages. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 0u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 0u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The converted file still reads back whole after the
	 * replay, and it is clean. */
	CHECK_EQ(size_after_mount2(snap1, "compressed", "inherit", "j.txt", &at.size), 0);
	CHECK_EQ(at.size, 200u * 1024);
	buf = malloc(200 * 1024);
	got = malloc(200 * 1024);
	if (buf && got) {
		memset(buf, 'B', 200 * 1024);
		CHECK_EQ(content_after_mount2(snap1, "compressed", "inherit", "j.txt",
					     got, 200 * 1024, 0), 0);
		CHECK_EQ(memcmp(got, buf, 200 * 1024), 0);
	}
	free(buf);
	free(got);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

int main(void)
{
	tool_paths();
	ntfs_set_logger(quiet_logger, NULL);
	test_create_journaled();
	test_flush_notify_retires_dirty_pages();
	test_flush_notify_no_sync_no_retire();
	test_mkdir_unjournaled();
	test_mkdir_journaled();
	test_create_in_root_unjournaled();
	test_unlink_journaled();
	test_unlink_in_root_unjournaled();
	test_rmdir_journaled();
	test_rename_journaled();
	test_rename_in_root_unjournaled();
	test_rename_over_journaled();
	test_rename_cross_dir_journaled();
	test_truncate_shrink_journaled();
	test_truncate_extend_journaled();
	test_truncate_resident_unjournaled();
	test_cwrite_overwrite_journaled();
	test_cwrite_append_journaled();
	test_cwrite_convert_unjournaled();
	test_setattr_journaled();
	test_setattr_size_joins();
	test_setattr_in_root_unjournaled();
	test_xattr_create_journaled();
	test_xattr_replace_journaled();
	test_xattr_big_create_journaled();
	test_xattr_big_replace_remove_journaled();
	test_xattr_in_root_journaled();
	test_ea_replace_journaled();
	test_ea_replace_grow_journaled();
	test_ea_remove_journaled();
	test_ea_create_journaled();
	test_ea_remove_last_journaled();
	test_ea_in_root_journaled();
	test_ea_failed_set_unjournaled();
	test_symlink_journaled();
	test_symlink_wsl_journaled();
	test_symlink_dir_journaled();
	test_symlink_in_root_unjournaled();
	test_unlink_reparse_journaled();
	test_mknod_journaled();
	printf("test_journal: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
