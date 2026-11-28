#!/bin/sh
# tools/perf-bench.sh — repeatable performance baseline for the NTFS core.
#
# Measures, on Linux, through ntfscli (the core, no FSKit, no kernel):
#   sequential write/read, random 4 KiB write/read IOPS (warm + cold),
#   fsync split out from the writes, create/stat/readdir/unlink rates.
# Plus a host-side ceiling for the same operations on a plain file, so a
# core number can be read against what the machine itself can do.
#
# Usage (from the repo root):
#   tools/perf-bench.sh [--img-size GiB] [--repeats N] [--out FILE]
#
# What this does NOT measure: anything on macOS. The phase-5 bar is
# Apple's FSKit exFAT on macOS; the numbers here are the Linux core
# baseline and must never be quoted as macOS numbers. The prior macOS
# measurements live in docs/progress/vfs.md (2026-09-13/14).
#
# Requires: tools/.local (mkntfs, from tools/build-ntfsprogs.sh), a built
# core (build/libntfscore.a) and ntfscli (tools/build/ntfscli/ntfscli).
# Dropping the host page cache needs root; without it the cold pass is
# skipped and marked as such.
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
export TMPDIR=${TMPDIR:-/home/hatch/workspace/ttntfs-work/tmp}
mkdir -p "$TMPDIR"

IMG_GIB=2
REPEATS=3
OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--img-size) IMG_GIB="$2"; shift 2 ;;
		--repeats) REPEATS="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		*) echo "usage: $0 [--img-size GiB] [--repeats N] [--out FILE]" >&2; exit 2 ;;
	esac
done

MKNFS="$ROOT/tools/.local/sbin/mkntfs"
NTFSCLI="$ROOT/tools/build/ntfscli/ntfscli"
[ -x "$MKNFS" ] || { echo "perf-bench: missing $MKNFS (run tools/build-ntfsprogs.sh)" >&2; exit 1; }
if [ ! -x "$NTFSCLI" ]; then
	echo "perf-bench: building core + ntfscli"
	cmake -S . -B build >/dev/null
	cmake --build build -j4 >/dev/null
	cmake -S tools/ntfscli -B tools/build/ntfscli >/dev/null
	cmake --build tools/build/ntfscli -j4 >/dev/null
fi

log() { printf '%s\n' "$*"; }
[ -n "$OUT" ] && exec >"$OUT" 2>&1

log "== perf baseline $(date -u +%Y-%m-%dT%H:%M:%SZ) =="
log "host: $(uname -srm), $(nproc) cpus, $(free -m | awk '/^Mem:/ {print $2}') MiB RAM"
log "image: ${IMG_GIB} GiB NTFS (mkntfs, 4 KiB clusters), bench --size 256 --files 1000 --randops 2000"
log "ntfscli: $NTFSCLI"
log ""

i=1
while [ "$i" -le "$REPEATS" ]; do
	IMG="$TMPDIR/perf-$i.img"
	rm -f "$IMG"
	truncate -s ${IMG_GIB}G "$IMG"
	"$MKNFS" -Q -F -s 4096 -c 4096 "$IMG" >/dev/null 2>&1
	log "-- repeat $i --"
	"$NTFSCLI" bench "$IMG" --size 256 --files 1000 --randops 2000
	log ""
	i=$((i + 1))
done

# Cold vs warm random reads on a persistent file (fresh process each pass).
IMG="$TMPDIR/perf-rand.img"
rm -f "$IMG"
truncate -s ${IMG_GIB}G "$IMG"
"$MKNFS" -Q -F -s 4096 -c 4096 "$IMG" >/dev/null 2>&1
HOSTFILE="$TMPDIR/perf-rand.bin"
head -c 268435456 /dev/urandom > "$HOSTFILE"
"$NTFSCLI" cp-in "$IMG" "$HOSTFILE" /rand.bin
log "-- random 4 KiB reads, persistent 256 MiB file --"
# A fresh ntfscli process starts with an empty in-process page cache, so
# this pass is cold for the core. The host page cache cannot be dropped
# in this container (/proc/sys/vm/drop_caches is read-only), so the host
# side stays warm — the label says so, do not read it as a cold-disk pass.
if echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; then
	log "(host page cache dropped)"
	COLD_LABEL="cold"
else
	log "(host page cache NOT droppable here: cold = fresh process only)"
	COLD_LABEL="cold-core"
fi
"$NTFSCLI" randread "$IMG" /rand.bin --ops 2000 --label "$COLD_LABEL" || true
"$NTFSCLI" randread "$IMG" /rand.bin --ops 2000 --label warm || true
log ""

# Host-side ceiling: same operations on a plain file (warm, then cold).
log "-- host ceiling (plain file, same 256 MiB, python3 os.pread/pwrite) --"
python3 - "$HOSTFILE" <<'PY'
import os, random, sys, time
path = sys.argv[1]
nblocks = 256 * 1024 * 1024 // 4096
ops = 2000
rng = random.Random(0x9e3779b9)
offs = [rng.randrange(nblocks) * 4096 for _ in range(ops)]
fd = os.open(path, os.O_RDWR)
t0 = time.monotonic()
for o in offs:
    os.pwrite(fd, b"\x00" * 4096, o)
t1 = time.monotonic()
wr = t1 - t0
t0 = time.monotonic()
os.fsync(fd)
t1 = time.monotonic()
print(f"host rand write 4 KiB x {ops}: {ops / wr:8.0f} IOPS (writes {wr * 1e3:.1f} ms, fsync {(t1 - t0) * 1e3:.1f} ms)")
t0 = time.monotonic()
ck = 0
for o in offs:
    b = os.pread(fd, 4096, o)
    ck += b[0]
t1 = time.monotonic()
print(f"host rand read  4 KiB x {ops} (warm): {ops / (t1 - t0):8.0f} IOPS (cksum {ck})")
os.close(fd)
PY
log ""
log "== end =="
