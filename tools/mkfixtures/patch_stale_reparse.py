#!/usr/bin/env python3
"""Plant a stale $Extend/$Reparse ($R) index entry for a file not yet created.

Why this exists: upstream 9692b1b4fc00 fixes update_reparse_data() swallowing
set_reparse_index()'s error, so a symlink whose index insert fails still
reported success while no reparse data existed on disk. The insert fails
with -EEXIST when the $R index already holds an entry for the file's
(tag, file_id) key -- ntfs_ie_add() returns -EEXIST when its lookup
finds the entry. A driver-created volume can never reach that state on
its own: the $R entry is removed when the file goes away, so the entry
is planted by hand, the way a crafted image would.

The entry is planted for the record the driver's next file creation
will take. On a fresh basic-4k.img copy allocation is strictly
sequential from the fixture's highest used record (294, per the
manifest): the first file created takes record 295, the next 296, and
so on, all with sequence number 1 (verified with ntfscli cp-in). The
test creates a sacrificial file first, reads its record number back
through the driver, and plants the entry for the NEXT record, so no
fixture fact is hard-coded here.

The $R index on this fixture lives in $Reparse (record 26): a resident
$INDEX_ROOT whose entries area holds only the END entry (index_length
32, allocated_size 32). The planted entry is 32 bytes -- 16-byte
index_entry_header, 12-byte key (reparse tag + file_id), 4-byte
filling -- inserted before END, with index_length and allocated_size
bumped to 64, the $INDEX_ROOT length and content size bumped by 32,
and bytes_in_use bumped by 32. The test's symlink then takes the
native tag (IO_REPARSE_TAG_SYMLINK, 0xA000000C) for a windows-safe
target, so the planted key is (0xA000000C, file_id).

usage: patch_stale_reparse.py IMG MFTNO [SEQ]
  MFTNO is the record the next created file will take, SEQ its
  sequence number (1 for a record never used before). The target
  record must be free, or the script refuses: a planted entry for a
  live file would make a real symlink fail -EEXIST for real.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch_reparse import Vol, fixup_undo, fixup_apply, u16, u32

AT_INDEX_ROOT = 0x90
AT_END = 0xFFFFFFFF
IO_REPARSE_TAG_SYMLINK = 0xA000000C

# index_entry_header (16) + key tag (4) + key file_id (8) + filling (4)
ENTRY_LEN = 32


def find_reparse(vol):
    """$Reparse is a child of $Extend (record 11); find it by its $R index."""
    for n in range(12, 64):
        rec, _ = vol.read_mft_raw(n)
        if rec[:4] != b"FILE":
            continue
        fixup_undo(rec, vol.bps)
        for o, t, l in vol.attrs(rec):
            if t != AT_INDEX_ROOT:
                continue
            name_len = rec[o + 9]
            name_off = u16(rec, o + 0x0A)
            name = bytes(rec[o + name_off:o + name_off + 2 * name_len])
            if name.decode("utf-16-le") == "$R":
                return n
    raise RuntimeError("$Reparse ($R index) not found")


def main():
    img = sys.argv[1]
    mftno = int(sys.argv[2])
    seq = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    vol = Vol(img)
    rno = find_reparse(vol)

    # The target record must be free: the driver will allocate it to the
    # next created file, which is the symlink the test then creates.
    tgt, _ = vol.read_mft_raw(mftno)
    assert tgt[:4] != b"FILE" or not (u16(tgt, 0x16) & 0x01), \
        "record %d is already in use" % mftno

    rec, off = vol.read_mft_raw(rno)
    rec = bytearray(rec)
    fixup_undo(rec, vol.bps)
    assert rec[:4] == b"FILE", "not an MFT record"

    root_off = None
    for o, t, l in vol.attrs(rec):
        if t != AT_INDEX_ROOT:
            continue
        name_len = rec[o + 9]
        name_off = u16(rec, o + 0x0A)
        name = bytes(rec[o + name_off:o + name_off + 2 * name_len])
        if name.decode("utf-16-le") == "$R":
            root_off, root_len = o, l
            break
    assert root_off is not None, "no $INDEX_ROOT $R in $Reparse"
    assert not rec[root_off + 8], "$INDEX_ROOT $R is non-resident"

    root_val = root_off + u16(rec, root_off + 0x14)
    assert u32(rec, root_val) == 0x0, "unexpected indexed attribute type"
    ih = root_val + 16  # INDEX_HEADER follows the 16-byte root header
    entries_off = u32(rec, ih)
    index_len = u32(rec, ih + 4)
    alloc = u32(rec, ih + 8)
    assert entries_off == 16, "entries_off %d, expected 16" % entries_off
    assert index_len == 32 and alloc == 32, \
        "$R root is not just the END entry (index_length %d, allocated %d)" \
        % (index_len, alloc)

    # The END entry is the last 16 bytes of the entries area.
    end_pos = ih + index_len - 16
    assert u16(rec, end_pos + 12) == 2, "last $R entry is not END"
    used = u32(rec, 0x18)
    assert u32(rec, used - 8) == AT_END, "record END marker not at bytes_in_use"
    assert used + ENTRY_LEN <= u32(rec, 0x1C), "no room in $Reparse record"

    file_id = mftno | (seq << 48)
    entry = struct.pack("<QHHHH", file_id, ENTRY_LEN, 12, 0, 0) \
        + struct.pack("<IQI", IO_REPARSE_TAG_SYMLINK, file_id, 0)
    assert len(entry) == ENTRY_LEN

    # Shift the END entry and the record END marker right by ENTRY_LEN,
    # then write the planted entry in front of END.
    rec[end_pos + ENTRY_LEN:used + ENTRY_LEN] = rec[end_pos:used]
    rec[end_pos:end_pos + ENTRY_LEN] = entry
    used += ENTRY_LEN

    # Grow the header fields that describe the root and the record.
    rec[ih + 4:ih + 8] = struct.pack("<I", index_len + ENTRY_LEN)
    rec[ih + 8:ih + 12] = struct.pack("<I", alloc + ENTRY_LEN)
    rec[root_off + 4:root_off + 8] = struct.pack("<I", root_len + ENTRY_LEN)
    rec[root_off + 16:root_off + 20] = struct.pack(
        "<I", u32(rec, root_off + 16) + ENTRY_LEN)
    rec[0x18:0x18 + 4] = struct.pack("<I", used)

    fixup_apply(rec, vol.bps)
    vol.f.seek(off)
    vol.f.write(rec)
    print("patched $Reparse (mft %d): planted $R entry (tag 0x%08x, "
          "file_id %d:%d), index_length 32 -> 64, bytes_in_use %d"
          % (rno, IO_REPARSE_TAG_SYMLINK, mftno, seq, used))


if __name__ == "__main__":
    main()
