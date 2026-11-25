#!/usr/bin/env python3
"""Shrink a directory's $INDEX_ROOT block size and free record space.

Why this exists: upstream dc09bf79b76f fixes a heap OOB write in
ntfs_ir_to_ib(), which memcpys all index-root entries into a freshly
allocated index block with no capacity check. To reproduce it the root
must hold more entry bytes than the index block can hold -- but the
port (like upstream) refuses to grow $INDEX_ROOT once the MFT record
is within 120 bytes of full (ntfs_attr_record_resize), and the
smallest legal index block is 512 bytes (block_size < 512 is rejected
on load), so a driver-created directory can never reach the overflow
on its own: at reparent its root holds 328 entry bytes against a 488
byte capacity.

This script makes the overflow reachable on a driver-created
directory, by hand, in the way a crafted image would:
  1. deletes $EA_INFORMATION + $EA from the directory's record
     (120 bytes freed; the $FILE_NAME packed_ea_size is zeroed too)
     and $SECURITY_DESCRIPTOR (104 bytes freed; the load/create
     paths never read a directory's own $SECURITY_DESCRIPTOR, so
     the root can grow past 488 bytes before the 120-byte headroom
     rule forces a reparent;
  2. sets $INDEX_ROOT index_block_size to 512 (the minimum the
     loader accepts), so the fresh index block holds 488 bytes.

After patching, adding files grows the root to 536 entry bytes and
the next add reparents: without the dc09bf79b76f check the memcpy
overflows the 512-byte block; with it ntfs_ir_to_ib() returns NULL
and the create fails with -EIO, leaving the directory intact.

usage: patch_ib_size.py IMG MFTNO [BLOCK_SIZE]
  BLOCK_SIZE defaults to 512. The directory must still be a small
  index (resident $INDEX_ROOT, no $INDEX_ALLOCATION yet).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32

AT_INDEX_ROOT = 0x90
AT_EA_INFORMATION = 0xD0
AT_EA = 0xE0
AT_SECURITY_DESCRIPTOR = 0x50
AT_END = 0xFFFFFFFF


def main():
    img, mftno = sys.argv[1], int(sys.argv[2])
    block_size = int(sys.argv[3]) if len(sys.argv) > 3 else 512
    assert block_size >= 512, "block_size < 512 is rejected on load"

    vol = Vol(img)
    rec, off = vol.read_mft_raw(mftno)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "not an MFT record"

    # Locate the attributes we need.
    attrs = {}
    for aoff, atype, alen in vol.attrs(rec):
        attrs[atype] = (aoff, alen)
    assert AT_INDEX_ROOT in attrs, "no $INDEX_ROOT"
    assert AT_EA in attrs, "no $EA to delete (already patched?)"
    assert AT_EA_INFORMATION in attrs, "no $EA_INFORMATION to delete"

    used = u32(rec, 0x18)

    # 1. Delete $EA_INFORMATION + $EA: they are the last attributes
    #    before END, so move END up over them and shrink bytes_in_use.
    ea_info_off, ea_info_len = attrs[AT_EA_INFORMATION]
    ea_off, ea_len = attrs[AT_EA]
    assert ea_info_off + ea_info_len == ea_off, "EA attrs not adjacent"
    end_off = ea_off + ea_len
    assert u32(rec, end_off) == AT_END, "END not after $EA"
    freed = ea_info_len + ea_len
    rec[end_off - freed:end_off - freed + 8] = rec[end_off:end_off + 8]
    used -= freed

    # 2. Delete $SECURITY_DESCRIPTOR: it sits between $FILE_NAME and
    #    $INDEX_ROOT, so shift everything after it up over it.
    sd_off, sd_len = attrs[AT_SECURITY_DESCRIPTOR]
    rec[sd_off:used - sd_len] = rec[sd_off + sd_len:used]
    used -= sd_len
    freed += sd_len
    rec[0x18:0x18 + 4] = struct.pack("<I", used)

    # Re-walk: the $INDEX_ROOT offset moved when $SECURITY_DESCRIPTOR
    # was deleted; $FILE_NAME did not (it precedes it).
    attrs = {}
    for aoff, atype, alen in vol.attrs(rec):
        attrs[atype] = (aoff, alen)

    # Zero the $FILE_NAME packed_ea_size (u16 at value offset + 60),
    # now stale. (file_attributes is at +56; do not touch it.)
    fn_off, _ = attrs[0x30]
    val_off = fn_off + u16(rec, fn_off + 0x14)
    assert u16(rec, val_off + 60) != 0, "ea_length already zero"
    rec[val_off + 60:val_off + 62] = b"\0\0"

    # 3. Shrink the index block size in $INDEX_ROOT (value offset + 8).
    root_off, _ = attrs[AT_INDEX_ROOT]
    root_val = root_off + u16(rec, root_off + 0x14)
    old_ib = u32(rec, root_val + 8)
    assert old_ib >= 512, "unexpected index_block_size %d" % old_ib
    rec[root_val + 8:root_val + 12] = struct.pack("<I", block_size)

    fixup_apply(rec, vol.bps)
    vol.f.seek(off)
    vol.f.write(rec)
    print("patched mft %d: index_block_size %d -> %d, freed %d bytes "
          "($EA/$EA_INFORMATION/$SECURITY_DESCRIPTOR deleted), "
          "bytes_in_use %d"
          % (mftno, old_ib, block_size, freed, used))


if __name__ == "__main__":
    main()
