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
	/* SetNewAttributeSizes: the $DATA size from redo/undo. */
	uint64_t sizes_redo_data, sizes_undo_data;
	uint64_t sizes_redo_total;
	uint64_t sizes_lsn;
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
	if (r->redo_op == LOP_SetBitsInNonresidentBitMap && r->redo && r->redo_len >= BR_SIZE) {
		const uint8_t *p = r->redo;
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
	printf("test_journal: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
