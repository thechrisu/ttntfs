#!/usr/bin/env python3
"""Shrink the $AttrDef system file's $DATA to SIZE bytes, for the
c8504fc1245f mount test.

Upstream c8504fc1245f rejects at mount a $AttrDef too small to hold
one attr_def entry (160 bytes): the old check only required the size
to be > 0, so a 120-byte table mounted, and creating a file then read
ad->type (offset 128 in the 160-byte entry) past the kvzalloc'd buffer
in ntfs_attr_find_in_attrdef(). This patcher sets the unnamed
non-resident $DATA of MFT record 4 (FILE_AttrDef) to data_size =
initialized_size = SIZE (default 120); allocated_size is left alone
(the clusters are still allocated). The mount must then fail with
-EINVAL; without the fix it succeeds.

usage: patch_attrdef_size.py IMG [SIZE]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply

AT_DATA = 0x80
FILE_ATTRDEF = 4


def main():
    img = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 120

    vol = Vol(img)
    rec, off = vol.read_mft_raw(FILE_ATTRDEF)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "record 4 is not an MFT record"

    target = None
    for o, t, l in vol.attrs(rec):
        if t == AT_DATA and rec[o + 8] and not rec[o + 9]:
            target = o
            break
    assert target is not None, "no unnamed non-resident $DATA in record 4"
    o = target

    old = struct.unpack_from("<Q", rec, o + 0x30)[0]
    struct.pack_into("<Q", rec, o + 0x30, size)  # data_size
    struct.pack_into("<Q", rec, o + 0x38, size)  # initialized_size
    print("patched $AttrDef $DATA at record offset %d: "
          "data_size %d -> %d" % (o, old, size))

    fixup_apply(rec, vol.bps)
    vol.f.seek(off)
    vol.f.write(rec)
    vol.f.close()


if __name__ == "__main__":
    main()
