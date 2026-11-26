#!/usr/bin/env python3
"""Plant an error-marked MFT reference in a root-directory $I30 index entry.

Why this exists: upstream d97a36bae86a normalizes every error-marked MFT
reference returned from ntfs_lookup_inode_by_name() to ERR_MREF(-EIO).
Before the fix, a crafted image could set the MREF error bit while leaving
the low bits arbitrary, and callers decoding the result with MREF_ERR()
would consume a bogus pseudo-errno: low bits 0x42 give MREF_ERR 66, a
positive "error" that IS_ERR() does not even recognise, so ntfs_lookup
reports success with a garbage inode pointer.

The patcher sets the indexed_file of the $I30 entry for NAME in the root
directory (record 5) to 0x0000800000000000 | LOW: the error bit set, low
bits arbitrary. The test then looks the name up through ntfs_lookup():
with the fix it returns -EIO; without it, it returns 0 with a bogus
handle (the old code's MREF_ERR is 66, which IS_ERR() misses).

The root index on this fixture is large: record 5 carries both a
resident $INDEX_ROOT (END entry only) and a non-resident $INDEX_ALLOCATION
whose INDX blocks hold the real entries. The patcher searches the root
first, then every allocation block.

Only the index entry's copy is patched; the file's own $FILE_NAME keeps
its real reference, the way a crafted image would disagree with itself.
No ntfsck run is meaningful on the result -- the image is deliberately
corrupt -- so the test does not fsck_clean it.

usage: patch_err_mref.py IMG NAME [LOW]
  LOW is the arbitrary low bits of the crafted mref (default 0x42).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32, u64

AT_INDEX_ROOT = 0x90
AT_INDEX_ALLOCATION = 0xA0
AT_END = 0xFFFFFFFF
MREF_ERR_BIT = 0x0000800000000000
INDEX_ENTRY_END = 0x02


def find_in_entries(buf, base, entries_off, index_len, needle):
    """Walk the entries at buf[base + entries_off .. base + index_len];
    return the entry offset (relative to buf) whose bytes contain the
    UTF-16LE needle, or None."""
    p = base + entries_off
    end = base + index_len
    while p + 16 <= end:
        length = u16(buf, p + 8)
        flags = u16(buf, p + 12)
        if flags & INDEX_ENTRY_END:
            break
        if length < 16:
            raise RuntimeError("index entry length %d at %d" % (length, p))
        if needle in bytes(buf[p:p + length]):
            return p
        p += length
    return None


def main():
    img = sys.argv[1]
    name = sys.argv[2]
    low = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x42
    needle = name.encode("utf-16-le")

    vol = Vol(img)
    rec, off = vol.read_mft_raw(5)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "record 5 is not an MFT record"

    target = None  # (where, file_off, buf, entry_off)
    for o, t, l in vol.attrs(rec):
        if t == AT_INDEX_ROOT:
            name_len = rec[o + 9]
            name_off = u16(rec, o + 0x0A)
            nm = bytes(rec[o + name_off:o + name_off + 2 * name_len])
            if nm.decode("utf-16-le") != "$I30":
                continue
            assert not rec[o + 8], "$INDEX_ROOT $I30 is non-resident"
            root_val = o + u16(rec, o + 0x14)
            assert u32(rec, root_val) == 0x30, "unexpected indexed attribute type"
            ih = root_val + 16  # INDEX_HEADER follows the 16-byte root header
            p = find_in_entries(rec, ih, u32(rec, ih), u32(rec, ih + 4), needle)
            if p is not None:
                target = ("$INDEX_ROOT", off, rec, p)
                break
        elif t == AT_INDEX_ALLOCATION:
            assert rec[o + 8], "$INDEX_ALLOCATION $I30 is resident"
            runs = vol.runlist_of((rec, off), AT_INDEX_ALLOCATION, undone=True)
            for vcn, lcn, length in runs:
                for i in range(length):
                    blk_off = (lcn + i) * vol.cs
                    vol.f.seek(blk_off)
                    blk = bytearray(vol.f.read(vol.idx_blk))
                    if blk[:4] != b"INDX":
                        raise RuntimeError("index block at %d is not INDX" % blk_off)
                    fixup_undo(blk, vol.bps)
                    ih = 24  # INDEX_HEADER follows the 24-byte block header
                    p = find_in_entries(blk, ih, u32(blk, ih),
                                        u32(blk, ih + 4), needle)
                    if p is not None:
                        target = ("$INDEX_ALLOCATION block @%d" % blk_off,
                                  blk_off, blk, p)
                        break
                if target is not None:
                    break
            if target is not None:
                break
    assert target is not None, "no $I30 entry for %r" % name

    where, file_off, buf, entry_off = target
    old = u64(buf, entry_off)
    struct.pack_into("<Q", buf, entry_off, MREF_ERR_BIT | low)
    print("patched $I30 entry for %r in %s at offset %d: mref 0x%016x -> 0x%016x"
          % (name, where, entry_off, old, MREF_ERR_BIT | low))

    fixup_apply(buf, vol.bps)
    vol.f.seek(file_off)
    vol.f.write(buf)
    vol.f.close()


if __name__ == "__main__":
    main()
