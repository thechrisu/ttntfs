#!/bin/bash
# Architecture consistency: the Xcode app/extension and the CMake core
# library must agree on the arch list, or the x86_64 app slice fails to
# link in CI (GAPS.md §4.2). The pins live in four places; this asserts
# they all name both arm64 and x86_64, and that the generated Xcode
# project does not override ARCHS anywhere (the xcconfig is the single
# source of truth). Runs everywhere -- no macOS, no SDK. The real
# universal build is CI's job (tools/ci.sh checks both slices with lipo).
#
#   tools/check-archs.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT" || exit 1

fail=0

# fskit/Config/Shared.xcconfig: ARCHS = arm64 x86_64
archs=$(grep -E '^ARCHS[[:space:]]*=' fskit/Config/Shared.xcconfig | head -1)
echo "xcconfig: ${archs:-<no ARCHS line>}"
case "$archs" in
*"arm64"*"x86_64"*|*"x86_64"*"arm64"*) ;;
*) echo "FAIL: Shared.xcconfig ARCHS must list both arm64 and x86_64" >&2; fail=1 ;;
esac

# CMakeLists.txt (top-level, logfile, pagecache): the
# CMAKE_OSX_ARCHITECTURES default must list both archs.
for f in CMakeLists.txt core/logfile/CMakeLists.txt platform/pagecache/CMakeLists.txt; do
	line=$(grep -E 'set\(CMAKE_OSX_ARCHITECTURES' "$f" | head -1)
	echo "$f: ${line:-<no CMAKE_OSX_ARCHITECTURES default>}"
	case "$line" in
	*"arm64"*"x86_64"*|*"x86_64"*"arm64"*) ;;
	*) echo "FAIL: $f CMAKE_OSX_ARCHITECTURES default must list both arm64 and x86_64" >&2; fail=1 ;;
	esac
done

# The generated project must not pin ARCHS itself.
if grep -q "ARCHS" fskit/NTFS.xcodeproj/project.pbxproj; then
	echo "FAIL: fskit/NTFS.xcodeproj/project.pbxproj sets ARCHS; the xcconfig must be the only place" >&2
	fail=1
fi

exit $fail
