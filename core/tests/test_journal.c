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
 *                        record that touched them. Replaying the dirty
 *                        snapshot redoes only the SetBits — the MFT
 *                        records were already flushed and their MR_LSN
 *                        stamps skip the rest — and the file is present
 *                        and ntfsck -n is clean afterwards. A second
 *                        replay of the same dirty snapshot redoes the
 *                        same single record: replay is idempotent.
 *   mkdir_unjournaled  a mkdir writes no client records at all, and the
 *                        directory is still there after a remount.
 *   create_in_root     a create in the volume root writes no client
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
 *                        an unlink in the root writes no client records
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
 *                        a rename in the root writes no client records
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

/* Mount @path, look @name up under @parent (NULL: the root), read its
 * full attributes (mode, times, flags), unmount. The same shape as
 * size_after_mount, for the setattr tests' post-replay checks. */
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
	 * record. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 16);
	if (s.n == 16) {
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

	/* Replay copy 1: the MFT records were already flushed, so only
	 * the two records that carry no LSN to skip on redo — the $MFT
	 * $DATA size extension and the SetBits — and the extension
	 * writes MFT record 0. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The file is present on the replayed volume, and it is clean. */
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "j.txt"), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * two records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

/* ---- 2. a mkdir writes no client records ------------------------------ */

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
	 * client records at all after a mkdir. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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
	 * slot the file's $DATA occupies. */
	/* The $FILE_NAME scan looks for the created name. */
	set_fn_name("jdir");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 16);
	if (s.n == 16) {
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

	/* Replay copy 1: the MFT records were already flushed, so only
	 * the two records that carry no LSN to skip on redo — the $MFT
	 * $DATA size extension and the SetBits — and the extension
	 * writes MFT record 0. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
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
	 * two records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
	unlink(snap2);
}

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

	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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

	/* The walk from the oldest LSN holds 25 records: the create's
	 * 14 that are still live (its 2 open records are already
	 * dispensable — oldest sits at its first data record — so 9
	 * data + Commit + 3 dumps + Forget) plus the unlink's 11.
	 * The unlink transaction is 2 open + 4 data + Commit + 3
	 * dumps + Forget: it touches two attributes, so it opens
	 * both ($MFT:$DATA for the index, the $FILE_NAME and the
	 * Deallocate; $MFT:$BITMAP for the ClearBits), and its data
	 * records are DeleteIndexEntryRoot, DeleteAttribute,
	 * DeallocateFileRecordSegment and ClearBitsInNonresidentBitMap
	 * — the free is part of the unlink transaction, not a
	 * separate evict one. The create's records stay live because
	 * the MFT pages they dirtied were never flushed to disk — no
	 * checkpoint is emitted after a sync, so the dirty-page table
	 * still names the create's LSNs as the oldest for those
	 * pages. That is correct NTFS behaviour: a crash before those
	 * pages flush needs the create's redo records. The unlink's
	 * records follow at positions 14..24. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 25);
	if (s.n == 25) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x40u);
		CHECK_EQ(s.redo_op[16], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[17], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[18], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[19], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[20], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[21], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[22], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[23], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[24], LOP_ForgetTransaction);
		/* The index, $FILE_NAME and Deallocate records target
		 * $MFT:$DATA (0x18): the index entry lives in the
		 * parent's record, the $FILE_NAME and the free in the
		 * file's own. The ClearBits targets $MFT:$BITMAP
		 * (0x40). */
		CHECK_EQ(s.target_attr[16], 0x18u);
		CHECK_EQ(s.target_attr[17], 0x18u);
		CHECK_EQ(s.target_attr[18], 0x18u);
		CHECK_EQ(s.target_attr[19], 0x40u);
		/* The file's record is at vcn ino/4 (1024-byte records
		 * in 4096-byte clusters), the parent's at sub_ino/4;
		 * the bitmap bit for the file's record is in cluster
		 * (ino/8)/4096 of $MFT:$BITMAP. */
		CHECK_EQ(s.target_vcn[16], sub_ino / 4);
		CHECK_EQ(s.target_vcn[17], ino / 4);
		CHECK_EQ(s.target_vcn[18], ino / 4);
		CHECK_EQ(s.target_vcn[19], (ino / 8) / 4096);
		lsn_del_idx = s.lsn[16];
		lsn_del_attr = s.lsn[17];
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

	/* Replay copy 1: the unlink's DeleteIndexEntryRoot and
	 * DeleteAttribute are skipped on their MR_LSN stamps and
	 * its Deallocate is skipped on its effect (the record is
	 * already free on disk — the effect-based skip), but the
	 * create's records are still live (see above) so the replay
	 * redoes exactly its two idempotent ones —
	 * SetNewAttributeSizes (MFT record 0) and
	 * SetBitsInNonresidentBitMap (the $MFT bitmap cluster) —
	 * plus the unlink's ClearBits, which clears the same
	 * cluster's bit again (idempotent: the clean unmount
	 * already cleared it, so the cluster count stays 1).
	 * The replayed image is ntfsck-clean: the free is in the
	 * unlink transaction, so the SetBits redo no longer
	 * resurrects the freed record's bit.
	 */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 3u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
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

	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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

	/* The walk holds the mkdir's 14 still-live records first (its
	 * two opens precede oldest_lsn and are dispensable), then the
	 * rmdir's full 11 — the rmdir's opens follow oldest_lsn, so
	 * they are held. Before the mkdir class the setup mkdir was
	 * unjournaled and the walk held only the rmdir's 11. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 25);
	if (s.n == 25) {
		/* The mkdir's first and last live records. */
		CHECK_EQ(s.redo_op[0], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.redo_op[13], LOP_ForgetTransaction);
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x40u);
		CHECK_EQ(s.redo_op[16], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[17], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[18], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[19], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[20], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[21], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[22], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[23], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[24], LOP_ForgetTransaction);
		CHECK_EQ(s.target_vcn[16], sub_ino / 4);
		CHECK_EQ(s.target_vcn[17], sub2_ino / 4);
		CHECK_EQ(s.target_vcn[18], sub2_ino / 4);
		CHECK_EQ(s.target_vcn[19], (sub2_ino / 8) / 4096);
	}

	memset(&an, 0, sizeof(an));
	CHECK_EQ(copy_file(snap, snap1), 0);
	CHECK_EQ(replay_path(snap1, &an), 0);
	/* Three records redo: the mkdir's $MFT $DATA size extension
	 * and SetBits (both idempotent) plus the rmdir's ClearBits
	 * (idempotent clear of an already-clear bit); the Deallocate
	 * is skipped on its effect — the record is already free on
	 * disk. */
	CHECK_EQ(an.records_redone, 3u);
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

	/* The walk holds 24 records: the create's 14 still-live ones
	 * (see test_unlink_journaled) plus the rename's 10 — 1 open
	 * ($MFT:$DATA: the rename allocates and frees nothing, so no
	 * $MFT:$BITMAP open record) + 4 data + Commit + 3 dumps +
	 * Forget. The rename's data records are the link half first
	 * (CreateAttribute for the new $FILE_NAME, AddIndexEntryRoot
	 * for the new entry) then the old name's removal
	 * (DeleteIndexEntryRoot, DeleteAttribute), in live order. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 24);
	if (s.n == 24) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[16], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[17], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[18], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[19], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[20], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[21], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[22], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[23], LOP_ForgetTransaction);
		CHECK_EQ(s.target_attr[15], 0x18u);
		CHECK_EQ(s.target_attr[16], 0x18u);
		CHECK_EQ(s.target_attr[17], 0x18u);
		CHECK_EQ(s.target_attr[18], 0x18u);
		/* The new $FILE_NAME and the old one's removal target
		 * the file's record; both index records target the
		 * parent's. */
		CHECK_EQ(s.target_vcn[15], ino / 4);
		CHECK_EQ(s.target_vcn[16], sub_ino / 4);
		CHECK_EQ(s.target_vcn[17], sub_ino / 4);
		CHECK_EQ(s.target_vcn[18], ino / 4);
		lsn_create_attr = s.lsn[15];
		lsn_add_idx = s.lsn[16];
		lsn_del_idx = s.lsn[17];
		lsn_del_attr = s.lsn[18];
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

	/* Replay: only the create's two idempotent records redo
	 * (SetNewAttributeSizes on record 0 and SetBits on the $MFT
	 * bitmap cluster) — the rename's four data records are all
	 * skipped on their MR_LSN stamps, because the file and the
	 * directory were flushed after the commit. The file is under
	 * its new name and ntfsck is clean. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
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

	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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
	/* The walk holds 45 records: the first create's 14 still-live
	 * ones (its 2 open records precede oldest — oldest sits at
	 * its first data record, the Initialize — so they are already
	 * dispensable), the second create's full 16 (its opens follow
	 * oldest, so they are still live; it stages a sizes record
	 * too, because allocating its record grows $MFT $DATA's
	 * data_size by one record), plus the rename's 15 — 2 open
	 * ($MFT:$DATA and $MFT:$BITMAP: the target's free touches
	 * the bitmap) + 8 data + Commit + 3 dumps + Forget. The
	 * rename's data records are the target's removal first
	 * (DeleteIndexEntryRoot, DeleteAttribute,
	 * DeallocateFileRecordSegment,
	 * ClearBitsInNonresidentBitMap), then the link half
	 * (CreateAttribute, AddIndexEntryRoot), then the old name's
	 * removal (DeleteIndexEntryRoot, DeleteAttribute). */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 45);
	if (s.n == 45) {
		CHECK_EQ(s.redo_op[30], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[30], 0x18u);
		CHECK_EQ(s.redo_op[31], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[31], 0x40u);
		CHECK_EQ(s.redo_op[32], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[33], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[34], LOP_DeallocateFileRecordSegment);
		CHECK_EQ(s.redo_op[35], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[36], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[37], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[38], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[39], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[40], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[41], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[42], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[43], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[44], LOP_ForgetTransaction);
		CHECK_EQ(s.target_vcn[32], sub_ino / 4);
		CHECK_EQ(s.target_vcn[33], k_ino / 4);
		CHECK_EQ(s.target_vcn[34], k_ino / 4);
		CHECK_EQ(s.target_vcn[35], (k_ino / 8) / 4096);
		CHECK_EQ(s.target_vcn[36], j_ino / 4);
		CHECK_EQ(s.target_vcn[37], sub_ino / 4);
		CHECK_EQ(s.target_vcn[38], sub_ino / 4);
		CHECK_EQ(s.target_vcn[39], j_ino / 4);
	}

	/* Replay: the two creates' idempotent records redo (each
	 * create's sizes + SetBits — every record allocation grows
	 * $MFT $DATA's data_size by one record, so both creates
	 * staged a sizes record) plus the rename's ClearBits for
	 * the target's freed record — the rest of the rename's
	 * records are skipped on their MR_LSN stamps (or, for the
	 * Deallocate, on its effect). The file is under the target
	 * name with the source's inode, and ntfsck is clean. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 5u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
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

	/* The walk holds 24 records: the create's 14 still-live ones
	 * plus the rename's 10 — the same four data records as a
	 * same-directory rename, split across the two parents: the
	 * link half targets d2, the old name's removal targets d1. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 24);
	if (s.n == 24) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[16], LOP_AddIndexEntryRoot);
		CHECK_EQ(s.redo_op[17], LOP_DeleteIndexEntryRoot);
		CHECK_EQ(s.redo_op[18], LOP_DeleteAttribute);
		CHECK_EQ(s.redo_op[19], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[20], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[21], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[22], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[23], LOP_ForgetTransaction);
		CHECK_EQ(s.target_vcn[15], ino / 4);
		CHECK_EQ(s.target_vcn[16], d2_ino / 4);
		CHECK_EQ(s.target_vcn[17], d1_ino / 4);
		CHECK_EQ(s.target_vcn[18], ino / 4);
		lsn_create_attr = s.lsn[15];
		lsn_add_idx = s.lsn[16];
		lsn_del_idx = s.lsn[17];
		lsn_del_attr = s.lsn[18];
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

	/* Replay: only the create's two idempotent records redo. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
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

	/* The walk: the create's 14 still-live records, then the
	 * setattr transaction's 13. ntfs_truncate routes through
	 * ntfs_setattr, so the resize joins the setattr transaction:
	 * the truncate's 6 (2 open + 4 data) plus the setattr's 2
	 * (the $SI 36 bytes, then a second UpdateFileNameRoot with
	 * the same dup — the setattr stages its own $FILE_NAME
	 * record from its begin snapshot, it does not dedupe
	 * against the truncate's), plus Commit + 3 dumps + Forget.
	 * The four truncate data records are the $Bitmap ClearBits
	 * for the freed run, the UpdateMappingPairs for the shorter
	 * run list, the SetNewAttributeSizes for the new sizes, and
	 * the UpdateFileNameRoot for the parent's $FILE_NAME — in
	 * that order: clusters first, then the pairs that name
	 * them, then the sizes, then the parent's duplicated
	 * sizes. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 27);
	if (s.n == 27) {
		/* The opens come in first-use order: the truncate
		 * stages the $Bitmap record first, so the volume
		 * $Bitmap ($Bitmap:$DATA) opens before $MFT:$DATA —
		 * the reverse of the create. The ClearBits targets
		 * the attribute the truncate opened first, and that
		 * attribute is NOT the one the create's SetBits used
		 * ($MFT:$BITMAP, MFT-record bits). */
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], s.clear_bits_target_attr);
		CHECK(s.target_attr[14] != s.bits_target_attr);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x18u);
		CHECK_EQ(s.redo_op[16], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[16], s.clear_bits_target_attr);
		CHECK(s.clear_bits_target_attr != s.bits_target_attr);
		CHECK_EQ(s.redo_op[17], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[17], 0x18u);
		CHECK_EQ(s.redo_op[18], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[18], 0x18u);
		CHECK_EQ(s.redo_op[19], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[19], 0x18u);
		/* The setattr's two records, staged at finish after
		 * the truncate's: the $SI 36 bytes (mtime/ctime moved
		 * by the resize) and a second UpdateFileNameRoot —
		 * same dup as [19], staged from the setattr's begin
		 * snapshot. Both address the file's record through
		 * $MFT:$DATA (0x18). */
		CHECK_EQ(s.redo_op[20], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[20], 0x18u);
		CHECK_EQ(s.redo_op[21], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[21], 0x18u);
		CHECK_EQ(s.redo_op[22], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[23], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[24], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[25], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[26], LOP_ForgetTransaction);
		/* The $FILE_NAME record names the new sizes (64 KiB)
		 * and undoes to the old ones (256 KiB); the parent's
		 * record is addressed through $MFT:$DATA, like the
		 * file's own. */
		CHECK_EQ(s.fn_redo_data, 64u * 1024u);
		CHECK_EQ(s.fn_undo_data, 256u * 1024u);
		CHECK_EQ(s.fn_redo_alloc, 64u * 1024u);
		CHECK_EQ(s.fn_undo_alloc, 256u * 1024u);
		/* The collector keeps the LAST $FILE_NAME record: the
		 * setattr's dup at [21], identical to the truncate's
		 * at [19]. */
		CHECK_EQ(s.fnupd_lsn, s.lsn[21]);
		/* The ClearBits frees 48 clusters, one run. (The
		 * SetBits fields still hold the create's 1-bit record;
		 * the clear has its own capture.) */
		CHECK_EQ(s.clear_bits_count, 48u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[16]);
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
		CHECK_EQ(s.sizes_lsn, s.lsn[18]);
		/* The pairs record is the $DATA attribute, at the
		 * same record offset as the sizes record. */
		CHECK_EQ(s.record_off[17], s.record_off[18]);
	}
	(void)i;

	/* Replay copy 1: five records redo — the create's sizes
	 * and SetBits, the truncate's ClearBits, and the two
	 * $FILE_NAME records (the truncate's and the setattr's
	 * identical second dup). The truncate's sizes record is skipped
	 * by the file record's MR_LSN stamp (the commit stamped it
	 * and the sync flushed it), as is the pairs record and the
	 * setattr's $SI record — but
	 * the $FILE_NAME records are NOT skipped: the commit stamps
	 * MR_LSN only into the file's record, not the parent's, so
	 * the parent's record carries no stamp to skip on (the same
	 * as record 0, whose stamp the replay applies itself).
	 * The redo is idempotent — it writes the same duplicated
	 * sizes the sync already flushed — and it writes the
	 * parent's record, so two MFT records are written, not
	 * one. Two clusters are written, not one: the create's
	 * SetBits lands on the $MFT:$BITMAP cluster and the
	 * truncate's ClearBits on the volume $Bitmap's own cluster
	 * — before the bitmap fix both landed on the same (wrong)
	 * cluster and the count was 1. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 5u);
	CHECK_EQ(an.mft_records_written, 2u);
	CHECK_EQ(an.clusters_written, 2u);
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
	CHECK_EQ(an.records_redone, 5u);
	CHECK_EQ(an.mft_records_written, 2u);
	CHECK_EQ(an.clusters_written, 2u);

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
	CHECK_EQ(s.n, 39);
	if (s.n == 39) {
		/* The shrink's records sit at [14..26] (its
		 * $FILE_NAME at [19], its setattr's $SI at [20] and
		 * second $FILE_NAME at [21], as in the shrink test).
		 * The extend's records sit at [27..38]: one open
		 * ($DATA — no $Bitmap record, so $BITMAP never
		 * opens), then pairs, sizes, the $SI flags, the
		 * parent's $FILE_NAME, then the setattr's $SI and
		 * second $FILE_NAME, Commit, dumps, Forget. */
		CHECK_EQ(s.redo_op[19], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[19], 0x18u);
		CHECK_EQ(s.redo_op[20], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[20], 0x18u);
		CHECK_EQ(s.redo_op[21], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[21], 0x18u);
		CHECK_EQ(s.redo_op[27], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[27], 0x18u);
		CHECK_EQ(s.redo_op[28], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[28], 0x18u);
		CHECK_EQ(s.redo_op[29], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[29], 0x18u);
		CHECK_EQ(s.redo_op[30], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[30], 0x18u);
		CHECK_EQ(s.redo_op[31], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[31], 0x18u);
		CHECK_EQ(s.redo_op[32], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[32], 0x18u);
		CHECK_EQ(s.redo_op[33], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[33], 0x18u);
		CHECK_EQ(s.redo_op[34], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[35], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[36], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[37], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[38], LOP_ForgetTransaction);
		/* The extend's $FILE_NAME record (the last one the
		 * collector saw — the setattr's second dup at [33])
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
		CHECK_EQ(s.fnupd_lsn, s.lsn[33]);
		/* The $SI record sets the sparse bit (0x200) and
		 * undoes to clear (0x0) — the $DATA flag flips in
		 * step, so the two agree after replay either way. The
		 * 4-byte capture ignores the setattr's 36-byte $SI
		 * record at [32], so this is still the truncate's
		 * flags record at [30]. */
		CHECK_EQ(s.res_redo, 0x200u);
		CHECK_EQ(s.res_undo, 0u);
		CHECK_EQ(s.res_lsn, s.lsn[30]);
		/* No bitmap record in the extend: the ClearBits
		 * fields still hold the shrink's 48-cluster free. */
		CHECK_EQ(s.clear_bits_count, 48u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[16]);
		/* The sizes record names the new data size. */
		CHECK_EQ(s.sizes_redo_data, 256u * 1024u);
		CHECK_EQ(s.sizes_undo_data, 64u * 1024u);
		CHECK_EQ(s.sizes_lsn, s.lsn[29]);
		/* The pairs record is the $DATA attribute, at the
		 * same record offset as the sizes record. */
		CHECK_EQ(s.record_off[28], s.record_off[29]);
	}

	/* Replay: seven records redo — the create's sizes and SetBits,
	 * the shrink's ClearBits, and the four $FILE_NAME records
	 * (shrink's truncate + setattr dups, extend's truncate +
	 * setattr dups). The extend adds no pairs, sizes or
	 * $SI redo: those are skipped by the file record's MR_LSN
	 * stamp (the commit stamped it and the sync flushed it), and
	 * it has no $Bitmap record to redo. The $FILE_NAME records
	 * redo because the commit stamps MR_LSN only into the file's
	 * record, not the parent's (see the shrink test) — the redo
	 * is idempotent and writes the parent's record, so two MFT
	 * records are written, not one. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 7u);
	CHECK_EQ(an.mft_records_written, 2u);
	CHECK_EQ(an.clusters_written, 2u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	CHECK_EQ(size_after_mount(snap1, "sub", "j.txt", &size), 0);
	CHECK_EQ(size, 256u * 1024u);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	unlink(orig);
	unlink(snap);
	unlink(snap1);
}

/* ---- 14. a truncate of a resident file writes no client records -------- */

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
	 * create journals (14 live records); the truncate must add
	 * none — walk before and after and compare. */
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
	CHECK_EQ(s.n, 16);

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

	/* Still 16: the truncate added no client records. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 16);

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

	/* The walk: the create's 14, then the setattr transaction's 9
	 * (one open + $SI + $EA + $FILE_NAME + Commit + 3 dumps +
	 * Forget). No $EA_INFORMATION record: the $LXMOD value changes
	 * but the entry count does not, so its 8 bytes are unchanged.
	 * No $Bitmap record and no second open: everything the
	 * setattr stages lives in the file's own record, addressed
	 * through $MFT:$DATA. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 23);
	if (s.n == 23) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_UpdateResidentValue);
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
		/* The $SI record (the 36-byte one, at [15]) names the
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
		CHECK_EQ(s.si_lsn, s.lsn[15]);
		/* The $EA record (at [16]) carries the new $LXMOD. */
		CHECK_EQ(s.ea_lxmod_redo, 0100444u);
		CHECK_EQ(s.ea_lxmod_undo, 0100644u);
		CHECK_EQ(s.ea_lsn, s.lsn[16]);
		/* The $FILE_NAME record (at [17]) carries the READONLY
		 * flag and undoes to the create's flags; the sizes do
		 * not move. */
		CHECK_EQ(s.fn_redo_flags, old.file_attributes | 0x1u);
		CHECK_EQ(s.fn_undo_flags, old.file_attributes);
		CHECK_EQ(s.fn_redo_data, 256u * 1024u);
		CHECK_EQ(s.fn_undo_data, 256u * 1024u);
		CHECK_EQ(s.fnupd_lsn, s.lsn[17]);
	}
	(void)i;

	/* Replay copy 1: three records redo — the create's sizes and
	 * SetBits, and the setattr's $FILE_NAME record. The $SI and
	 * $EA records are skipped by the file record's MR_LSN stamp
	 * (the commit stamped it and the sync flushed it); the
	 * $FILE_NAME record is NOT skipped, the commit stamps MR_LSN
	 * only into the file's record, not the parent's. The redo is
	 * idempotent. One cluster is written: the create's SetBits
	 * lands on the $MFT:$BITMAP cluster; the $FILE_NAME redo
	 * writes the parent's MFT record, not a data cluster. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 3u);
	CHECK_EQ(an.mft_records_written, 2u);
	CHECK_EQ(an.clusters_written, 1u);
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
	 * three records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 3u);
	CHECK_EQ(an.mft_records_written, 2u);
	CHECK_EQ(an.clusters_written, 1u);

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

	/* The walk: the create's 14, then one 14-record transaction:
	 * the truncate's 6 (2 open + ClearBits + pairs + sizes +
	 * $FILE_NAME), the setattr's 3 ($SI + $EA + a second
	 * $FILE_NAME with the same dup), and one Commit + 3 dumps +
	 * Forget — not two transactions. The $SI here carries the
	 * resize's mtime/ctime (current_time) as well as the
	 * READONLY flag. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 28);
	if (s.n == 28) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x18u);
		CHECK_EQ(s.redo_op[16], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.redo_op[17], LOP_UpdateMappingPairs);
		CHECK_EQ(s.target_attr[17], 0x18u);
		CHECK_EQ(s.redo_op[18], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[18], 0x18u);
		CHECK_EQ(s.redo_op[19], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[19], 0x18u);
		CHECK_EQ(s.redo_op[20], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[20], 0x18u);
		CHECK_EQ(s.redo_op[21], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[21], 0x18u);
		CHECK_EQ(s.redo_op[22], LOP_UpdateFileNameRoot);
		CHECK_EQ(s.target_attr[22], 0x18u);
		CHECK_EQ(s.redo_op[23], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[24], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[25], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[26], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[27], LOP_ForgetTransaction);
		/* Exactly two commits in the whole log: the create's
		 * and this one. A second transaction for the resize
		 * would make three. */
		for (i = 0; i < s.n; i++)
			if (s.redo_op[i] == LOP_CommitTransaction)
				commits++;
		CHECK_EQ(commits, 2);
		/* The setattr's $SI (at [20], the last 36-byte one)
		 * carries the READONLY flag; the resize moved the
		 * times, so redo and undo differ there too. */
		CHECK_EQ(s.si_redo_flags, old.file_attributes | 0x1u);
		CHECK_EQ(s.si_undo_flags, old.file_attributes);
		CHECK(s.si_redo_mtime != s.si_undo_mtime);
		CHECK_EQ(s.si_lsn, s.lsn[20]);
		/* The $EA record (at [21]) carries the new $LXMOD. */
		CHECK_EQ(s.ea_lxmod_redo, 0100444u);
		CHECK_EQ(s.ea_lxmod_undo, 0100644u);
		CHECK_EQ(s.ea_lsn, s.lsn[21]);
		/* The last $FILE_NAME record (the setattr's dup at
		 * [22]) names the new sizes. */
		CHECK_EQ(s.fn_redo_data, 64u * 1024u);
		CHECK_EQ(s.fn_undo_data, 256u * 1024u);
		CHECK_EQ(s.fnupd_lsn, s.lsn[22]);
	}
	(void)i;

	/* Replay: five records redo — the create's sizes and SetBits,
	 * the truncate's ClearBits, and the two $FILE_NAME records
	 * (the truncate's and the setattr's identical second dup).
	 * The pairs, sizes, $SI and $EA records are skipped by the
	 * file record's MR_LSN stamp. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 5u);
	CHECK_EQ(an.mft_records_written, 2u);
	CHECK_EQ(an.clusters_written, 2u);
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
	 * lives in $INDEX_ALLOCATION), so the walk is empty before the
	 * setattr — the same baseline the root-create test pins. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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

	/* Still empty: the setattr added no client records. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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

	/* The walk: the create's 14, then the xattr transaction's 8
	 * (one open — $MFT:$DATA — the $SI ctime record, the ADS
	 * CreateAttribute, Commit, three table dumps, Forget). No
	 * $Bitmap record and no second open: a resident create
	 * allocates no clusters. No $FILE_NAME record: ADS sizes are
	 * not in the parent's dup. */
	set_ads_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 22);
	if (s.n == 22) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[15], 0x18u);
		CHECK_EQ(s.redo_op[16], LOP_CreateAttribute);
		CHECK_EQ(s.target_attr[16], 0x18u);
		CHECK_EQ(s.redo_op[17], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[18], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[19], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[20], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[21], LOP_ForgetTransaction);
		/* The $SI record (at [15]) names a later ctime than
		 * the create's — the setxattr success path only bumps
		 * ctime — and undoes to the create's ctime. */
		CHECK(s.si_redo_ctime > s.si_undo_ctime);
		CHECK_EQ(s.si_lsn, s.lsn[15]);
		/* The ADS create is the only CreateAttribute carrying
		 * "user.a"; the $FILE_NAME create is the only one
		 * carrying "j.txt". */
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[16]);
		CHECK_EQ(s.fn_name_hits, 1);
	}

	/* Replay copy 1: only the create's two idempotent records
	 * redo — the xattr's records are skipped on the file
	 * record's MR_LSN stamp. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The value reads back after a remount. */
	CHECK_EQ(xattr_after_mount(snap1, "sub", "j.txt", "user.a",
				   buf, sizeof(buf), &len), 0);
	CHECK_EQ(len, 3u);
	CHECK_EQ(memcmp(buf, "abc", 3), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot redoes the same two
	 * records. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);

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

	/* The walk: the create's 14, the create-xattr transaction's
	 * 8, then the replace transaction's 8 (one open, the $SI
	 * record, the value record, Commit, three dumps, Forget). */
	set_ads_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 30);
	if (s.n == 30) {
		CHECK_EQ(s.redo_op[22], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[22], 0x18u);
		CHECK_EQ(s.redo_op[23], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[23], 0x18u);
		CHECK_EQ(s.redo_op[24], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[24], 0x18u);
		CHECK_EQ(s.redo_op[25], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[26], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[27], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[28], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[29], LOP_ForgetTransaction);
		/* The value record (at [24]) carries the new value
		 * whole and undoes to the old value whole. */
		CHECK_EQ(s.ads_value_redo_len, 6u);
		CHECK_EQ(memcmp(s.ads_value_redo, "abcdef", 6), 0);
		CHECK_EQ(s.ads_value_undo_len, 3u);
		CHECK_EQ(memcmp(s.ads_value_undo, "abc", 3), 0);
		CHECK_EQ(s.ads_value_lsn, s.lsn[24]);
		/* The ADS was created once, in the first xattr
		 * transaction. */
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[16]);
	}

	/* Replay: only the create's two idempotent records redo. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 2u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 1u);

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

	/* The walk: the create's 14, then the xattr transaction's 10
	 * (two opens — $MFT:$DATA then $MFT:$BITMAP, in first-use
	 * order — the $SI record, the ADS CreateAttribute, the
	 * SetBits for its two clusters, Commit, three dumps,
	 * Forget). */
	set_ads_name("user.big");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 24);
	if (s.n == 24) {
		CHECK_EQ(s.redo_op[14], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[14], 0x18u);
		CHECK_EQ(s.redo_op[15], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[15], 0x68u);
		CHECK_EQ(s.redo_op[16], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[16], 0x18u);
		CHECK_EQ(s.redo_op[17], LOP_CreateAttribute);
		CHECK_EQ(s.target_attr[17], 0x18u);
		CHECK_EQ(s.redo_op[18], LOP_SetBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[18], 0x68u);
		CHECK_EQ(s.redo_op[19], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[20], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[21], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[22], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[23], LOP_ForgetTransaction);
		/* The SetBits names two clusters: 8 KiB at 4 KiB per
		 * cluster. */
		CHECK_EQ(s.bits_target_attr, 0x68u);
		CHECK_EQ(s.bits_count, 2u);
		CHECK_EQ(s.bits_lsn, s.lsn[18]);
		CHECK_EQ(s.ads_create_hits, 1);
		CHECK_EQ(s.ads_create_lsn, s.lsn[17]);
	}

	/* Replay: the create's two records plus the xattr's SetBits
	 * (the $Bitmap is never MR_LSN-stamped). The CreateAttribute
	 * and $SI are skipped on the file record's stamp. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 3u);
	CHECK_EQ(an.mft_records_written, 1u);
	CHECK_EQ(an.clusters_written, 2u);
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

	/* The walk: the create's 14, the create-xattr transaction's
	 * 10, the replace transaction's 11 (two opens, the $SI
	 * record, sizes, pairs, SetBits, Commit, three dumps,
	 * Forget), then the remove transaction's 10 (two opens, the
	 * $SI record, DeleteAttribute, ClearBits, Commit, three
	 * dumps, Forget). */
	set_ads_name("user.big");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 45);
	if (s.n == 45) {
		/* Replace transaction: opens, $SI, sizes, pairs. */
		CHECK_EQ(s.redo_op[24], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[24], 0x18u);
		CHECK_EQ(s.redo_op[25], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[25], 0x68u);
		CHECK_EQ(s.redo_op[26], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[26], 0x18u);
		CHECK_EQ(s.redo_op[27], LOP_SetNewAttributeSizes);
		CHECK_EQ(s.target_attr[27], 0x18u);
		CHECK_EQ(s.redo_op[28], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[28], 0x18u);
		CHECK_EQ(s.redo_op[29], LOP_SetBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[29], 0x68u);
		CHECK_EQ(s.redo_op[30], LOP_CommitTransaction);
		/* The sizes record names the 8 KiB -> 16 KiB growth. */
		/* Two sizes records: the create (0 -> 256 KiB) and the
		 * replace (8 KiB -> 16 KiB). */
		CHECK_EQ(s.sizes_count, 2);
		CHECK_EQ(s.sizes_redo_data, 16u * 1024u);
		CHECK_EQ(s.sizes_undo_data, 8u * 1024u);
		/* Remove transaction: opens, $SI, DeleteAttribute,
		 * ClearBits. */
		CHECK_EQ(s.redo_op[35], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[35], 0x18u);
		CHECK_EQ(s.redo_op[36], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[36], 0x68u);
		CHECK_EQ(s.redo_op[37], LOP_UpdateResidentValue);
		CHECK_EQ(s.target_attr[37], 0x18u);
		CHECK_EQ(s.redo_op[38], LOP_DeleteAttribute);
		CHECK_EQ(s.target_attr[38], 0x18u);
		CHECK_EQ(s.redo_op[39], LOP_ClearBitsInNonresidentBitMap);
		CHECK_EQ(s.target_attr[39], 0x68u);
		CHECK_EQ(s.redo_op[40], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[44], LOP_ForgetTransaction);
		/* The ClearBits frees the four 16 KiB clusters. */
		CHECK_EQ(s.clear_bits_target_attr, 0x68u);
		CHECK_EQ(s.clear_bits_count, 4u);
		CHECK_EQ(s.clear_bits_lsn, s.lsn[39]);
	}

	/* Replay: the create's two records, the big-create SetBits,
	 * the replace SetBits and the remove ClearBits — the
	 * $Bitmap records are never MR_LSN-stamped. The sizes,
	 * pairs, CreateAttribute, DeleteAttribute and $SI records
	 * are skipped on the file record's stamp. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 5u);
	CHECK_EQ(an.mft_records_written, 1u);
	/* Two distinct dirty overlay clusters are written back:
	 * the MFT record cluster and the $Bitmap cluster. */
	CHECK_EQ(an.clusters_written, 2u);
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
	 * lives in $INDEX_ALLOCATION), so the walk is empty before
	 * the xattr — the same baseline the root-create test pins. */
	err = ntfs_create(root, "r.txt", 0100644, &f);
	CHECK_EQ(err, 0);
	if (err)
		goto out;
	CHECK_EQ(ntfs_volume_sync(vol), 0);
	CHECK_EQ(copy_file(orig, snap), 0);
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 0);

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

	/* The walk holds just the xattr transaction's 8 records. */
	set_ads_name("user.a");
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 8);
	if (s.n == 8) {
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


int main(void)
{
	tool_paths();
	ntfs_set_logger(quiet_logger, NULL);
	test_create_journaled();
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
	test_setattr_journaled();
	test_setattr_size_joins();
	test_setattr_in_root_unjournaled();
	test_xattr_create_journaled();
	test_xattr_replace_journaled();
	test_xattr_big_create_journaled();
	test_xattr_big_replace_remove_journaled();
	test_xattr_in_root_journaled();
	printf("test_journal: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
