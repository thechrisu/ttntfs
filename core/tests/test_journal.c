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
};

static int collect_rec(const struct ntfs_log_record *r, void *ctx)
{
	struct rec_seen *s = ctx;
	static const uint8_t name_utf16[] = { 'j', 0, '.', 0, 't', 0, 'x', 0, 't', 0 };
	size_t i;

	if (r->record_type != LFS_RECORD_TYPE_CLIENT || s->n >= 64)
		return 0;
	s->lsn[s->n] = r->lsn;
	s->redo_op[s->n] = r->redo_op;
	s->target_attr[s->n] = r->target_attr;
	s->target_vcn[s->n] = r->target_vcn;
	s->record_off[s->n] = r->record_off;
	if (r->redo_op == LOP_CreateAttribute && r->redo && r->redo_len >= sizeof(name_utf16)) {
		const uint8_t *p = r->redo;
		for (i = 0; i + sizeof(name_utf16) <= r->redo_len; i++) {
			if (!memcmp(p + i, name_utf16, sizeof(name_utf16))) {
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
	 * Forget. The oldest LSN is the dirty-page table's oldest
	 * entry, the Initialize record's LSN.
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

	/* The walk: exactly one transaction, 2 open + 8 data + Commit +
	 * 3 dumps + Forget, in the order the create staged them. */
	CHECK_EQ(walk_log(snap, &s), 0);
	CHECK_EQ(s.n, 15);
	if (s.n == 15) {
		static const uint16_t want_op[8] = {
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
		for (i = 0; i < 8; i++) {
			CHECK_EQ(s.redo_op[i + 2], want_op[i]);
			/* First seven target $MFT:$DATA (0x18); the
			 * SetBits targets $MFT:$BITMAP (0x40). */
			CHECK_EQ(s.target_attr[i + 2], i < 7 ? 0x18u : 0x40u);
		}
		CHECK_EQ(s.redo_op[10], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[11], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[12], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[13], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[14], LOP_ForgetTransaction);
		/* The $FILE_NAME create is the last CreateAttribute, the
		 * record right before the index entry. */
		lsn_fn = s.lsn[7];
		lsn_index = s.lsn[8];
		CHECK_EQ(s.redo_op[7], LOP_CreateAttribute);
		CHECK_EQ(s.redo_op[8], LOP_AddIndexEntryRoot);
	}
	/* The name appears in exactly one CreateAttribute's redo: the
	 * $FILE_NAME one, and it is the record before the index entry. */
	CHECK_EQ(s.fn_name_hits, 1);
	CHECK_EQ(s.fn_lsn, lsn_fn);
	/* The SetBits names the new record number, one bit. */
	CHECK_EQ(s.bits_bit, (uint32_t)ino);
	CHECK_EQ(s.bits_count, 1u);
	CHECK_EQ(s.bits_lsn, s.lsn[9]);
	/* The SetBits targets the bitmap's first cluster. */
	CHECK_EQ(s.target_vcn[9], 0u);

	/* Both touched records carry an MR_LSN at or past the record
	 * that touched them: the replay's skip check. */
	CHECK_EQ(read_mft_lsn(snap, ino, &mrlsn_file), 0);
	CHECK(mrlsn_file != 0);
	CHECK(mrlsn_file >= lsn_fn);
	CHECK_EQ(read_mft_lsn(snap, sub_ino, &mrlsn_dir), 0);
	CHECK(mrlsn_dir != 0);
	CHECK(mrlsn_dir >= lsn_index);

	/* Replay copy 1: the MFT records were already flushed, so only
	 * the SetBits — which carries no LSN to skip on — redoes. */
	CHECK_EQ(copy_file(snap, snap1), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap1, &an), 0);
	CHECK_EQ(an.records_redone, 1u);
	CHECK_EQ(an.mft_records_written, 0u);
	CHECK_EQ(an.clusters_written, 1u);
	CHECK_EQ(an.transactions_committed, 0u);
	CHECK_EQ(an.transactions_active, 0u);

	/* The file is present on the replayed volume, and it is clean. */
	CHECK_EQ(lookup_nested_after_mount(snap1, "sub", "j.txt"), 0);
	CHECK_EQ(run_ntfsck_clean(snap1), 0);

	/* Replay copy 2: the same dirty snapshot replays to the same
	 * single record. Replay is idempotent. */
	CHECK_EQ(copy_file(snap, snap2), 0);
	memset(&an, 0, sizeof(an));
	CHECK_EQ(replay_path(snap2, &an), 0);
	CHECK_EQ(an.records_redone, 1u);
	CHECK_EQ(an.mft_records_written, 0u);
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

/* ---- 3. a create in the root falls back to unjournaled -------------- */

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

int main(void)
{
	tool_paths();
	ntfs_set_logger(quiet_logger, NULL);
	test_create_journaled();
	test_mkdir_unjournaled();
	test_create_in_root_unjournaled();
	printf("test_journal: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
