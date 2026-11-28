#!/bin/sh
# Merge two static libraries into one, portably (Linux / GNU ar).
# macOS uses Apple's libtool -static instead (see top-level CMakeLists.txt).
# Usage: merge-static-libs.sh OUT.a IN1.a IN2.a
# GNU ar's MRI script handles duplicate object basenames correctly,
# unlike extracting both archives into one directory.
set -eu
OUT=$1; IN1=$2; IN2=$3
AR=${AR:-ar}
MRI=$(mktemp)
trap 'rm -f "$MRI"' EXIT INT TERM
printf 'CREATE %s\nADDLIB %s\nADDLIB %s\nSAVE\nEND\n' "$OUT" "$IN1" "$IN2" > "$MRI"
"$AR" -M < "$MRI"
${RANLIB:-ranlib} "$OUT"
