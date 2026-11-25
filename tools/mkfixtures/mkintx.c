/* mkintx.c - create a REAL ntfs-3g Interix symlink on an image.
 *
 * Scratch verification tool, not part of the test suite: it answers "is the
 * ntfscp + patch_interix.py fixture byte-identical to what ntfs-3g itself
 * writes?" by calling libntfs-3g's own ntfs_create_symlink. Build:
 *
 *   cc -O2 -Itools/.local/include -o /tmp/mkintx tools/mkfixtures/mkintx.c \
 *      tools/.local/lib/libntfs-3g.a
 *
 * Usage: mkintx IMAGE NAME TARGET -- prints the MFT number of the link.
 * Compare with: ntfsinfo -m -F /NAME IMAGE, and with the same dump of the
 * patched fixture; the $STANDARD_INFORMATION / $FILE_NAME attributes and
 * the $DATA bytes must agree.
 */
#define _GNU_SOURCE /* so glibc defines __timespec_defined before ntfstime.h */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h> /* defines st_mtime, which ntfstime.h keys its guard on */
#include <time.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/layout.h>

int main(int argc, char **argv)
{
	ntfs_volume *vol;
	ntfs_inode *root, *ni;
	ntfschar *uname = NULL, *utarget = NULL;
	int uname_len, utarget_len;

	if (argc != 4) {
		fprintf(stderr, "usage: %s IMAGE NAME TARGET\n", argv[0]);
		return 2;
	}
	vol = ntfs_mount(argv[1], 0);
	if (!vol) {
		perror("ntfs_mount");
		return 1;
	}
	root = ntfs_inode_open(vol, FILE_root);
	if (!root) {
		perror("ntfs_inode_open(root)");
		ntfs_umount(vol, 0);
		return 1;
	}
	uname_len = ntfs_mbstoucs(argv[2], &uname);
	utarget_len = ntfs_mbstoucs(argv[3], &utarget);
	if (uname_len < 0 || utarget_len < 0) {
		fprintf(stderr, "ntfs_mbstoucs failed\n");
		ntfs_umount(vol, 0);
		return 1;
	}
	ni = ntfs_create_symlink(root, 0, uname, uname_len, utarget, utarget_len);
	if (!ni) {
		perror("ntfs_create_symlink");
		ntfs_umount(vol, 0);
		return 1;
	}
	printf("%lld\n", (long long)ni->mft_no);
	ntfs_inode_close(ni);
	ntfs_inode_close(root);
	if (ntfs_umount(vol, 0) != 0) {
		perror("ntfs_umount");
		return 1;
	}
	return 0;
}
