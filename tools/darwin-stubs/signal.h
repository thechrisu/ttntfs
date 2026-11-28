/* Preprocess-only stub for the Darwin cross-check (tools/check-darwin.sh).
 * Deliberately empty: it stands in for the macOS SDK header so the
 * preprocessor can evaluate the Darwin branches of the platform layer
 * without an SDK. It intentionally defines nothing -- in particular no
 * glibc macros -- so a Linux-only assumption in a Darwin branch fails
 * here the same way it fails on the macOS CI runner.
 *
 * Needed by core/tests/test_alloc.c (upstream's allocator test, which
 * installs a SIGSEGV/SIGBUS handler around a guard-page probe); the
 * real header exists on the macOS CI runner. */
#ifndef TTNTFS_STUB_SIGNAL_H
#define TTNTFS_STUB_SIGNAL_H
#endif
