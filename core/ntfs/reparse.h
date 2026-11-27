/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2008-2021 Jean-Pierre Andre
 * Copyright (c) 2025 LG Electronics Co., Ltd.
 */

#ifndef _LINUX_NTFS_REPARSE_H
#define _LINUX_NTFS_REPARSE_H

#include "layout.h"

extern __le16 reparse_index_name[];

/* Index entry in $Extend/$Reparse (layout.h: struct reparse_index_key).
 * The header is followed by the 12-byte key (tag + file_id) and a
 * 4-byte filling: 32 bytes total. The journal staging in journal.c
 * scans $Reparse's $R root for the entry a symlink create just
 * added, so the definition lives here, not in reparse.c. */
struct reparse_index {
	struct index_entry_header header;
	struct reparse_index_key key;
	__le32 filling;
};

/* Return the MFT record number of $Extend/$Reparse, or -ENOENT when
 * it cannot be found. The journal staging in journal.c needs the
 * $Reparse record to find the $R index entry a symlink create just
 * added. */
int ntfs_reparse_mft_no(struct ntfs_volume *vol, u64 *mft_no_out);

/* @dir_record: the MFT record carries MFT_RECORD_IS_DIRECTORY. A junction is
 * only valid on one; a symlink may sit on either. */
unsigned int ntfs_make_symlink(struct ntfs_inode *ni, bool dir_record);

/*
 * ntfs-3g Interix symlink (docs/GAPS.md §2.1): a plain file record with
 * FILE_ATTR_SYSTEM, no reparse point and no I30 index, whose unnamed $DATA
 * is the 8-byte magic "IntxLNK\1" followed by the target in UTF-16LE.
 * Returns S_IFLNK and fills ni->target on a match, else 0. The directory
 * listing type for the same check, for readdir entries that carry
 * FILE_ATTR_SYSTEM but no reparse tag. */
unsigned int ntfs_make_interix_symlink(struct ntfs_inode *ni);
unsigned int ntfs_interix_dt_types(struct ntfs_volume *vol, unsigned long mref);

/*
 * Windows symbolic link reparse data, [MS-FSCC] 2.1.2.4. Offsets are from byte
 * 0 of path_buffer, lengths are bytes and exclude any terminator, strings are
 * UTF-16LE. Both names may appear in either order in path_buffer.
 */
struct ntfs_win_symlink {
	__le16 subst_name_offset;
	__le16 subst_name_length;
	__le16 print_name_offset;
	__le16 print_name_length;
	__le32 flags;
	__le16 path_buffer[];
} __packed;

#define SYMLINK_FLAG_RELATIVE	cpu_to_le32(0x00000001)

/* [MS-FSCC] 2.1.2: a reparse data buffer, header included, is at most
 * 16 KiB. A native symlink stores its target twice in UTF-16 (substitute
 * and print name), so a target longer than 4091 UTF-16 units cannot be
 * expressed in the native tag at all -- 8 + 12 + 4 * 4092 = 16388. */
#define NTFS_MAX_REPARSE_DATA_BUFFER_SIZE	16384

/* True when a native Windows symlink for @target_len UTF-16 units fits in
 * a reparse data buffer; false when the caller must fall back to the WSL
 * tag, which stores the target once, in UTF-8. */
bool ntfs_win_symlink_fits(int target_len);

int ntfs_reparse_set_win_symlink(struct ntfs_inode *ni,
		const __le16 *target, int target_len);

/* True when @target (UTF-16, @len units) can be written as a native Windows
 * symlink and read back unchanged; false when it needs the WSL tag to survive.
 * Refuses the characters Windows forbids in a name, and a literal backslash,
 * which the native form cannot distinguish from a separator. */
bool ntfs_symlink_target_is_windows_safe(const __le16 *target, int len);
unsigned int ntfs_reparse_tag_dt_types(struct ntfs_volume *vol, unsigned long mref);
int ntfs_reparse_set_wsl_symlink(struct ntfs_inode *ni,
			const __le16 *target, int target_len);
int ntfs_reparse_set_wsl_not_symlink(struct ntfs_inode *ni, mode_t mode);
int ntfs_delete_reparse_index(struct ntfs_inode *ni);

/* NOTE: ntfs_remove_ntfs_reparse_data was declared here but never
 * defined or called in this port — it exists only in libntfs-3g,
 * where it removes reparse data from an inode (xattrs.c, dir.c).
 * This driver has no such path: the setters run only at create, and
 * removal is ntfs_delete_reparse_index at unlink. The dead
 * declaration was removed in the reparse class (2026-11-27). */

#endif /* _LINUX_NTFS_REPARSE_H */
