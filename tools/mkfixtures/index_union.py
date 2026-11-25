#!/usr/bin/env python3
"""Read or patch the $FILE_NAME union of a directory index entry.

Why this exists: a symlink's $FILE_NAME carries a four-byte union -- packed
EA size for an ordinary file, the reparse tag for a reparse point. chkdsk
rewrites that union in the *index* entry to the EA form (packed size 0x2d,
reserved 0x0f) whenever the MFT record carries $EA_INFORMATION, and Windows
then lists the link as a zero-byte file (docs/UPSTREAM-BUGS.md finding 23).
ntfsinfo only shows the MFT-resident copy, so this script reads and writes
the index copy, which is the one Windows displays.

usage: index_union.py IMG NAME [--parent MFTNO] [--set-ea-form] [--set-tag TAG]
  With no --set-*, prints the union of NAME's entry under parent (default
  root, MFT 5) as `union=0x%08x` plus the decoded halves, and exits 0.
  --set-ea-form writes the chkdsk damage (packed 0x2d, reserved 0x0f).
  --set-tag writes a reparse tag, e.g. 0xA000000C.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32, u64

AT_INDEX_ROOT = 0x90
AT_INDEX_ALLOCATION = 0xA0
AT_END = 0xFFFFFFFF


def scan_entries(buf, base, end):
    """Yield (offset_of_union, mref, name_bytes) for each index entry."""
    p = base
    while p < end:
        elen = u16(buf, p + 8)
        klen = u16(buf, p + 10)
        flags = u16(buf, p + 12)
        if flags & 2:  # END
            return
        if elen == 0 or klen == 0:
            return
        mref = u64(buf, p) & 0xFFFFFFFFFFFF
        fn = p + 16
        nlen = buf[fn + 0x40]
        name = bytes(buf[fn + 0x42:fn + 0x42 + 2 * nlen])
        yield fn + 0x3C, mref, name
        p += elen


def find_union(vol, parent, name_utf16):
    """Return (kind, buf, union_offset, writeback) for the entry, or None."""
    rec, off = vol.read_mft_raw(parent)
    fixup_undo(rec)
    for o, t, l in vol.attrs(rec):
        if t == AT_INDEX_ROOT and rec[o + 9] == 4:
            vo = u16(rec, o + 0x14)
            ir = o + vo
            ih = ir + 16
            eo = u32(rec, ih + 0)
            il = u32(rec, ih + 4)

            def wb():
                fixup_apply(rec)
                _, off2 = vol.read_mft_raw(parent)
                vol.f.seek(off2)
                vol.f.write(rec)

            for uoff, mref, name in scan_entries(rec, ih + eo, ih + il):
                if name == name_utf16:
                    return ('root', rec, uoff, wb)
        if t == AT_INDEX_ALLOCATION and rec[o + 9] == 4:
            runs = vol.runlist_of((bytes(rec), 0), AT_INDEX_ALLOCATION,
                                  undone=True)
            size = u64(rec, o + 0x30)
            for boff in range(0, size, vol.idx_blk):
                doff = vol.vcn_to_off(runs, boff)
                vol.f.seek(doff)
                blk = bytearray(vol.f.read(vol.idx_blk))
                if blk[:4] != b'INDX':
                    continue
                fixup_undo(blk)
                ih = 0x18
                eo = u32(blk, ih + 0)
                il = u32(blk, ih + 4)

                def wb(blk=blk, doff=doff):
                    fixup_apply(blk)
                    vol.f.seek(doff)
                    vol.f.write(blk)

                for uoff, mref, name in scan_entries(blk, ih + eo, ih + il):
                    if name == name_utf16:
                        return ('block@%d' % boff, blk, uoff, wb)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('name')
    ap.add_argument('--parent', type=int, default=5)
    ap.add_argument('--set-ea-form', action='store_true')
    ap.add_argument('--set-tag', type=lambda x: int(x, 0), default=None)
    args = ap.parse_args()

    if args.set_ea_form and args.set_tag is not None:
        ap.error('--set-ea-form and --set-tag are exclusive')
    vol = Vol(args.img)
    name_utf16 = args.name.encode('utf-16-le')
    found = find_union(vol, args.parent, name_utf16)
    if found is None:
        print("no index entry for %r under mft %d" % (args.name, args.parent),
              file=sys.stderr)
        return 1
    kind, buf, uoff, wb = found
    before = u32(buf, uoff)
    if args.set_ea_form or args.set_tag is not None:
        new = 0x000F002D if args.set_ea_form else args.set_tag
        struct.pack_into('<I', buf, uoff, new)
        wb()
        vol.f.flush()
        print("%s: union 0x%08x -> 0x%08x (%s)" % (args.name, before, new, kind))
    else:
        packed = before & 0xFFFF
        reserved = before >> 16
        print("union=0x%08x packed_ea_size=0x%04x reserved=0x%04x (%s)"
              % (before, packed, reserved, kind))
    return 0


if __name__ == '__main__':
    sys.exit(main())
