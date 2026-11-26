#!/usr/bin/env python3
"""Patch a file's non-resident $DATA attribute for the a83e82b0ec3a +
ffc755828de1 validator tests.

Upstream ffc755828de1 raises min_len from 64 to 72 for sparse/compressed
non-resident attributes (the compressed_size field); upstream a83e82b0ec3a
requires the name to lie within the attribute without overlapping the
mapping pairs, and requires 8 bytes of room after mapping_pairs_offset
for the compressed_size field a conversion to sparse would add. The
port's inline validator in ntfs_attr_find() had none of these checks.

Modes (all on the first matching non-resident $DATA in record MFTNO):
  room64      unnamed $DATA, length := 64. mapping_pairs_offset stays 64,
              so room after it is 0 < 8: the new room check rejects it.
  sparse64    unnamed $DATA, length := 64 and ATTR_IS_SPARSE set: the new
              min_len-72 check rejects it (the room check is skipped for
              sparse attributes, so this isolates the min_len fix).
  mpinside    named $DATA (ADS NAME), mapping_pairs_offset :=
              name_offset + name_length (a value strictly inside the
              name: name_offset < mp_offset < name_end). The old
              validator passes (mp_offset >= 64) and ntfs_attr_iget's
              own name_offset < mp_offset check passes, so a size query
              succeeds without the fix; the new name check
              (name_end > mp_offset) rejects it. mp_offset := 64 would
              also trip the new name check, but ntfs_attr_iget already
              refuses name_offset >= mp_offset, so that variant fails in
              BOTH builds and cannot discriminate.

usage: patch_nonres_attr.py IMG MFTNO MODE [NAME]
  NAME is the ADS name for mpinside (default "user.big").
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32

AT_DATA = 0x80
AT_END = 0xFFFFFFFF
ATTR_IS_SPARSE = 0x8000


def attr_name(rec, o):
    n = rec[o + 9]
    off = u16(rec, o + 0x0A)
    return bytes(rec[o + off:o + off + 2 * n]).decode("utf-16-le")


def main():
    img = sys.argv[1]
    mftno = int(sys.argv[2])
    mode = sys.argv[3]
    name = sys.argv[4] if len(sys.argv) > 4 else "user.big"

    vol = Vol(img)
    rec, off = vol.read_mft_raw(mftno)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "record %d is not an MFT record" % mftno

    target = None
    for o, t, l in vol.attrs(rec):
        if t != AT_DATA or not rec[o + 8]:
            continue
        nm = attr_name(rec, o) if rec[o + 9] else ""
        if mode == "mpinside":
            if nm == name:
                target = o
                break
        else:
            if not nm:
                target = o
                break
    assert target is not None, "no matching non-resident $DATA in record %d" % mftno
    o = target

    if mode == "room64":
        struct.pack_into("<I", rec, o + 4, 64)
        print("patched $DATA at record offset %d: length := 64" % o)
    elif mode == "sparse64":
        struct.pack_into("<I", rec, o + 4, 64)
        struct.pack_into("<H", rec, o + 12, u16(rec, o + 12) | ATTR_IS_SPARSE)
        print("patched $DATA at record offset %d: length := 64, sparse set" % o)
    elif mode == "mpinside":
        name_off = u16(rec, o + 0x0A)
        name_len = rec[o + 9]
        new_mp = name_off + name_len
        struct.pack_into("<H", rec, o + 0x20, new_mp)
        print("patched $DATA %r at record offset %d: mapping_pairs_offset := %d "
              "(name [%d, %d))" % (name, o, new_mp, name_off,
                                   name_off + 2 * name_len))
    else:
        raise SystemExit("unknown mode %r" % mode)

    fixup_apply(rec, vol.bps)
    vol.f.seek(off)
    vol.f.write(rec)
    vol.f.close()


if __name__ == "__main__":
    main()
