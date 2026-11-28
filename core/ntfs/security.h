// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ntfs_sd_dacl_mode() - map a Windows security descriptor's DACL to Unix
 * permission bits (docs/ACL-DESIGN.md §2).
 *
 * v1 is the read path only: this file is pure (no I/O, no globals), so the
 * parser is testable offline on synthetic SDs. A crafted SD must produce
 * an error, never an out-of-bounds read: every offset/length is validated
 * against the buffer before it is dereferenced.
 */
#ifndef NTFS_SECURITY_H
#define NTFS_SECURITY_H

#include <linux/types.h>

/*
 * Map @sd (a self-relative security descriptor, @len bytes) to Unix
 * permission bits (0000-0777, no type bits) for a file (@is_dir false)
 * or directory (@is_dir true).
 *
 * Returns 0 and stores the mode in @mode_out, or a negative errno:
 *   -EINVAL   malformed SD (bad revision, not self-relative, an offset
 *             outside the buffer, a truncated ACE/SID, an ACL that runs
 *             past its stated size, ...). The caller falls back to
 *             today's behaviour (0777 & ~mask) and never fails the load.
 *   -ENODATA  the SD carries no DACL information this mapping can use
 *             (empty DACL: present, zero ACEs). Also a caller fallback;
 *             -ENODATA (not a mode of 0000) is deliberate, so a test can
 *             tell "empty DACL" from "malformed".
 *
 * A NULL DACL (SE_DACL_PRESENT clear, or dacl offset 0) means full
 * access in Windows and maps to 0777, returned as 0.
 */
int ntfs_sd_dacl_mode(const void *sd, size_t len, bool is_dir,
		      unsigned int *mode_out);

#endif /* NTFS_SECURITY_H */
