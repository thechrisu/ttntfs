/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * mkdirtyseed.c - build a seed image with a DIRTY $LogFile for the fuzz
 * harness (GAPS.md §5.2).
 *
 *   mkdirtyseed -o OUT.img [--mkntfs PATH]
 *
 * The clean seeds (mkseeds.c) are built by libntfs-3g, which never
 * writes journal records: their $LogFile is empty, so the fuzzer's
 * mount+replay path replays nothing and mutations of the log area
 * never reach the replay parser. This generator mounts a fresh mkntfs
 * image with THIS driver, performs five journaled creates in a fresh
 * directory, syncs, and snapshots the image while it is still mounted
 * -- exactly the test_create_journaled cut point ("flushed metadata
 * plus the still-dirty log -- what a crash at this instant leaves").
 * The unmount afterwards clean-closes the ORIGINAL; the snapshot
 * keeps the dirty log: the first transaction checkpointed (the first
 * commit always checkpoints), the other four did not (the checkpoint
 * default is every 128), so a mount with NTFS_MOUNT_REPLAY_JOURNAL
 * must walk forward from the checkpoint and redo them.
 *
 * The creates go in a directory this driver just made, not the volume
 * root: the mkntfs root already has an $INDEX_ALLOCATION block and a
 * create there lands in the allocation block, which the journal
 * cannot describe; a fresh directory has no allocation block, so the
 * entries land in its $INDEX_ROOT and the creates journal (same
 * reason as in test_create_journaled).
 *
 * Shape-deterministic (same records, same layout on every run); bytes
 * vary in $SI timestamps, which come from the wall clock -- the corpus
 * is generated, not committed, so that is fine. The acceptance test
 * is tools/fuzz.sh check: the seed must mount and walk (replay
 * redoes the four uncheckpointed creates).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/kernel.h>
#include <ntfsport/bdev.h>
#include <ntfscore.h>

#define SEED_SIZE (4ULL * 1024ULL * 1024ULL)
#define N_CREATES 5

static const char *g_out = NULL;
static const char *g_mkntfs = NULL;

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "mkdirtyseed: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void quiet_sink(int level, const char *msg, void *ctx)
{
	(void)level;
	(void)msg;
	(void)ctx;
}

/* Build a fresh 4 MiB NTFS volume at @path (mirrors
 * test_journal.c:make_image, minus the 16 MiB size). */
static void make_image(const char *path)
{
	char cmd[2600];
	int rc;

	snprintf(cmd, sizeof(cmd),
		 "dd if=/dev/zero of='%s' bs=1048576 count=4 >/dev/null 2>&1 && "
		 "'%s' -Q -F -L FUZZDIRTY '%s' >/dev/null 2>&1",
		 path, g_mkntfs, path);
	rc = system(cmd);
	if (rc != 0)
		die("mkntfs failed for %s", path);
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
	if (out < 0) {
		close(in);
		return -errno;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) {
			close(in);
			close(out);
			return -EIO;
		}
	close(in);
	if (close(out) != 0)
		return -errno;
	return (n < 0) ? -errno : 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: mkdirtyseed -o OUT.img [--mkntfs PATH]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	char img[1024], tmp[1024];
	struct ntfs_mount_options mopts = {
		.flags = NTFS_MOUNT_RDONLY_FALLBACK,
		.uid = 0, .gid = 0, .fmask = 022, .dmask = 022,
	};
	struct ntfs_bdev *dev = NULL;
	ntfs_volume_t *vol = NULL;
	ntfs_inode_t *root = NULL, *d = NULL, *f = NULL;
	const char *td = getenv("TMPDIR");
	int err = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			g_out = argv[++i];
		else if (!strcmp(argv[i], "--mkntfs") && i + 1 < argc)
			g_mkntfs = argv[++i];
		else
			usage();
	}
	if (!g_out)
		usage();
	if (!g_mkntfs)
		g_mkntfs = "tools/.local/sbin/mkntfs";
	if (!td || !*td)
		td = "/tmp";
	snprintf(tmp, sizeof(tmp), "%s", td);
	if (tmp[0] && tmp[strlen(tmp) - 1] == '/')
		tmp[strlen(tmp) - 1] = '\0';
	/* Cap the scratch path so the snprintf below cannot truncate:
	 * the %.900s precision bounds tmp for -Wformat-truncation. */
	snprintf(img, sizeof(img), "%.900s/ttntfs-dirtyseed-%d.img", tmp, (int)getpid());

	platform_set_log_sink(quiet_sink, NULL);
	make_image(img);

	dev = ntfs_bdev_open_path(img, false);
	if (!dev)
		die("open %s: %s", img, strerror(errno));
	err = ntfs_mount(dev, &mopts, &vol);
	if (err)
		die("mount %s: %s", img, strerror(err));
	err = ntfs_volume_root(vol, &root);
	if (err)
		die("root inode: %s", strerror(err));

	err = ntfs_mkdir(root, "sub", 0100755, &d);
	if (err)
		die("mkdir sub: %s", strerror(err));
	for (i = 0; i < N_CREATES; i++) {
		char name[32];

		snprintf(name, sizeof(name), "j%d.txt", i + 1);
		err = ntfs_create(d, name, 0100644, &f);
		if (err)
			die("create %s: %s", name, strerror(err));
		ntfs_inode_put(f);
		f = NULL;
	}
	/* Sanity: the last file is visible on the live mount. */
	{
		ntfs_inode_t *g = NULL;

		err = ntfs_lookup(d, "j5.txt", &g);
		if (err)
			die("lookup j5.txt: %s", strerror(err));
		ntfs_inode_put(g);
	}

	/* Flush the metadata, then snapshot while still mounted: the
	 * log stays dirty (only the first commit checkpointed). */
	err = ntfs_volume_sync(vol);
	if (err)
		die("sync: %s", strerror(err));
	if (copy_file(img, g_out) != 0)
		die("copy %s -> %s: %s", img, g_out, strerror(errno));

	ntfs_inode_put(d);
	ntfs_inode_put(root);
	ntfs_unmount(vol);
	ntfs_bdev_close(dev);
	unlink(img);
	printf("seed-dirty-log: 4 MiB image, 5 journaled creates (1 checkpointed, 4 uncheckpointed)\n");
	return 0;
}
