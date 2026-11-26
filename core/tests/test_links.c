// SPDX-License-Identifier: GPL-2.0
/*
 * test_links.c - hard links, symlinks, timestamps and permissions.
 *
 * All four were implemented and none was exercised by anything. What each
 * group is here to catch:
 *
 *   hard links   A link count that is one too high leaves the MFT record and
 *                its clusters allocated after the last name is gone: a silent
 *                leak. One too low frees a record another name still points
 *                at: silent data loss, and the loss surfaces later, on a
 *                different file, when the record is reused. Neither shows up
 *                in a read-back of the file just touched, so every mutating
 *                case here ends in ntfsck.
 *   symlinks     Written as WSL reparse points (IO_REPARSE_TAG_LX_SYMLINK).
 *                The target is the only copy of the information; if readlink
 *                loses or truncates it the link is unrecoverable, and nothing
 *                else on the volume looks wrong.
 *   timestamps   NTFS counts 100 ns ticks from 1601 and Unix counts seconds
 *                from 1970. A conversion that is off by the epoch constant
 *                puts every file 369 years away, and a conversion that is off
 *                by a factor of ten is not obviously wrong on a screen.
 *   permissions  noowners: the mode the caller sees comes from the mount
 *                options, not from the volume. If fmask/dmask stop being
 *                applied, files quietly become world-writable.
 *
 * Verified against the implementation before being asserted: core/vfs/namei.c
 * (__ntfs_link, ntfs_delete, __ntfs_create), core/vfs/api.c (mode_of,
 * fill_attr, ntfs_setattr, ntfs_symlink, ntfs_readlink), core/vfs/file.c
 * (ntfs_vfs_setattr), core/ntfs/reparse.c and core/ntfs/time.h. Where the port
 * refuses something, or does something surprising, the comment says so.
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
#include "ntfs_image.h"
/*
 * The conversion itself, so the known values below are checked directly and
 * not only through a mount. Included by relative path on purpose: core/ntfs is
 * NOT on this target's include path, because a time.h there would shadow the
 * system <time.h> that linux/time64.h needs.
 */
#include <linux/types.h>
#include <linux/xattr.h>
#include <asm/byteorder.h>
#include <linux/math64.h>
#include "../ntfs/time.h"

/* $EA test hook: the VFS xattr handler table is exported from ea.c
 * (see ea.h); the test drives handlers[0]->set/get directly because
 * the public ntfs_setxattr() stores xattrs as ADS and never reaches
 * ntfs_set_ea(). Declared here rather than including ea.h, whose
 * internal headers collide with ntfscore.h. */
extern const struct xattr_handler *const ntfs_xattr_handlers[];

/* Scratch images go under $TMPDIR when set: /tmp is a 512 MiB tmpfs on
 * some machines and the 16 TiB geometry image needs ~600 MiB while
 * mkntfs materialises $Bitmap. */
static const char *scratch_dir(void)
{
	const char *t = getenv("TMPDIR");
	return t && *t ? t : "/tmp";
}


static int failures, checks;
#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define CHECK_MSG(cond, ...) do {					\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

static const char *images_dir(void)
{
	const char *d = getenv("NTFS_IMAGES");
	return d && *d ? d : "tools/images";
}

/* ---- a scratch copy of a fixture ------------------------------------- */

/*
 * Each test gets its own copy under its own name: the fixtures are shared with
 * every other test and a repair run, or a half-written image, would be a
 * measurement of the repair rather than of the driver.
 */
static char scratch[1024];

static int copy_fixture(const char *tag)
{
	char src[1024];
	int in, out;
	char buf[1 << 16];
	ssize_t n;

	snprintf(src, sizeof(src), "%s/basic-4k.img", images_dir());
	snprintf(scratch, sizeof(scratch), "%s/ttntfs-links-%d-%s.img", scratch_dir(),
		 (int)getpid(), tag);
	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(scratch, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { close(in); return -1; }
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) { close(in); close(out); return -1; }
	close(in);
	close(out);
	return 0;
}

/*
 * ntfsck catches what a read-back cannot: a link count that disagrees with the
 * number of FILE_NAME attributes, an index entry with no attribute behind it,
 * an MFT record still marked in use with nothing referencing it. Always -n:
 * repairing the image under test would measure the repair.
 */
static void fsck_clean(const char *what)
{
	const char *ck = getenv("NTFS_NTFSCK");
	char cmd[2048];
	int rc;

	if (!ck || !*ck || access(ck, X_OK) != 0) {
		fprintf(stderr, "SKIP fsck(%s): NTFS_NTFSCK not usable\n", what);
		return;
	}
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >/dev/null 2>&1", ck, scratch);
	rc = system(cmd);
	checks++;
	if (rc != 0) {
		failures++;
		fprintf(stderr, "FAIL ntfsck -n after %s: exit %d (image left at %s)\n",
			what, rc, scratch);
		/* Show what it found; the image is about to be unlinked. */
		snprintf(cmd, sizeof(cmd), "'%s' -n '%s' 2>&1 | tail -20 1>&2", ck, scratch);
		rc = system(cmd);
		(void)rc;
	}
}

/* ---- mount / unmount ------------------------------------------------- */

struct vol {
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	ntfs_inode_t *root;
};

static int vol_up_flags(struct vol *v, uint32_t flags, uint16_t fmask, uint16_t dmask,
			uid_t uid, gid_t gid);

static int vol_up(struct vol *v, uint16_t fmask, uint16_t dmask,
		  uid_t uid, gid_t gid)
{
	return vol_up_flags(v, 0, fmask, dmask, uid, gid);
}

static int vol_up_flags(struct vol *v, uint32_t flags, uint16_t fmask, uint16_t dmask,
			uid_t uid, gid_t gid)
{
	struct ntfs_mount_options o = { .flags = flags, .uid = uid, .gid = gid,
					.fmask = fmask, .dmask = dmask };
	int err;

	memset(v, 0, sizeof(*v));
	v->dev = ntfs_bdev_open_path(scratch, false);
	if (!v->dev)
		return -ENOENT;
	err = ntfs_mount(v->dev, &o, &v->vol);
	if (err) {
		ntfs_bdev_close(v->dev);
		v->dev = NULL;
		return err;
	}
	err = ntfs_volume_root(v->vol, &v->root);
	if (err) {
		ntfs_unmount(v->vol);
		ntfs_bdev_close(v->dev);
		v->dev = NULL;
		return err;
	}
	return 0;
}

static void vol_down(struct vol *v)
{
	if (!v->dev)
		return;
	ntfs_inode_put(v->root);
	ntfs_unmount(v->vol);
	ntfs_bdev_close(v->dev);
	v->dev = NULL;
}

/* ---- 1. hard links: the count, the shared inode, the shared data ----- */

/*
 * The whole point of a hard link is that the two names are one file. If they
 * ever stop sharing an inode number the caller's inode cache splits in two and
 * a write through one name is invisible through the other.
 */
static void test_hard_links(void)
{
	struct vol v;
	ntfs_inode_t *a = NULL, *b = NULL;
	struct ntfs_attr aa, ab;
	char buf[32];
	uint64_t ino;

	if (copy_fixture("hardlinks") != 0) {
		fprintf(stderr, "SKIP test_hard_links: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_hard_links: mount failed\n");
		unlink(scratch);
		return;
	}

	CHECK(ntfs_create(v.root, "one.txt", 0100644, &a) == 0);
	if (!a) goto out;
	CHECK(ntfs_write(a, "hello", 5, 0) == 5);
	CHECK(ntfs_getattr(a, &aa) == 0);
	CHECK(aa.nlink == 1);
	ino = aa.inode_no;

	/* Second name. */
	CHECK(ntfs_link(a, v.root, "two.txt") == 0);
	CHECK(ntfs_getattr(a, &aa) == 0);
	CHECK_MSG(aa.nlink == 2, "nlink after link is %u, expected 2", aa.nlink);

	CHECK(ntfs_lookup(v.root, "two.txt", &b) == 0);
	if (b) {
		CHECK(ntfs_getattr(b, &ab) == 0);
		CHECK_MSG(ab.inode_no == ino,
			  "the two names are different inodes: %llu vs %llu",
			  (unsigned long long)ab.inode_no, (unsigned long long)ino);
		CHECK(ab.nlink == 2);
		CHECK(ab.size == 5);
		memset(buf, 0, sizeof(buf));
		CHECK(ntfs_read(b, buf, 5, 0) == 5);
		CHECK(memcmp(buf, "hello", 5) == 0);

		/* A write through the new name must be visible through the old. */
		CHECK(ntfs_write(b, "HELLO", 5, 0) == 5);
		memset(buf, 0, sizeof(buf));
		CHECK(ntfs_read(a, buf, 5, 0) == 5);
		CHECK_MSG(memcmp(buf, "HELLO", 5) == 0,
			  "the two names do not share data: read '%.5s'", buf);
		ntfs_inode_put(b);
		b = NULL;
	}

	/* Drop the first name. The file must still be there under the second. */
	CHECK(ntfs_unlink(v.root, "one.txt") == 0);
	CHECK(ntfs_getattr(a, &aa) == 0);
	CHECK_MSG(aa.nlink == 1, "nlink after unlinking one of two names is %u, expected 1",
		  aa.nlink);
	CHECK(ntfs_lookup(v.root, "one.txt", &b) == -ENOENT);
	CHECK(ntfs_lookup(v.root, "two.txt", &b) == 0);
	if (b) {
		memset(buf, 0, sizeof(buf));
		CHECK_MSG(ntfs_read(b, buf, 5, 0) == 5,
			  "the surviving name is unreadable after the other was removed");
		CHECK(memcmp(buf, "HELLO", 5) == 0);
		ntfs_inode_put(b);
		b = NULL;
	}
	ntfs_inode_put(a);
	a = NULL;
	vol_down(&v);
	fsck_clean("hard link create + one unlink");

	/* The count and the data must come back off disk, not out of the cache. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_hard_links: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_lookup(v.root, "two.txt", &b) == 0);
	if (b) {
		CHECK(ntfs_getattr(b, &ab) == 0);
		CHECK_MSG(ab.nlink == 1, "nlink off disk is %u, expected 1", ab.nlink);
		CHECK_MSG(ab.inode_no == ino, "inode number changed across a remount");
		memset(buf, 0, sizeof(buf));
		CHECK(ntfs_read(b, buf, 5, 0) == 5);
		CHECK(memcmp(buf, "HELLO", 5) == 0);
		ntfs_inode_put(b);
		b = NULL;
	}
	/* Last name: now the file really goes. */
	CHECK(ntfs_unlink(v.root, "two.txt") == 0);
	CHECK(ntfs_lookup(v.root, "two.txt", &b) == -ENOENT);
out:
	if (a) ntfs_inode_put(a);
	vol_down(&v);
	/* The MFT record must be free and nothing must still point at it. */
	fsck_clean("hard link last unlink");
	unlink(scratch);
	printf("test_hard_links\n");
}

/* ---- 2. what ntfs_link refuses -------------------------------------- */

/*
 * A hard link to a directory makes the index a cycle: no fsck can untangle it
 * afterwards, so the refusal is the safety property, not an inconvenience.
 */
static void test_hard_link_refusals(void)
{
	struct vol v;
	ntfs_inode_t *d = NULL, *f = NULL, *l = NULL;
	char buf[16];
	size_t n;

	if (copy_fixture("linkrefuse") != 0) {
		fprintf(stderr, "SKIP test_hard_link_refusals: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_hard_link_refusals: mount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_mkdir(v.root, "dir1", 0755, &d) == 0);
	CHECK(ntfs_create(v.root, "file1", 0100644, &f) == 0);
	if (!d || !f) goto out;

	CHECK_MSG(ntfs_link(d, v.root, "dir2") == -EPERM,
		  "a hard link to a directory was not refused");
	CHECK(ntfs_lookup(v.root, "dir2", &l) == -ENOENT);

	CHECK_MSG(ntfs_link(f, v.root, "file1") == -EEXIST,
		  "linking over an existing name was not refused");
	/* The target of the link must be a directory. */
	CHECK(ntfs_link(f, f, "x") == -ENOTDIR);
	/* Empty and path-shaped names are not single components. */
	CHECK(ntfs_link(f, v.root, "") == -EINVAL);
	CHECK(ntfs_link(f, v.root, "a/b") == -EINVAL);
	CHECK(ntfs_link(f, v.root, ".") == -EINVAL);

	/* And the two remove calls do not substitute for each other. */
	CHECK(ntfs_rmdir(v.root, "file1") == -ENOTDIR);
	CHECK(ntfs_unlink(v.root, "dir1") == -EISDIR);

	/* readlink only answers for symlinks. */
	n = 0;
	CHECK(ntfs_readlink(f, buf, sizeof(buf), &n) == -EINVAL);

	/* Nothing above may have changed the link count. */
	{
		struct ntfs_attr at;
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.nlink == 1, "a refused link still bumped nlink to %u", at.nlink);
	}
out:
	if (d) ntfs_inode_put(d);
	if (f) ntfs_inode_put(f);
	vol_down(&v);
	fsck_clean("refused links");
	unlink(scratch);
	printf("test_hard_link_refusals\n");
}

/* ---- 3. many links, up to the cap ----------------------------------- */

/* Extra names the cap allows on top of the one the file was created with. */
#define MANY_LINKS (NTFS_LINK_MAX - 1)

/*
 * NTFS_LINK_MAX is Windows' limit, not an on-disk one: link_count is a __le16
 * and the FILE_NAME attributes spill into extents, so the structure holds
 * 65535, and upstream Linux fs/ntfs enforces nothing at all. Microsoft
 * documents 1023 links creatable with CreateHardLink on top of the name the
 * file was created with, so 1024 names in total, and Windows fails past it. A
 * volume this driver writes has to stay usable on Windows, so the port
 * enforces the same number and returns -EMLINK.
 *
 * (The project previously carried a bare "1023" in the FSKit pathconf block
 * and nothing enforced it anywhere. 1023 is what Microsoft says can be
 * *added*; the count a file carries, and what pathconf reports, is 1024.)
 *
 * This is also the only case that forces the FILE_NAME attributes out of the
 * base MFT record and into extents via $ATTRIBUTE_LIST, which is exactly where
 * a link count and the real number of names drift apart. Hence the fsck.
 */
static void test_many_links(void)
{
	struct vol v;
	ntfs_inode_t *f = NULL, *l = NULL;
	struct ntfs_attr at;
	char nm[32];
	uint64_t ino = 0;
	int i, made = 0, err;

	if (copy_fixture("manylinks") != 0) {
		fprintf(stderr, "SKIP test_many_links: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_many_links: mount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_create(v.root, "many.bin", 0100644, &f) == 0);
	if (!f) goto out;
	CHECK(ntfs_write(f, "payload", 7, 0) == 7);
	ino = ntfs_inode_number(f);

	for (i = 0; i < MANY_LINKS; i++) {
		snprintf(nm, sizeof(nm), "ln%04d", i);
		err = ntfs_link(f, v.root, nm);
		if (err) {
			fprintf(stderr, "  link #%d (%s) failed: %d\n", i + 2, nm, err);
			break;
		}
		made++;
	}
	CHECK_MSG(made == MANY_LINKS,
		  "only %d of %d extra links were created; the cap bites early",
		  made, MANY_LINKS);
	CHECK(ntfs_getattr(f, &at) == 0);
	CHECK_MSG(at.nlink == (uint32_t)made + 1,
		  "nlink is %u after %d extra links, expected %d",
		  at.nlink, made, made + 1);
	CHECK_MSG(at.nlink == NTFS_LINK_MAX,
		  "the file should now be sitting exactly on the cap: nlink %u, cap %d",
		  at.nlink, NTFS_LINK_MAX);

	/*
	 * The name after the cap is refused, and refused without a trace: the
	 * count must not move and the name must not appear in the directory.
	 * A half-done link here is the leak this whole group exists to catch.
	 */
	CHECK_MSG(ntfs_link(f, v.root, "one-too-many") == -EMLINK,
		  "the %dth name was not refused with -EMLINK", NTFS_LINK_MAX + 1);
	CHECK(ntfs_getattr(f, &at) == 0);
	CHECK_MSG(at.nlink == NTFS_LINK_MAX,
		  "the refused link still moved nlink to %u", at.nlink);
	CHECK(ntfs_lookup(v.root, "one-too-many", &l) == -ENOENT);

	/*
	 * A file sitting on the cap must still be renameable. rename() adds the
	 * new name before dropping the old one, so a cap checked inside
	 * __ntfs_link() would refuse this even though the number of names does
	 * not change. Rename one of the links and put it back.
	 */
	CHECK_MSG(ntfs_rename(v.root, "ln0000", v.root, "renamed") == 0,
		  "renaming a name of a file at the cap was refused");
	CHECK(ntfs_getattr(f, &at) == 0);
	CHECK_MSG(at.nlink == NTFS_LINK_MAX,
		  "rename at the cap changed nlink to %u", at.nlink);
	CHECK(ntfs_rename(v.root, "renamed", v.root, "ln0000") == 0);

	ntfs_inode_put(f);
	f = NULL;
	vol_down(&v);
	fsck_clean("hard links up to the cap");

	/* The count must be right off disk too. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_many_links: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	CHECK(ntfs_lookup(v.root, "ln0000", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG(at.nlink == (uint32_t)made + 1,
			  "nlink off disk is %u, expected %d", at.nlink, made + 1);
		CHECK(at.inode_no == ino);
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Take them all off again. The count must come back down to 1, not to
	 * some remainder: a residue here is a record that will never be freed. */
	for (i = 0; i < made; i++) {
		snprintf(nm, sizeof(nm), "ln%04d", i);
		if (ntfs_unlink(v.root, nm) != 0) {
			CHECK_MSG(0, "unlink of %s failed", nm);
			break;
		}
	}
	CHECK(ntfs_lookup(v.root, "many.bin", &f) == 0);
	if (f) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.nlink == 1, "nlink after removing every extra name is %u, expected 1",
			  at.nlink);
	}
out:
	if (f) ntfs_inode_put(f);
	vol_down(&v);
	fsck_clean("every extra link removed again");
	unlink(scratch);
	printf("test_many_links (%d extra links, cap %d)\n", made, NTFS_LINK_MAX);
}

/* ---- 4. symlinks ---------------------------------------------------- */

static void check_target(ntfs_inode_t *l, const char *want, const char *what)
{
	char buf[2048];
	size_t n = 0;
	int err;

	err = ntfs_readlink(l, buf, sizeof(buf), &n);
	checks++;
	if (err) {
		failures++;
		fprintf(stderr, "FAIL readlink(%s): %d\n", what, err);
		return;
	}
	checks++;
	if (n != strlen(want) || memcmp(buf, want, n) != 0) {
		failures++;
		fprintf(stderr, "FAIL readlink(%s): got '%.*s' (%zu bytes), expected '%s'\n",
			what, (int)n, buf, n, want);
	}
}

/*
 * Symlinks are written as WSL reparse points (IO_REPARSE_TAG_LX_SYMLINK,
 * core/ntfs/reparse.c). The target string in the reparse attribute is the only
 * record of where the link points, and nothing else on the volume can be used
 * to reconstruct it, so a readlink that truncates or mangles it destroys the
 * link with no other symptom.
 */

/* The reparse tag actually on disk for @name in the root, or 0. Read from the
 * image, not through the driver, so it tests what was written rather than what
 * the driver believes it wrote. */
static uint32_t disk_reparse_tag(const char *name)
{
	const char *info = getenv("NTFS_NTFSINFO");
	char cmd[PATH_MAX + 300], line[256];
	uint32_t tag = 0;
	FILE *p;

	if (!info || !*info || access(info, X_OK) != 0)
		return 0;
	snprintf(cmd, sizeof(cmd),
		 "%s -F /%s -v %s 2>/dev/null | grep -A40 'REPARSE_POINT (0xc0)' | "
		 "grep -m1 'Reparse tag:' | grep -oE '0x[0-9a-fA-F]+'",
		 info, name, scratch);
	p = popen(cmd, "r");
	if (!p)
		return 0;
	if (fgets(line, sizeof(line), p))
		tag = (uint32_t)strtoul(line, NULL, 16);
	pclose(p);
	return tag;
}

/*
 * Which reparse tag a symlink gets, and that it survives a remount either way.
 *
 * Finding 20. Upstream writes only IO_REPARSE_TAG_LX_SYMLINK, which Windows
 * refuses to follow and Linux ntfs3 cannot read. IO_REPARSE_TAG_SYMLINK is
 * followed by every reader there is, so it is the default whenever the target
 * can be expressed in it. Targets Windows cannot name -- : * ? " < > | -- and a
 * literal backslash, which the native form cannot tell from a separator, keep
 * the WSL tag. That is the per-link rule Microsoft's own DrvFs applies. The
 * wsl_symlinks mount flag forces the old behaviour for every link.
 *
 * Two things this pins that the first version of the writer got wrong or
 * nearly wrong: a literal backslash must NOT go native (it came back as '/'),
 * and getattr must report the target length after a remount for BOTH tags,
 * because ni->target used to be filled only for the WSL tag and native links
 * stat()ed at 0 bytes.
 */
/*
 * Windows directory symlinks and junctions must read as links, resolve to the
 * right place, and delete without touching their target.
 *
 * Inherited defect: upstream decided the inode type by MFT_RECORD_IS_DIRECTORY
 * before looking at the reparse point, so a mklink /D or mklink /J -- a
 * directory record WITH a reparse point -- loaded as an empty folder. That is
 * every junction on a Windows system disk. ntfs3, ntfs-3g and WSL present them
 * as symlinks; ntfsplus fixed the same bug in June 2026.
 *
 * Nothing on macOS can create one, so the fixture is built here: directories
 * made through our own driver, then tools/mkfixtures/patch_reparse.py rewrites
 * each record into the exact bytes Windows leaves. The cases cover both tags,
 * a relative and an absolute target, a mount-volume GUID, a junction one level
 * deep (the "../" prefix must count depth), and a cross-volume one.
 *
 * Two things this pins that the first version got wrong: the WSL $LXMOD EA
 * had already put S_IFDIR in i_mode, and OR-ing S_IFLNK on top produced a type
 * that was neither; and readlink must report the length on -ERANGE.
 */
/*
 * A symlink to an existing directory is written as a directory record --
 * Windows' <SYMLINKD> -- and everything else as a file record.
 *
 * Windows has two kinds of symlink and demands the choice at creation; POSIX
 * has one. A file-type link to a directory is followed on Windows but lists as
 * <SYMLINK> and cannot be `cd`ed through. So, as WSL's DrvFs does, the target
 * is resolved at symlink(2) time and a directory record is written when it is
 * an existing directory here, reached by a relative path that crosses no other
 * link. The read side must present that record as a link, and deleting it must
 * remove the link and nothing else.
 *
 * The record type is read from the image, not through the driver, because the
 * driver presents both kinds identically and could hide a wrong choice.
 */
static void test_symlink_to_directory_is_a_directory_record(void)
{
	static const struct { const char *dir, *name, *target; bool want_dir; } cases[] = {
		{ NULL,  "to-dir",     "realdir",    true  },	/* existing dir */
		{ "sub", "to-dir-up",  "../realdir", true  },	/* via .. */
		{ NULL,  "to-file",    "afile.txt",  false },	/* a file */
		{ NULL,  "to-missing", "nothere",    false },	/* dangling */
		{ NULL,  "to-dir-abs", "/realdir",   false },	/* absolute: refused */
		{ NULL,  "via-link",   "to-dir/",    false },	/* crosses a link */
	};
	const size_t N = sizeof(cases) / sizeof(cases[0]);
	const char *info = getenv("NTFS_NTFSINFO");
	struct vol v;
	ntfs_inode_t *x = NULL, *sub = NULL, *l = NULL;
	size_t i;

	printf("test_symlink_to_directory_is_a_directory_record\n");
	if (copy_fixture("dirlink") != 0) { fprintf(stderr, "SKIP: no fixture\n"); return; }
	if (vol_up(&v, 022, 022, 501, 20)) { unlink(scratch); return; }

	CHECK(ntfs_mkdir(v.root, "realdir", 0755, &x) == 0);
	if (x) { ntfs_inode_t *f; if (ntfs_create(x, "inside.txt", 0100644, &f) == 0) ntfs_inode_put(f); ntfs_inode_put(x); x = NULL; }
	CHECK(ntfs_mkdir(v.root, "sub", 0755, &sub) == 0);
	CHECK(ntfs_create(v.root, "afile.txt", 0100644, &x) == 0);
	if (x) { ntfs_inode_put(x); x = NULL; }
	for (i = 0; i < N; i++) {
		ntfs_inode_t *d = cases[i].dir ? sub : v.root;

		CHECK_MSG(ntfs_symlink(d, cases[i].name, cases[i].target, &l) == 0, "symlink(%s)", cases[i].name);
		if (l) { ntfs_inode_put(l); l = NULL; }
	}
	if (sub) { ntfs_inode_put(sub); sub = NULL; }
	vol_down(&v);

	/* The record type, from the image. */
	if (info && *info && access(info, X_OK) == 0) {
		for (i = 0; i < N; i++) {
			char cmd[PATH_MAX + 300], line[256];
			FILE *p;
			bool is_dir = false, seen = false;

			snprintf(cmd, sizeof(cmd), "%s -F /%s%s%s %s 2>/dev/null | grep -m1 'Flags:'",
				 info, cases[i].dir ? cases[i].dir : "", cases[i].dir ? "/" : "",
				 cases[i].name, scratch);
			p = popen(cmd, "r");
			if (p && fgets(line, sizeof(line), p)) { seen = true; is_dir = strstr(line, "DIRECTORY") != NULL; }
			if (p) pclose(p);
			if (!seen) { fprintf(stderr, "SKIP record type(%s): ntfsinfo gave nothing\n", cases[i].name); continue; }
			CHECK_MSG(is_dir == cases[i].want_dir,
				  "%s -> '%s': written as a %s record, expected %s",
				  cases[i].name, cases[i].target, is_dir ? "DIRECTORY" : "FILE",
				  cases[i].want_dir ? "DIRECTORY" : "FILE");
		}
	}

	/* Read back after remount: all links, right targets, right sizes. */
	if (vol_up(&v, 022, 022, 501, 20)) { failures++; checks++; unlink(scratch); return; }
	for (i = 0; i < N; i++) {
		struct ntfs_attr at;
		char buf[128];
		size_t n = 0;
		ntfs_inode_t *d = v.root;

		if (cases[i].dir) { CHECK(ntfs_lookup(v.root, cases[i].dir, &sub) == 0); d = sub; }
		CHECK_MSG(ntfs_lookup(d, cases[i].name, &l) == 0, "lookup(%s)", cases[i].name);
		if (l) {
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG((at.mode & S_IFMT) == S_IFLNK, "%s: not a link after remount (mode %o)", cases[i].name, at.mode);
			CHECK(ntfs_readlink(l, buf, sizeof(buf), &n) == 0);
			CHECK_MSG(n == strlen(cases[i].target) && !memcmp(buf, cases[i].target, n),
				  "%s: readlink '%.*s', expected '%s'", cases[i].name, (int)n, buf, cases[i].target);
			CHECK(at.size == (int64_t)n);
			ntfs_inode_put(l); l = NULL;
		}
		if (sub) { ntfs_inode_put(sub); sub = NULL; }
	}
	/* Deleting the directory-record link removes the link, never the target. */
	CHECK(ntfs_rmdir(v.root, "to-dir") == -ENOTDIR);
	CHECK(ntfs_unlink(v.root, "to-dir") == 0);
	CHECK_MSG(ntfs_lookup(v.root, "realdir", &x) == 0, "the target must survive");
	if (x) { ntfs_inode_t *f; CHECK_MSG(ntfs_lookup(x, "inside.txt", &f) == 0, "the target's contents must survive"); if (f) ntfs_inode_put(f); ntfs_inode_put(x); x = NULL; }
	vol_down(&v);
	fsck_clean("symlink to directory");
	unlink(scratch);
}

static void test_windows_dir_links(void)
{
	static const struct {
		const char *path, *kind, *target, *print, *expect;
		int depth;			/* directories between link and root */
	} cases[] = {
		{ "jdir",      "junction",    "\\??\\C:\\target", "C:\\target", "target",  0 },
		{ "sdir",      "symlink-abs", "\\??\\C:\\target", "C:\\target", "target",  0 },
		{ "rdir",      "symlink-rel", "target",           "target",     "target",  0 },
		{ "vdir",      "volume",      "\\??\\Volume{3f0a1c2e-5b6d-4e7f-8a9b-0c1d2e3f4a5b}\\", "",
		  "/??/Volume{3f0a1c2e-5b6d-4e7f-8a9b-0c1d2e3f4a5b}/", 0 },
		{ "sub/updir", "junction",    "\\??\\C:\\sub",    "C:\\sub",    "../sub",  1 },
		{ "fdir",      "junction",    "\\??\\D:\\other",  "D:\\other",  "other",   0 },
	};
	const size_t N = sizeof(cases) / sizeof(cases[0]);
	const char *py = getenv("NTFS_PATCH_REPARSE");
	struct vol v;
	ntfs_inode_t *d = NULL, *sub = NULL, *t = NULL, *f = NULL, *l = NULL;
	char cmd[PATH_MAX * 2 + 256];
	size_t i;
	int rc;

	printf("test_windows_dir_links\n");
	if (!py || !*py || access(py, R_OK) != 0) {
		fprintf(stderr, "SKIP test_windows_dir_links: NTFS_PATCH_REPARSE not set\n");
		return;
	}
	if (copy_fixture("winlinks") != 0) {
		fprintf(stderr, "SKIP test_windows_dir_links: no fixture\n");
		return;
	}

	/* Phase 1: plain directories, plus a real target with a file in it. */
	if (vol_up(&v, 022, 022, 501, 20)) { unlink(scratch); return; }
	CHECK(ntfs_mkdir(v.root, "target", 0755, &t) == 0);
	if (t) {
		CHECK(ntfs_create(t, "hello.txt", 0100644, &f) == 0);
		if (f) { CHECK(ntfs_write(f, "hi\n", 3, 0) == 3); ntfs_inode_put(f); f = NULL; }
		ntfs_inode_put(t); t = NULL;
	}
	CHECK(ntfs_mkdir(v.root, "sub", 0755, &sub) == 0);
	for (i = 0; i < N; i++) {
		const char *slash = strchr(cases[i].path, '/');

		if (slash) {
			CHECK(ntfs_mkdir(sub, slash + 1, 0755, &d) == 0);
		} else {
			CHECK(ntfs_mkdir(v.root, cases[i].path, 0755, &d) == 0);
		}
		if (d) { ntfs_inode_put(d); d = NULL; }
	}
	if (sub) { ntfs_inode_put(sub); sub = NULL; }
	vol_down(&v);

	/* Phase 2: turn each directory into what Windows would have left. The
	 * MFT number comes from ntfsinfo so the test does not depend on
	 * allocation order. */
	for (i = 0; i < N; i++) {
		const char *info = getenv("NTFS_NTFSINFO");
		char line[64];
		FILE *p;
		long mftno = -1;

		if (!info) break;
		snprintf(cmd, sizeof(cmd),
			 "%s -F /%s %s 2>/dev/null | grep -m1 'Dumping Inode' | grep -oE '[0-9]+' | head -1",
			 info, cases[i].path, scratch);
		p = popen(cmd, "r");
		if (p && fgets(line, sizeof(line), p)) mftno = strtol(line, NULL, 10);
		if (p) pclose(p);
		CHECK_MSG(mftno > 0, "could not find the MFT number of %s", cases[i].path);
		if (mftno <= 0) continue;
		snprintf(cmd, sizeof(cmd), "python3 %s %s %ld %s '%s' '%s' >/dev/null 2>&1",
			 py, scratch, mftno, cases[i].kind, cases[i].target, cases[i].print);
		rc = system(cmd);
		CHECK_MSG(rc == 0, "patch_reparse.py failed for %s (rc %d)", cases[i].path, rc);
	}

	/* Phase 3: read them back. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_windows_dir_links: remount failed\n");
		failures++; checks++; unlink(scratch); return;
	}
	for (i = 0; i < N; i++) {
		struct ntfs_attr at;
		char buf[256];
		size_t n = 0;
		ntfs_inode_t *dir = v.root;
		const char *name = cases[i].path, *slash = strchr(name, '/');

		if (slash) { CHECK(ntfs_lookup(v.root, "sub", &sub) == 0); dir = sub; name = slash + 1; }
		CHECK_MSG(ntfs_lookup(dir, name, &l) == 0, "lookup(%s)", cases[i].path);
		if (l) {
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG((at.mode & S_IFMT) == S_IFLNK,
				  "%s (%s): mode %o is not S_IFLNK -- a Windows directory link "
				  "read as something else", cases[i].path, cases[i].kind, at.mode);
			CHECK_MSG(ntfs_readlink(l, buf, sizeof(buf), &n) == 0, "readlink(%s)", cases[i].path);
			CHECK_MSG(n == strlen(cases[i].expect) && !memcmp(buf, cases[i].expect, n),
				  "%s: readlink '%.*s', expected '%s'", cases[i].path, (int)n, buf, cases[i].expect);
			CHECK_MSG(at.size == (int64_t)n, "%s: size %lld != readlink length %zu",
				  cases[i].path, (long long)at.size, n);
			ntfs_inode_put(l); l = NULL;
		}
		if (sub) { ntfs_inode_put(sub); sub = NULL; }
	}

	/* Phase 4: the sharp edge. Removing a junction removes the junction. */
	CHECK_MSG(ntfs_rmdir(v.root, "jdir") == -ENOTDIR, "rmdir on a junction must be ENOTDIR");
	CHECK_MSG(ntfs_unlink(v.root, "jdir") == 0, "unlink on a junction must remove it");
	CHECK(ntfs_lookup(v.root, "jdir", &l) == -ENOENT);
	CHECK_MSG(ntfs_lookup(v.root, "target", &t) == 0, "the junction's TARGET must survive");
	if (t) {
		CHECK_MSG(ntfs_lookup(t, "hello.txt", &f) == 0, "the target's contents must survive");
		if (f) { ntfs_inode_put(f); f = NULL; }
		ntfs_inode_put(t); t = NULL;
	}
	/* and a directory record must not be hard-linkable, however it presents */
	if (ntfs_lookup(v.root, "sdir", &l) == 0 && l) {
		CHECK_MSG(ntfs_link(l, v.root, "sdir-hl") == -EPERM,
			  "hard-linking a directory-record symlink must be refused");
		ntfs_inode_put(l); l = NULL;
	}
	vol_down(&v);
	fsck_clean("windows directory links");
	unlink(scratch);
}

/*
 * ntfs-3g Interix symlinks (docs/GAPS.md §2.1).
 *
 * Stock ntfs-3g -- the most deployed third-party NTFS driver -- writes its
 * symlinks as plain files with FILE_ATTR_SYSTEM whose $DATA is the 8-byte
 * magic "IntxLNK\1" followed by the target in UTF-16LE, no NUL terminator.
 * Until this fix they read here as regular files with binary contents. The
 * format was verified against ntfs-3g 2026.7.7 source (libntfs/dir.c
 * ntfs_create_symlink, src/ntfs-3g.c ntfs_fuse_readlink) and against a link
 * created by libntfs-3g itself (tools/mkfixtures/mkintx.c): SI/FN
 * attributes SYSTEM ARCHIVE (0x24), $DATA "IntxLNK\1" + UTF-16LE target.
 *
 * No FUSE exists on this VM, so the fixture is built the way the Windows
 * directory links are: files are created through our own driver with the
 * right $DATA bytes, then tools/mkfixtures/patch_interix.py flips the
 * SYSTEM bit in $STANDARD_INFORMATION, $FILE_NAME and the parent's index
 * entry -- the three places ntfs-3g sets it.
 *
 * Cases: relative and absolute targets, a non-ASCII target (UTF-16
 * conversion), a 3000-character target (forces $DATA non-resident, so the
 * bounded read is exercised past the resident case), and three negative
 * controls -- SYSTEM without the magic, magic without SYSTEM, and the
 * magic alone (an empty target is not a link: ntfs-3g's readlink rejects
 * it with -EINVAL). A link is also unlinked to show only the link goes.
 */
static size_t intx_ascii(const char *target, uint8_t *buf, size_t cap)
{
	static const uint8_t magic[8] = { 'I', 'n', 't', 'x', 'L', 'N', 'K', 1 };
	size_t n = strlen(target), i;

	if (8 + 2 * n > cap)
		return 0;
	memcpy(buf, magic, 8);
	for (i = 0; i < n; i++) {
		buf[8 + 2 * i] = (uint8_t)target[i];
		buf[8 + 2 * i + 1] = 0;
	}
	return 8 + 2 * n;
}

static size_t intx_units(const uint16_t *units, size_t nunits,
			 uint8_t *buf, size_t cap)
{
	static const uint8_t magic[8] = { 'I', 'n', 't', 'x', 'L', 'N', 'K', 1 };
	size_t i;

	if (8 + 2 * nunits > cap)
		return 0;
	memcpy(buf, magic, 8);
	for (i = 0; i < nunits; i++) {
		buf[8 + 2 * i] = (uint8_t)(units[i] & 0xff);
		buf[8 + 2 * i + 1] = (uint8_t)(units[i] >> 8);
	}
	return 8 + 2 * nunits;
}

static int write_plain(ntfs_inode_t *dir, const char *name,
		       const void *data, size_t len)
{
	ntfs_inode_t *f = NULL;
	int rc = ntfs_create(dir, name, 0100644, &f);

	if (rc)
		return rc;
	if (len) {
		ssize_t w = ntfs_write(f, data, len, 0);

		if (w != (ssize_t)len)
			rc = -EIO;
	}
	ntfs_inode_put(f);
	return rc;
}

struct dt_entry {
	char name[64];
	int type;
};
static struct dt_entry dt_list[32];
static int dt_n;

static int collect_type_cb(const struct ntfs_dirent *e, void *ctx)
{
	(void)ctx;
	if (dt_n < 32 && e->name_len < sizeof(dt_list[0].name)) {
		memcpy(dt_list[dt_n].name, e->name, e->name_len);
		dt_list[dt_n].name[e->name_len] = 0;
		dt_list[dt_n].type = e->type;
		dt_n++;
	}
	return 0;
}

static int dt_type_of(const char *name)
{
	int i;

	for (i = 0; i < dt_n; i++)
		if (!strcmp(dt_list[i].name, name))
			return dt_list[i].type;
	return -1;
}

static void test_interix_symlinks(void)
{
	static const uint16_t uni_units[] =
		{ 0x74, 0xe4, 0x72, 0x67, 0x65, 0x74, 0x2f, 0xfc }; /* tärget/ü */
	static const char *patch[] =
		{ "ilink", "ilink-abs", "ilink-uni", "ilink-long",
		  "plain-sys", "magic-only" };
	static struct {
		const char *name, *expect;
		int is_link;
	} cases[] = {
		{ "ilink",      "target",               1 },
		{ "ilink-abs",  "/target",              1 },
		{ "ilink-uni",  "t\xc3\xa4rget/\xc3\xbc", 1 },
		{ "ilink-long", NULL,                   1 },
		{ "plain-sys",  NULL,                   0 },
		{ "magic-nosys", NULL,                  0 },
		{ "magic-only", NULL,                   0 },
	};
	const char *py = getenv("NTFS_PATCH_INTERIX");
	struct vol v;
	uint8_t buf[8192];
	char long_target[3001];
	char cmd[PATH_MAX * 2 + 256];
	size_t i, n;
	int rc;

	printf("test_interix_symlinks\n");
	if (!py || !*py || access(py, R_OK) != 0) {
		fprintf(stderr, "SKIP test_interix_symlinks: NTFS_PATCH_INTERIX not set\n");
		return;
	}
	if (copy_fixture("interix") != 0) {
		fprintf(stderr, "SKIP test_interix_symlinks: no fixture\n");
		return;
	}

	/* The long target: 2993 a's + "/target" = 3000 chars, so $DATA is
	 * 6008 bytes and cannot stay resident in a 1024-byte record. */
	memset(long_target, 'a', 2993);
	memcpy(long_target + 2993, "/target", 7);
	long_target[3000] = 0;
	cases[3].expect = long_target;

	/* Phase 1: a real target file, the links with IntxLNK $DATA, and
	 * the three negative controls. */
	if (vol_up(&v, 022, 022, 501, 20)) { unlink(scratch); return; }
	CHECK(write_plain(v.root, "target", "hello\n", 6) == 0);
	n = intx_ascii("target", buf, sizeof(buf));
	CHECK(n > 0 && write_plain(v.root, "ilink", buf, n) == 0);
	n = intx_ascii("/target", buf, sizeof(buf));
	CHECK(n > 0 && write_plain(v.root, "ilink-abs", buf, n) == 0);
	n = intx_units(uni_units, sizeof(uni_units) / sizeof(uni_units[0]),
		       buf, sizeof(buf));
	CHECK(n > 0 && write_plain(v.root, "ilink-uni", buf, n) == 0);
	n = intx_ascii(long_target, buf, sizeof(buf));
	CHECK(n > 0 && write_plain(v.root, "ilink-long", buf, n) == 0);
	CHECK(write_plain(v.root, "plain-sys", "hello\n", 6) == 0);
	n = intx_ascii("target", buf, sizeof(buf));
	CHECK(n > 0 && write_plain(v.root, "magic-nosys", buf, n) == 0);
	CHECK(write_plain(v.root, "magic-only",
			  (const uint8_t[]){ 'I','n','t','x','L','N','K', 1 }, 8) == 0);
	vol_down(&v);

	/* Phase 2: flip SYSTEM on every case except magic-nosys. The MFT
	 * number comes from ntfsinfo so the test does not depend on
	 * allocation order. */
	for (i = 0; i < sizeof(patch) / sizeof(patch[0]); i++) {
		const char *info = getenv("NTFS_NTFSINFO");
		char line[64];
		FILE *p;
		long mftno = -1;

		if (!info) break;
		snprintf(cmd, sizeof(cmd),
			 "%s -F /%s %s 2>/dev/null | grep -m1 'Dumping Inode' | grep -oE '[0-9]+' | head -1",
			 info, patch[i], scratch);
		p = popen(cmd, "r");
		if (p && fgets(line, sizeof(line), p)) mftno = strtol(line, NULL, 10);
		if (p) pclose(p);
		CHECK_MSG(mftno > 0, "could not find the MFT number of %s", patch[i]);
		if (mftno <= 0) continue;
		snprintf(cmd, sizeof(cmd), "python3 %s %s %ld >/dev/null 2>&1",
			 py, scratch, mftno);
		rc = system(cmd);
		CHECK_MSG(rc == 0, "patch_interix.py failed for %s (rc %d)", patch[i], rc);
	}

	/* Phase 3: read them back. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_interix_symlinks: remount failed\n");
		failures++; checks++; unlink(scratch); return;
	}
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct ntfs_attr at;
		char rbuf[4096];
		ntfs_inode_t *l = NULL;
		size_t rn = 0;

		CHECK_MSG(ntfs_lookup(v.root, cases[i].name, &l) == 0,
			  "lookup(%s)", cases[i].name);
		if (!l) continue;
		CHECK(ntfs_getattr(l, &at) == 0);
		if (cases[i].is_link) {
			CHECK_MSG((at.mode & S_IFMT) == S_IFLNK,
				  "%s: mode %o is not S_IFLNK -- an ntfs-3g Interix "
				  "link read as something else", cases[i].name, at.mode);
			CHECK_MSG(ntfs_readlink(l, rbuf, sizeof(rbuf), &rn) == 0,
				  "readlink(%s)", cases[i].name);
			CHECK_MSG(rn == strlen(cases[i].expect) &&
				  !memcmp(rbuf, cases[i].expect, rn),
				  "%s: readlink '%.*s', expected '%s'", cases[i].name,
				  (int)rn, rbuf, cases[i].expect);
			CHECK_MSG(at.size == (int64_t)rn,
				  "%s: size %lld != readlink length %zu",
				  cases[i].name, (long long)at.size, rn);
		} else {
			CHECK_MSG((at.mode & S_IFMT) == S_IFREG,
				  "%s: mode %o is not S_IFREG -- a negative control "
				  "read as a link", cases[i].name, at.mode);
		}
		ntfs_inode_put(l); l = NULL;
	}

	/* The negative controls must still be readable as the plain files
	 * they are: SYSTEM without the magic keeps its bytes, and the magic
	 * without SYSTEM is inert data. */
	{
		ntfs_inode_t *f = NULL;
		char rbuf[64];
		ssize_t r;

		CHECK(ntfs_lookup(v.root, "plain-sys", &f) == 0);
		if (f) {
			r = ntfs_read(f, rbuf, 6, 0);
			CHECK_MSG(r == 6 && !memcmp(rbuf, "hello\n", 6),
				  "plain-sys content changed: %zd bytes", r);
			ntfs_inode_put(f); f = NULL;
		}
		CHECK(ntfs_lookup(v.root, "magic-nosys", &f) == 0);
		if (f) {
			r = ntfs_read(f, rbuf, 20, 0);
			CHECK_MSG(r == 20 && !memcmp(rbuf, "IntxLNK\1", 8),
				  "magic-nosys content changed: %zd bytes", r);
			ntfs_inode_put(f); f = NULL;
		}
	}

	/* readdir must agree with stat: links list as symlinks, the SYSTEM
	 * file without the magic as a file. */
	{
		uint64_t cookie = 0;
		bool eof = false;

		dt_n = 0;
		while (!eof && ntfs_readdir(v.root, &cookie, false,
					    collect_type_cb, NULL, &eof) == 0)
			;
		CHECK_MSG(dt_type_of("ilink") == NTFS_ITEM_SYMLINK,
			  "readdir: ilink type %d, not SYMLINK", dt_type_of("ilink"));
		CHECK_MSG(dt_type_of("ilink-long") == NTFS_ITEM_SYMLINK,
			  "readdir: ilink-long type %d, not SYMLINK", dt_type_of("ilink-long"));
		CHECK_MSG(dt_type_of("plain-sys") == NTFS_ITEM_FILE,
			  "readdir: plain-sys type %d, not FILE", dt_type_of("plain-sys"));
		CHECK_MSG(dt_type_of("magic-nosys") == NTFS_ITEM_FILE,
			  "readdir: magic-nosys type %d, not FILE", dt_type_of("magic-nosys"));
	}

	/* Phase 4: removing a link removes the link. */
	CHECK_MSG(ntfs_unlink(v.root, "ilink") == 0, "unlink on an Interix link must remove it");
	{
		ntfs_inode_t *gone = NULL;

		CHECK(ntfs_lookup(v.root, "ilink", &gone) == -ENOENT);
		if (gone) ntfs_inode_put(gone);
	}
	{
		ntfs_inode_t *t = NULL;

		CHECK_MSG(ntfs_lookup(v.root, "target", &t) == 0,
			  "the Interix link's TARGET must survive");
		if (t) ntfs_inode_put(t);
	}
	vol_down(&v);
	fsck_clean("ntfs-3g Interix symlinks");
	unlink(scratch);
}

/*
 * The two on-disk copies of a symlink's $FILE_NAME must agree on the reparse
 * tag.
 *
 * $FILE_NAME carries a four-byte union: packed EA size for an ordinary file,
 * the reparse tag for a reparse point. Upstream wrote the EA size into it
 * unconditionally, so the MFT-resident copy of every symlink claimed a tag of
 * 0x2d while the index copy, corrected later by the inode sync, held the real
 * one. chkdsk never complained, but after a Windows round trip on 2026-09-17
 * the symlinks on a test stick came back listing as zero-byte plain files: the
 * disagreement had been resolved in favour of the wrong copy. Windows reads the
 * tag from the directory index; anything that reconciles the two copies from
 * the MFT side destroys it.
 *
 * The MFT copy is read with ntfsinfo -- which prints exactly that field -- so
 * this checks the bytes, not the driver's opinion of them.
 */
static void test_symlink_filename_union_is_the_tag(void)
{
	const char *info = getenv("NTFS_NTFSINFO");
	struct vol v;
	ntfs_inode_t *l = NULL;
	char cmd[PATH_MAX + 200], line[256];
	FILE *p;
	unsigned long got = 0;

	printf("test_symlink_filename_union_is_the_tag\n");
	if (!info || !*info || access(info, X_OK) != 0) {
		fprintf(stderr, "SKIP test_symlink_filename_union_is_the_tag: NTFS_NTFSINFO not set\n");
		return;
	}
	if (copy_fixture("fnunion") != 0) { fprintf(stderr, "SKIP: no fixture\n"); return; }
	if (vol_up(&v, 022, 022, 501, 20)) { unlink(scratch); return; }
	CHECK(ntfs_symlink(v.root, "lnk", "t.txt", &l) == 0);
	if (l) { ntfs_inode_put(l); l = NULL; }
	vol_down(&v);

	snprintf(cmd, sizeof(cmd), "%s -F /lnk %s 2>/dev/null | grep -m1 'Reparse point tag' | grep -oE '0x[0-9a-fA-F]+'",
		 info, scratch);
	p = popen(cmd, "r");
	if (p && fgets(line, sizeof(line), p))
		got = strtoul(line, NULL, 16);
	if (p) pclose(p);
	CHECK_MSG(got == 0xA000000Cul,
		  "MFT-resident $FILE_NAME union reads 0x%08lx; must be the reparse tag 0xa000000c, "
		  "not the packed EA size (0x2d). The index copy is right; this one gets it "
		  "clobbered on a Windows round trip.", got);

	/*
	 * And no $EA on the link at all. [MS-FSA]: reparse points and extended
	 * attributes are mutually exclusive. chkdsk enforces that by rewriting
	 * the index entry's union to the EA form whenever $EA_INFORMATION is
	 * present, which turned every symlink on a test stick into a zero-byte
	 * file after a round trip (2026-09-18). ntfs3 skips EAs on symlinks for
	 * the same reason.
	 */
	snprintf(cmd, sizeof(cmd), "%s -F /lnk %s 2>/dev/null | grep -c 'EA_INFORMATION'", info, scratch);
	p = popen(cmd, "r");
	got = 99;
	if (p && fgets(line, sizeof(line), p))
		got = strtoul(line, NULL, 10);
	if (p) pclose(p);
	CHECK_MSG(got == 0,
		  "a symlink carries $EA_INFORMATION; chkdsk will rewrite its index entry as "
		  "an EA file and Windows will stop listing it as a link");
	unlink(scratch);
}

static void test_symlink_tag_choice(void)
{
	static const struct {
		const char *name, *target;
		uint32_t want_tag;		/* with default flags */
	} cases[] = {
		{ "n-rel",     "target.txt",       0xA000000Cu },	/* native */
		{ "n-deep",    "dir/sub/f.txt",    0xA000000Cu },
		{ "n-up",      "../sib/y.txt",     0xA000000Cu },
		{ "n-unixabs", "/usr/bin/foo",     0xA000000Cu },	/* ntfs3 convention */
		{ "n-utf8",    "caf\xc3\xa9/\xe6\x97\xa5.txt", 0xA000000Cu },
		{ "w-colon",   "a:b",              0xA000001Du },	/* WSL fallback */
		{ "w-star",    "x*y",              0xA000001Du },
		{ "w-quote",   "say\"hi\"",        0xA000001Du },
		{ "w-bslash",  "a\\b",             0xA000001Du },	/* the near miss */
	};
	const size_t N = sizeof(cases) / sizeof(cases[0]);
	int pass;

	printf("test_symlink_tag_choice\n");

	/* pass 0: default flags, per-link choice. pass 1: wsl_symlinks forced. */
	for (pass = 0; pass < 2; pass++) {
		uint32_t flags = pass ? NTFS_MOUNT_WSL_SYMLINKS : 0;
		struct vol v;
		ntfs_inode_t *l = NULL;
		size_t i;

		if (copy_fixture(pass ? "tagchoice-wsl" : "tagchoice") != 0) {
			fprintf(stderr, "SKIP test_symlink_tag_choice: no fixture\n");
			return;
		}
		if (vol_up_flags(&v, flags, 022, 022, 501, 20)) {
			fprintf(stderr, "SKIP test_symlink_tag_choice: mount failed\n");
			unlink(scratch);
			return;
		}
		for (i = 0; i < N; i++) {
			CHECK_MSG(ntfs_symlink(v.root, cases[i].name, cases[i].target, &l) == 0,
				  "symlink(%s) failed", cases[i].name);
			if (l) { ntfs_inode_put(l); l = NULL; }
		}
		vol_down(&v);

		/* What is on disk, read independently of the driver. */
		for (i = 0; i < N; i++) {
			uint32_t want = pass ? 0xA000001Du : cases[i].want_tag;
			uint32_t got = disk_reparse_tag(cases[i].name);

			if (!got) {
				fprintf(stderr, "SKIP tag(%s): ntfsinfo unavailable\n", cases[i].name);
				continue;
			}
			CHECK_MSG(got == want, "%s%s: on-disk tag 0x%08x, expected 0x%08x",
				  cases[i].name, pass ? " (wsl_symlinks)" : "", got, want);
		}

		/* Remount: a cached read proves nothing. Target and size must both
		 * survive, for both tags. */
		if (vol_up_flags(&v, flags, 022, 022, 501, 20)) {
			fprintf(stderr, "FAIL test_symlink_tag_choice: remount failed\n");
			failures++; checks++;
			unlink(scratch);
			return;
		}
		for (i = 0; i < N; i++) {
			struct ntfs_attr at;
			char buf[256];
			size_t n = 0;

			CHECK_MSG(ntfs_lookup(v.root, cases[i].name, &l) == 0,
				  "lookup(%s) after remount", cases[i].name);
			if (!l)
				continue;
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG((at.mode & S_IFMT) == S_IFLNK, "%s is not a symlink after remount",
				  cases[i].name);
			CHECK_MSG(ntfs_readlink(l, buf, sizeof(buf), &n) == 0, "readlink(%s)", cases[i].name);
			CHECK_MSG(n == strlen(cases[i].target) && !memcmp(buf, cases[i].target, n),
				  "readlink(%s): got '%.*s', expected '%s'", cases[i].name, (int)n, buf,
				  cases[i].target);
			CHECK_MSG(at.size == (int64_t)strlen(cases[i].target),
				  "%s: size %lld after remount, expected %zu (finding 20: native links "
				  "used to stat at 0)", cases[i].name, (long long)at.size,
				  strlen(cases[i].target));
			ntfs_inode_put(l); l = NULL;
		}
		vol_down(&v);
		fsck_clean("symlink tag choice");
		unlink(scratch);
	}
}

/*
 * Upstream 9692b1b4fc00: update_reparse_data() ignored set_reparse_index()'s
 * return value, so a symlink whose $R index insert failed -- here, because
 * the index already holds an entry for the file's (tag, file_id) key, which
 * ntfs_ie_add() refuses with -EEXIST -- still reported success, while no
 * reparse data existed on disk. Without the fix ntfs_symlink() returns 0
 * and the link exists with reparse data but no index entry; with it the
 * create fails with -EEXIST and __ntfs_create's err_out frees the MFT
 * record, so no half-created file is left behind.
 *
 * The stale entry cannot arise from the driver: the $R entry is removed
 * when the file goes away. tools/mkfixtures/patch_stale_reparse.py plants
 * it by hand for the record the symlink will take. Which record that is
 * is learned at runtime: the first file created on a fresh basic-4k.img
 * copy takes the record after the fixture's highest (294), and allocation
 * is strictly sequential, so a sacrificial file first, read back through
 * ntfs_inode_number(), tells the patcher the next record. The entry is
 * planted for the native tag: the target "target.txt" is windows-safe, so
 * the driver takes the native tag, not the WSL fallback.
 */
static void test_reparse_index_failure_propagates(void)
{
	const char *py = getenv("NTFS_PATCH_STALE_REPARSE");
	struct vol v;
	ntfs_inode_t *sac = NULL, *l = NULL, *f = NULL;
	char cmd[2048];
	uint64_t sac_no;
	int err, rc;

	printf("test_reparse_index_failure_propagates\n");

	if (copy_fixture("stale-reparse") != 0) {
		fprintf(stderr, "SKIP test_reparse_index_failure_propagates: no fixture\n");
		return;
	}
	if (!py || !*py) {
		fprintf(stderr, "SKIP test_reparse_index_failure_propagates: NTFS_PATCH_STALE_REPARSE not set\n");
		unlink(scratch);
		return;
	}

	/* Phase 1: learn the next record number from a sacrificial file. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_reparse_index_failure_propagates: mount failed\n");
		unlink(scratch);
		return;
	}
	err = ntfs_create(v.root, "sacrifice.txt", 0100644, &sac);
	CHECK_MSG(err == 0, "sacrificial create failed: %d", err);
	if (err) {
		vol_down(&v);
		unlink(scratch);
		return;
	}
	sac_no = ntfs_inode_number(sac);
	ntfs_inode_put(sac);
	sac = NULL;
	CHECK(ntfs_volume_sync(v.vol) == 0);
	vol_down(&v);

	/* Phase 2: plant the stale $R entry for the next record, then the
	 * symlink that would take it. */
	snprintf(cmd, sizeof(cmd), "python3 %s %s %llu 1 >/dev/null 2>&1",
		 py, scratch, (unsigned long long)(sac_no + 1));
	rc = system(cmd);
	CHECK_MSG(rc == 0, "patch_stale_reparse.py failed (rc %d)", rc);
	if (rc != 0) {
		unlink(scratch);
		return;
	}

	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_reparse_index_failure_propagates: remount failed\n");
		failures++;
		checks++;
		unlink(scratch);
		return;
	}
	err = ntfs_symlink(v.root, "dup.lnk", "target.txt", &l);
	CHECK_MSG(err == -EEXIST,
		  "symlink with a stale $R entry returned %d, expected -EEXIST (-17); "
		  "without upstream 9692b1b4fc00 the failure is swallowed and the create "
		  "reports success", err);
	if (l) {
		ntfs_inode_put(l);
		l = NULL;
	}

	/* No half-created file: the failed create frees its MFT record. */
	CHECK_MSG(ntfs_lookup(v.root, "dup.lnk", &f) != 0,
		  "the failed symlink create left a file behind");
	if (f) {
		ntfs_inode_put(f);
		f = NULL;
	}

	vol_down(&v);
	fsck_clean("stale reparse index");
	unlink(scratch);
}

static void test_symlinks(void)
{
	struct vol v;
	ntfs_inode_t *l = NULL, *f = NULL;
	struct ntfs_attr at;
	char big[PATH_MAX + 8], buf[8];
	size_t n;

	if (copy_fixture("symlinks") != 0) {
		fprintf(stderr, "SKIP test_symlinks: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_symlinks: mount failed\n");
		unlink(scratch);
		return;
	}

	/* Absolute target. */
	CHECK(ntfs_symlink(v.root, "abs.lnk", "/etc/hosts", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK(at.type == NTFS_ITEM_SYMLINK);
		CHECK_MSG((at.mode & S_IFMT) == S_IFLNK, "mode %o is not S_IFLNK", at.mode);
		/* The reported size is the target length, which is what a
		 * caller sizing a readlink buffer relies on. */
		CHECK_MSG(at.size == strlen("/etc/hosts"),
			  "symlink size is %llu, expected the target length %zu",
			  (unsigned long long)at.size, strlen("/etc/hosts"));
		CHECK(at.nlink == 1);
		check_target(l, "/etc/hosts", "abs.lnk");
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Relative target: must be stored verbatim, not resolved or rebased. */
	CHECK(ntfs_symlink(v.root, "rel.lnk", "../sib/y.txt", &l) == 0);
	if (l) { check_target(l, "../sib/y.txt", "rel.lnk"); ntfs_inode_put(l); l = NULL; }

	/* Dangling: nothing resolves the target at create time, and nothing
	 * should. A link to a name that does not exist yet is legal. */
	CHECK(ntfs_symlink(v.root, "dangle.lnk", "no/such/file", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK(at.type == NTFS_ITEM_SYMLINK);
		check_target(l, "no/such/file", "dangle.lnk");
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Non-ASCII survives the UTF-8 -> UTF-16 -> UTF-8 round trip through
	 * the reparse attribute. */
	CHECK(ntfs_symlink(v.root, "utf8.lnk", "caf\xc3\xa9/\xe2\x98\x83.txt", &l) == 0);
	if (l) { check_target(l, "caf\xc3\xa9/\xe2\x98\x83.txt", "utf8.lnk"); ntfs_inode_put(l); l = NULL; }

	/*
	 * A backslash in a WSL target stays a backslash. ntfs_readlink only
	 * rewrites "\" to "/" (and strips "\??\" and a drive letter) on the
	 * OTHER path, for a Windows IO_REPARSE_TAG_SYMLINK written by Windows.
	 * Applying that rewrite here would corrupt a legitimate POSIX name.
	 */
	CHECK(ntfs_symlink(v.root, "bs.lnk", "a\\b", &l) == 0);
	if (l) { check_target(l, "a\\b", "bs.lnk"); ntfs_inode_put(l); l = NULL; }

	/* A symlink pointing at a real file is still a symlink: it is not
	 * followed, and its size is the target's length, not the file's. */
	CHECK(ntfs_create(v.root, "real.txt", 0100644, &f) == 0);
	if (f) { CHECK(ntfs_write(f, "DATA", 4, 0) == 4); ntfs_inode_put(f); f = NULL; }
	CHECK(ntfs_symlink(v.root, "to-real.lnk", "real.txt", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK(at.type == NTFS_ITEM_SYMLINK);
		CHECK_MSG(at.size == strlen("real.txt"),
			  "symlink to an existing file reports size %llu, "
			  "so it was followed", (unsigned long long)at.size);
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Buffer handling. A short buffer must fail without writing a partial
	 * target, and must still report the length the caller needs. */
	CHECK(ntfs_lookup(v.root, "abs.lnk", &l) == 0);
	if (l) {
		n = 0;
		CHECK(ntfs_readlink(l, buf, sizeof(buf), &n) == -ERANGE);
		CHECK_MSG(n == strlen("/etc/hosts"),
			  "-ERANGE reported len_out %zu, so the caller cannot size a retry", n);
		/* buf == NULL is a size query. */
		n = 0;
		CHECK(ntfs_readlink(l, NULL, 0, &n) == 0);
		CHECK(n == strlen("/etc/hosts"));
		ntfs_inode_put(l);
		l = NULL;
	}

	/* Refusals. An empty target has nowhere to point; the length limit is
	 * PATH_MAX, checked in ntfs_symlink() before anything is written. */
	CHECK(ntfs_symlink(v.root, "empty.lnk", "", &l) == -EINVAL);
	memset(big, 'a', PATH_MAX - 1);
	big[PATH_MAX - 1] = 0;			/* PATH_MAX-1 bytes: the longest accepted */
	CHECK(ntfs_symlink(v.root, "max.lnk", big, &l) == 0);
	if (l) { CHECK(ntfs_getattr(l, &at) == 0);
		 CHECK(at.size == PATH_MAX - 1); ntfs_inode_put(l); l = NULL; }
	big[PATH_MAX - 1] = 'a';
	big[PATH_MAX] = 0;			/* one over */
	CHECK(ntfs_symlink(v.root, "toolong.lnk", big, &l) == -ENAMETOOLONG);
	CHECK(ntfs_lookup(v.root, "toolong.lnk", &l) == -ENOENT);
	CHECK(ntfs_symlink(v.root, "abs.lnk", "/x", &l) == -EEXIST);

	/*
	 * A symlink has no addressable data stream. The target lives in the
	 * reparse point and getattr reports the target length as the size, so a
	 * write to $DATA used to succeed and leave the two disagreeing: 14 bytes
	 * in the stream, size still 10, and the bytes survived a remount and
	 * ntfsck without anything ever reading them again. POSIX has no
	 * write-to-a-symlink operation -- the layer above resolves the link and
	 * writes the target -- so every door to that stream is refused here.
	 * Not EISDIR and not EPERM: EINVAL, which is what Linux gives for an
	 * operation the file type does not have.
	 */
	CHECK(ntfs_lookup(v.root, "abs.lnk", &l) == 0);
	if (l) {
		uint64_t was;

		CHECK(ntfs_getattr(l, &at) == 0);
		was = at.size;
		CHECK_MSG(ntfs_write(l, "CLOBBER", 7, 0) == -EINVAL,
			  "writing a symlink's $DATA was not refused");
		CHECK_MSG(ntfs_read(l, buf, sizeof(buf), 0) == -EINVAL,
			  "reading a symlink's $DATA was not refused");
		CHECK_MSG(ntfs_truncate(l, 3) == -EINVAL,
			  "truncating a symlink was not refused");
		CHECK_MSG(ntfs_fallocate(l, 0, 4096, false) == -EINVAL,
			  "fallocate on a symlink was not refused");
		/* Refused means nothing moved: the size the caller sees and the
		 * target itself are both still what they were. */
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG(at.size == was, "a refused write still changed size to %llu",
			  (unsigned long long)at.size);
		check_target(l, "/etc/hosts", "abs.lnk after refused writes");
		/* Mode and times stay settable: only the size is refused. */
		at.mode = 0100600;
		CHECK_MSG(ntfs_setattr(l, &at, NTFS_SETATTR_MODE) == 0,
			  "setting the mode of a symlink was refused too");
		CHECK_MSG(ntfs_setattr(l, &at, NTFS_SETATTR_SIZE) == -EINVAL,
			  "NTFS_SETATTR_SIZE on a symlink was not refused");
		ntfs_inode_put(l);
		l = NULL;
	}

	vol_down(&v);
	fsck_clean("symlink creation");

	/*
	 * Remount. On the way in the target comes from ntfs_make_symlink()
	 * decoding the reparse attribute, a completely different path from the
	 * in-memory ni->target used above, so this is the one that proves the
	 * link is on the disk rather than in the cache.
	 */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_symlinks: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	{
		static const struct { const char *name, *target; } want[] = {
			{ "abs.lnk",     "/etc/hosts" },
			{ "rel.lnk",     "../sib/y.txt" },
			{ "dangle.lnk",  "no/such/file" },
			{ "utf8.lnk",    "caf\xc3\xa9/\xe2\x98\x83.txt" },
			{ "bs.lnk",      "a\\b" },
			{ "to-real.lnk", "real.txt" },
		};
		for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
			if (ntfs_lookup(v.root, want[i].name, &l) != 0) {
				CHECK_MSG(0, "%s is gone after a remount", want[i].name);
				continue;
			}
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG(at.type == NTFS_ITEM_SYMLINK,
				  "%s came back as item type %d, not a symlink",
				  want[i].name, at.type);
			CHECK_MSG(at.reparse_tag != 0, "%s has no reparse tag", want[i].name);
			check_target(l, want[i].target, want[i].name);
			ntfs_inode_put(l);
			l = NULL;
		}
		/* The long one too: its target does not fit the base MFT record
		 * comfortably, which is where a truncation would show. */
		if (ntfs_lookup(v.root, "max.lnk", &l) == 0) {
			n = 0;
			CHECK(ntfs_readlink(l, NULL, 0, &n) == 0);
			CHECK_MSG(n == PATH_MAX - 1,
				  "the %d-byte target came back as %zu bytes", PATH_MAX - 1, n);
			ntfs_inode_put(l);
			l = NULL;
		} else {
			CHECK_MSG(0, "max.lnk is gone after a remount");
		}
	}

	/* Deletion: a symlink goes away with ntfs_unlink, and taking it away
	 * must not take its target with it. */
	CHECK(ntfs_unlink(v.root, "to-real.lnk") == 0);
	CHECK(ntfs_lookup(v.root, "to-real.lnk", &l) == -ENOENT);
	CHECK_MSG(ntfs_lookup(v.root, "real.txt", &f) == 0,
		  "deleting a symlink deleted what it pointed at");
	if (f) { ntfs_inode_put(f); f = NULL; }
	CHECK(ntfs_unlink(v.root, "dangle.lnk") == 0);
	CHECK(ntfs_lookup(v.root, "dangle.lnk", &l) == -ENOENT);
	/* rmdir must not accept one. */
	CHECK(ntfs_rmdir(v.root, "abs.lnk") == -ENOTDIR);

	vol_down(&v);
	/* A reparse point has an entry in $Extend/$Reparse as well as its
	 * attribute; ntfs_delete removes it, and ntfsck is what notices if it
	 * did not. */
	fsck_clean("symlink deletion");
	unlink(scratch);
	printf("test_symlinks\n");
}

/* ---- 5. timestamps -------------------------------------------------- */

/*
 * The conversion on its own, at values that can be checked by hand. An error
 * in the epoch constant or the 100 ns scale moves every file on the volume,
 * and both mistakes produce dates that still look like dates.
 */
static void test_time_conversion(void)
{
	static const struct {
		const char *what;
		int64_t sec;
		int32_t nsec;
		uint64_t ntfs;		/* 100 ns ticks since 1601-01-01 */
	} known[] = {
		/* The NTFS epoch itself. */
		{ "1601-01-01T00:00:00Z", -11644473600LL, 0, 0ULL },
		/* One tick. This is the resolution: 100 ns. */
		{ "1601-01-01T00:00:00.0000001Z", -11644473600LL, 100, 1ULL },
		/* The Unix epoch, i.e. the offset constant itself:
		 * 134774 days * 86400 s * 10^7 ticks. */
		{ "1970-01-01T00:00:00Z", 0, 0, 116444736000000000ULL },
		{ "2001-01-01T00:00:00Z", 978307200LL, 0, 126227808000000000ULL },
		/* Past the signed 32-bit second, where a 32-bit time_t wraps. */
		{ "2038-01-19T03:14:08Z", 2147483648LL, 0, 137919572480000000ULL },
		/* A sub-second value that is an exact number of ticks. */
		/* (1234567890 + 11644473600) * 10^7 + 1234567 */
		{ "2009-02-13T23:31:30.1234567Z", 1234567890LL, 123456700, 128790414901234567ULL },
	};
	size_t i;

	CHECK_MSG(NTFS_TIME_OFFSET == 11644473600LL,
		  "the 1601->1970 offset is %lld s, expected 11644473600",
		  (long long)NTFS_TIME_OFFSET);

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		struct timespec64 ts = { .tv_sec = known[i].sec, .tv_nsec = known[i].nsec };
		u64 got = le64_to_cpu(utc2ntfs(ts));
		struct timespec64 back = ntfs2utc(cpu_to_le64(known[i].ntfs));

		CHECK_MSG(got == known[i].ntfs,
			  "utc2ntfs(%s) = %llu, expected %llu", known[i].what,
			  (unsigned long long)got, (unsigned long long)known[i].ntfs);
		/*
		 * ntfs2utc() must return a NORMALISED timespec at every value,
		 * including before 1970. div_s64_rem() truncates toward zero, so
		 * unfixed it hands back a negative tv_nsec for any pre-1970
		 * instant off a whole second (tick 1 gave tv_sec = -11644473599,
		 * tv_nsec = -999999900). The instant was arithmetically right
		 * either way, which is why nothing on disk was ever damaged, but
		 * these fields are copied into struct ntfs_timespec and handed
		 * over the public ABI, and POSIX requires 0 <= nsec < 10^9.
		 * core/ntfs/time.h borrows a second; see the PORT: comment there.
		 */
		CHECK_MSG(back.tv_nsec >= 0 && back.tv_nsec < 1000000000L,
			  "ntfs2utc(%s) is denormalised: tv_nsec = %ld, must be in [0, 10^9)",
			  known[i].what, (long)back.tv_nsec);
		CHECK_MSG(back.tv_sec == known[i].sec && back.tv_nsec == known[i].nsec,
			  "ntfs2utc(%s) = %lld.%09ld, expected %lld.%09d",
			  known[i].what, (long long)back.tv_sec, (long)back.tv_nsec,
			  (long long)known[i].sec, known[i].nsec);
	}

	/*
	 * Normalising must not cost the round trip: borrowing a second moves a
	 * whole second from tv_sec into tv_nsec, so utc2ntfs() has to give the
	 * original tick count back. Walk the ticks either side of both epochs,
	 * which is where the sign of the remainder changes.
	 */
	{
		static const uint64_t edges[] = {
			0, 1, 2, 9999999, 10000000, 10000001, 12345,
			116444735999999999ULL, 116444736000000000ULL,
			116444736000000001ULL,
		};
		size_t j;

		for (j = 0; j < sizeof(edges) / sizeof(edges[0]); j++) {
			struct timespec64 ts = ntfs2utc(cpu_to_le64(edges[j]));
			u64 back = le64_to_cpu(utc2ntfs(ts));

			CHECK_MSG(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L,
				  "ntfs2utc(tick %llu) denormalised: %lld.%09ld",
				  (unsigned long long)edges[j],
				  (long long)ts.tv_sec, (long)ts.tv_nsec);
			CHECK_MSG(back == edges[j],
				  "tick %llu round-tripped to %llu",
				  (unsigned long long)edges[j],
				  (unsigned long long)back);
		}
	}

	/* Sub-tick precision is lost, and must be lost downwards: rounding up
	 * would make an mtime move forward every time the inode is written. */
	{
		struct timespec64 ts = { .tv_sec = 1000000000LL, .tv_nsec = 99 };
		struct timespec64 back = ntfs2utc(utc2ntfs(ts));

		CHECK_MSG(back.tv_sec == 1000000000LL && back.tv_nsec == 0,
			  "99 ns truncated to %lld.%09ld, expected .000000000",
			  (long long)back.tv_sec, (long)back.tv_nsec);
	}
	printf("test_time_conversion\n");
}

/*
 * And the same values through a real mount: set, unmount, mount again, read
 * back. This is what catches a timestamp that is written to
 * $STANDARD_INFORMATION but never to the FILE_NAME copies, or the reverse.
 */
static void test_timestamps(void)
{
	static const struct { const char *name; int64_t sec; int32_t nsec; } cases[] = {
		{ "epoch1601", -11644473600LL, 0 },
		{ "unixzero",  0, 0 },
		{ "y2009",     1234567890LL, 123456700 },
		{ "y2038",     2147483648LL, 0 },
		{ "y2500",     16725225600LL, 0 },
		/* Not a whole tick: must come back truncated, not rounded. */
		{ "subtick",   1000000000LL, 999999999 },
	};
	struct vol v;
	ntfs_inode_t *f = NULL;
	struct ntfs_attr at;
	size_t i;
	int64_t before_m;

	if (copy_fixture("timestamps") != 0) {
		fprintf(stderr, "SKIP test_timestamps: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_timestamps: mount failed\n");
		unlink(scratch);
		return;
	}

	/* A fresh file gets a creation time, and it is not zero: a crtime of
	 * 1601 is what a missing conversion looks like. */
	CHECK(ntfs_create(v.root, "fresh.txt", 0100644, &f) == 0);
	if (f) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.crtime.sec > 1600000000LL,
			  "a new file's crtime is %lld, which is not a plausible now",
			  (long long)at.crtime.sec);
		CHECK(at.mtime.sec > 1600000000LL);

		/* A write must move mtime forward. An mtime that does not move
		 * makes every backup and sync tool skip the changed file. */
		{
			struct ntfs_attr set;
			memset(&set, 0, sizeof(set));
			set.mtime.sec = 1000; set.mtime.nsec = 0;
			CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_MTIME) == 0);
			CHECK(ntfs_getattr(f, &at) == 0);
			before_m = at.mtime.sec;
			CHECK(before_m == 1000);
			CHECK(ntfs_write(f, "x", 1, 0) == 1);
			CHECK(ntfs_getattr(f, &at) == 0);
			CHECK_MSG(at.mtime.sec > before_m,
				  "mtime did not move on write (still %lld)",
				  (long long)at.mtime.sec);
		}
		ntfs_inode_put(f);
		f = NULL;
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct ntfs_attr set;

		if (ntfs_create(v.root, cases[i].name, 0100644, &f) != 0) {
			CHECK_MSG(0, "create %s failed", cases[i].name);
			continue;
		}
		memset(&set, 0, sizeof(set));
		set.atime.sec = set.mtime.sec = set.crtime.sec = cases[i].sec;
		set.atime.nsec = set.mtime.nsec = set.crtime.nsec = cases[i].nsec;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_ATIME | NTFS_SETATTR_MTIME |
					    NTFS_SETATTR_CRTIME) == 0);
		/* Before any write-back: setattr must be visible immediately. */
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.mtime.sec == cases[i].sec && at.mtime.nsec == cases[i].nsec,
			  "%s: setattr mtime read back as %lld.%09d in memory",
			  cases[i].name, (long long)at.mtime.sec, at.mtime.nsec);
		ntfs_inode_put(f);
		f = NULL;
	}
	vol_down(&v);
	fsck_clean("timestamp writes");

	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_timestamps: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		/* What the disk can hold: whole 100 ns ticks. */
		int64_t want_sec = cases[i].sec;
		int32_t want_nsec = cases[i].nsec / 100 * 100;

		if (ntfs_lookup(v.root, cases[i].name, &f) != 0) {
			CHECK_MSG(0, "%s is gone after a remount", cases[i].name);
			continue;
		}
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.mtime.sec == want_sec && at.mtime.nsec == want_nsec,
			  "%s: mtime off disk is %lld.%09d, expected %lld.%09d",
			  cases[i].name, (long long)at.mtime.sec, at.mtime.nsec,
			  (long long)want_sec, want_nsec);
		CHECK_MSG(at.atime.sec == want_sec && at.atime.nsec == want_nsec,
			  "%s: atime off disk is %lld.%09d, expected %lld.%09d",
			  cases[i].name, (long long)at.atime.sec, at.atime.nsec,
			  (long long)want_sec, want_nsec);
		CHECK_MSG(at.crtime.sec == want_sec && at.crtime.nsec == want_nsec,
			  "%s: crtime off disk is %lld.%09d, expected %lld.%09d",
			  cases[i].name, (long long)at.crtime.sec, at.crtime.nsec,
			  (long long)want_sec, want_nsec);
		ntfs_inode_put(f);
		f = NULL;
	}
	vol_down(&v);
	unlink(scratch);
	printf("test_timestamps\n");
}

/* ---- 6. permissions ------------------------------------------------- */

/*
 * noowners. NTFS has no POSIX mode, so core/vfs/api.c mode_of() synthesises
 * one from the mount's fmask/dmask every time, and fill_attr() reports the
 * mount's uid/gid. Nothing stored on the volume feeds into it. If the masks
 * ever stop being applied every file on a USB stick becomes world-writable
 * with no visible change anywhere else.
 */
static void test_permissions(void)
{
	struct vol v;
	ntfs_inode_t *f = NULL, *d = NULL, *l = NULL;
	struct ntfs_attr at, set;

	if (copy_fixture("perms") != 0) {
		fprintf(stderr, "SKIP test_permissions: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_permissions: mount failed\n");
		unlink(scratch);
		return;
	}

	/* The mode passed to ntfs_create is NOT what comes back: 0777 & ~fmask
	 * is. Asking for 0600 and being given 0755 is the contract, odd as it
	 * looks, and the caller must not be able to talk the driver into
	 * storing per-file permissions it cannot honour later. */
	CHECK(ntfs_create(v.root, "f.bin", 0100600, &f) == 0);
	if (f) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~022),
			  "file mode is %o, expected %o (0777 & ~fmask 022)",
			  at.mode & 07777, 0777 & ~022);
		CHECK((at.mode & S_IFMT) == S_IFREG);
		CHECK_MSG(at.uid == 501 && at.gid == 20,
			  "uid/gid are %u/%u, expected the mount's 501/20", at.uid, at.gid);
	}
	CHECK(ntfs_mkdir(v.root, "d", 0700, &d) == 0);
	if (d) {
		CHECK(ntfs_getattr(d, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~022),
			  "dir mode is %o, expected %o (0777 & ~dmask 022)",
			  at.mode & 07777, 0777 & ~022);
		CHECK((at.mode & S_IFMT) == S_IFDIR);
	}
	/* A symlink is always 0777: neither mask applies, because the mode of a
	 * symlink is never consulted for access on any POSIX system. */
	CHECK(ntfs_symlink(v.root, "s.lnk", "target", &l) == 0);
	if (l) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0777,
			  "symlink mode is %o, expected 0777", at.mode & 07777);
	}

	/*
	 * setattr(MODE) does not store a mode either. The only thing it can
	 * express on NTFS is the READONLY file attribute, taken from the write
	 * bits; the mode then comes back through the mask again, so clearing
	 * write on a volume mounted fmask=022 gives 0555, not 0400.
	 */
	if (f) {
		memset(&set, 0, sizeof(set));
		set.mode = 0100400;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_MODE) == 0);
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0555,
			  "after chmod 0400 the mode is %o, expected 0555", at.mode & 07777);
		CHECK_MSG(at.file_attributes & 0x1,
			  "chmod 0400 did not set the NTFS READONLY attribute (attrs %08x)",
			  at.file_attributes);

		memset(&set, 0, sizeof(set));
		set.mode = 0100644;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_MODE) == 0);
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0755,
			  "after chmod 0644 the mode is %o, expected 0755", at.mode & 07777);
		CHECK_MSG(!(at.file_attributes & 0x1),
			  "chmod 0644 did not clear READONLY (attrs %08x)", at.file_attributes);

		/* NTFS_SETATTR_UID/GID is accepted and ignored: noowners means
		 * there is nowhere to put it, and failing would break chown
		 * calls that macOS makes on its own. */
		memset(&set, 0, sizeof(set));
		set.uid = 4242; set.gid = 4242;
		CHECK(ntfs_setattr(f, &set, NTFS_SETATTR_UID | NTFS_SETATTR_GID) == 0);
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG(at.uid == 501 && at.gid == 20,
			  "a chown was honoured: uid/gid are now %u/%u", at.uid, at.gid);
	}
	if (f) { ntfs_inode_put(f); f = NULL; }
	if (d) { ntfs_inode_put(d); d = NULL; }
	if (l) { ntfs_inode_put(l); l = NULL; }
	vol_down(&v);

	/*
	 * Remount with different masks. The same files must report different
	 * modes: this is what proves the mode comes from the mount options and
	 * not from something that was stored at create time.
	 */
	if (vol_up(&v, 077, 027, 0, 0)) {
		fprintf(stderr, "FAIL test_permissions: remount failed\n");
		failures++; checks++;
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "f.bin", &f) == 0) {
		CHECK(ntfs_getattr(f, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~077),
			  "under fmask 077 the file mode is %o, expected %o",
			  at.mode & 07777, 0777 & ~077);
		CHECK_MSG(at.uid == 0 && at.gid == 0,
			  "under uid=0,gid=0 the file reports %u/%u", at.uid, at.gid);
		ntfs_inode_put(f);
		f = NULL;
	} else {
		CHECK_MSG(0, "f.bin is gone after a remount");
	}
	if (ntfs_lookup(v.root, "d", &d) == 0) {
		CHECK(ntfs_getattr(d, &at) == 0);
		CHECK_MSG((at.mode & 07777) == (0777 & ~027),
			  "under dmask 027 the dir mode is %o, expected %o",
			  at.mode & 07777, 0777 & ~027);
		ntfs_inode_put(d);
		d = NULL;
	} else {
		CHECK_MSG(0, "d is gone after a remount");
	}
	if (ntfs_lookup(v.root, "s.lnk", &l) == 0) {
		CHECK(ntfs_getattr(l, &at) == 0);
		CHECK_MSG((at.mode & 07777) == 0777,
			  "a symlink picked up fmask: mode is %o", at.mode & 07777);
		ntfs_inode_put(l);
		l = NULL;
	}
	vol_down(&v);
	unlink(scratch);
	printf("test_permissions\n");
}

/*
 * $EA migration for pre-e6b3418 symlinks (docs/GAPS.md §2.5).
 *
 * Before e6b3418 every link this driver created carried $LXUID/$LXGID/$LXMOD,
 * and chkdsk enforces [MS-FSA]'s rule that reparse points and extended
 * attributes are mutually exclusive by recomputing each directory index
 * entry's duplicated $FILE_NAME from the MFT record and writing the EA form
 * of the union (packed size 0x2d, reserved 0x0f) over the reparse tag.
 * Windows then lists the link as a zero-byte file. New links carry no $EA;
 * the migration strips it from an old link the first time it is loaded on a
 * read-write mount, and re-syncs the index entry to the real tag, so the
 * link reads as a link again without waiting for the next chkdsk.
 *
 * Nothing on this VM can create an old-style link -- the driver stopped
 * writing $EA on symlinks -- so the fixture is built the way the Interix
 * one is: a link is created through the driver, then
 * tools/mkfixtures/patch_ea.py inserts $EA_INFORMATION + $EA with exactly
 * the bytes ntfs_set_ea writes (verified against a regular file), and
 * tools/mkfixtures/index_union.py --set-ea-form writes the chkdsk damage
 * into the index entry. ntfsinfo reads the MFT copy, index_union.py reads
 * the index copy: the two copies are checked separately because they are
 * what chkdsk reconciles.
 *
 * Cases: the MFT strip ($EA_INFORMATION gone after one RW load), the index
 * repair (union back to the tag after the same load), the link still
 * resolving, a second link proving a read-only mount does not migrate, and
 * ntfsck clean throughout.
 */
static int index_union_of(const char *tool, const char *name, unsigned long *out)
{
	char cmd[PATH_MAX * 2 + 256];
	char line[256];
	FILE *p;

	snprintf(cmd, sizeof(cmd), "python3 %s %s %s 2>/dev/null",
		 tool, scratch, name);
	p = popen(cmd, "r");
	if (!p || !fgets(line, sizeof(line), p)) {
		if (p) pclose(p);
		return -1;
	}
	if (p) pclose(p);
	return sscanf(line, "union=0x%lx", out) == 1 ? 0 : -1;
}

static int ea_info_count(const char *info, const char *name)
{
	char cmd[PATH_MAX * 2 + 256];
	char line[64];
	FILE *p;
	int got = -1;

	snprintf(cmd, sizeof(cmd), "%s -F /%s %s 2>/dev/null | grep -c 'EA_INFORMATION'",
		 info, name, scratch);
	p = popen(cmd, "r");
	if (p && fgets(line, sizeof(line), p))
		got = (int)strtol(line, NULL, 10);
	if (p) pclose(p);
	return got;
}

static long mft_number_of(const char *info, const char *name)
{
	char cmd[PATH_MAX * 2 + 256];
	char line[64];
	FILE *p;
	long mftno = -1;

	snprintf(cmd, sizeof(cmd),
		 "%s -F /%s %s 2>/dev/null | grep -m1 'Dumping Inode' | grep -oE '[0-9]+' | head -1",
		 info, name, scratch);
	p = popen(cmd, "r");
	if (p && fgets(line, sizeof(line), p))
		mftno = strtol(line, NULL, 10);
	if (p) pclose(p);
	return mftno;
}

static void test_symlink_ea_migration(void)
{
	const char *info = getenv("NTFS_NTFSINFO");
	const char *patch_ea = getenv("NTFS_PATCH_EA");
	const char *index_union = getenv("NTFS_INDEX_UNION");
	struct vol v;
	ntfs_inode_t *l = NULL;
	char cmd[PATH_MAX * 2 + 256];
	unsigned long u = 0;
	int rc;

	printf("test_symlink_ea_migration\n");
	if (!info || !*info || access(info, X_OK) != 0 ||
	    !patch_ea || !*patch_ea || access(patch_ea, R_OK) != 0 ||
	    !index_union || !*index_union || access(index_union, R_OK) != 0) {
		fprintf(stderr, "SKIP test_symlink_ea_migration: NTFS_NTFSINFO/NTFS_PATCH_EA/NTFS_INDEX_UNION not set\n");
		return;
	}
	if (copy_fixture("eamig") != 0) {
		fprintf(stderr, "SKIP test_symlink_ea_migration: no fixture\n");
		return;
	}

	/* Phase 1: two links, then $EA inserted into both by hand. */
	if (vol_up(&v, 022, 022, 501, 20)) { unlink(scratch); return; }
	CHECK(write_plain(v.root, "target.txt", "hello\n", 6) == 0);
	CHECK(ntfs_symlink(v.root, "old.lnk", "target.txt", &l) == 0);
	if (l) { ntfs_inode_put(l); l = NULL; }
	CHECK(ntfs_symlink(v.root, "old2.lnk", "target.txt", &l) == 0);
	if (l) { ntfs_inode_put(l); l = NULL; }
	vol_down(&v);
	for (int i = 0; i < 2; i++) {
		const char *name = i ? "old2.lnk" : "old.lnk";
		long mftno = mft_number_of(info, name);
		int ea_count;

		CHECK_MSG(mftno > 0, "could not find the MFT number of %s", name);
		if (mftno <= 0) continue;
		snprintf(cmd, sizeof(cmd), "python3 %s %s %ld >/dev/null 2>&1",
			 patch_ea, scratch, mftno);
		rc = system(cmd);
		CHECK_MSG(rc == 0, "patch_ea.py failed for %s (rc %d)", name, rc);
		ea_count = ea_info_count(info, name);
		CHECK_MSG(ea_count == 1,
			  "%s: $EA_INFORMATION count %d, expected 1 before migration",
			  name, ea_count);
		CHECK_MSG(index_union_of(index_union, name, &u) == 0 && u == 0xA000000Cu,
			  "%s: index union 0x%08lx before damage, expected the tag",
			  name, u);
	}
	/* chkdsk damage on the first link only: the EA form of the union. */
	snprintf(cmd, sizeof(cmd), "python3 %s %s old.lnk --set-ea-form >/dev/null 2>&1",
		 index_union, scratch);
	rc = system(cmd);
	CHECK_MSG(rc == 0, "index_union.py --set-ea-form failed (rc %d)", rc);
	CHECK_MSG(index_union_of(index_union, "old.lnk", &u) == 0 && u == 0x000F002Du,
		  "old.lnk: index union 0x%08lx after damage, expected the EA form",
		  u);

	/* Phase 2: one RW load migrates the first link, MFT and index. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_symlink_ea_migration: remount failed\n");
		failures++; checks++; unlink(scratch); return;
	}
	{
		struct ntfs_attr at;
		char rbuf[64];
		size_t rn = 0;

		CHECK_MSG(ntfs_lookup(v.root, "old.lnk", &l) == 0, "lookup(old.lnk)");
		if (l) {
			CHECK(ntfs_getattr(l, &at) == 0);
			CHECK_MSG((at.mode & S_IFMT) == S_IFLNK,
				  "old.lnk: mode %o is not S_IFLNK after migration",
				  at.mode);
			CHECK_MSG(ntfs_readlink(l, rbuf, sizeof(rbuf), &rn) == 0,
				  "readlink(old.lnk)");
			CHECK_MSG(rn == strlen("target.txt") &&
				  !memcmp(rbuf, "target.txt", rn),
				  "old.lnk: readlink '%.*s', expected 'target.txt'",
				  (int)rn, rbuf);
			ntfs_inode_put(l); l = NULL;
		}
	}
	vol_down(&v);
	CHECK_MSG(ea_info_count(info, "old.lnk") == 0,
		  "old.lnk: $EA_INFORMATION still present after migration");
	CHECK_MSG(index_union_of(index_union, "old.lnk", &u) == 0 && u == 0xA000000Cu,
		  "old.lnk: index union 0x%08lx after migration, expected the tag",
		  u);
	fsck_clean("symlink $EA migration");

	/* Phase 3: the migrated link still resolves on a fresh mount. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_symlink_ea_migration: remount failed\n");
		failures++; checks++; unlink(scratch); return;
	}
	{
		char rbuf[64];
		size_t rn = 0;

		CHECK_MSG(ntfs_lookup(v.root, "old.lnk", &l) == 0, "lookup(old.lnk)");
		if (l) {
			CHECK_MSG(ntfs_readlink(l, rbuf, sizeof(rbuf), &rn) == 0,
				  "readlink(old.lnk) after migration");
			CHECK_MSG(rn == strlen("target.txt") &&
				  !memcmp(rbuf, "target.txt", rn),
				  "old.lnk: readlink '%.*s' after migration",
				  (int)rn, rbuf);
			ntfs_inode_put(l); l = NULL;
		}
	}
	vol_down(&v);

	/* Phase 4: a read-only mount must not migrate the second link. */
	if (vol_up_flags(&v, NTFS_MOUNT_RDONLY, 022, 022, 501, 20)) {
		fprintf(stderr, "FAIL test_symlink_ea_migration: read-only remount failed\n");
		failures++; checks++; unlink(scratch); return;
	}
	CHECK_MSG(ntfs_lookup(v.root, "old2.lnk", &l) == 0, "lookup(old2.lnk)");
	if (l) { ntfs_inode_put(l); l = NULL; }
	vol_down(&v);
	{
		int ea_count = ea_info_count(info, "old2.lnk");

		CHECK_MSG(ea_count == 1,
			  "old2.lnk: $EA_INFORMATION count %d after a read-only mount, expected 1 (unmigrated)",
			  ea_count);
	}
	fsck_clean("read-only mount leaves $EA alone");
	unlink(scratch);
}

/* ---- 15. MFT record reuse validates usa_ofs (upstream 81684340963d) --- */

/*
 * When ntfs_mft_record_alloc() reuses a free MFT record it reads the old
 * update sequence number straight from the on-disk record:
 * usn = *(__le16 *)((u8 *)m + le16_to_cpu(m->usa_ofs)). The folio still
 * holds unvalidated, MST-protected bytes there, so a corrupted usa_ofs
 * reads outside the record (KASAN use-after-free on a real volume).
 *
 * Deterministic trigger: mount sets vol->mft_data_pos = 24, so after a
 * remount the allocator reuses the lowest free record >= 24. Create A
 * (record R, discovered via getattr -- 295 on basic-4k.img, whose $MFT
 * bitmap already has records 24..294 in use), C (record R+1) and D
 * (record R+2, a live "FILE" record), delete C, unmount, patch C's
 * usa_ofs to mft_record_size (1024). R+1 is a multiple of 4, so its
 * record starts a 4 KiB page and the unguarded read at m+1024 lands on
 * the first two bytes of record R+2 -- "FI" = 0x4946 -- deterministically
 * (a non-page-aligned target would read past the mapped folio instead,
 * which is the same bug but with a heap-garbage value). Remount, create
 * B: it reuses record R+1. Without the fix the preserved usn 0x4946 is
 * restored into B's record (and then incremented by each
 * pre_write_mst_fixup, so it reads back as 0x4948); with the fix usn is
 * left 0 and the record keeps a fresh small USN (layout sets 1, each
 * fixup increments it: 3 in this flow). The remount between patch and
 * create matters: the allocator reads through the page cache, so
 * patching while mounted would be invisible.
 */
static uint16_t le16_at(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return le16_to_cpu(v);
}

static void test_mft_alloc_usa_ofs_validated(void)
{
	struct vol v;
	ntfs_inode_t *a = NULL, *c = NULL, *d = NULL, *b = NULL;
	struct ntfs_attr attr;
	struct ntfs_image img;
	uint64_t rec_no = 0, target = 0, rec_off;
	uint8_t rec[512];
	uint16_t usa_ofs, usn, patch;
	int fd;

	printf("test_mft_alloc_usa_ofs_validated\n");
	if (copy_fixture("usaofs") != 0) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: mount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_create(v.root, "a.txt", 0100644, &a) == 0);
	if (a) {
		CHECK(ntfs_getattr(a, &attr) == 0);
		rec_no = attr.inode_no;
		ntfs_inode_put(a);
		a = NULL;
	}
	CHECK(ntfs_create(v.root, "c.txt", 0100644, &c) == 0);
	if (c) {
		CHECK(ntfs_getattr(c, &attr) == 0);
		CHECK(attr.inode_no == rec_no + 1);
		target = attr.inode_no;
		/* The target must start a page, or the unguarded read lands
		 * past the mapped folio instead of on the next record. */
		CHECK(target % 4 == 0);
		ntfs_inode_put(c);
		c = NULL;
	}
	CHECK(ntfs_create(v.root, "d.txt", 0100644, &d) == 0);
	if (d) {
		CHECK(ntfs_getattr(d, &attr) == 0);
		/* D must be the very next record: its first two bytes ("FI")
		 * are what the unguarded read lands on. */
		CHECK(attr.inode_no == rec_no + 2);
		ntfs_inode_put(d);
		d = NULL;
	}
	CHECK(ntfs_unlink(v.root, "c.txt") == 0);
	vol_down(&v);

	/* Patch the target record's usa_ofs (u16 at record offset 4). */
	if (ntfs_image_open(&img, scratch, true, 0, 0) != 0) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: cannot open image\n");
		unlink(scratch);
		return;
	}
	rec_off = ntfs_image_map(img.mft_runs, img.n_mft_runs, img.cluster_size,
				 target * img.mft_record_size);
	if (rec_off == UINT64_MAX) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: cannot map record %llu\n",
			(unsigned long long)target);
		ntfs_image_close(&img);
		unlink(scratch);
		return;
	}
	patch = cpu_to_le16((uint16_t)img.mft_record_size);
	fd = open(scratch, O_RDWR);
	if (fd < 0 || pwrite(fd, &patch, sizeof(patch), (off_t)(rec_off + 4)) != (ssize_t)sizeof(patch)) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: cannot patch image\n");
		if (fd >= 0)
			close(fd);
		ntfs_image_close(&img);
		unlink(scratch);
		return;
	}
	close(fd);
	ntfs_image_close(&img);

	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: remount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_create(v.root, "b.txt", 0100644, &b) == 0);
	if (b) {
		CHECK(ntfs_getattr(b, &attr) == 0);
		CHECK(attr.inode_no == target);
		ntfs_inode_put(b);
		b = NULL;
	}
	vol_down(&v);

	/* Read the target record back from the image. */
	if (ntfs_image_open(&img, scratch, false, 0, 0) != 0) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: cannot reopen image\n");
		unlink(scratch);
		return;
	}
	rec_off = ntfs_image_map(img.mft_runs, img.n_mft_runs, img.cluster_size,
				 target * img.mft_record_size);
	fd = open(scratch, O_RDONLY);
	if (fd < 0 || pread(fd, rec, sizeof(rec), (off_t)rec_off) != (ssize_t)sizeof(rec)) {
		fprintf(stderr, "SKIP test_mft_alloc_usa_ofs_validated: cannot read record %llu\n",
			(unsigned long long)target);
		if (fd >= 0)
			close(fd);
		ntfs_image_close(&img);
		unlink(scratch);
		return;
	}
	close(fd);
	ntfs_image_close(&img);
	usa_ofs = le16_at(rec + 4);
	CHECK(usa_ofs != 0);
	CHECK(usa_ofs < sizeof(rec));
	if (usa_ofs >= sizeof(rec)) {
		unlink(scratch);
		return;
	}
	usn = le16_at(rec + usa_ofs);
	/* Fresh sequence number, not the neighbour's bytes: layout sets 1
	 * and each pre_write_mst_fixup() cyclically increments it (3 in
	 * this flow: layout, the alloc write, one more flush). The
	 * unguarded read would have restored 0x4946 ("FI") and the same
	 * increments would leave 0x4948 -- far above any genuine count
	 * for a record written a handful of times. */
	CHECK(usn >= 1 && usn < 0x100);
	if (!(usn >= 1 && usn < 0x100))
		fprintf(stderr, "FAIL usa_ofs test: reused record USN 0x%04x, expected a fresh small value\n", usn);
	fsck_clean("usaofs");
	unlink(scratch);
}

/* ---- 16. Error-marked MFT references are normalized (upstream d97a36bae86a) --- */

/*
 * ntfs_lookup_inode_by_name() returns MFT references read from directory
 * index entries on disk. A crafted image can set the MREF error bit while
 * leaving the low bits arbitrary; callers decode lookup failures with
 * MREF_ERR() and would consume a bogus pseudo-errno. The fix normalizes
 * every error-marked reference to ERR_MREF(-EIO) at the source, in all
 * four lookup return paths.
 *
 * The patcher sets the $I30 entry for victim.txt to error-bit | 0x42.
 * With the fix ntfs_lookup() returns -EIO; without it MREF_ERR() is 66,
 * a positive "error" IS_ERR() does not recognise, so ntfs_lookup reports
 * success (err 0) with a garbage inode pointer. The test checks err and
 * that no handle came back, and never touches the handle: on the no-fix
 * path it is bogus. No fsck_clean here -- the image is deliberately
 * corrupt, and ntfsck would flag the index entry disagreeing with the
 * file's own $FILE_NAME.
 */
static void test_lookup_err_mref_normalized(void)
{
	struct vol v;
	ntfs_inode_t *f = NULL;
	const char *py = getenv("NTFS_PATCH_ERR_MREF");
	char cmd[PATH_MAX * 2 + 256];
	int rc, err;

	printf("test_lookup_err_mref_normalized\n");
	if (!py || !*py || access(py, R_OK) != 0) {
		fprintf(stderr, "SKIP test_lookup_err_mref_normalized: NTFS_PATCH_ERR_MREF not set\n");
		return;
	}
	if (copy_fixture("errmref") != 0) {
		fprintf(stderr, "SKIP test_lookup_err_mref_normalized: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_lookup_err_mref_normalized: mount failed\n");
		unlink(scratch);
		return;
	}
	CHECK(ntfs_create(v.root, "victim.txt", 0100644, &f) == 0);
	if (f) {
		ntfs_inode_put(f);
		f = NULL;
	}
	vol_down(&v);

	snprintf(cmd, sizeof(cmd), "python3 %s %s victim.txt 0x42 >/dev/null 2>&1",
		 py, scratch);
	rc = system(cmd);
	CHECK_MSG(rc == 0, "patch_err_mref.py failed (rc %d)", rc);
	if (rc != 0) {
		unlink(scratch);
		return;
	}

	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_lookup_err_mref_normalized: remount failed\n");
		unlink(scratch);
		return;
	}
	err = ntfs_lookup(v.root, "victim.txt", &f);
	CHECK(err == -EIO);
	CHECK(f == NULL);
	if (err != -EIO)
		fprintf(stderr, "FAIL err-mref lookup: ntfs_lookup returned %d, expected -EIO (-5)\n", err);
	/* Never ntfs_inode_put(f): on the no-fix path it is a bogus handle. */
	vol_down(&v);
	unlink(scratch);
}

/* ---- 17. Non-resident attribute validation (upstream a83e82b0ec3a + ffc755828de1) --- */

/*
 * The port's inline validator in ntfs_attr_find() checked only
 * mapping_pairs_offset bounds, with min_len 64 for every non-resident
 * attribute. Upstream ffc755828de1 raises min_len to 72 for
 * sparse/compressed attributes (the compressed_size field), and upstream
 * a83e82b0ec3a requires the name to lie within the attribute without
 * overlapping the mapping pairs, plus 8 bytes of room after
 * mapping_pairs_offset for the compressed_size field a conversion to
 * sparse would add. A failed validation makes ntfs_attr_find() return
 * -EIO ("is corrupt. Run chkdsk.").
 *
 * Three cases, each on its own fixture copy (a validation failure sets
 * the volume error flag, so cases must not share a mount):
 *   room64:    unnamed $DATA length := 64, mp_offset stays 64, so room
 *              after it is 0 < 8 -- the new room check rejects it.
 *   sparse64:  same, plus ATTR_IS_SPARSE set -- the room check is
 *              skipped for sparse attributes, so only the new min_len-72
 *              check rejects it; this isolates ffc755828de1.
 *   mpinside:  a large xattr's named $DATA (ADS) has its
 *              mapping_pairs_offset := name_offset + name_length,
 *              strictly inside the name, so name_end > mp_offset --
 *              the new name check rejects it. The name itself is
 *              untouched, so the name comparison still matches and the
 *              validator is reached; mp_offset stays > name_offset,
 *              so ntfs_attr_iget's own check passes and the old code's
 *              size query succeeds. (mp_offset := 64 would also trip
 *              the new check, but ntfs_attr_iget already refuses
 *              name_offset >= mp_offset, so it fails in both builds.)
 * Without the fix all three operations succeed; with it they fail with
 * -EIO. The trigger must stop at the attribute find: ntfs_lookup() for
 * the unnamed $DATA (iget looks it up via ntfs_attr_lookup) and a
 * getxattr SIZE query (buf NULL) for the ADS. A read must NOT be the
 * trigger: the mapping-pairs decompressor bounds the pairs by the
 * attribute length, so a length-64 $DATA fails there in BOTH builds,
 * and a garbage runlist from the name bytes fails the ADS read in
 * both builds. No fsck_clean: the images are deliberately corrupt.
 */
static int nonres_case_setup(struct vol *v, const char *tag, bool with_xattr,
			     uint64_t *rec_no_out)
{
	ntfs_inode_t *f = NULL;
	struct ntfs_attr attr;
	static char big[8192];
	int err = 0;

	memset(big, 'A', sizeof(big));
	if (copy_fixture(tag) != 0)
		return -1;
	if (vol_up(v, 022, 022, 501, 20))
		return -1;
	if (ntfs_create(v->root, "f.bin", 0100644, &f) != 0)
		err = -1;
	if (!err && ntfs_write(f, big, sizeof(big), 0) != (ssize_t)sizeof(big))
		err = -1;
	if (!err && ntfs_getattr(f, &attr) == 0)
		*rec_no_out = attr.inode_no;
	else if (!err)
		err = -1;
	if (!err && with_xattr &&
	    ntfs_setxattr(f, "user.big", big, sizeof(big), 0) != 0)
		err = -1;
	if (f) {
		ntfs_inode_put(f);
		f = NULL;
	}
	vol_down(v);
	return err;
}

static int nonres_case_patch(const char *mode, uint64_t rec_no, const char *name)
{
	const char *py = getenv("NTFS_PATCH_NONRES_ATTR");
	char cmd[PATH_MAX * 2 + 256];
	int rc;

	if (!py || !*py || access(py, R_OK) != 0)
		return -2;
	snprintf(cmd, sizeof(cmd), "python3 %s %s %llu %s %s >/dev/null 2>&1",
		 py, scratch, (unsigned long long)rec_no, mode, name);
	rc = system(cmd);
	return rc == 0 ? 0 : -1;
}

static void test_nonres_attr_validation(void)
{
	static const struct {
		const char *tag, *mode, *what;
		bool with_xattr;
	} cases[] = {
		{ "nonres-room", "room64", "room after mapping pairs", false },
		{ "nonres-sparse", "sparse64", "sparse min_len 72", false },
		{ "nonres-mp", "mpinside", "name overlaps mapping pairs", true },
	};
	struct vol v;
	size_t i, n = sizeof(cases) / sizeof(cases[0]);

	printf("test_nonres_attr_validation\n");
	for (i = 0; i < n; i++) {
		ntfs_inode_t *f = NULL;
		uint64_t rec_no = 0;
		size_t len = 0;
		int err, prc;

		if (nonres_case_setup(&v, cases[i].tag, cases[i].with_xattr, &rec_no) != 0) {
			fprintf(stderr, "SKIP nonres %s: setup failed\n", cases[i].what);
			unlink(scratch);
			continue;
		}
		prc = nonres_case_patch(cases[i].mode, rec_no, "user.big");
		if (prc == -2) {
			fprintf(stderr, "SKIP test_nonres_attr_validation: NTFS_PATCH_NONRES_ATTR not set\n");
			unlink(scratch);
			return;
		}
		CHECK_MSG(prc == 0, "patch_nonres_attr.py %s failed", cases[i].mode);
		if (prc != 0) {
			unlink(scratch);
			continue;
		}
		if (vol_up(&v, 022, 022, 501, 20)) {
			fprintf(stderr, "SKIP nonres %s: remount failed\n", cases[i].what);
			unlink(scratch);
			continue;
		}
		err = ntfs_lookup(v.root, "f.bin", &f);
		if (cases[i].with_xattr) {
			/* The file lookup itself must succeed (its unnamed
			 * $DATA is intact); the ADS size query is the
			 * trigger -- it stops at the attribute find and
			 * never reads the (garbage) runlist. */
			CHECK(err == 0);
			if (err == 0 && f) {
				err = ntfs_getxattr(f, "user.big", NULL, 0, &len);
				ntfs_inode_put(f);
				f = NULL;
			}
		} else if (err == 0 && f) {
			ntfs_inode_put(f);
			f = NULL;
		}
		CHECK(err == -EIO);
		if (err != -EIO)
			fprintf(stderr, "FAIL nonres %s: lookup/read returned %d, expected -EIO (-5)\n",
				cases[i].what, err);
		vol_down(&v);
		unlink(scratch);
	}
}

/*
 * Upstream 650a2751c9ae: ntfs_set_ea() used to check only the NEW
 * entry's size against the $AttrDef $EA limit (64 KiB), and only at
 * the end -- after a replacement had already removed the old entry
 * and committed the shortened $EA stream and $EA_INFORMATION. A
 * replacement whose FINAL stream exceeds the limit therefore
 * destroyed the old value instead of failing cleanly, and an add
 * whose final stream exceeds the limit was only caught at the
 * $EA resize in the write path, failing with -EIO and setting the
 * volume error flag instead of a clean -EFBIG. The fix checks the
 * FINAL size before mutating: ea_info_qsize - ea_size +
 * new_ea_size on replace, ea_info_qsize + new_ea_size on add, and the
 * trailing check keeps only the ea_packed > 0xffff test.
 *
 * Entry point: the public ntfs_setxattr() stores xattrs as ADS
 * (named $DATA), so it never reaches ntfs_set_ea(). $EA is reached
 * through the VFS xattr handler (ntfs_xattr_handlers[0]), which is
 * the upstream-representative path (upstream's setxattr handler is
 * exactly this function) and is also what WSL-inode creation and
 * ACL sets use internally. The test drives handlers[0]->set/get
 * directly; the ntfs_inode_t handle IS the struct inode (see VI()
 * in core/vfs/api.c), so the cast is the glue's own identity.
 *
 * The $EA max is 65536 (verified from the fixture's $AttrDef with
 * ntfscat). ea_packed (5 + name + value per entry) and the stream
 * size (ALIGN(9 + name + value, 4) per entry) differ by ~4 bytes per
 * entry, so the totals are chosen so the stream crosses 65536 while
 * the packed total stays <= 65535 -- otherwise the trailing packed
 * check fires in BOTH builds and the test is vacuous. Note
 * ntfs_create() also writes three hidden WSL EAs ($LXUID/$LXGID/
 * $LXMOD, 6-char name + 4-byte value each: 15 packed / 20 stream),
 * a fixed +45 packed / +60 stream on top of the test's own EAs:
 *   add:     7 x 8192-value EAs (stream 8208, packed 8204 each;
 *            stream 57516, packed 57473 with the $LX* EAs), then
 *            add an 8040-value EA (stream 8056, packed 8052):
 *            final stream 65572 > 65536, final packed 65525
 *            <= 65535. New: -EFBIG before writing. Old: -EIO from
 *            the $EA resize, plus the volume error flag; the 8th
 *            EA is absent in both, so the return code
 *            discriminates.
 *   replace: 15 x 4096-value EAs (stream 4116, packed 4109 each;
 *            stream 61800, packed 61680 with the $LX* EAs), then
 *            replace one with an 8192-value EA (stream 8212,
 *            packed 8205): final stream 65896 > 65536, final
 *            packed 65776 > 65535. Both builds return -EFBIG, but
 *            the new build returns it BEFORE mutating (old value
 *            intact) while the old build commits the shortened
 *            stream first (old value gone).
 * No fixture patcher needed.
 */
static int ea_set(ntfs_inode_t *f, const char *name, const void *val,
		  size_t size)
{
	const struct xattr_handler *h = ntfs_xattr_handlers[0];

	return h->set(h, NULL, NULL, (struct inode *)f, name, val, size, 0);
}

static int ea_get(ntfs_inode_t *f, const char *name, void *buf, size_t size)
{
	const struct xattr_handler *h = ntfs_xattr_handlers[0];

	return h->get(h, NULL, (struct inode *)f, name, buf, size);
}

static void test_ea_final_size_validated(void)
{
	static char val4k[4096], val8k[8192], val8040[8040];
	struct vol v;
	ntfs_inode_t *f = NULL;
	char name[16], buf[8192];
	int err, i;

	printf("test_ea_final_size_validated\n");
	memset(val4k, 'A', sizeof(val4k));
	memset(val8k, 'A', sizeof(val8k));
	memset(val8040, 'A', sizeof(val8040));

	/* Each case runs on its own fixture copy: in the old build the
	 * add case sets the volume error flag (see below), which would
	 * poison the replace case on a shared mount. */

	/* Add case: 7 x 8192, then an 8040 that would push the stream
	 * past 64 KiB. */
	if (copy_fixture("basic-4k.img") != 0) {
		fprintf(stderr, "SKIP test_ea_final_size_validated: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_ea_final_size_validated: mount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_create(v.root, "ea.bin", 0100644, &f) != 0 || !f) {
		fprintf(stderr, "SKIP test_ea_final_size_validated: create failed\n");
		vol_down(&v);
		unlink(scratch);
		return;
	}
	for (i = 0; i < 7; i++) {
		snprintf(name, sizeof(name), "user.b%d", i);
		if (ea_set(f, name, val8k, sizeof(val8k)) != 0) {
			fprintf(stderr, "SKIP test_ea_final_size_validated: setup add %d failed\n", i);
			ntfs_inode_put(f);
			vol_down(&v);
			unlink(scratch);
			return;
		}
	}
	/* New: hunk 2 rejects the add with -EFBIG before writing.
	 * Old: the add proceeds, the $EA resize to 65572 trips the
	 * $AttrDef bounds check in the write path and fails with -EIO
	 * (and sets the volume error flag). Either way the 8th EA is
	 * absent; the return code is the discriminator. */
	err = ea_set(f, "user.b7", val8040, sizeof(val8040));
	CHECK(err == -EFBIG);
	if (err != -EFBIG)
		fprintf(stderr, "FAIL ea add: set returned %d, expected -EFBIG (-27)\n", err);
	err = ea_get(f, "user.b7", buf, sizeof(buf));
	CHECK(err < 0);
	if (err >= 0)
		fprintf(stderr, "FAIL ea add: over-limit EA present (%d bytes)\n", err);
	ntfs_inode_put(f);
	f = NULL;
	vol_down(&v);
	unlink(scratch);

	/* Replace case: 15 x 4096, then replace one with 8192. The new
	 * value differs from the old one, so the identical-value early
	 * return cannot mask the check. */
	if (copy_fixture("basic-4k.img") != 0) {
		fprintf(stderr, "SKIP test_ea_final_size_validated: no fixture (replace)\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_ea_final_size_validated: mount failed (replace)\n");
		unlink(scratch);
		return;
	}
	if (ntfs_create(v.root, "ea2.bin", 0100644, &f) != 0 || !f) {
		fprintf(stderr, "SKIP test_ea_final_size_validated: create2 failed\n");
		vol_down(&v);
		unlink(scratch);
		return;
	}
	for (i = 0; i < 15; i++) {
		snprintf(name, sizeof(name), "user.a%02d", i);
		if (ea_set(f, name, val4k, sizeof(val4k)) != 0) {
			fprintf(stderr, "SKIP test_ea_final_size_validated: setup replace %d failed\n", i);
			ntfs_inode_put(f);
			vol_down(&v);
			unlink(scratch);
			return;
		}
	}
	memset(val8k, 'B', sizeof(val8k));
	err = ea_set(f, "user.a00", val8k, sizeof(val8k));
	CHECK(err == -EFBIG);
	if (err != -EFBIG)
		fprintf(stderr, "FAIL ea replace: set returned %d, expected -EFBIG (-27)\n", err);
	/* The old value must still be there, intact. */
	err = ea_get(f, "user.a00", buf, sizeof(buf));
	CHECK(err == (int)sizeof(val4k));
	if (err != (int)sizeof(val4k))
		fprintf(stderr, "FAIL ea replace: old value %s (get=%d, expected 4096)\n",
			err < 0 ? "gone" : "shortened", err);
	ntfs_inode_put(f);
	vol_down(&v);
	unlink(scratch);
}

/*
 * Fork-found bug (no upstream fix in the v7.1 -> v7.3 drift):
 * ntfs_set_ea() clears the HasEA flag when creating the first $EA
 * on a file that has none. The local ea_info_qsize is never updated
 * on the create path (only p_ea_info->ea_query_length is), so the
 * out: label sees 0 and calls NInoClearHasEA even though the $EA
 * was just created; ntfs_get_ea() then returns -ENODATA on the same
 * mount until remount (the inode reload sets HasEA from the on-disk
 * $EA). The fix is one line in core/ntfs/ea.c: keep the local
 * ea_info_qsize in sync with the $EA_INFORMATION just written.
 *
 * The file must come from the fixture, not ntfs_create():
 * driver-created files already carry the three hidden WSL EAs, so
 * their $EA exists and the set goes through ntfs_write_ea()
 * instead of the create path. /files/sz/0 in basic-4k.img was
 * created by mkfixtures/libntfs-3g and has no $EA (the pre-set get
 * returns -ENODATA).
 *
 * Placement matters, and this test guards it: the increment must
 * sit AFTER the $EA write, because the append branch writes the
 * new entry at offset ea_info_qsize. Incrementing earlier lands
 * the entry one entry too far and leaves a gap the lookup never
 * crosses -- the second (add-path) get below fails -ENODATA with
 * the stream one entry longer than $EA_INFORMATION claims.
 *
 * Note: on a set that fails after the query_length update the flag
 * can now be set while the stream was not written; that is the
 * failure-path hole upstream ac727d86fb84 closes (leave HasEA
 * untouched on setxattr failure), still in the port queue.
 */
static void test_ea_first_set_same_mount_get(void)
{
	static char val[64];
	struct vol v;
	ntfs_inode_t *files = NULL, *sz = NULL, *f = NULL;
	char buf[64];
	int err;

	printf("test_ea_first_set_same_mount_get\n");
	memset(val, 'C', sizeof(val));
	if (copy_fixture("hasea") != 0) {
		fprintf(stderr, "SKIP test_ea_first_set_same_mount_get: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_ea_first_set_same_mount_get: mount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "files", &files) != 0 || !files ||
	    ntfs_lookup(files, "sz", &sz) != 0 || !sz ||
	    ntfs_lookup(sz, "0", &f) != 0 || !f) {
		fprintf(stderr, "SKIP test_ea_first_set_same_mount_get: lookup failed\n");
		if (f) ntfs_inode_put(f);
		if (sz) ntfs_inode_put(sz);
		if (files) ntfs_inode_put(files);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	/* Sanity: the fixture file has no $EA yet. */
	err = ea_get(f, "user.first", buf, sizeof(buf));
	CHECK(err == -ENODATA);
	if (err != -ENODATA) {
		fprintf(stderr, "SKIP test_ea_first_set_same_mount_get: fixture file already has an EA (get=%d)\n", err);
		ntfs_inode_put(f);
		ntfs_inode_put(sz);
		ntfs_inode_put(files);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	/* First $EA on a file with none: takes the create_ea_info path. */
	err = ea_set(f, "user.first", val, sizeof(val));
	CHECK(err == 0);
	if (err != 0)
		fprintf(stderr, "FAIL hasea: set returned %d, expected 0\n", err);
	/* Same mount, no remount: the discriminator. Without the fix
	 * this returns -ENODATA (-61) because HasEA was cleared. */
	err = ea_get(f, "user.first", buf, sizeof(buf));
	CHECK(err == (int)sizeof(val));
	if (err != (int)sizeof(val))
		fprintf(stderr, "FAIL hasea: same-mount get returned %d, expected %d\n",
			err, (int)sizeof(val));
	if (err == (int)sizeof(val)) {
		CHECK(memcmp(buf, val, sizeof(val)) == 0);
		if (memcmp(buf, val, sizeof(val)) != 0)
			fprintf(stderr, "FAIL hasea: same-mount get value mismatch\n");
	}
	/* A second EA on the same file takes the add path; the write
	 * offset there is ea_info_qsize, so this also guards the fix
	 * placement (see the comment above). */
	err = ea_set(f, "user.second", val, sizeof(val));
	CHECK(err == 0);
	if (err != 0)
		fprintf(stderr, "FAIL hasea: second set returned %d, expected 0\n", err);
	err = ea_get(f, "user.second", buf, sizeof(buf));
	CHECK(err == (int)sizeof(val));
	if (err != (int)sizeof(val))
		fprintf(stderr, "FAIL hasea: second get returned %d, expected %d\n",
			err, (int)sizeof(val));
	ntfs_inode_put(f);
	ntfs_inode_put(sz);
	ntfs_inode_put(files);
	f = sz = files = NULL;
	vol_down(&v);

	/* Remount: both EAs have to parse back from disk. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_ea_first_set_same_mount_get: remount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "files", &files) != 0 || !files ||
	    ntfs_lookup(files, "sz", &sz) != 0 || !sz ||
	    ntfs_lookup(sz, "0", &f) != 0 || !f) {
		fprintf(stderr, "FAIL test_ea_first_set_same_mount_get: lookup after remount failed\n");
		failures++; checks++;
		if (f) ntfs_inode_put(f);
		if (sz) ntfs_inode_put(sz);
		if (files) ntfs_inode_put(files);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	err = ea_get(f, "user.first", buf, sizeof(buf));
	CHECK(err == (int)sizeof(val));
	if (err != (int)sizeof(val))
		fprintf(stderr, "FAIL hasea: remount get (first) returned %d, expected %d\n",
			err, (int)sizeof(val));
	err = ea_get(f, "user.second", buf, sizeof(buf));
	CHECK(err == (int)sizeof(val));
	if (err != (int)sizeof(val))
		fprintf(stderr, "FAIL hasea: remount get (second) returned %d, expected %d\n",
			err, (int)sizeof(val));
	ntfs_inode_put(f);
	ntfs_inode_put(sz);
	ntfs_inode_put(files);
	vol_down(&v);
	unlink(scratch);
}

/*
 * Upstream 56cc42a7b387 ("ntfs: return errors from ntfs_attr_readall"):
 * ntfs_attr_readall() returned NULL on every failure -- attribute
 * lookup, allocation and read -- so every caller lost the errno and
 * substituted its own: ntfs_get_ea() reported -ENODATA for a failed
 * $EA read, ntfs_listxattr() reported an empty list, and the reparse
 * parser treated an allocation/I/O error as "no reparse data". The
 * fix returns ERR_PTR() with the original error, and every caller
 * propagates it (ntfs_get_ea / ntfs_set_ea / ntfs_listxattr in ea.c,
 * ntfs_ibm_get_free in index.c, the two reparse.c sites; the
 * port-only readlink fallback in core/vfs/api.c normalises the
 * ERR_PTR to NULL because its out: path kvfrees the pointer).
 *
 * The failure is produced by hand, the way a crafted image would:
 * a file with one 8 KiB EA has a non-resident $EA stream (an 8.2 KiB
 * stream cannot fit in the MFT record), and
 * tools/mkfixtures/patch_oob_ea.py rewrites its first run to start
 * past the end of the volume, so the pread inside readall fails
 * with -EIO (the runlist decoder refuses the out-of-volume LCN).
 * Without the fix the get returns -ENODATA; with it, -EIO. The set
 * path propagates too: replacing the EA must fail with the read
 * error instead of silently dropping the stream. No fsck_clean:
 * the image is deliberately corrupt by design.
 */
static void test_readall_error_propagates(void)
{
	static char val8k[8192];
	struct vol v;
	ntfs_inode_t *f = NULL;
	uint64_t mft_no;
	char cmd[2048], out[256];
	const char *py = getenv("NTFS_PATCH_OOB_EA");
	char buf[8192];
	int err;
	FILE *p;
	int rc;

	printf("test_readall_error_propagates\n");
	if (!py || !*py || access(py, R_OK) != 0) {
		fprintf(stderr, "SKIP test_readall_error_propagates: NTFS_PATCH_OOB_EA not set\n");
		return;
	}
	memset(val8k, 'A', sizeof(val8k));
	if (copy_fixture("readall-ea") != 0) {
		fprintf(stderr, "SKIP test_readall_error_propagates: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_readall_error_propagates: mount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_create(v.root, "ea3.bin", 0100644, &f) != 0 || !f) {
		fprintf(stderr, "SKIP test_readall_error_propagates: create failed\n");
		vol_down(&v);
		unlink(scratch);
		return;
	}
	mft_no = ntfs_inode_number(f);
	if (ea_set(f, "user.big", val8k, sizeof(val8k)) != 0) {
		fprintf(stderr, "SKIP test_readall_error_propagates: setup set failed\n");
		ntfs_inode_put(f);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	/* The EA reads back before patching, so a failure after the
	 * patch is the patch's doing, not the setup's. */
	err = ea_get(f, "user.big", buf, sizeof(buf));
	if (err != 8192) {
		fprintf(stderr, "SKIP test_readall_error_propagates: setup get returned %d\n", err);
		ntfs_inode_put(f);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	CHECK(ntfs_volume_sync(v.vol) == 0);
	ntfs_inode_put(f);
	f = NULL;
	vol_down(&v);

	/* Rewrite the $EA run out of the volume, on disk. */
	snprintf(cmd, sizeof(cmd), "python3 %s %s %llu",
		 py, scratch, (unsigned long long)mft_no);
	p = popen(cmd, "r");
	if (!p || !fgets(out, sizeof(out), p)) {
		fprintf(stderr, "FAIL patch_oob_ea.py produced no output\n");
		failures++; checks++;
		if (p) pclose(p);
		unlink(scratch);
		return;
	}
	rc = pclose(p);
	CHECK(rc == 0);
	{
		long nr_clusters, cs, old_lcn, old_len, new_lcn, new_len;
		CHECK(sscanf(out, "%ld %ld %ld %ld %ld %ld", &nr_clusters, &cs,
			     &old_lcn, &old_len, &new_lcn, &new_len) == 6);
		CHECK(new_lcn >= nr_clusters);
		CHECK(new_len == old_len);
	}

	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_readall_error_propagates: remount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "ea3.bin", &f) != 0 || !f) {
		fprintf(stderr, "SKIP test_readall_error_propagates: lookup failed\n");
		vol_down(&v);
		unlink(scratch);
		return;
	}
	/* New: the $EA read fails with -EIO and the real error reaches
	 * the caller. Old: readall returned NULL and the get reported
	 * -ENODATA, as if the EA did not exist -- the get is the
	 * discriminator. The set assertion guards the new behaviour
	 * (it returns -EIO in the old build too, via the create path
	 * failing, so it does not discriminate on its own). */
	err = ea_get(f, "user.big", buf, sizeof(buf));
	CHECK(err == -EIO);
	if (err != -EIO)
		fprintf(stderr, "FAIL ea get on corrupt $EA: returned %d, expected -EIO (-5)\n", err);
	/* The set path propagates too: replacing the EA must fail with
	 * the read error instead of silently dropping the stream. */
	err = ea_set(f, "user.big", val8k, sizeof(val8k));
	CHECK(err == -EIO);
	if (err != -EIO)
		fprintf(stderr, "FAIL ea set on corrupt $EA: returned %d, expected -EIO (-5)\n", err);
	ntfs_inode_put(f);
	vol_down(&v);
	unlink(scratch);
}

/*
 * Upstream 8efe00b098b5 + ba1b61ddaa76: ntfs_resident_attr_record_add()
 * collapsed every failure to -EIO at its put_err_out label, and
 * ntfs_non_resident_attr_record_add() returned the literal -1
 * (propagated as -EPERM to userspace). Both now return err.
 *
 * The resident collapse defeated the resident-to-non-resident
 * fallback in ntfs_attr_add(), which relies on seeing -ENOSPC to
 * convert the attribute when the MFT record has no room. The
 * trigger is a first EA on a file with no $EA at all: ntfs_set_ea()
 * calls ntfs_attr_add(AT_EA, ..., new_ea_size) with the value, and
 * for an 8 KiB value the resident record (~8.2 KiB) fits in no
 * record -- not even a fresh extent -- so the resident add fails
 * -ENOSPC and the fallback must convert it to non-resident.
 * Without the fix the set fails with -EIO; with it, it succeeds
 * and the EA reads back byte-for-byte after a remount (same-mount
 * read-back is broken by a separate HasEA bug, see below).
 *
 * The file must come from the fixture, not ntfs_create(): driver-
 * created files already carry the three hidden WSL EAs, so their
 * $EA exists and the set goes through ntfs_write_ea() instead.
 * /files/sz/0 in basic-4k.img was created by mkfixtures/libntfs-3g
 * and has no $EA (the get returns -ENODATA before the set).
 * No fsck_clean: the image is driver-modified scratch, and the
 * remount read-back is the assertion that matters.
 *
 * The non-resident hunk (ba1b61ddaa76) has no behavioural test:
 * the non-resident record is ~80 bytes and ntfs_attr_add only
 * calls it on a record it already checked has room, so its
 * put_err_out is unreachable through the public API; it is proven
 * by the verbatim one-line diff, per the round-8 precedent.
 */
static void test_attr_add_enospc_fallback(void)
{
	static char val8k[8192];
	struct vol v;
	ntfs_inode_t *files = NULL, *sz = NULL, *f = NULL;
	char buf[8192];
	int err;

	printf("test_attr_add_enospc_fallback\n");
	memset(val8k, 'B', sizeof(val8k));
	if (copy_fixture("attr-add-fallback") != 0) {
		fprintf(stderr, "SKIP test_attr_add_enospc_fallback: no fixture\n");
		return;
	}
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_attr_add_enospc_fallback: mount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "files", &files) != 0 || !files ||
	    ntfs_lookup(files, "sz", &sz) != 0 || !sz ||
	    ntfs_lookup(sz, "0", &f) != 0 || !f) {
		fprintf(stderr, "SKIP test_attr_add_enospc_fallback: lookup failed\n");
		if (f) ntfs_inode_put(f);
		if (sz) ntfs_inode_put(sz);
		if (files) ntfs_inode_put(files);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	/* Sanity: the fixture file has no $EA yet. */
	err = ea_get(f, "user.big", buf, sizeof(buf));
	CHECK(err == -ENODATA);
	if (err != -ENODATA) {
		fprintf(stderr, "SKIP test_attr_add_enospc_fallback: fixture file already has an EA (get=%d)\n", err);
		ntfs_inode_put(f);
		ntfs_inode_put(sz);
		ntfs_inode_put(files);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	/* New: the resident add fails -ENOSPC and the fallback converts
	 * the $EA to non-resident. Old: the collapse turns it into -EIO
	 * and the set fails. */
	err = ea_set(f, "user.big", val8k, sizeof(val8k));
	CHECK(err == 0);
	if (err != 0)
		fprintf(stderr, "FAIL ea set with 8 KiB value: returned %d, expected 0\n", err);
	/* Same-mount get: this was broken by the HasEA bug (see
	 * test_ea_first_set_same_mount_get) and is fixed with it --
	 * the 8 KiB $EA is non-resident, so this also covers the
	 * non-resident same-mount path. */
	err = ea_get(f, "user.big", buf, sizeof(buf));
	CHECK(err == (int)sizeof(val8k));
	if (err != (int)sizeof(val8k))
		fprintf(stderr, "FAIL ea same-mount get with 8 KiB value: returned %d, expected %d\n",
			err, (int)sizeof(val8k));
	ntfs_inode_put(f);
	ntfs_inode_put(sz);
	ntfs_inode_put(files);
	f = sz = files = NULL;
	vol_down(&v);

	/* Remount: the non-resident $EA has to parse back. */
	if (vol_up(&v, 022, 022, 501, 20)) {
		fprintf(stderr, "SKIP test_attr_add_enospc_fallback: remount failed\n");
		unlink(scratch);
		return;
	}
	if (ntfs_lookup(v.root, "files", &files) != 0 || !files ||
	    ntfs_lookup(files, "sz", &sz) != 0 || !sz ||
	    ntfs_lookup(sz, "0", &f) != 0 || !f) {
		fprintf(stderr, "FAIL test_attr_add_enospc_fallback: lookup after remount failed\n");
		failures++; checks++;
		if (f) ntfs_inode_put(f);
		if (sz) ntfs_inode_put(sz);
		if (files) ntfs_inode_put(files);
		vol_down(&v);
		unlink(scratch);
		return;
	}
	err = ea_get(f, "user.big", buf, sizeof(buf));
	CHECK(err == 8192);
	if (err == 8192)
		CHECK(memcmp(buf, val8k, sizeof(val8k)) == 0);
	if (err != 8192)
		fprintf(stderr, "FAIL ea get after remount: returned %d, expected 8192\n", err);
	ntfs_inode_put(f);
	ntfs_inode_put(sz);
	ntfs_inode_put(files);
	vol_down(&v);
	unlink(scratch);
}

/*
 * Upstream 5b6eedd7cc29: ntfs_icx_ib_sync_write() called
 * post_write_mst_fixup() when ntfs_ib_write() returned an error,
 * intending to restore the buffer after a failed write. But
 * ntfs_ib_write() also returns an error when pre_write_mst_fixup()
 * validation fails on an invalid index_block, and post_write assumes
 * the block is valid and does no boundary checks. With usa_count = 0
 * the count underflows to 0xffff and post_write walks ~33 MB past the
 * 4 KiB buffer (KASAN slab-out-of-bounds in the upstream report). The
 * fix moves the post_write call into ntfs_ib_write(), after the
 * pwrite, so it only runs when pre_write succeeded.
 *
 * The crafted block still reads fine: post_read_mst_fixup() treats an
 * invalid USA as "not protected" and returns 0, so the directory loads
 * and a setattr + fsync on an existing root directory reaches the sync
 * write (fsync -> __ntfs_write_inode -> ntfs_inode_sync_filename ->
 * ntfs_icx_ib_sync_write). The trigger must use an EXISTING entry:
 * creating a file first does not work -- the create's own index write
 * fails at pre_write and is swallowed, so the entry never lands on
 * disk and the fsync-time index lookup returns -ENOENT before any
 * sync write. Setting the directory's crtime (to its current value)
 * sets FileNameDirty, which is what makes __ntfs_write_inode call
 * sync_filename at all; the entry is on disk, so the lookup finds it
 * and the sync write runs on the crafted block. The trigger runs in a
 * forked child: without the fix the child dies with SIGSEGV in
 * post_write; with the fix it exits cleanly. __ntfs_write_inode()
 * ignores sync_filename's return, so fsync reports 0 either way --
 * survival, not the return value, is the assertion. The parent CHECKs
 * the wait status, so the suite survives the negative control.
 * Fixture patcher tools/mkfixtures/patch_ib_usa.py sets usa_count = 0
 * on the root's single INDX block. No fsck_clean: the image is
 * deliberately corrupt.
 */
static void test_ib_sync_write_no_post_fixup(void)
{
	const char *py = getenv("NTFS_PATCH_IB_USA");
	char cmd[PATH_MAX * 2 + 256];
	pid_t pid;
	int rc, st;

	printf("test_ib_sync_write_no_post_fixup\n");
	if (!py || !*py || access(py, R_OK) != 0) {
		fprintf(stderr, "SKIP test_ib_sync_write_no_post_fixup: NTFS_PATCH_IB_USA not set\n");
		return;
	}
	if (copy_fixture("ibus") != 0) {
		fprintf(stderr, "SKIP test_ib_sync_write_no_post_fixup: no fixture\n");
		return;
	}
	snprintf(cmd, sizeof(cmd), "python3 %s %s >/dev/null 2>&1", py, scratch);
	rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "SKIP test_ib_sync_write_no_post_fixup: patcher failed (rc %d)\n", rc);
		unlink(scratch);
		return;
	}
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "SKIP test_ib_sync_write_no_post_fixup: fork failed\n");
		unlink(scratch);
		return;
	}
	if (pid == 0) {
		/* Child: mount, look up the existing "ads" directory, set
		 * its crtime to its current value (sets FileNameDirty with
		 * no on-disk change), fsync it. With the fix the sync write
		 * fails cleanly with -EIO inside and the child exits;
		 * without it post_write_mst_fixup() runs on the invalid
		 * block and the child segfaults. "ads" is used (not
		 * "links"): the crafted block's sector ends are never
		 * restored on read, so an entry whose mref spans a sector
		 * end ("links" at 1528, sequence bytes at 1534-1535) reads
		 * back the USN 0xa instead of its true sequence 1 and the
		 * lookup fails with stale-reference -EIO; "ads" (entry at
		 * 1240) does not span one. */
		struct vol v;
		ntfs_inode_t *d = NULL;
		struct ntfs_attr at;

		if (vol_up(&v, 022, 022, 501, 20))
			_exit(2);
		if (ntfs_lookup(v.root, "ads", &d) != 0 || !d)
			_exit(3);
		if (ntfs_getattr(d, &at) != 0)
			_exit(4);
		if (ntfs_setattr(d, &at, NTFS_SETATTR_CRTIME) != 0)
			_exit(5);
		/* __ntfs_write_inode() calls ntfs_inode_sync_filename()
		 * but ignores its return, so fsync reports 0 even when
		 * the index sync write fails: survival is the assertion
		 * here, not the return value. */
		(void)ntfs_fsync(d, false);
		ntfs_inode_put(d);
		vol_down(&v);
		_exit(0);
	}
	if (waitpid(pid, &st, 0) < 0) {
		fprintf(stderr, "FAIL ib sync write: waitpid failed\n");
		CHECK(0);
		unlink(scratch);
		return;
	}
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	if (WIFSIGNALED(st))
		fprintf(stderr, "FAIL ib sync write: child killed by signal %d (expected clean exit)\n", WTERMSIG(st));
	else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		fprintf(stderr, "FAIL ib sync write: child exit status %d (expected 0)\n", WEXITSTATUS(st));
	unlink(scratch);
}

int main(void)
{
	test_hard_links();
	test_hard_link_refusals();
	test_many_links();
	test_symlinks();
	test_symlink_filename_union_is_the_tag();
	test_symlink_tag_choice();
	test_reparse_index_failure_propagates();
	test_windows_dir_links();
	test_interix_symlinks();
	test_symlink_ea_migration();
	test_symlink_to_directory_is_a_directory_record();
	test_time_conversion();
	test_timestamps();
	test_permissions();
	test_mft_alloc_usa_ofs_validated();
	test_lookup_err_mref_normalized();
	test_nonres_attr_validation();
	test_ea_final_size_validated();
	test_ea_first_set_same_mount_get();
	test_readall_error_propagates();
	test_attr_add_enospc_fallback();
	test_ib_sync_write_no_post_fixup();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
