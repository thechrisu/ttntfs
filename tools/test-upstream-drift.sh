#!/bin/bash
# tools/test-upstream-drift.sh - self-test for tools/upstream-drift.sh
# (GAPS.md §5.3). No network: builds a fixture git repo with two tags
# and runs the drift script against it with env overrides.
#
#   tools/test-upstream-drift.sh
#
# Checks:
#   1. port-diff on the real trees reproduces the independently
#      measured baseline (SUMMARY 18 1004 115 9 0).
#   2. upstream --tag <pin> on the fixture exits 0 (CLEAN).
#   3. upstream --tag <newer> exits 1 (DRIFT), names the changed and
#      added files, and lists the fixture commit.
#   4. A tampered pristine copy exits 2 (pin MISMATCH).
#   5. --out writes the same report that goes to stdout.
set -uo pipefail
. "$(dirname "$0")/env.sh"

SCRIPT="$NTFS_TOOLS_DIR/upstream-drift.sh"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/drift-selftest.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail=0

check() { # check <name> <expected-exit> <actual-exit>
	if [ "$2" -eq "$3" ]; then
		echo "PASS: $1 (exit $3)"
	else
		echo "FAIL: $1 (want exit $2, got $3)" >&2
		fail=1
	fi
}

expect_in() { # expect_in <name> <file> <pattern...>
	local name="$1" file="$2"; shift 2
	local p
	for p in "$@"; do
		if grep -Fq "$p" "$file"; then
			echo "PASS: $name contains '$p'"
		else
			echo "FAIL: $name missing '$p' in $file" >&2
			fail=1
		fi
	done
}

# --- 1. port-diff baseline (real trees) -------------------------
"$SCRIPT" port-diff > "$WORK/port-diff.txt" 2>&1
rc=$?
check "port-diff exit" 0 "$rc"
expect_in "port-diff" "$WORK/port-diff.txt" "SUMMARY 18 1004 115 9 0" "reparse.c"

# --- fixture repo: v9.9 (pin) -> v9.10 (one file changed, one added,
# one removed) ----------------------------------------------------
FIX="$WORK/fixture"
mkdir -p "$FIX/fs/ntfs"
cd "$FIX" || exit 1
git init --quiet -b main .
git config user.email drift-test@example.invalid
git config user.name "drift test"
printf 'line one\nline two\n' > fs/ntfs/a.c
printf 'stable\n' > fs/ntfs/b.c
git add fs/ntfs && git commit --quiet -m "pin state" && git tag v9.9
PIN_SHA=$(git rev-parse HEAD)
mkdir -p "$WORK/pristine"
git archive v9.9 fs/ntfs | tar -x -C "$WORK/pristine"
printf 'line one\nline two changed\nline three\n' > fs/ntfs/a.c
printf 'new file\n' > fs/ntfs/c.c
git rm --quiet fs/ntfs/b.c
git add fs/ntfs && git commit --quiet -m "ntfs: change a.c, add c.c, drop b.c" && git tag v9.10

export UPSTREAM_DRIFT_REPO="$FIX"
export UPSTREAM_DRIFT_PIN_TAG="v9.9"
export UPSTREAM_DRIFT_PIN_COMMIT="$PIN_SHA"
export UPSTREAM_DRIFT_PRISTINE="$WORK/pristine/fs/ntfs"

# --- 2. clean: target == pin ------------------------------------
"$SCRIPT" upstream --tag v9.9 > "$WORK/clean.txt" 2>&1
rc=$?
check "upstream clean exit" 0 "$rc"
expect_in "upstream clean" "$WORK/clean.txt" "CLEAN: no upstream drift" "OK: pristine copy matches v9.9"

# --- 3. drift: target == v9.10 ----------------------------------
"$SCRIPT" upstream --tag v9.10 --out "$WORK/drift.txt" > "$WORK/drift-stdout.txt" 2>&1
rc=$?
check "upstream drift exit" 1 "$rc"
expect_in "upstream drift" "$WORK/drift.txt" "DRIFT:" "a.c" "c.c" "b.c" "ntfs: change a.c, add c.c, drop b.c"
# stdout is the two progress lines plus the report; --out is the
# report alone. Compare accordingly.
if cmp -s "$WORK/drift.txt" <(tail -n +3 "$WORK/drift-stdout.txt"); then
	echo "PASS: --out file matches stdout (minus progress header)"
else
	echo "FAIL: --out file differs from stdout report" >&2
	fail=1
fi

# --- 4. pin mismatch --------------------------------------------
printf 'tampered\n' >> "$WORK/pristine/fs/ntfs/a.c"
"$SCRIPT" upstream --tag v9.9 > "$WORK/mismatch.txt" 2>&1
rc=$?
check "pin mismatch exit" 2 "$rc"
expect_in "pin mismatch" "$WORK/mismatch.txt" "MISMATCH"

if [ "$fail" -eq 0 ]; then
	echo "upstream-drift self-test: all PASS"
else
	echo "upstream-drift self-test: FAILURES" >&2
fi
exit "$fail"
