# Upstream sources

Pristine copies. Never edit files here; the port lives in `core/`.

| Path | Origin | Ref |
|---|---|---|
| `linux-v7.1/fs/ntfs/` | https://github.com/torvalds/linux `fs/ntfs` (the "ntfsplus" driver by Namjae Jeon, merged for 7.1) | tag `v7.1`, commit `8cd9520d35a6c38db6567e97dd93b1f11f185dc6` |

Refresh:

    git clone --depth=1 --branch <tag> --filter=blob:none --sparse https://github.com/torvalds/linux.git
    cd linux && git sparse-checkout set fs/ntfs
    rm -rf ../upstream/linux-vX.Y/fs/ntfs && cp -R fs/ntfs ../upstream/linux-vX.Y/fs/ntfs

Diff `core/` against this directory to see exactly what the port changed.

Drift tracking: `tools/upstream-drift.sh` (GAPS.md §5.3). `port-diff`
diffs `core/ntfs/` against this directory (no network);
`upstream` compares this pin against the latest stable tag, verifies
this copy still matches the pin byte-for-byte, and lists the commits
that touched `fs/ntfs` in between. `.github/workflows/upstream-drift.yml`
runs it weekly and fails on drift -- that is the triage list. Do not
move the pin (refresh this directory to a newer tag) until the drift
has been triaged; the next run then compares against the new pin.
