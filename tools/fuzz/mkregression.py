#!/usr/bin/env python3
"""Build fuzz regression images for the §5.2 leaks (GAPS.md §5.2).

    tools/fuzz/mkregression.py SEED.img OUT.img [MODE]

MODE "secure" (default): zeroes the "FILE" magic of MFT record 9
($Secure), which is loaded after $Volume, so the mount reads the
volume label and then fails. Regression for the volume-label leak:
ntfs_fill_super() freed the volume with a bare kfree(vol) and the
label leaked (43 bytes per mount attempt).

MODE "logfile-runs": corrupts $LogFile's $DATA run list so the first
pair decodes (allocating the run list) and the next pair is malformed
(header 0x10: zero length). Regression for the run-list leak:
decode_runs() left the half-built list behind on error, and
ntfs_image_parse() never freed the $MFT runs when a later step failed
(192 bytes per mount attempt). ntfs_image_open() also leaked its fd
on a failed parse; same fix, same commit.

MODE "boot-cpr": sets the boot sector's clusters-per-MFT-record byte
to -56. Regression for the shift-overflow UB: ntfs_image_parse()
computed 1u << -cpr with cpr = -56 -- undefined behaviour, caught by
UBSan on the first 600 s fuzz run. The driver's parse_ntfs_boot_sector
has the same unguarded shift (plus a zero sectors-per-cluster path
that wraps ffs(0) - 1 into a huge shift); both are fixed in the same
commit, and the driver's copy is reachable on a read-only mount, where
$LogFile replay does not run first.

Run the fuzzer binary (ASan+LSan) over the output with -runs=1: before
the fix it reports a leak, after the fix it exits clean. The plain
standalone runner cannot see the leak (no sanitizer); the CI fuzz job
runs these.
"""
import struct
import sys


def load_seed(seed_path):
    with open(seed_path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 512 or data[3:11] != b"NTFS    ":
        raise ValueError(f"{seed_path}: not an NTFS image")
    bps = struct.unpack_from("<H", data, 11)[0]
    spc = data[13]
    mft_lcn = struct.unpack_from("<Q", data, 48)[0]
    rec_size_byte = struct.unpack_from("<b", data, 64)[0]
    if rec_size_byte < 0:
        rec_size = 1 << (-rec_size_byte)
    else:
        rec_size = rec_size_byte * bps * spc
    mft_off = mft_lcn * spc * bps
    return data, mft_off, rec_size


def make_secure(data, mft_off, rec_size):
    secure_off = mft_off + 9 * rec_size
    if data[secure_off:secure_off + 4] != b"FILE":
        raise ValueError("record 9 is not a FILE record")
    # $Secure (record 9) unreadable: mount reads the label from $Volume
    # (record 3) and then fails loading $Secure.
    data[secure_off:secure_off + 4] = b"\x00\x00\x00\x00"
    return "$Secure record magic zeroed"


def make_logfile_runs(data, mft_off, rec_size):
    rec_off = mft_off + 2 * rec_size
    rec = data[rec_off:rec_off + rec_size]
    if rec[:4] != b"FILE":
        raise ValueError("record 2 is not a FILE record")
    o = struct.unpack_from("<H", rec, 0x14)[0]
    while o + 8 <= rec_size:
        typ, ln = struct.unpack_from("<II", rec, o)
        if typ == 0xFFFFFFFF:
            raise ValueError("no $DATA attribute in record 2")
        if typ == 0x80 and rec[o + 8]:  # non-resident $DATA
            mpo = struct.unpack_from("<H", rec, o + 0x20)[0]
            p = o + mpo
            # Walk the pairs to the 0x00 terminator, then break it:
            # 0x10 is a header with zero length, which decode_runs
            # rejects -- but only after the first pair allocated.
            while p < o + ln:
                h = rec[p]
                if h == 0:
                    data[rec_off + p] = 0x10
                    return "$LogFile run-list terminator replaced with a zero-length pair"
                ls, os = h & 0xF, h >> 4
                p += 1 + ls + os
            raise ValueError("$DATA run list has no terminator")
        o += ln
    raise ValueError("record 2 attribute walk ran off the end")


def make_boot_cpr(data, mft_off, rec_size):
    # Clusters-per-MFT-record byte at boot sector offset 0x40. -56 makes
    # ntfs_image_parse() shift 1u << 56 (undefined behaviour); the fix
    # rejects anything below -16 before shifting.
    if data[0x40] == 0xC8:
        raise ValueError("seed already has cpr -56")
    data[0x40] = 0xC8  # -56
    return "boot sector clusters-per-MFT-record set to -56"


MODES = {"secure": make_secure, "logfile-runs": make_logfile_runs, "boot-cpr": make_boot_cpr}


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    seed_path, out_path = sys.argv[1], sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) == 4 else "secure"
    if mode not in MODES:
        print(f"unknown mode {mode!r}, want one of {sorted(MODES)}", file=sys.stderr)
        return 2
    try:
        data, mft_off, rec_size = load_seed(seed_path)
        note = MODES[mode](data, mft_off, rec_size)
    except ValueError as e:
        print(str(e), file=sys.stderr)
        return 1
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"{out_path}: {note} (MFT at {mft_off}, record size {rec_size})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
