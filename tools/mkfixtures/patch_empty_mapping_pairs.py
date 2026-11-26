#!/usr/bin/env python3
"""Empty a file's $DATA mapping-pairs array, on disk.

Why this exists: upstream 766062a82e1c ("ntfs: reject invalid empty
mapping pairs") adds the validation the mapping-pairs decoder was
missing: an empty array is valid only for a zero-length attribute
(highest_vcn == vcn - 1 and all sizes zero). A driver-created volume
can never reach the invalid state on its own -- the allocator always
writes at least one run for a nonzero file -- so the array is emptied
by hand, the way a crafted image would.

The patcher zeroes the first byte of the file's non-resident $DATA
mapping-pairs array (the array terminator) and leaves everything
else alone: the run bytes stay in the record past the terminator,
highest_vcn and all sizes stay as the driver wrote them, so the
attribute still describes a nonzero file with no runs at all. Only
one byte changes, inside the record, and the update-sequence fixups
are redone the same way the other patchers do it.

usage: patch_empty_mapping_pairs.py IMG MFTNO
  Prints: highest_vcn allocated_size data_size initialized_size
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32

AT_DATA = 0x80


def main():
    img, mftno = sys.argv[1], int(sys.argv[2])

    vol = Vol(img)
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
    assert rec[p] != 0, "mapping pairs already empty in record %d" % mftno
    highest_vcn = struct.unpack_from('<q', rec, data_off + 0x18)[0]
    allocated = struct.unpack_from('<Q', rec, data_off + 0x28)[0]
    data_size = struct.unpack_from('<Q', rec, data_off + 0x30)[0]
    initialized = struct.unpack_from('<Q', rec, data_off + 0x38)[0]
    assert data_size > 0, "file is already empty in record %d" % mftno

    rec[p] = 0

    fixup_apply(rec, vol.bps)
    vol.f.seek(off)
    vol.f.write(rec)
    vol.f.close()

    print("%d %d %d %d" % (highest_vcn, allocated, data_size, initialized))


if __name__ == "__main__":
    main()
