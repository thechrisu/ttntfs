#!/usr/bin/env python3
"""Set FILE_ATTR_SYSTEM on a file's MFT record, $FILE_NAME and parent index entry.

Why this exists: ntfs-3g's Interix symlinks are plain files with
FILE_ATTR_SYSTEM whose $DATA is "IntxLNK\\1" + UTF-16LE target. Nothing on
this VM can create one -- there is no FUSE to mount ntfs-3g under -- so the
test creates the file through our own driver with the right $DATA bytes, and
this script flips the SYSTEM bit in the three places ntfs-3g sets it
($STANDARD_INFORMATION, $FILE_NAME, parent index entry), the same three
places patch_reparse.py patches for the reparse bit. ntfsinfo decodes the
result and ntfsck passes it.

The $DATA bytes are NOT touched: the caller (the test, or ntfscp) has already
written the magic + target. A file patched here with anything else in $DATA
is a plain SYSTEM file, which is the negative control.

usage: patch_interix.py IMG MFTNO
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import (Vol, fixup_undo, fixup_apply, patch_index_entry,
                           u16, u32, u64)

FILE_ATTR_SYSTEM = 0x4
AT_STANDARD_INFORMATION = 0x10
AT_FILE_NAME = 0x30


def patch_record(vol, mftno):
    rec, off = vol.read_mft_raw(mftno)
    assert rec[:4] == b'FILE'
    fixup_undo(rec)
    assert not (u16(rec, 0x16) & 2), "record must not be a directory"
    fn_name = None
    parent = None
    for o, t, l in vol.attrs(rec):
        vo = u16(rec, o + 0x14)
        if t == AT_STANDARD_INFORMATION:
            fa = u32(rec, o + vo + 0x20) | FILE_ATTR_SYSTEM
            struct.pack_into('<I', rec, o + vo + 0x20, fa)
        elif t == AT_FILE_NAME:
            fa = u32(rec, o + vo + 0x38) | FILE_ATTR_SYSTEM
            struct.pack_into('<I', rec, o + vo + 0x38, fa)
            nlen = rec[o + vo + 0x40]
            fn_name = bytes(rec[o + vo + 0x42:o + vo + 0x42 + 2 * nlen])
            parent = u64(rec, o + vo) & 0xFFFFFFFFFFFF
    assert fn_name is not None, "no $FILE_NAME in record"
    fixup_apply(rec)
    vol.f.seek(off)
    vol.f.write(rec)
    return fn_name, parent


def main():
    img, mftno = sys.argv[1], int(sys.argv[2])
    vol = Vol(img)
    fn_name, parent = patch_record(vol, mftno)
    where = patch_index_entry(vol, parent, fn_name, mftno, None,
                              flag=FILE_ATTR_SYSTEM)
    vol.f.flush()
    print("patched mft %d: FILE_ATTR_SYSTEM set; index entry: %s" % (mftno, where))


if __name__ == '__main__':
    main()
