#!/bin/bash
# Everything that can be checked without hardware, signing, or a human.
#
#   tools/ci.sh              run all stages
#   tools/ci.sh --quick      skip the fixture suite (the slow one)
#   tools/ci.sh --asan       run only the ASan stage
#   tools/ci.sh --list       print the stages and exit
#
# Why this exists. Until 2026-09-14 nothing ran these suites automatically, and
# two failures sat unnoticed for a day each: logfile_unit hung in ASan's own
# init so its 243 checks never executed, and a stale header aborted ntfslog on
# every fixture. Both were caught by tests that already existed. A test nobody
# runs is not coverage.
#
# NOT run here, and why:
#   fskit/scripts/mount-test.sh   needs the FSKit module enabled and a real
#                                 mount; no hosted runner can do it
#   fskit/scripts/package.sh      needs the Developer ID certificate and a
#                                 notarization credential
# Those two stay manual, so a release still needs a human.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT" || exit 1

QUICK=0
ASAN=0
for a in "$@"; do
	case "$a" in
	--quick) QUICK=1 ;;
	--asan) ASAN=1 ;;
	--list) sed -n '/^[^#]*run_stage "/{s/.*run_stage "\([^"]*\)".*/  \1/p;}' "$0" | grep -v '\[\^' ; exit 0 ;;
	*) echo "usage: $0 [--quick] [--asan] [--list]" >&2; exit 2 ;;
	esac
done

fail=0
declare -a results
start_all=$(date +%s)

run_stage() {
	local name="$1"; shift
	local t0 t1 rc
	printf '\n=== %s ===\n' "$name"
	t0=$(date +%s)
	"$@"
	rc=$?
	t1=$(date +%s)
	if [ $rc -eq 0 ]; then
		results+=("  PASS  $name ($((t1 - t0))s)")
	else
		results+=("  FAIL  $name ($((t1 - t0))s, exit $rc)")
		fail=1
	fi
	return 0		# keep going: one failure should not hide the others
}

# A Mach-O file must carry both slices (GAPS.md §4.2). Prints the archs
# it found, so a failure says which slice is missing. macOS-only (lipo).
check_universal() {
	local file="$1" archs
	[ -f "$file" ] || { echo "not built: $file"; return 1; }
	archs=$(lipo -archs "$file") || return 1
	echo "$file: $archs"
	echo "$archs" | grep -qw arm64 || { echo "missing arm64 slice: $file"; return 1; }
	echo "$archs" | grep -qw x86_64 || { echo "missing x86_64 slice: $file"; return 1; }
	return 0
}

# --- toolchain -------------------------------------------------------------
# run-tests.sh builds ntfsprogs itself if missing, but doing it here keeps the
# slow part in its own stage so a CI log shows where the time went.
stage_toolchain() {
	"$HERE/build-ntfsprogs.sh" || return 1
	# ntfsprogs-plus is the only real structural checker available without
	# Windows. It needs autotools, which macOS does not ship; if they are
	# missing this stage fails loudly rather than letting the fixture suite
	# quietly skip its strongest check.
	"$HERE/build-ntfsprogs-plus.sh" || return 1
}

# --- C core and platform ---------------------------------------------------
stage_ctest() {
	cmake -S . -B build >/dev/null || return 1
	cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null || return 1
	# The Xcode app links build/libntfscore.a for both archs (GAPS.md
	# §4.2). Linux has no lipo and builds single-arch; check only on macOS.
	if [ "$(uname -s)" = Darwin ]; then
		check_universal build/libntfscore.a || return 1
	fi
	ctest --test-dir build --output-on-failure
}

# --- the same tests, instrumented ------------------------------------------
# A separate build directory on purpose: the default tree produces the
# libntfscore.a the Xcode app links, and an instrumented static library cannot
# be linked into a shipping app. ASan lives in stage_asan below, not here,
# because it needs its own build dir and its own stage (it runs on macOS 26
# too -- see stage_asan).
stage_ubsan() {
	cmake -S . -B build-ubsan -DNTFS_UBSAN=ON -DNTFS_LOGFILE_UBSAN=ON >/dev/null || return 1
	cmake --build build-ubsan -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null || return 1
	ctest --test-dir build-ubsan --output-on-failure
}

# --- the same tests under AddressSanitizer ---------------------------------
# Own build dir, like UBSan: the app must stay sanitizer-free. Runs on
# Linux and on macOS 26, via `tools/ci.sh --asan`, from the dedicated
# `asan` job in .github/workflows/ci.yml (macos-26) and from
# tools/pre-push.sh stage 9. History: on macOS 26 ASan deadlocked inside
# __asan::AsanInitInternal() before main() (confirmed 2026-09-14, every
# test; one runaway burned a core for 23 h), so the stage was Linux-only
# until 2026-11-26, when the user asked for macOS 26 to be fixed rather
# than skipped. The CI job on macos-26 re-tested it on the current
# runner image / Xcode and the hang is gone: run 45785443966's asan job
# executed all 14 tests in 35.56 s with no stall, and run 45787227610's
# asan job went green after the stage's own summary assertion was fixed
# to accept the macOS ctest format (d3fbebe3). Not yet in the default
# ci.sh sequence: the checks job on macos-26 runs the plain suite, and
# this stage stays separately runnable in its dedicated job (joining it
# would roughly double the checks job's macOS time).
# Per-test timeout 300 s: the slowest test under ASan on Linux is now
# test_crash (9,869 checks since the journal classes landed: 127 s on a
# quiet box, 248 s under load, measured 2026-11-27/28), so 300 s is ~2.4x
# headroom over the quiet runtime and ~1.2x over the loaded one; a hang
# still becomes a failed test, never a runaway. The old 180 s bound was
# set when the slowest test was links at 20-95 s (measured 2026-11-26,
# with and without the 4e646ecd4475 change alike: 18-33 s quiet, 43-93 s
# loaded); it started failing test_crash on timeout alone once the
# journal classes grew the suite (rounds 28 and 29 and stream B, three
# consecutive rounds, zero assertion failures, the crash suite passing
# standalone every time). The 60 s bound before that was set when the
# slowest test was ~20 s; it started failing links on timeout alone once
# the box got loaded (2026-11-26: six consecutive ASan failures, all
# links timeouts, no assertion or ASan error; two live dumps showed the
# process running or in normal I/O wait, progressing through the suite
# -- no deadlock). The stage asserts the suite actually executed: ctest must
# report 100% passed over at least one test, and at least one test must
# have a Passed line. That is the 2026-09-14 failure mode -- a run that
# executes nothing must fail loudly, never read green. The summary line
# differs by platform: Linux ctest prints "100% tests passed, 0 tests
# failed out of N", macOS ctest prints "100% tests passed out of N"
# (no ", 0 tests failed" clause) -- the macos-26 asan job of run
# 45785443966 failed on exactly this: 14 tests passed in 35.56 s, the
# hang gone, but the old pattern only accepted the Linux form. Accept
# both forms; the Passed-line check is what rules out a suite that ran
# nothing but still printed 100%.
stage_asan() {
	cmake -S . -B build-asan -DNTFS_ASAN=ON -DNTFS_LOGFILE_ASAN=ON >/dev/null || return 1
	cmake --build build-asan -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null || return 1
	local out rc
	out=$(ctest --test-dir build-asan --timeout 300 --output-on-failure 2>&1)
	rc=$?
	printf '%s\n' "$out"
	[ $rc -eq 0 ] || return 1
	printf '%s\n' "$out" | grep -Eq '100% tests passed(, 0 tests failed)? out of [1-9][0-9]*' || {
		echo "ASan stage: ctest exited 0 but the suite did not execute its tests -- failing loudly (see the 2026-09-14 note above)."
		return 1
	}
	printf '%s\n' "$out" | grep -Eq ' +Passed +[0-9]+\.[0-9]+ sec' || {
		echo "ASan stage: ctest reported 100% passed but no test has a Passed line -- failing loudly (see the 2026-09-14 note above)."
		return 1
	}
	return 0
}

# --- the differential fixture suite ----------------------------------------
stage_fixtures() {
	NTFS_REQUIRE_FSCK=1 "$HERE/run-tests.sh" all
}

# --- the app's Swift unit tests --------------------------------------------
# Deployment target is macOS 26, so this needs an SDK that has FSKit. A runner
# image older than that cannot build it; say so and fail, rather than reporting
# a green run that tested nothing.
stage_swift() {
	command -v xcodebuild >/dev/null 2>&1 || { echo "no xcodebuild"; return 1; }
	command -v xcodegen  >/dev/null 2>&1 || { echo "no xcodegen (brew install xcodegen)"; return 1; }
	( cd fskit && xcodegen ) >/dev/null || return 1
	xcodebuild test -project fskit/NTFS.xcodeproj -scheme NTFSTests \
		-derivedDataPath "${DERIVED_DATA:-/tmp/ttntfs-ci-dd}" \
		2>&1 | grep -E 'Executed|error:|\*\* TEST|Testing failed' | tail -6
	return "${PIPESTATUS[0]}"
}

# --- the app and extension must still compile ------------------------------
# Debug only: Release is Developer ID signed and CI has no certificate.
stage_build_app() {
	command -v xcodebuild >/dev/null 2>&1 || { echo "no xcodebuild"; return 1; }
	local dd="${DERIVED_DATA:-/tmp/ttntfs-ci-dd}"
	# ARCHS is explicit on the command line, not just in
	# fskit/Config/Shared.xcconfig: the macos-26-arm64 runner image
	# update 20261117 -> 20261123 (2026-11-26/27) started producing
	# arm64-only app binaries (BUILD SUCCEEDED, then the universal
	# check failed `missing x86_64 slice`, three runs in a row) with
	# identical project settings. Command-line settings override any
	# image-default arch resolution; the xcconfig stays the single
	# source of truth for the project itself.
	xcodebuild -project fskit/NTFS.xcodeproj -scheme NTFS -configuration Debug \
		-derivedDataPath "$dd" \
		ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO \
		CODE_SIGNING_ALLOWED=NO build \
		2>&1 | grep -E '^\*\* BUILD|error:' | tail -4
	[ "${PIPESTATUS[0]}" -eq 0 ] || return 1
	# Universal check (GAPS.md §4.2): the app and the extension must each
	# carry both slices, or Intel Macs get a build that cannot run. lipo
	# is macOS-only, and this stage only runs where xcodebuild exists.
	# The extension binary is located with find, not a hardcoded
	# PlugIns path: ExtensionKit extensions do not live under
	# Contents/PlugIns (that assumption failed the first universal
	# run even though the build itself succeeded).
	local app_bin ext_bin
	app_bin=$(find "$dd/Build/Products/Debug/TT NTFS Native.app/Contents/MacOS" -name "TT NTFS Native" -type f 2>/dev/null | head -1)
	ext_bin=$(find "$dd/Build/Products/Debug/TT NTFS Native.app" -name NTFSExtension -type f 2>/dev/null | head -1)
	[ -n "$app_bin" ] || { echo "app binary not found in app bundle"; return 1; }
	[ -n "$ext_bin" ] || { echo "extension binary not found in app bundle"; return 1; }
	check_universal "$app_bin" || return 1
	check_universal "$ext_bin" || return 1
	return 0
}

# ASan-only mode: the toolchain first (the fixture-creating tests need
# mkntfs/ntfsck; stage_toolchain is a stamp no-op when already built),
# then just that stage, then the summary. Not part of the default sequence
# (see stage_asan). This branch sits before the default stages so --asan
# never runs them.
if [ "$ASAN" = 1 ]; then
	run_stage "toolchain (ntfsprogs + ntfsprogs-plus)" stage_toolchain
	run_stage "ctest under ASan" stage_asan
	printf '\n==================== ci summary ====================\n'
	printf '%s\n' "${results[@]}"
	printf 'total %ss\n' "$(( $(date +%s) - start_all ))"
	echo "(--asan: only the ASan stage ran)"
	exit $fail
fi

run_stage "toolchain (ntfsprogs + ntfsprogs-plus)" stage_toolchain
run_stage "ctest (platform, mount decisions, logfile, scripts)" stage_ctest
run_stage "ctest under UBSan" stage_ubsan
[ "$QUICK" = 1 ] || run_stage "fixtures (run-tests.sh all, fsck required)" stage_fixtures
run_stage "swift unit tests" stage_swift
run_stage "app + extension build (Debug, unsigned)" stage_build_app

printf '\n==================== ci summary ====================\n'
printf '%s\n' "${results[@]}"
printf 'total %ss\n' "$(( $(date +%s) - start_all ))"
[ "$QUICK" = 1 ] && echo "(--quick: the fixture suite was skipped)"
exit $fail
