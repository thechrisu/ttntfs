#!/usr/bin/env python3
"""Point a file's first $DATA run outside the volume, on disk.

Why this exists: upstream 19cac7902a8a ("ntfs: bound the free-cluster
bitmap scan to the volume") and fea9e4488f38 ("ntfs: verify run length
exceeding volume boundary") add the bounds checks the mapping-pairs
decoder was missing: a run whose starting LCN is at or beyond
nr_clusters, or whose LCN + length runs past nr_clusters, must be
refused at decode time. A driver-created volume can never reach that
state on its own -- the allocator only hands out in-volume clusters --
so the run is rewritten by hand, the way a crafted image would.

The patcher rebuilds the first run of the file's non-resident $DATA
with a 3-byte length and a 3-byte offset field (room for any LCN the
test needs), shifting the following attributes inside the record and
redoing the update-sequence fixups. Only the LCN changes: the length,
highest_vcn and sizes stay as the driver wrote them, so the runlist
still describes the file. It also appends 0x42 ('B') marker bytes to
the host file past the end of the volume, covering the rewritten run,
so a read that escapes the volume returns known bytes instead of EOF:
without the fix the read succeeds and returns the marker; with the
fix the decode fails and the read returns an error.

usage: patch_oob_run.py IMG MFTNO MODE [LCN]
  MODE = start | run-end
  start:   first run starts at LCN (default nr_clusters + 10)
  run-end: first run starts at LCN (default nr_clusters - 5), length
           unchanged, so LCN + length runs past nr_clusters
  Prints: nr_clusters cluster_size old_lcn old_len new_lcn new_len
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32

AT_DATA = 0x80
AT_END = 0xFFFFFFFF
MARKER = 0x42


def main():
    img, mftno, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    assert mode in ("start", "run-end"), "MODE must be start or run-end"
    lcn_arg = int(sys.argv[4]) if len(sys.argv) > 4 else None

    vol = Vol(img)
    # nr_clusters comes from the boot sector: total sectors / sectors per
    # cluster, as the driver computes it (volume size in clusters).
    vol.f.seek(0x28)
    total_sectors = struct.unpack('<Q', vol.f.read(8))[0]
    vol.f.seek(0x0D)
    spc = vol.f.read(1)[0]
    nr_clusters = total_sectors // spc

    rec, off = vol.read_mft_raw(mftno)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "record %d is not FILE" % mftno

    data_off = None
    for o, t, l in vol.attrs(rec):
        if t == AT_DATA and rec[o + 8]:
            data_off = o
            break
    assert data_off is not None, "no non-resident $DATA in record %d" % mftno

    mp = u16(rec, data_off + 0x20)
    p = data_off + mp
    attr_end = data_off + u32(rec, data_off + 4)
    # Decode the runs; the test files are single-run (contiguous).
    runs = []
    q = p
    while rec[q]:
        h = rec[q]
        ls, os_ = h & 15, h >> 4
        length = int.from_bytes(rec[q + 1:q + 1 + ls], 'little')
        delta = int.from_bytes(rec[q + 1 + ls:q + 1 + ls + os_], 'little',
                               signed=True)
        runs.append((q, h, ls, os_, length, delta))
        q += 1 + ls + os_
    assert len(runs) == 1, "expected exactly one run, got %d" % len(runs)
    _, _, _, _, old_len, old_delta = runs[0]
    old_lcn = old_delta  # first run: previous LCN is 0

    if lcn_arg is not None:
        new_lcn = lcn_arg
    elif mode == "start":
        new_lcn = nr_clusters + 10
    else:
        new_lcn = nr_clusters - 5
    assert new_lcn >= 0, "negative LCN"
    if mode == "start":
        assert new_lcn >= nr_clusters, "start mode needs LCN >= nr_clusters"
    else:
        assert new_lcn < nr_clusters, "run-end mode needs LCN < nr_clusters"
        assert new_lcn + old_len > nr_clusters, \
            "run-end mode needs LCN + length > nr_clusters"

    # Rebuild the run: header 0x33 (3-byte length, 3-byte offset),
    # length unchanged, delta = new_lcn (first run, previous LCN 0).
    new_run = bytes([0x33]) + old_len.to_bytes(3, 'little') + \
        new_lcn.to_bytes(3, 'little', signed=True)
    old_array_len = (q + 1) - p  # runs + 0x00 terminator
    new_array = new_run + b"\x00"
    shift = len(new_array) - old_array_len
    used = u32(rec, 0x18)
    alloc = u32(rec, 0x1C)
    assert used + shift <= alloc, "no room in record %d for wider run" % mftno
    # Shift everything after the old array (attributes after $DATA, the
    # END marker) by `shift`, then splice in the new array.
    tail_start = q + 1
    tail = bytes(rec[tail_start:used])
    rec[tail_start + shift:used + shift] = tail
    rec[p:p + len(new_array)] = new_array
    # $DATA length and record bytes_in_use grow by the shift.
    struct.pack_into('<I', rec, data_off + 4,
                     u32(rec, data_off + 4) + shift)
    struct.pack_into('<I', rec, 0x18, used + shift)

    fixup_apply(rec, vol.bps)
    vol.f.seek(off)
    vol.f.write(rec)

    # Marker past the end of the volume, covering the rewritten run, so
    # an escaping read returns known bytes.
    lo = max(nr_clusters, new_lcn) * vol.cs
    hi = (new_lcn + old_len) * vol.cs
    if hi > lo:
        vol.f.seek(lo)
        vol.f.write(bytes([MARKER]) * (hi - lo))
    vol.f.close()

    print("%d %d %d %d %d %d" % (nr_clusters, vol.cs, old_lcn, old_len,
                                 new_lcn, old_len))


if __name__ == "__main__":
    main()
