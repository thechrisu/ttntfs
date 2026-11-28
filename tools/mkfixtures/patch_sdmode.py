#!/usr/bin/env python3
"""Grow a driver's $STANDARD_INFORMATION to version 3 and set security_id.

Why this exists: the driver creates $SI as version 1.2 (48 bytes), which
has no security_id slot (namei.c: "windows will upgrade it to version 3
if needed"). Windows would upgrade it on first touch; nothing on macOS
can. To test the §2.2 ACL read path without a Windows round trip, this
script turns a driver-created file or directory into what Windows would
have left: a 72-byte $SI whose v3 fields carry the given security_id,
grown in place (the tail of the record shifts by 24 bytes, the attribute
length, value length and the record's bytes_in_use all grow by 24, and
the update-sequence fixups are redone). The $FILE_NAME copy is untouched
(it carries no security_id).

core/tests/test_links.c drives it (test_sdmode_wiring).

usage: patch_sdmode.py IMG MFTNO SECURITY_ID
  SECURITY_ID is decimal; 256 and 257 are the two ids in a fresh mkntfs
  image's $SDS (see docs/ACL-DESIGN.md §2.3).
"""
import struct, sys

AT_STANDARD_INFORMATION = 0x10
AT_END = 0xFFFFFFFF


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def fixup_undo(rec, sector=512):
    usa_ofs, usa_cnt = u16(rec, 4), u16(rec, 6)
    usn = rec[usa_ofs:usa_ofs + 2]
    for i in range(1, usa_cnt):
        pos = i * sector - 2
        assert rec[pos:pos + 2] == usn, "fixup mismatch at %d" % pos
        rec[pos:pos + 2] = rec[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]


def fixup_apply(rec, sector=512, bump=True):
    usa_ofs, usa_cnt = u16(rec, 4), u16(rec, 6)
    usn = u16(rec, usa_ofs)
    if bump:
        usn = (usn + 1) & 0xFFFF
        if usn == 0:
            usn = 1
    struct.pack_into('<H', rec, usa_ofs, usn)
    for i in range(1, usa_cnt):
        pos = i * sector - 2
        rec[usa_ofs + 2 * i:usa_ofs + 2 * i + 2] = rec[pos:pos + 2]
        struct.pack_into('<H', rec, pos, usn)


class Vol:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        bs = self.f.read(512)
        self.bps = u16(bs, 0x0B)
        spc = bs[0x0D]
        self.cs = self.bps * spc
        self.mft_lcn = u64(bs, 0x30)
        cpr = struct.unpack_from('<b', bs, 0x40)[0]
        self.mft_rec = (1 << -cpr) if cpr < 0 else cpr * self.cs
        cpi = struct.unpack_from('<b', bs, 0x44)[0]
        self.idx_blk = (1 << -cpi) if cpi < 0 else cpi * self.cs
        # runlist of $MFT
        self.mft_runs = self.runlist_of(self.read_mft_raw(0), 0x80)

    def read_mft_raw(self, n):
        # only handles the first extent of $MFT for records we care about
        if hasattr(self, 'mft_runs'):
            off = self.vcn_to_off(self.mft_runs, n * self.mft_rec)
        else:
            off = self.mft_lcn * self.cs + n * self.mft_rec
        self.f.seek(off)
        return bytearray(self.f.read(self.mft_rec)), off

    def vcn_to_off(self, runs, byte_off):
        for vcn, lcn, length in runs:
            lo, hi = vcn * self.cs, (vcn + length) * self.cs
            if lo <= byte_off < hi:
                return lcn * self.cs + (byte_off - lo)
        raise RuntimeError("byte offset not mapped")

    def attrs(self, rec):
        o = u16(rec, 0x14)
        while u32(rec, o) != AT_END:
            yield o, u32(rec, o), u32(rec, o + 4)
            o += u32(rec, o + 4)
        yield o, AT_END, 8

    def runlist_of(self, recoff, atype, undone=False):
        rec = bytearray(recoff[0])
        if not undone:
            fixup_undo(rec)
        for o, t, l in self.attrs(rec):
            if t == atype and rec[o + 8]:
                mp = u16(rec, o + 0x20)
                p = o + mp
                runs, vcn, lcn = [], 0, 0
                while rec[p]:
                    h = rec[p]; ls, os_ = h & 15, h >> 4
                    length = int.from_bytes(rec[p + 1:p + 1 + ls], 'little')
                    d = int.from_bytes(rec[p + 1 + ls:p + 1 + ls + os_], 'little', signed=True)
                    lcn += d
                    runs.append((vcn, lcn, length)); vcn += length
                    p += 1 + ls + os_
                return runs
        raise RuntimeError("no non-resident attr 0x%x" % atype)


def patch_record(vol, mftno, security_id):
    rec, off = vol.read_mft_raw(mftno)
    assert rec[0:4] == b'FILE', "record %d is not FILE" % mftno
    fixup_undo(rec)
    for o, t, l in vol.attrs(rec):
        if t != AT_STANDARD_INFORMATION:
            continue
        assert not rec[o + 8], "$SI must be resident"
        value_len = u32(rec, o + 16)
        value_off = u16(rec, o + 20)
        assert value_len in (48, 72), \
            "$SI value length %d, expected 48 (v1.2) or 72 (v3)" % value_len
        if value_len == 72:
            # Already v3: just set the id in place.
            struct.pack_into('<I', rec, o + value_off + 52, security_id)
            break
        # v1.2 (48) -> v3 (72): keep the first 36 bytes (times + flags),
        # replace the 12 reserved bytes and append the 24-byte v3 tail.
        # v3 fields: maximum_versions, version_number, class_id, owner_id,
        # security_id, quota_charged, usn -- all zero except security_id.
        v3 = struct.pack('<IIIIIQQ', 0, 0, 0, 0, security_id, 0, 0)
        assert len(v3) == 36
        new_value = bytes(rec[o + value_off:o + value_off + 36]) + v3
        assert len(new_value) == 72
        bytes_in_use = u32(rec, 0x18)
        assert bytes_in_use + 24 <= len(rec), "no room to grow $SI"
        tail = bytes(rec[o + value_off + 48:bytes_in_use])
        rec[o + value_off:o + value_off + 48] = new_value[:48]
        rec[o + value_off + 48:o + value_off + 72] = new_value[48:]
        # Shift the tail 24 bytes: splice it back after the grown value.
        rec[o + value_off + 72:o + value_off + 72 + len(tail)] = tail
        struct.pack_into('<I', rec, o + 4, l + 24)   # attr length
        struct.pack_into('<I', rec, o + 16, 72)      # value length
        struct.pack_into('<I', rec, 0x18, bytes_in_use + 24)
        break
    else:
        raise RuntimeError("no $SI in record %d" % mftno)
    fixup_apply(rec)
    vol.f.seek(off)
    vol.f.write(rec)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    vol = Vol(sys.argv[1])
    patch_record(vol, int(sys.argv[2]), int(sys.argv[3]))
    vol.f.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
