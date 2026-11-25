/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_STRING_H
#define _LINUX_STRING_H

#include <string.h>
#include <strings.h>
#include <linux/types.h>

#define unsafe_memcpy(dst, src, len, justification) memcpy(dst, src, len)
/* strlcpy: glibc >= 2.38 declares it in <string.h>; older glibc / other
 * libcs do not (it lives in libbsd there). Provide it here only when the
 * system does not, so callers using strlcpy() directly (bdev_file.c,
 * fs_parser.c, mem.c, test_bdev.c) work everywhere. macOS keeps the
 * system one. strlcpy_() is implemented directly, so it never depends
 * on which case applies.
 * NOTE: __GLIBC_PREREQ may only be mentioned inside a nested #if that is
 * skipped entirely on non-glibc targets. A single-line #if with && still
 * expands it on macOS, where it is undefined -- that broke the macOS CI
 * build (checks run 45661009566). */
#if defined(__linux__)
# ifdef __GLIBC__
#  if __GLIBC_PREREQ(2, 38)
#   define NTFS_HAVE_SYSTEM_STRLCPY 1
#  endif
# endif
# ifndef NTFS_HAVE_SYSTEM_STRLCPY
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);
	if (size) {
		size_t c = n < size - 1 ? n : size - 1;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}
# endif
#endif
static inline size_t strlcpy_(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);
	if (size) {
		size_t c = n < size - 1 ? n : size - 1;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}
size_t strscpy(char *dst, const char *src, size_t size);
static inline int strncasecmp_(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline void *memscan(void *addr, int c, size_t size)
{ unsigned char *p = addr; while (size) { if (*p == (unsigned char)c) return p; p++; size--; } return p; }
char *strreplace(char *s, char old, char new_);
static inline bool memchr_inv_zero(const void *p, size_t n)
{ const unsigned char *c = p; while (n--) if (*c++) return false; return true; }
void *memchr_inv(const void *s, int c, size_t n);
#define sysfs_streq(a, b) (strcmp(a, b) == 0)
#define strstarts(s, p) (strncmp(s, p, strlen(p)) == 0)

#endif /* _LINUX_STRING_H */
