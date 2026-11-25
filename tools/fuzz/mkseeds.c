/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * mkseeds.c - build small NTFS seed images for the fuzz harness
 * (GAPS.md §5.2, tools/fuzz.sh).
 *
 *   ./mkseeds -o DIR [--mkntfs PATH] [--force]
 *
 * The fixture images in tools/images are 32-256 MiB: far too big for a
 * fuzz corpus (libFuzzer writes a whole input per execution, and the
 * mount would read a whole $MFT). Seeds are 4 MiB each and deliberately
 * dense: every structure the parser has to handle is present in a few
 * kilobytes, so a mutation lands in interesting bytes instead of empty
 * $Bitmap:
 *
 *   seed-basic.img (4K clusters)     dirs, nested dirs, text/binary
 *                                     files, native + Interix symlinks,
 *                                     ADS/xattrs, long + non-ASCII names,
 *                                     a hardlink, an empty file.
 *   seed-small-cluster.img (512B)     same shape at 512-byte clusters.
 *   seed-compressed.img (4K clusters) compressed dir with compressible
 *                                     and incompressible files, a sparse
 *                                     file, a plain file.
 *
 * Built with libntfs-3g (tools/.local), mirroring tools/mkfixtures.
 * Deterministic: fixed fill bytes and a fixed volume serial derived
 * from the seed name, so regenerating gives the same bytes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#define MiB (1024ULL * 1024ULL)
#define SEED_SIZE (4 * MiB)

static const char *g_outdir = "corpus";
static const char *g_mkntfs = NULL;
static bool g_force = false;

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "mkseeds: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static uint64_t fnv1a(const char *s, uint64_t h)
{
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 1099511628211ULL;
	}
	return h;
}

static int run(const char **argv)
{
	pid_t pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (!pid) {
		execv(argv[0], (char *const *)argv);
		perror(argv[0]);
		_exit(127);
	}
	int st;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int ucs(const char *utf8, ntfschar **out)
{
	int len = ntfs_mbstoucs(utf8, out);
	if (len < 0)
		die("ntfs_mbstoucs(%s)", utf8);
	if (len > 255)
		die("name too long (%d UTF-16 units): %s", len, utf8);
	return len;
}

static void split_path(const char *path, char *dir, size_t dirsz, const char **base)
{
	const char *slash = strrchr(path, '/');
	if (!slash)
		die("path must be absolute: %s", path);
	size_t n = (size_t)(slash - path);
	if (n == 0)
		n = 1;
	if (n + 1 > dirsz)
		die("path too long: %s", path);
	memcpy(dir, path, n);
	dir[n] = 0;
	*base = slash + 1;
}

struct seed {
	const char *name;
	uint32_t cluster;
	const char *label;
	char img[1024];
	ntfs_volume *vol;
	uint64_t files, dirs;
};

static void seed_begin(struct seed *s)
{
	char csz[32], lbl[64];
	snprintf(s->img, sizeof(s->img), "%s/%s.img", g_outdir, s->name);
	snprintf(csz, sizeof(csz), "%u", s->cluster);
	snprintf(lbl, sizeof(lbl), "%s", s->label);

	unlink(s->img);
	int fd = open(s->img, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, (off_t)SEED_SIZE))
		die("create %s", s->img);
	close(fd);

	const char *argv[] = { g_mkntfs, "-F", "-Q", "-q", "-c", csz, "-L", lbl, s->img, NULL };
	if (run(argv))
		die("%s failed for %s", g_mkntfs, s->name);

	/* Fixed serial, so regenerating gives the same bytes. */
	char serial[64], labelbin[1024];
	snprintf(serial, sizeof(serial), "--new-serial=%016" PRIx64,
		 fnv1a(s->name, 0x243f6a8885a308d3ULL));
	const char *slash = strrchr(g_mkntfs, '/');
	if (slash)
		snprintf(labelbin, sizeof(labelbin), "%.*s/ntfslabel", (int)(slash - g_mkntfs), g_mkntfs);
	else
		snprintf(labelbin, sizeof(labelbin), "ntfslabel");
	const char *argv2[] = { labelbin, serial, s->img, NULL };
	if (run(argv2))
		die("%s failed for %s", labelbin, s->name);

	s->vol = ntfs_mount(s->img, NTFS_MNT_NONE);
	if (!s->vol)
		die("ntfs_mount(%s)", s->img);
	NVolSetCompression(s->vol); /* allow compressed streams */
	s->files = s->dirs = 0;
}

static void seed_end(struct seed *s)
{
	if (s->vol && ntfs_umount(s->vol, FALSE))
		die("ntfs_umount(%s)", s->img);
	s->vol = NULL;
	struct stat st;
	if (stat(s->img, &st))
		die("stat %s", s->img);
	printf("%-24s %4" PRIu64 " MiB image, cluster %6u, %3" PRIu64 " files, %2" PRIu64 " dirs\n",
	       s->name, (uint64_t)(SEED_SIZE / MiB), s->cluster, s->files, s->dirs);
}

static ntfs_inode *seed_open(struct seed *s, const char *path)
{
	ntfs_inode *ni = ntfs_pathname_to_inode(s->vol, NULL, path);
	if (!ni)
		die("lookup %s", path);
	return ni;
}

static void seed_close(ntfs_inode *ni)
{
	if (ni && ntfs_inode_close(ni))
		die("ntfs_inode_close(%" PRIu64 ")", (uint64_t)ni->mft_no);
}

static ntfs_inode *seed_create(struct seed *s, const char *path, mode_t type)
{
	char dpath[1024];
	const char *base;
	split_path(path, dpath, sizeof(dpath), &base);
	ntfs_inode *dir = seed_open(s, dpath);
	ntfschar *uname = NULL;
	int ulen = ucs(base, &uname);
	ntfs_inode *ni = ntfs_create(dir, const_cpu_to_le32(0), uname, (u8)ulen, type);
	free(uname);
	if (!ni)
		die("ntfs_create(%s)", path);
	if (S_ISDIR(type))
		s->dirs++;
	else
		s->files++;
	seed_close(dir);
	return ni;
}

static void seed_mkdir(struct seed *s, const char *path)
{
	seed_close(seed_create(s, path, S_IFDIR));
}

/* Deterministic fill: LCG, fixed seed per call. */
static void fill_buf(uint8_t *buf, size_t len, uint64_t state, bool text)
{
	static const char words[] = "the quick brown fox jumps over the lazy dog 0123456789\n";
	for (size_t i = 0; i < len; i++) {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		if (text)
			buf[i] = (uint8_t)words[(state >> 33) % (sizeof(words) - 1)];
		else
			buf[i] = (uint8_t)(state >> 32);
	}
}

static void seed_write(struct seed *s, ntfs_inode *ni, const char *stream,
		       uint64_t off, uint64_t len, bool text)
{
	ntfschar *uname = AT_UNNAMED, *alloc = NULL;
	int ulen = 0;
	if (stream) {
		ulen = ucs(stream, &alloc);
		uname = alloc;
		if (!ntfs_attr_exist(ni, AT_DATA, uname, ulen) &&
		    ntfs_attr_add(ni, AT_DATA, uname, (u8)ulen, NULL, 0))
			die("ntfs_attr_add(%s)", stream);
	}
	/* ntfs_attr_open() takes ownership of a non-constant name. */
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, uname, ulen);
	if (!na)
		die("ntfs_attr_open");
	uint8_t buf[8192];
	uint64_t pos = 0;
	while (pos < len) {
		size_t chunk = len - pos < sizeof(buf) ? (size_t)(len - pos) : sizeof(buf);
		fill_buf(buf, chunk, 0x9e3779b97f4a7c15ULL + off + pos, text);
		s64 w = ntfs_attr_pwrite(na, off + pos, chunk, buf);
		if (w != (s64)chunk)
			die("ntfs_attr_pwrite");
		pos += chunk;
	}
	ntfs_attr_close(na);
}

static void seed_file(struct seed *s, const char *path, uint64_t len, bool text)
{
	ntfs_inode *ni = seed_create(s, path, S_IFREG);
	if (len)
		seed_write(s, ni, NULL, 0, len, text);
	seed_close(ni);
}

static void seed_symlink(struct seed *s, const char *path, const char *target)
{
	char dpath[1024];
	const char *base;
	split_path(path, dpath, sizeof(dpath), &base);
	ntfs_inode *dir = seed_open(s, dpath);
	ntfschar *uname = NULL, *tname = NULL;
	int ulen = ucs(base, &uname);
	int tlen = ntfs_mbstoucs(target, &tname);
	if (tlen < 0)
		die("ntfs_mbstoucs(%s)", target);
	ntfs_inode *ni = ntfs_create_symlink(dir, const_cpu_to_le32(0), uname, (u8)ulen,
					      tname, tlen);
	free(uname);
	free(tname);
	if (!ni)
		die("ntfs_create_symlink(%s)", path);
	s->files++;
	seed_close(ni);
	seed_close(dir);
}

/* Interix symlink: a regular file whose $DATA is the IntxLNK magic plus a
 * UTF-16LE target, with FILE_ATTR_SYSTEM set -- the format ntfs-3g writes
 * and §2.1 now reads. */
static void seed_interix(struct seed *s, const char *path, const char *target)
{
	ntfs_inode *ni = seed_create(s, path, S_IFREG);
	size_t tlen = strlen(target);
	uint8_t *buf = malloc(8 + 2 * tlen);
	if (!buf)
		die("malloc");
	memcpy(buf, "IntxLNK\x01", 8);
	for (size_t i = 0; i < tlen; i++) {
		buf[8 + 2 * i] = (uint8_t)target[i];
		buf[8 + 2 * i + 1] = 0;
	}
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		die("ntfs_attr_open");
	if (ntfs_attr_pwrite(na, 0, 8 + 2 * tlen, buf) != (s64)(8 + 2 * tlen))
		die("ntfs_attr_pwrite");
	ntfs_attr_close(na);
	free(buf);
	le32 attrib = ni->flags | FILE_ATTR_SYSTEM;
	if (ntfs_set_ntfs_attrib(ni, (const char *)&attrib, sizeof(attrib), 0))
		die("ntfs_set_ntfs_attrib(SYSTEM)");
	seed_close(ni);
}

static void seed_compressed_dir(struct seed *s, const char *path)
{
	ntfs_inode *dir = seed_create(s, path, S_IFDIR);
	le32 attrib = dir->flags | FILE_ATTR_COMPRESSED;
	if (ntfs_set_ntfs_attrib(dir, (const char *)&attrib, sizeof(attrib), 0))
		die("ntfs_set_ntfs_attrib(compressed dir)");
	seed_close(dir);
}

static void seed_sparse(struct seed *s, const char *path, uint64_t total,
			uint64_t at, uint64_t len)
{
	ntfs_inode *ni = seed_create(s, path, S_IFREG);
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		die("ntfs_attr_open");
	if (ntfs_attr_truncate(na, (s64)total))
		die("ntfs_attr_truncate");
	ntfs_attr_close(na);
	seed_write(s, ni, NULL, at, len, false);
	seed_close(ni);
}

/* ---- the three seeds ----------------------------------------------------- */

static void build_basic(struct seed *s)
{
	seed_mkdir(s, "/docs");
	seed_mkdir(s, "/docs/nested");
	seed_mkdir(s, "/docs/nested/deep");
	seed_mkdir(s, "/bin");
	seed_file(s, "/readme.txt", 512, true);
	seed_file(s, "/docs/notes.txt", 4096, true);
	seed_file(s, "/docs/nested/deep/data.bin", 100 * 1024, false);
	seed_file(s, "/bin/tool", 64 * 1024, true);
	seed_file(s, "/empty", 0, true);
	seed_symlink(s, "/link-to-readme", "/readme.txt");
	seed_interix(s, "/link-intx", "/docs/notes.txt");
	/* ADS / xattrs. */
	ntfs_inode *ni = seed_open(s, "/readme.txt");
	seed_write(s, ni, "com.apple.FinderInfo", 0, 32, false);
	seed_close(ni);
	ni = seed_open(s, "/docs/notes.txt");
	seed_write(s, ni, "zone.identifier", 0, 64, true);
	seed_close(ni);
	/* Long name, non-ASCII name, hardlink. */
	char longname[220];
	memset(longname, 'a', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = 0;
	char path[300];
	snprintf(path, sizeof(path), "/%s.txt", longname);
	seed_file(s, path, 128, true);
	seed_file(s, "/caf\xc3\xa9-\xce\xbb.txt", 256, true);
	ni = seed_open(s, "/readme.txt");
	ntfschar *uname = NULL;
	int ulen = ucs("readme-alias", &uname);
	ntfs_inode *root = seed_open(s, "/");
	if (ntfs_link(ni, root, uname, (u8)ulen))
		die("ntfs_link");
	free(uname);
	seed_close(ni);
	seed_close(root);
}

static void build_small_cluster(struct seed *s)
{
	seed_mkdir(s, "/docs");
	seed_file(s, "/a.txt", 2048, true);
	seed_file(s, "/docs/b.bin", 64 * 1024, false);
	seed_file(s, "/docs/empty", 0, true);
	seed_symlink(s, "/link-a", "/a.txt");
	ntfs_inode *ni = seed_open(s, "/a.txt");
	seed_write(s, ni, "com.apple.FinderInfo", 0, 32, false);
	seed_close(ni);
}

static void build_compressed(struct seed *s)
{
	seed_compressed_dir(s, "/packed");
	seed_file(s, "/packed/big.txt", 100 * 1024, true);   /* compresses well */
	seed_file(s, "/packed/rand.bin", 48 * 1024, false);  /* stored raw */
	seed_sparse(s, "/sparse.bin", 256 * 1024, 128 * 1024, 4096);
	seed_file(s, "/plain.txt", 1024, true);
}

static const struct {
	const char *name;
	uint32_t cluster;
	const char *label;
	void (*build)(struct seed *);
} seeds[] = {
	{ "seed-basic", 4096, "FUZZBASIC", build_basic },
	{ "seed-small-cluster", 512, "FUZZ512", build_small_cluster },
	{ "seed-compressed", 4096, "FUZZCOMP", build_compressed },
};

static void usage(void)
{
	fprintf(stderr, "usage: mkseeds [-o OUTDIR] [--mkntfs PATH] [--force]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	g_mkntfs = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			g_outdir = argv[++i];
		else if (!strcmp(argv[i], "--mkntfs") && i + 1 < argc)
			g_mkntfs = argv[++i];
		else if (!strcmp(argv[i], "--force"))
			g_force = true;
		else
			usage();
	}
	if (!g_mkntfs) {
		/* Default: the built ntfsprogs, next to this binary's tree
		 * (tools/fuzz/mkseeds -> tools/.local/sbin/mkntfs). The
		 * Makefile always passes --mkntfs explicitly; this is only
		 * for hand runs. /proc/self/exe is Linux-only. */
#ifdef __linux__
		static char dflt[1024];
		char exe[1024];
		ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		if (n < 0)
			die("readlink /proc/self/exe");
		exe[n] = 0;
		char *slash = strrchr(exe, '/');
		if (!slash)
			die("no directory in exe path");
		snprintf(dflt, sizeof(dflt), "%.*s/../.local/sbin/mkntfs",
			 (int)(slash - exe), exe);
		g_mkntfs = dflt;
#else
		die("pass --mkntfs PATH (no default on this platform)");
#endif
	}
	if (mkdir(g_outdir, 0755) && errno != EEXIST)
		die("mkdir %s", g_outdir);

	for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		char img[1024];
		snprintf(img, sizeof(img), "%s/%s.img", g_outdir, seeds[i].name);
		if (!g_force && access(img, R_OK) == 0) {
			printf("%-24s exists (use --force to regenerate)\n", seeds[i].name);
			continue;
		}
		struct seed s = { .name = seeds[i].name, .cluster = seeds[i].cluster,
				  .label = seeds[i].label };
		seed_begin(&s);
		seeds[i].build(&s);
		seed_end(&s);
	}
	return 0;
}
