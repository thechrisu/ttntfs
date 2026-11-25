#!/usr/bin/env python3
"""Insert $EA + $EA_INFORMATION ($LXUID/$LXGID/$LXMOD) into a symlink's MFT record.

Why this exists: builds before e6b3418 wrote $LXUID/$LXGID/$LXMOD on every
inode, symlinks included. New builds skip $EA on symlinks, so nothing on this
VM can create an old-style link any more -- but the $EA migration
(docs/GAPS.md §2.5) needs one to migrate. This script writes exactly the bytes
the old build wrote, verified against a regular file created by the current
driver (same ntfs_set_ea code path, unchanged since): three 20-byte entries
(next_entry_offset 20 even for the last, flags 0, name "$LXUID" etc., 4-byte
LE values), $EA_INFORMATION with packed length 0x2d and query length 0x3c.
ntfsinfo decodes the result and ntfsck passes it.

The $FILE_NAME union is left alone: a link created by a build between c8a6fb3
and e6b3418 already carries the reparse tag there, and the index copy too.
The chkdsk damage -- the EA form of the union in the index entry -- is a
separate step, done by index_union.py --set-ea-form.

usage: patch_ea.py IMG MFTNO [UID GID]
  UID/GID default to 501/20 (the test mount's uid/gid); mode is 0xA1FF
  (S_IFLNK|0777) -- a symlink's mode by definition.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32

AT_EA_INFORMATION = 0xD0
AT_EA = 0xE0
AT_END = 0xFFFFFFFF


def ea_value(uid, gid, mode):
    out = bytearray()
    for name, val in ((b"$LXUID", uid), (b"$LXGID", gid), (b"$LXMOD", mode)):
        entry = struct.pack('<IBBH', 20, 0, len(name), 4)
        entry += name + b'\0' + struct.pack('<I', val)
        entry += b'\0' * (20 - len(entry))
        assert len(entry) == 20
        out += entry
    assert len(out) == 60
    return bytes(out)


def ea_info_value():
    # ea_length 0x2d = packed sizes 15+15+15; need_ea_count 0;
    # ea_query_length 0x3c = stored sizes 20+20+20.
    return struct.pack('<HHI', 0x2D, 0, 0x3C)


def insert_attr(rec, atype, value):
    """Insert a resident unnamed attribute in type order; returns new used."""
    used = u32(rec, 0x18)
    alloc = u32(rec, 0x1C)
    inst = u16(rec, 0x28)
    alen = (0x18 + len(value) + 7) & ~7
    assert used + alen <= alloc, "record full"
    insert_at = None
    o = u16(rec, 0x14)
    while u32(rec, o) != AT_END:
        if insert_at is None and u32(rec, o) > atype:
            insert_at = o
        o += u32(rec, o + 4)
    if insert_at is None:
        # No attribute sorts after this one: insert before the END marker.
        insert_at = o
    hdr = struct.pack('<IIBBHHHIHBB', atype, alen, 0, 0, 0x18, 0, inst,
                      len(value), 0x18, 0, 0)
    attr = hdr + value + b'\0' * (alen - 0x18 - len(value))
    tail = rec[insert_at:used]
    rec[insert_at:insert_at + alen] = attr
    rec[insert_at + alen:insert_at + alen + len(tail)] = tail
    struct.pack_into('<I', rec, 0x18, used + alen)
    struct.pack_into('<H', rec, 0x28, inst + 1)
    return used + alen


def main():
    img, mftno = sys.argv[1], int(sys.argv[2])
    uid = int(sys.argv[3]) if len(sys.argv) > 3 else 501
    gid = int(sys.argv[4]) if len(sys.argv) > 4 else 20
    vol = Vol(img)
    rec, off = vol.read_mft_raw(mftno)
    assert rec[:4] == b'FILE'
    fixup_undo(rec)
    assert not (u16(rec, 0x16) & 2), "record must not be a directory"
    for o, t, l in vol.attrs(rec):
        assert t not in (AT_EA_INFORMATION, AT_EA), "record already has $EA"
    insert_attr(rec, AT_EA_INFORMATION, ea_info_value())
    insert_attr(rec, AT_EA, ea_value(uid, gid, 0xA1FF))
    fixup_apply(rec)
    vol.f.seek(off)
    vol.f.write(rec)
    vol.f.flush()
    print("patched mft %d: $EA_INFORMATION + $EA ($LXUID=%d $LXGID=%d $LXMOD=0xA1FF)"
          % (mftno, uid, gid))


if __name__ == '__main__':
    main()
