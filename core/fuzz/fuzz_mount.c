/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * fuzz_mount.c - libFuzzer target for the on-disk NTFS parser (GAPS.md §5.2).
 *
 * NTFS-3G's 2026 CVEs were crafted-image heap overflows in its parser; the
 * same class of risk applies to any NTFS parser, including this one. This
 * target feeds mutated image bytes to the driver's mount + walk path and
 * lets AddressSanitizer / UBSan catch what the parser gets wrong:
 *
 *   - boot sector, $MFT record, attribute, runlist and $LogFile replay
 *     parsing (the mount is read-write with NTFS_MOUNT_REPLAY_JOURNAL,
 *     so the replay path is exercised too, on a throwaway copy),
 *   - directory index parsing (recursive readdir),
 *   - symlink / reparse-point parsing (readlink),
 *   - alternate-data-stream / xattr parsing (listxattr + getxattr),
 *   - compressed / sparse data reads (bounded reads at two offsets).
 *
 * It is deliberately read-path only: the write path needs a valid image
 * plus a valid operation sequence, which is a second harness. Mounting is
 * on a temp copy, so a mutated image can never touch a real volume.
 *
 * Bounds (so one input cannot hang the fuzzer or grow without limit):
 *   - inputs over 4 MiB are rejected outright (run with -max_len=4194304);
 *   - the walk is depth <= 4, <= 256 entries total, <= 16 KiB read per
 *     file, first xattr stream only, <= 4 KiB read from it.
 *
 * Two build modes, same source:
 *   - libFuzzer:  clang -fsanitize=fuzzer,address,undefined
 *                  (CMake option -DNTFS_FUZZ=ON; Clang only).
 *   - standalone: -DNTFS_FUZZ_STANDALONE, plain main() that mounts and
 *                  walks each file on the command line once. That is the
 *                  deterministic corpus check tools/fuzz.sh runs, and what
 *                  pre-push runs; sanitizer aborts still fire on a crash.
 *                  --expect-fail inverts the verdict: it exits 0 iff
 *                  every file FAILS to mount, for the regression images
 *                  (tools/fuzz/mkregression.py), whose whole point is a
 *                  mount that must fail cleanly -- no crash, no leak.
 *                  (The leak half is only visible under ASan+LSan, so
 *                  the regression images also run through the libFuzzer
 *                  binary in CI; see tools/fuzz.sh regression.)
 *
 * Read-only variant (stream E, GAPS.md §5.2): -DNTFS_FUZZ_READONLY
 * builds the same source as a second target (ntfs_fuzz_ro /
 * ntfs_fuzz_standalone_ro) that opens the image read-only and mounts
 * with NTFS_MOUNT_RDONLY and no replay. That reaches the driver's own
 * geometry parse (ntfs_fill_super / parse_ntfs_boot_sector) without
 * $LogFile replay running first -- the path the boot-cpr regression
 * came through, and one the RW target never exercises.
 *
 * The driver's log goes to a no-op sink here: a corrupted image can log
 * on nearly every operation, and under libFuzzer that would drown the
 * output. (NTFS_LOG_LEVEL would do the same, but a sink is deterministic
 * and does not depend on the environment.)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/kernel.h>
#include <ntfsport/bdev.h>
#include <ntfscore.h>

#define FUZZ_MAX_INPUT (4u * 1024u * 1024u) /* 4 MiB */
#define FUZZ_MAX_DEPTH 4
#define FUZZ_MAX_ENTRIES 256
#define FUZZ_MAX_READ (16u * 1024u) /* per file, per offset */
#define FUZZ_MAX_XATTR_READ (4u * 1024u)
#define FUZZ_MAX_NAME 1024 /* NTFS names are <= 255 UTF-16 units */

static void quiet_sink(int level, const char *msg, void *ctx)
{
	(void)level;
	(void)msg;
	(void)ctx;
}

static void fuzz_init_once(void)
{
	static int done;
	if (!done) {
		platform_set_log_sink(quiet_sink, NULL);
		done = 1;
	}
}

/* ---- walk -------------------------------------------------------------- */

struct walk_ctx {
	ntfs_volume_t *vol;
	unsigned entries;
	bool stop;
};

struct dent {
	char name[FUZZ_MAX_NAME];
	uint64_t ino;
	int type;
	bool has_attr;
	struct ntfs_attr attr;
};

struct dlist {
	struct dent *v;
	size_t n, cap;
};

static int collect_cb(const struct ntfs_dirent *e, void *ctx)
{
	struct dlist *l = ctx;
	if (l->n == l->cap) {
		size_t ncap = l->cap ? l->cap * 2 : 32;
		struct dent *nv = realloc(l->v, ncap * sizeof(*nv));
		if (!nv)
			return -ENOMEM;
		l->v = nv;
		l->cap = ncap;
	}
	struct dent *d = &l->v[l->n];
	if (e->name_len >= FUZZ_MAX_NAME)
		return 0; /* corrupt length claim: skip, do not truncate */
	memcpy(d->name, e->name, e->name_len);
	d->name[e->name_len] = '\0';
	d->ino = e->inode_no;
	d->type = e->type;
	d->has_attr = e->has_attr;
	if (e->has_attr)
		d->attr = e->attr;
	l->n++;
	return 0;
}

static void read_some(ntfs_inode_t *ni, uint64_t size)
{
	static uint8_t buf[FUZZ_MAX_READ];
	uint64_t offs[2] = { 0, size / 2 };
	for (size_t i = 0; i < 2; i++) {
		uint64_t off = offs[i];
		if (off >= size)
			continue;
		size_t want = size - off < FUZZ_MAX_READ ? (size_t)(size - off) : FUZZ_MAX_READ;
		(void)ntfs_read(ni, buf, want, off);
	}
}

static void read_xattrs(ntfs_inode_t *ni)
{
	size_t len = 0;
	if (ntfs_listxattr(ni, NULL, 0, &len) || !len || len > FUZZ_MAX_XATTR_READ)
		return;
	char *names = malloc(len);
	if (!names)
		return;
	if (ntfs_listxattr(ni, names, len, &len) == 0 && len > 0) {
		/* First stream only: the point is the ADS parser, not the data. */
		size_t vlen = 0;
		if (ntfs_getxattr(ni, names, NULL, 0, &vlen) == 0 && vlen > 0) {
			if (vlen > FUZZ_MAX_XATTR_READ)
				vlen = FUZZ_MAX_XATTR_READ;
			void *vbuf = malloc(vlen);
			if (vbuf) {
				(void)ntfs_getxattr(ni, names, vbuf, vlen, &vlen);
				free(vbuf);
			}
		}
	}
	free(names);
}

static void walk_dir(struct walk_ctx *w, ntfs_inode_t *dir, unsigned depth)
{
	if (w->stop || depth > FUZZ_MAX_DEPTH)
		return;
	struct dlist l = { 0 };
	uint64_t cookie = 0;
	bool eof = false;
	while (!eof) {
		if (ntfs_readdir(dir, &cookie, true, collect_cb, &l, &eof))
			break;
		if (l.n >= FUZZ_MAX_ENTRIES) {
			eof = true;
			break;
		}
	}
	for (size_t i = 0; i < l.n && !w->stop; i++) {
		struct dent *d = &l.v[i];
		if (!strcmp(d->name, ".") || !strcmp(d->name, ".."))
			continue;
		if (++w->entries > FUZZ_MAX_ENTRIES) {
			w->stop = true;
			break;
		}
		ntfs_inode_t *ni = NULL;
		if (ntfs_lookup(dir, d->name, &ni))
			continue;
		if (d->type == NTFS_ITEM_DIR && depth < FUZZ_MAX_DEPTH) {
			walk_dir(w, ni, depth + 1);
		} else if (d->type == NTFS_ITEM_SYMLINK) {
			char target[4096];
			size_t tlen = 0;
			(void)ntfs_readlink(ni, target, sizeof(target), &tlen);
		} else if (d->type == NTFS_ITEM_FILE) {
			uint64_t size = d->has_attr ? d->attr.size : 0;
			if (size)
				read_some(ni, size);
		}
		if (d->has_attr && d->attr.has_ads)
			read_xattrs(ni);
		ntfs_inode_put(ni);
	}
	free(l.v);
}

/* ---- one input --------------------------------------------------------- */

/* Returns 1 if the image mounted and was walked, 0 if the input was
 * rejected or the mount failed. (A mount failure is not an error for
 * the fuzzer entry -- most mutations break the boot sector -- but the
 * standalone runner reports it, so a seed that never mounts cannot
 * pass the corpus check silently.) */
static int fuzz_one(const uint8_t *data, size_t size)
{
	if (size < 512 || size > FUZZ_MAX_INPUT)
		return 0;
	fuzz_init_once();

	const char *tmpdir = getenv("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	char path[4096];
	/* mkstemp requires the template to END in XXXXXX; a suffix after
	 * them (".img") makes it fail with EINVAL. That exact bug shipped
	 * in the first version of this file: every input returned before
	 * mounting, and the standalone runner printed "ok" anyway. */
	if (snprintf(path, sizeof(path), "%s/ntfs-fuzz-XXXXXX", tmpdir) >= (int)sizeof(path))
		return 0;
	int fd = mkstemp(path);
	if (fd < 0)
		return 0;
	size_t off = 0;
	while (off < size) {
		ssize_t n = write(fd, data + off, size - off);
		if (n <= 0)
			break;
		off += (size_t)n;
	}
	close(fd);
	if (off != size) {
		unlink(path);
		return 0;
	}

	int mounted = 0;
#ifdef NTFS_FUZZ_READONLY
	/* Read-only variant: no replay, so the driver's own geometry
	 * parse is reached without $LogFile replay running first. */
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);
#else
	struct ntfs_bdev *dev = ntfs_bdev_open_path(path, false);
#endif
	if (!dev)
		goto out_unlink;
	{
		struct ntfs_mount_options o = {
#ifdef NTFS_FUZZ_READONLY
			.flags = NTFS_MOUNT_RDONLY,
#else
			.flags = NTFS_MOUNT_REPLAY_JOURNAL,
#endif
			.uid = 0, .gid = 0, .fmask = 022, .dmask = 022,
		};
		ntfs_volume_t *vol = NULL;
		if (ntfs_mount(dev, &o, &vol)) {
			ntfs_bdev_close(dev);
			goto out_unlink;
		}
		mounted = 1;
		ntfs_inode_t *root = NULL;
		if (ntfs_volume_root(vol, &root) == 0) {
			struct walk_ctx w = { .vol = vol, .entries = 0, .stop = false };
			walk_dir(&w, root, 0);
			ntfs_inode_put(root);
		}
		(void)ntfs_unmount(vol);
		ntfs_bdev_close(dev);
	}
out_unlink:
	unlink(path);
	return mounted;
}

/* ---- entries ----------------------------------------------------------- */

#ifdef NTFS_FUZZ_STANDALONE
int main(int argc, char **argv)
{
	fuzz_init_once();
	/* --expect-fail: the regression images (tools/fuzz/mkregression.py)
	 * are volumes that must fail to mount. Exit 0 iff every file fails;
	 * a mount that succeeds means the corruption no longer reaches the
	 * intended failure path, and the regression is vacuous. */
	int expect_fail = 0;
	int arg0 = 1;
	if (argc > 1 && strcmp(argv[1], "--expect-fail") == 0) {
		expect_fail = 1;
		arg0 = 2;
	}
	if (argc < arg0 + 1) {
		fprintf(stderr, "usage: %s [--expect-fail] IMAGE...\n", argv[0]);
		return 2;
	}
	int failed = 0;
	for (int i = arg0; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) {
			fprintf(stderr, "%s: cannot open: %s\n", argv[i], strerror(errno));
			return 1;
		}
		if (fseek(f, 0, SEEK_END) != 0) {
			fclose(f);
			return 1;
		}
		long n = ftell(f);
		if (n < 0) {
			fclose(f);
			return 1;
		}
		rewind(f);
		uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
		if (!buf) {
			fclose(f);
			return 1;
		}
		size_t got = fread(buf, 1, (size_t)n, f);
		fclose(f);
		if (got != (size_t)n) {
			free(buf);
			return 1;
		}
		int mounted = fuzz_one(buf, (size_t)n);
		free(buf);
		if (mounted) {
			if (expect_fail) {
				printf("%s: mounted (expected failure)\n", argv[i]);
				failed = 1;
			} else {
				printf("%s: mounted\n", argv[i]);
			}
		} else {
			if (expect_fail)
				printf("%s: MOUNT FAILED (expected)\n", argv[i]);
			else {
				printf("%s: MOUNT FAILED\n", argv[i]);
				failed = 1;
			}
		}
	}
	return failed ? 1 : 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	(void)fuzz_one(data, size);
	return 0;
}
#endif
