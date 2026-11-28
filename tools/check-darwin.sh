#!/bin/bash
# Darwin cross-check: preprocess every C file for x86_64-apple-darwin.
#
#   tools/check-darwin.sh
#
# Why this exists. The Linux build passed while the macOS CI build failed
# (checks run 45661009566): platform/include/linux/string.h used
# __GLIBC_PREREQ in a single-line #if, which clang expands even in the
# branch that would short-circuit, and which is undefined on macOS.
# Nothing in the Linux build can catch that class of bug, because glibc
# defines the macro. This check preprocesses the whole C tree with a
# Darwin target and no SDK, so Darwin-only branches are evaluated the
# way the macOS runner evaluates them.
#
# What it is and is not. It is a *preprocess-only* check (-E): it catches
# preprocessor errors, missing includes, and Linux-only assumptions in
# #if branches. It does not type-check or compile -- that needs the macOS
# SDK, which this VM does not have, and stays CI's job (the ctest, Swift,
# and app-build stages). System headers are satisfied by empty stubs in
# tools/darwin-stubs/, which deliberately define nothing, so a Darwin
# branch that leans on a glibc macro fails here exactly as on macOS.
#
# Requires clang (the compiler macOS uses). Install: apt install clang.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
CLANG=${CLANG:-clang}

command -v "$CLANG" >/dev/null 2>&1 || {
	echo "check-darwin: clang not found (apt install clang)" >&2
	exit 2
}

fail=0
checked=0
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
while IFS= read -r f; do
	checked=$((checked + 1))
	if ! "$CLANG" --target=x86_64-apple-darwin -E -nostdinc \
		-I "$HERE/darwin-stubs" \
		-iquote "$ROOT/core/ntfs" \
		-iquote "$ROOT/core/vfs" \
		-I "$ROOT/platform/include" -I "$ROOT/core/include" \
		-I "$ROOT/core/logfile" -I "$ROOT/tools/common" \
		"$ROOT/$f" -o /dev/null 2>"$tmp"; then
		echo "FAIL (darwin preprocess): $f"
		sed 's/^/    /' "$tmp"
		fail=1
	fi
done <<EOF
$(cd "$ROOT" && find core platform tools/ntfscli -name '*.c' | sort)
EOF

if [ "$fail" -eq 0 ]; then
	echo "check-darwin: PASS ($checked files preprocessed for x86_64-apple-darwin)"
fi
exit $fail
