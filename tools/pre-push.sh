#!/bin/bash
# Fast local pre-push checks: everything CI runs that can run here, plus
# the Darwin cross-check that catches macOS-only build breaks before
# they reach the runner.
#
#   tools/pre-push.sh
#
# Stages (fast first, so a cheap failure stops the expensive ones):
#   1. check-darwin.sh   preprocess the C tree for x86_64-apple-darwin
#                        (catches the __GLIBC_PREREQ class of breakage
#                        that the Linux build cannot see)
#   2. shellcheck        tools/*.sh (skipped with a warning if absent)
#   3. python compile    tools/*.py
#   4. workflow YAML     .github/workflows/*.yml parse check
#   5. ci.sh --quick     toolchain + ctest + UBSan (skips the fixture
#                        suite; the full suite is what CI is for)
#   6. fuzz check        tools/fuzz.sh check: the standalone runner mounts
#                        every seed and fails cleanly on every regression
#                        image (the leak half needs ASan, so it is the CI
#                        fuzz job's job, not pre-push's)
#   7. upstream port-diff tools/upstream-drift.sh port-diff: what the port
#                        changed against the pinned pristine copy. Local,
#                        <1s, informational -- always passes. The numbers
#                        are the discipline (GAPS.md §5.3); the network
#                        half (upstream vs latest tag) is the weekly CI
#                        job's job, not pre-push's.
#   8. arch consistency tools/check-archs.sh: the Xcode ARCHS and the
#                        CMake CMAKE_OSX_ARCHITECTURES default must both
#                        name arm64 and x86_64 (GAPS.md §4.2). Runs
#                        everywhere; the real universal build is CI's job.
#
# Not covered here, by design -- CI owns them:
#   - the fixture suite (tools/run-tests.sh all): slow; run it
#     explicitly before pushing a change to the core if you can
#   - the ASan leak regression (tools/fuzz.sh regression): needs Clang;
#     run it explicitly, and the CI fuzz job runs it weekly
#   - Swift unit tests and the app build: need xcodebuild / macOS SDK
#   - the mount test: needs a signed FSKit extension and a real mount
#
# Install as the pre-push hook with tools/install-hooks.sh.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT" || exit 1

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
}

stage_darwin() { "$HERE/check-darwin.sh"; }

stage_shellcheck() {
	command -v shellcheck >/dev/null 2>&1 || {
		echo "shellcheck not installed, skipping (apt install shellcheck)"
		return 0
	}
	shellcheck -S warning "$HERE"/*.sh
}

stage_python() {
	python3 -m py_compile "$HERE"/*.py
}

stage_workflows() {
	python3 - "$@" <<'PY'
import glob, sys, yaml
for path in sorted(glob.glob(".github/workflows/*.yml")):
    with open(path) as f:
        yaml.safe_load(f)
    print(f"yaml ok: {path}")
PY
}

stage_ci_quick() {
	if command -v xcodebuild >/dev/null 2>&1; then
		"$HERE/ci.sh" --quick
		return $?
	fi
	# No xcodebuild on Linux: the Swift-unit-tests and app-build stages
	# cannot pass here, by design. Pass iff the stages that can run here
	# all passed; the other two are CI's job (see the header).
	local out
	out=$("$HERE/ci.sh" --quick 2>&1)
	printf '%s\n' "$out"
	local stage
	for stage in "toolchain (ntfsprogs + ntfsprogs-plus)" \
		"ctest (platform, mount decisions, logfile, scripts)" \
		"ctest under UBSan"; do
		printf '%s\n' "$out" | grep -Fq "  PASS  $stage (" || return 1
	done
	return 0
}

stage_fuzz_check() {
	# Needs the normal build dir (ci.sh --quick above builds it) and
	# the seed corpus (generated on first run). TMPDIR: /tmp is a 512M
	# tmpfs on some machines; the harness mounts 4 MiB images, so the
	# default is fine, but respect an explicit TMPDIR when set.
	TMPDIR="${TMPDIR:-/tmp}" "$HERE/fuzz.sh" check
}

stage_upstream_port_diff() {
	"$HERE/upstream-drift.sh" port-diff
}

stage_archs() {
	"$HERE/check-archs.sh"
}

run_stage "darwin preprocess (macOS-only breakage)" stage_darwin
run_stage "shellcheck (tools/*.sh)" stage_shellcheck
run_stage "python compile (tools/*.py)" stage_python
run_stage "workflow YAML syntax" stage_workflows
run_stage "ci.sh --quick (toolchain + ctest + UBSan)" stage_ci_quick
run_stage "fuzz check (seeds + regression images)" stage_fuzz_check
run_stage "upstream port-diff (core/ntfs vs pinned pristine)" stage_upstream_port_diff
run_stage "arch consistency (Xcode ARCHS vs CMake default)" stage_archs

printf '\n==================== pre-push summary ====================\n'
printf '%s\n' "${results[@]}"
printf 'total %ss\n' "$(( $(date +%s) - start_all ))"
echo "(fixture suite, ASan fuzz regression, Swift tests, app build and mount test are CI-only; see header)"
exit $fail
