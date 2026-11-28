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

/*
 * ntfs_sd_mode - the SD for a security_id, mapped to Unix mode bits.
 *
 * Looks the descriptor up in $Secure:$SDS (linear scan; the stream is
 * small and the result is cached per volume), parses it with
 * ntfs_sd_dacl_mode(), and returns the perm bits (0000-0777, no type
 * bits) in @mode_out. @is_dir selects the directory mapping
 * (FILE_DELETE_CHILD grants write on directories only).
 *
 * Returns 0, or a negative errno, all of which mean "fall back to
 * today's behaviour" for the caller, never "fail the inode load":
 *   -ENODATA  no $Secure, no $SDS, or no entry for @security_id
 *             (including @security_id 0: no SD assigned).
 *   -EINVAL   the $SDS entry or the SD fails validation.
 *   -EIO / -ENOMEM  the $SDS read itself failed.
 * Only successful parses are cached, so a failure re-reads $SDS on
 * the next load of the same id; that keeps a corrupt $SDS visible
 * instead of cached as "no mode".
 */
struct ntfs_volume;
int ntfs_sd_mode(struct ntfs_volume *vol, u32 security_id, bool is_dir,
		 unsigned int *mode_out);

/* Cache observability for tests: lifetime hits/misses of ntfs_sd_mode(). */
void ntfs_sd_cache_stats(struct ntfs_volume *vol, unsigned long *hits,
			 unsigned long *misses);

#endif /* NTFS_SECURITY_H */
