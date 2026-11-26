# TT NTFS Native — gaps to Paragon / Tuxera, and what this driver still needs

Written 2026-11-25, from the repo's own docs (`docs/TODO.md`,
`docs/PORTING.md`, `docs/UPSTREAM-BUGS.md`, `docs/LOGFILE.md`) plus the
fixes landed on the `thechrisu/ttntfs` fork. Each item says how it is
verified. Rule for every fix: **thorough e2e tests, with the evidence in
the commit message** (test names, counts, fixture results, `chkdsk` /
`ntfsck` output). A fix without that evidence does not land.

Status words: `open` (do it), `done` (landed, with the commit),
`blocked` (needs something this VM cannot provide), `wontfix`
(deliberate stance, recorded so it is not re-litigated).

Work order is the order below: data-safety first, then features,
then performance, then integration, then testing/shipping.

## 1. Data safety — fix first, these can eat data

### 1.1 Compressed-file creation corrupts files — done (2026-11-25)
Files created in a Windows-compressed folder now come out compressed and
read back intact. Two root causes, both in the fork's write path, not
upstream's flag: (1) `do_write()` routed a fresh compressed file -- still
resident -- through the ordinary write path, so raw data landed in a
compressed attribute's clusters in a shape no reader can decode (65,536
bytes OK, 65,537 EIO); it now pre-expands and routes non-resident
compressed writes through `ntfs_compress_write()`. (2)
`ntfs_non_resident_attr_expand()` placed the runlist terminator inside
the last hole block for compressed attributes (`first_free_vcn`
unrounded), so the final block was unreadable after a remount and the
extend punch failed with -EINVAL; `first_free_vcn` is now rounded up to
a compression-block boundary for compressed attributes. Gates flipped
with the fix: `super.c` sets `NV_Compression` (volumes >= 3, <= 4K
clusters), `namei.c` inherits COMPRESSED for regular files as well as
directories. `test_create_in_compressed_directory` now asserts the
created files carry the flag, are stored smaller than their logical
size, and read back after a remount.
Evidence: `test_compress` 412 checks 0 failures; plain ctest 15/15;
UBSan ctest 15/15; `tools/run-tests.sh all` 265 passed 0 failed 0
skipped with `NTFS_REQUIRE_FSCK=1`; ntfs-3g `ntfscat` reads a 200,000-byte
file written into a compressed folder in full (was 196,608 then "Failed
to read a compressed cluster" at 0x30000), our `cat` the same;
`tools/pre-push.sh` green. Still outstanding: a Windows round trip
(`chkdsk` clean, byte-for-byte read-back) -- needs a Windows machine.

### 1.2 No journal writing, only replay — open, large, scoped (2026-11-25)
The driver replays Windows' `$LogFile` on mount but never writes its
own records. Metadata is write-back, flushed within 1 s and on
sync/unmount, so a yank mid-write has no journal to recover from.
Full `$LogFile` journaling (writing records, not just replay) is the
item that makes yanks safe (`docs/PORTING.md` §6). Scoped in
`docs/JOURNAL-DESIGN.md` (this round); no implementation yet, by
design — the finding is that it is several months of focused work,
comparable to the phase-4 replay work. Headlines: no writer exists
anywhere in the ecosystem this driver is built from (vendored
`logfile.c`, ntfs-3g, ntfs3 `fslog.c` are all replay-only or nothing;
the ecosystem's recovery path is the dirty flag plus `chkdsk`); on a
read-write mount the journal is retired (`mark_clean`, 2 pages) and
every later metadata write is unjournaled; Windows is a co-reader of
our records from the first commit (a yank leaves them in the log for
Windows to replay), so the bar is `chkdsk` + Windows-mount
acceptance, not self-agreement; WAL means log-at-commit-time with a
synchronous log flush before the operation's folios are eligible for
background writeback; write v1.1 records first (already proven
accepted by Windows via `mark_clean`). Phased plan in the design doc:
Phase 0 gates (§1.3 replay-atomicity proof, §1.5 real v2.0 image),
Phase 1 LFS writer infrastructure (2–4 weeks), Phase 2 one operation
class end-to-end (file creation, 2–3 weeks), Phase 3 class by class
(~8 classes, 2–4 months), Phase 4 wrap/checkpoint/performance,
Phase 5 Windows round-trip per class. Next round starts at Phase 0/1.

### 1.3 Replay atomicity unproven — open
Nothing demonstrated a partial write during replay, nothing rules one
out (`docs/UPSTREAM-BUGS.md` finding 17b). Flush writes records one at
a time; the overlay protects planning only. This is why replay stays
opt-in per volume. Verify: fault-injection test that kills the process
mid-replay and shows the volume still mounts / replays cleanly.

### 1.4 Hibernated volumes untested — blocked (needs Windows)
Fast Startup (B2) is handled. A real hibernated volume (B1) was
attempted twice and nobody managed to strand a write mid-hibernation,
so that path is untested. Needs a Windows machine that can hibernate
mid-write.

### 1.5 `$LogFile` v2.0 acceptance — done (needs a real image)
`core/ntfs/logfile.c` now accepts version 2.0 as well as 1.1 (fork
commit 1d7f15ef), so mounts stop logging "LogFile version 2.0 is not
supported". Still open: there is no real v2.0 log image to test
against — the logfile suite covers it only synthetically. Close it by
capturing a real v2.0 log from a Windows machine and adding it to the
fixtures.

### 1.6 8 KB xattr not checked — done (2026-11-25)

`test_names.c` gained `test_xattr_big_persists`: an 8 KB `user.big`
xattr (the phase-2 gate's F-xattr group, `tools/phase2-gate/write.py`)
plus a small `user.comment`, set on one file, then read back
byte-for-byte after a remount, listed, and read by `ntfscat` as an
ordinary named `$DATA` stream — with `ntfsck -n` run while the stream
is present, which the 300 KiB test never did (it only checked the
volume after removing the large stream). `ntfsinfo` on a driver-written
image confirms the named `$DATA` is non-resident (8000 bytes data,
8192 allocated) — the different storage path the inline limit forces.
Evidence: test_names 471 checks, 0 failures; non-vacuity
proven by flipping one expectation (got 8000, expected 8001);
ntfsinfo residency check above. Still open, as before: a Windows
round trip (`dir /r`, chkdsk) needs a Windows machine.

## 2. Features Paragon / Tuxera have that this doesn't

### 2.1 Cannot read ntfs-3g Interix symlinks — done (2026-11-25)

A file with FILE_ATTR_SYSTEM set, no I30 index and an unnamed $DATA
starting with the 8-byte `IntxLNK\x01` magic is now presented as
S_IFLNK, with the UTF-16LE target (no NUL terminator) decoded into
`ni->target`, so `ntfs_readlink` / `link_target` work unchanged. The
detection mirrors ntfs-3g's own rules (`libntfs/dir.c`:
`ntfs_dir_entry_type`, `ntfs_fuse_readlink`): magic match, size
8 < n <= 8 + 2*PATH_MAX, SYSTEM set, no reparse point. At load time
the $DATA size is checked in the MFT record first and resident data is
read straight from the record; an attribute inode is created only for
non-resident $DATA. Readdir returns DT_LNK for the same entries.

Format verified against ntfsprogs-plus 1.0.0 source and against a
gold-standard link built with real `ntfs_create_symlink` (ntfs-3g
2026.7.7, `tools/mkfixtures/mkintx.c`): a fixture patched with
`tools/mkfixtures/patch_interix.py` matches the gold record
byte-for-byte (MFT 295, SI/FN 0x24, $DATA resident 20 bytes).

Tests: `test_links` 624 checks, 0 failures (new `test_interix_symlinks`:
relative / absolute / non-ASCII targets, 3000-char non-resident target,
negative controls SYSTEM-without-magic / magic-without-SYSTEM /
magic-only, readdir types, unlink removes only the link, ntfsck clean);
rebuilt `ntfscli` lists the link as `l----` size 6. Full battery:
plain ctest 15/15, UBSan ctest 15/15, `tools/run-tests.sh all` with
NTFS_REQUIRE_FSCK=1: 265 passed, 0 failed, 0 skipped;
`tools/pre-push.sh` green (Linux stages; Swift/app are CI-only).

The work also exposed and fixed a real driver bug: an attribute inode
kept alive past `ntfs_put_super` (it holds a reference on its base and
is evicted only in `sb_free`) could keep a base inode that is re-dirtied
after the unmount sync — $Volume, by clearing the dirty flag — alive
until eviction, where writeback dereferenced the already-NULL
`vol->mft_ino` and segfaulted (`test_crash test_data_before_metadata`).
`__ntfs_write_inode` now returns -EIO when `vol->mft_ino` is NULL: the
write is impossible once teardown has released $MFT, and the data is
discarded with the volume anyway. The reparse-point read path has the
same shape, so the guard protects it too. The Interix detector itself
no longer creates attribute inodes for metadata files in the first
place ($Volume's empty $DATA, $MFT, $LogFile, $UpCase are all ruled out
by the record-level size check).

### 2.2 No ACL / security-descriptor mapping — open, by design so far
Permissions are effectively ignored (`noowners` semantics, SIDs
preserved, nothing enforced). Paragon and Tuxera map NTFS ACLs to
Unix permissions. Decide the mapping first; the v1 stance is recorded
in `docs/PORTING.md` §6.

### 2.3 No TRIM for SSDs — blocked (no FSKit path)
There is no FSKit path to issue discards. Not fixable from the driver
side; record it and move on unless Apple adds one.

### 2.4 No verify / repair in the app — wontfix (stance)
Deliberate: chkdsk is the tool and the project will not pretend
otherwise. Journal recovery is the safe subset and it is done. Paragon
ships Disk Utility verify/repair; this does not, on purpose.

### 2.5 `$EA` migration for pre-e6b3418 symlinks — done (2026-11-25)
Symlinks created before commit e6b3418 still carried `$EA`, and chkdsk
still rewrote their index entries. A lazy migration now repairs them: the
first time such a link is loaded on a read-write mount,
`ntfs_migrate_symlink_ea()` removes `$EA` and `$EA_INFORMATION`, clears the
HasEA flag, and re-syncs the parent index entry's union to the real reparse
tag with `ntfs_inode_sync_filename()`, so Windows lists it as a link again
without waiting for the next chkdsk. Read-only mounts never migrate (a
dirty volume mounts read-only, and migration is a write). Nothing is lost:
mode 0777, uid/gid mount-wide. On any failure the link is left as it was,
still usable on the Mac.
Evidence: new `test_symlink_ea_migration` in `test_links` (651 checks, 0
failures, was 624) -- a link hand-built into the pre-e6b3418 state with
`tools/mkfixtures/patch_ea.py` ($EA bytes verified against a regular file
the driver created), the chkdsk damage written with
`tools/mkfixtures/index_union.py --set-ea-form`, then one RW load: `$EA`
gone, index union back to the tag 0xa000000c, link still resolves, ntfsck
clean; a second link proves a read-only mount leaves `$EA` alone. Plain
ctest 15/15, UBSan ctest 15/15, `tools/run-tests.sh all` 265 passed 0
failed 0 skipped with `NTFS_REQUIRE_FSCK=1`, `tools/pre-push.sh` green.
Still outstanding: a `chkdsk` round trip on Windows (the local checks are
ntfsck + direct index-byte reads, which is what chkdsk reconciles).
Side fixes in the same commit: the merged archive `build/libntfscore.a`
went stale silently -- the custom merge command depended on the `ntfs`
core/platform targets by name only, which is an ordering dependency, not
a file dependency, so a plain build never re-merged after a core change
(ntfscli linked the stale archive). The command now also depends on the
two archive files via `$<TARGET_FILE:...>`; verified a plain build
re-merges after a touch. Docs corrected too: every `e045c2e` in
TODO.md/UPSTREAM-BUGS.md/GAPS.md was a stale hash (no such object in this
clone) -- the fix commit is `e6b3418`.

## 3. Performance — the structural gap

### 3.1 Userspace costs what a kernel driver doesn't — open, mitigate only
Paragon runs in the kernel. This runs in userspace over FSKit, so
every metadata op pays an XPC round trip (~0.3–0.6 ms, 10–20× a kernel
`stat`). A warm inode cache mitigates it; it cannot be eliminated.
Measure, don't guess: benchmark metadata ops against Apple's exFAT
and record the numbers per release.

### 3.2 Random 4 KiB writes miss the bar — open
Phase 5 bar is "within 2× of Apple's exFAT"; random 4 KiB writes still
miss it. Attributed to the macOS buffer cache rather than the driver —
prove that attribution with a benchmark before changing code.

## 4. macOS integration

### 4.1 Disk Utility shows volumes as "Unknown" — blocked (needs Apple)
`FSShortName` must be `ttntfs` and `diskutil` resolves VolumeKind
against `/System/Library/Filesystems`. Using `ntfs` fixes the label
and breaks mounting with ECONNREFUSED from `fskitd`. Not fixable
without Apple changing FSKit, or shipping an untested
`/Library/Filesystems/ttntfs.fs` bundle (puts a file outside the app
bundle — test it in a VM before ever shipping it).

### 4.2 Arm64 only, no x86_64 app build — done (2026-11-25)

The app and extension now build universal (arm64 + x86_64). Four pins
changed in one commit: `CMAKE_OSX_ARCHITECTURES` defaults to
`arm64;x86_64` in `CMakeLists.txt`, `core/logfile/CMakeLists.txt` and
`platform/pagecache/CMakeLists.txt` (an explicit `-D` still wins), and
`fskit/Config/Shared.xcconfig` sets `ARCHS = arm64 x86_64` — the
generated project sets ARCHS nowhere, so the xcconfig is the single
source of truth. CI verifies both slices with `lipo` in `tools/ci.sh`:
`stage_ctest` checks `build/libntfscore.a`, `stage_build_app` checks
the app and extension binaries. `tools/check-archs.sh` (pre-push
stage 8) pins the four-way consistency locally and is verified by
negative test. Follow-up fix the same day: the first universal CI run
failed only on the check itself — `stage_build_app` looked for the
extension binary under `Contents/PlugIns`, but ExtensionKit extensions
do not live there; the build had succeeded and `lipo` had already
verified `libntfscore.a: x86_64 arm64`. The check now locates both
binaries with `find` across the whole app bundle. No arch-specific
code stood in the way: the only inline
asm in the tree is the arch-neutral `barrier()` memory clobber, and
`tools/check-darwin.sh` already preprocesses the whole C tree for
x86_64-apple-darwin.

Still open: nothing has ever *run* on Intel hardware — the universal
build is verified by compilation and `lipo` on the arm64 runner only.
A first run on a real Intel Mac (or a Rosetta-less x86_64 VM) is the
remaining check; no Intel hardware is available here.

### 4.3 macOS 26 only — wontfix unless demand appears
Deployment target is 26.0, one FSKit API generation. 15.4+ support
only if demand appears.

### 4.4 Enabling the extension is fragile — open, partly Apple
The System Settings toggle is broken for third-party modules on
26.3/26.6.2 (the app enables itself instead, which is why it ships
unsandboxed with Developer ID); on macOS 27 the app can no longer edit
the list and must wait for the user in Settings. Track Apple's
behaviour per release; keep `fskit/scripts/enable-module.sh` working
on every supported version.

### 4.5 No CI signing — blocked (needs a paid Apple Developer account)
The mount test cannot pass until the app is signed with the managed
`com.apple.developer.fskit.fsmodule` entitlement. Stock runners have
no signing identity. The owner declined the paid route on 2026-11-25
(~£79/yr) as not worth it: the mount code is byte-identical to
upstream v0.4.2, which the maintainer already mounted real disks with,
so the risk was judged low. Revisit only if the mount path itself
changes.

## 5. Testing and shipping

### 5.1 FSKit request path only tested by hand — blocked (see 4.5)
`fskit/scripts/mount-test.sh` is the only path to the FSKit request
path and cannot run in CI without signing. It runs manually on the
maintainer's machine. No local substitute exists on this VM.

### 5.2 No fuzzing of the on-disk parser — done
NTFS-3G's 2026 CVEs were crafted-image heap overflows in its parser;
the same class of risk applies to any NTFS parser, including this
one. Fix: add a fuzz harness (libFuzzer target or a `ntfscli`-based
corpus runner), seed it with the fixture images, run it in CI on a
schedule, and fix what it finds. Verify: harness runs, corpus grows,
first findings triaged.

Done (round 4). Harness: `core/fuzz/fuzz_mount.c` (libFuzzer target +
standalone runner; mounts the input as a temp-file image, RW with
$LogFile replay, bounded walk), `tools/fuzz/mkseeds.c` (3 deterministic
4 MiB seeds: basic / 512 B clusters / compressed), `tools/fuzz.sh`
(seeds / check / regression / build / run), CI workflow
`.github/workflows/fuzz.yml` (weekly Sunday 03:00 UTC + dispatch,
ubuntu-latest, own concurrency group `fuzz-${{ github.ref }}` so it can
never cancel a push run, gated on the same 20% budget guard as CI).
`tools/pre-push.sh` runs `tools/fuzz.sh check` (seeds mount, regression
images fail cleanly); the ASan+LSan leak regression is CI's job.

First findings, all fixed in the same commit with deterministic
regression images (`tools/fuzz/mkregression.py`, generated, never
committed; each verified non-vacuous -- it fails on the unfixed code
and passes on the fixed code):
- 43-byte volume-label leak: `ntfs_fill_super()` freed the volume
  with a bare `kfree(vol)` on mount failure; `volume_label` is only
  freed in `ntfs_volume_free()`. Regression `leak-volume-label.img`
  ($Secure record magic zeroed; label read, then load fails).
- 192-byte $LogFile run-list leak: `decode_runs()` left a half-built
  list on error and `ntfs_image_parse()` never freed the $MFT runs when
  a later step failed; `ntfs_image_open()` also leaked its fd on a
  failed parse. Regression `leak-logfile-runs.img` (first pair valid,
  terminator replaced with a zero-length pair).
- Shift-overflow UB: boot sector clusters-per-MFT-record -56 made
  `ntfs_image_parse()` compute `1u << 56`. Fixed there (bound -cpr to
  -16; sectors-per-cluster threshold aligned to the driver's 0xf4) and
  in the driver's `parse_ntfs_boot_sector()` (same shifts, plus a zero
  sectors-per-cluster guard and `INT_MAX` guards on the positive
  branches) -- the driver's copy is reachable on a read-only mount,
  where replay does not run first, and is pinned by a new
  `test_boot_sector_geometry_refusals()` case in `test_mount.c`.
  Regression `ubsan-boot-cpr.img` (record-size byte set to -56).

Evidence: plain ctest 15/15, UBSan ctest 15/15, fixture suite 265/0/0,
`tools/fuzz.sh check` (3 seeds mount, 3 regressions fail as expected),
`tools/fuzz.sh regression` (ASan+LSan, exit 0), 600 s fuzz run
(9,498 exec, 43 exec/s, 352 new units, 1 UBSan finding -- the shift
above) and a post-fix 300 s run (clean), ntfsck clean on all 3 seeds.

Follow-ups (not done): a dirty-$LogFile seed (replay only ever sees
clean logs from the seeds) and a read-only-mount fuzz variant (the
target mounts RW with replay, so `ntfs_fill_super()`'s own geometry
parse is only reached after replay accepts the image).

### 5.3 Upstream tracking has no automation — done 2026-11-25
Tier 0/1 files must stay diffable against upstream Linux `fs/ntfs`
so kernel fixes can be merged. `tools/upstream-drift.sh` now does it,
with `tools/upstream_drift.py` (GitHub API: tag dates + paginated
path-filtered commit list) and `tools/test-upstream-drift.sh`
(self-test, no network, fixture repo):
- `port-diff`: core/ntfs vs the pinned pristine copy, no network,
  per-file +/-, upstream-only and port-only files. Also stage 7 of
  `tools/pre-push.sh` (informational, <1s). Current numbers: 18 files
  differ, +1004 −115; reparse.c (+370) and reparse.h (+42 on a 21-line
  file) are the most diverged.
- `upstream [--tag TAG] [--out FILE]`: verifies the vendored pristine
  copy still matches the pinned tag byte-for-byte (mismatch is an
  error, exit 2), diffs pinned fs/ntfs against the target tag, lists
  the commits touching fs/ntfs in between. Exits 0 clean, 1 on drift.
  Fetch is cheap by design: only the two tag tips, depth 1, trees
  excluded (`--filter=tree:0`), then fs/ntfs sparse-checked out at
  each tag — a full clone, or `git archive` in a blob-filtered clone,
  downloads the whole history/tree and takes minutes (both tried).
- `.github/workflows/upstream-drift.yml`: weekly Sunday 04:00 UTC +
  dispatch, ubuntu-latest, own concurrency group (can never cancel a
  push run), no budget guard (no macOS minutes; ~1 min/week). Fails
  on drift; the report is uploaded as an artifact.

Evidence: `upstream --tag v7.1` exits 0 in ~17 s (pristine copy
matches v7.1 byte-for-byte, 52 files); `upstream` vs latest v7.3
exits 1 in ~25 s with the full report (saved to /tmp/drift-v73.txt);
self-test 15/15 PASS (clean/drift/mismatch exit paths, --out matches
stdout, port-diff baseline reproduced); pre-push 7/7 (see below).

First report, 2026-11-25: v7.1 → v7.3 is 31 files changed (+2944
−1411) and 124 commits. Headliners: WOF support (new `lib/`,
`wof.c`, `ntfs_codec.h`; LZX/XPRESS decompressors), `quota.c`/`quota.h`
removed, and fixes in exactly the areas the port has touched —
compress.c (+201 −170: compressed-write ordering, context reuse,
initialized size after writes), ea.c (+242 −74: EA stream rewrite
before metadata update, empty-pair removal, HasEA on setxattr
failure), reparse.c (+468 −67: native-symlink support in 7.2,
simplified `ntfs_reparse_set_native_symlink`), attrib.c (+472 −151),
mft.c (+370 −229: `$MFTMirr` write offset spanning folios, runlist
lock, `$MFT` repack), index.c (+238 −103: index_root heap OOB write
in `ntfs_ir_to_ib`, index root relocation), plus boot-sector
validation (`sectors_per_cluster`, MFT LCNs — the same class the
§5.2 fuzz fix addressed) and runlist realloc hardening.

Follow-up — triaged 2026-11-25, see `docs/UPSTREAM-TRIAGE-7.3.md`.
All 124 commits classified (patch read + port code checked at the
cited lines), plus one more recovered from a merge aggregate: the
quota removal (`cbe287fd65f1`, "remove unsupported quota handling")
only exists in the `dbad5db3a0bb` merge diff — no individual commit
touching `fs/ntfs/quota.c` is in the 124. Verdicts over 125 items:
**port 61, already-covered 13, not-applicable 35, needs-work 16**.
Headlines: the port independently implemented native Windows
symlinks already (absolute/relative following + creation all
already-covered — only the `native_symlink=` mount option is
missing); `ntfs_new_attr_flags` carries five unfixed bugs in one
function (`0ab6dc1ac460` et al — port as one commit); the worst
single item is the index_root heap OOB write (`dc09bf79b76f`);
compressed writes punch old clusters before the new data lands
(`eb67671cd814`); the port's rw-remount path always fails -EROFS
because of the dead quota marker (`cbe287fd65f1`); WOF is the
biggest remaining feature (9 commits, no `wof.c` / codec interface /
decompressors in the port). The pin does not move until the port
items land — moving it now would just re-baseline the drift report
around 61 known-missing fixes.

Porting progress (one commit per item, in the order above):
- 2026-11-25: `dc09bf79b76f` (index_root heap OOB write, the worst
  single item) — verbatim capacity check in `ntfs_ir_to_ib()`;
  regression test `test_index_root_oversized_reparent` in
  `core/tests/test_bigdir.c` with fixture patcher
  `tools/mkfixtures/patch_ib_size.py`. Without the fix the reparent
  overflows the 512-byte block and leaves the directory unreadable
  (every lookup -EIO); with it the create fails cleanly with -EIO
  and the entries survive. Evidence: bigdir 507 checks 0 failures
  with the fix, 10 lookup failures without it; plain ctest 15/15,
  UBSan ctest 15/15, `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1.
- 2026-11-25: the `ntfs_new_attr_flags` cluster — `7ed0028126b5`
  (resident conversion), `9969d58a9b04` (no-change resource leak),
  `0ab6dc1ac460` (payload before shrink), `acb1095fd2db` (memmove
  overlap), `8d139e3635c8` (extent record pointer) — ported as one
  commit: the composed function is byte-identical to upstream v7.3's
  `ntfs_new_attr_flags` (208 lines, verified against torvalds/linux
  v7.3 `fs/ntfs/ea.c`, sha b4fcfbe2da4c). Regression test
  `test_attr_flags` in `core/tests/test_compress.c`, driven through
  the VFS setxattr handler (`system.ntfs_attrib`) — the production
  entry point — because the test API cannot reach the VFS
  `struct ntfs_inode *`. The resident-conversion and shrink/re-enable
  cases fail without the fix (lookup -95 after remount, ntfsck exit
  4); the leak, memmove-overlap, and extent-pointer fixes are
  carried verbatim and proven by the byte-identical diff (no
  behavioural test is possible for them: the leak's map pin count is
  not observable from the test API, the overlap trigger is
  mathematically unreachable via the flag toggle, and the extent
  case needs an attribute in an extent record). Evidence: attr_flags
  44 checks 0 failures with the fix, 5 failures without it; plain
  ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265 passed
  0 failed with NTFS_REQUIRE_FSCK=1.
- 2026-11-25: `eb67671cd814` (compressed write punches old clusters
  before the new data lands) — ported: `ntfs_write_cb()` now allocates
  the new run, writes the new block, and only then punches the old
  block and merges the runlist, with a shared `free_rlc` exit that
  frees the new clusters on any failure after allocation. Upstream's
  bio_alloc NULL check and `cb_clusters` alias do not apply (the port
  writes through a single `ntfs_bdev_write`, no bio; the punch still
  takes `ni->itype.compressed.block_clusters`). The port's punch
  returns 0/-errno, so upstream's bare `if (err)` after the moved
  punch ports verbatim. Regression test
  `test_compressed_block_survives_failed_rewrite` in
  `core/tests/test_compress.c`: a 16 MB volume is filled until a
  one-cluster allocation fails, then a compressed block is overwritten
  with incompressible data (needs a whole new 16-cluster block).
  Without the fix the overwrite still fails ENOSPC but the old block
  is already punched away and reads back wrong after a remount (1
  failure); with the fix the old block reads back byte for byte.
  Evidence: failed_rewrite 25 checks 0 failures with the fix, 1
  failure without it; plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
- 2026-11-25: `9692b1b4fc00` (reparse-index insertion failure
  swallowed, symlink creation falsely succeeds) — ported:
  `update_reparse_data()` now keeps `set_reparse_index()`'s return
  in `err` and propagates it, so the failure is reported even when
  there was no previous reparse data (`oldsize == 0` — the case that
  bit symlink creation; the old code only entered the cleanup branch
  when `oldsize > 0`, discarding the error entirely otherwise).
  Regression test `test_reparse_index_failure_propagates` in
  `core/tests/test_links.c` with new fixture patcher
  `tools/mkfixtures/patch_stale_reparse.py`: it plants a stale `$R`
  entry (native tag, the file's (tag, file_id) key) for the record the
  next created file will take — the number is learned at runtime from
  a sacrificial file via `ntfs_inode_number()`, since allocation is
  sequential from the fixture's highest used record (294). The
  planted entry makes `ntfs_ie_add()` refuse with -EEXIST, the exact
  failure upstream's fix propagates. Evidence: test_links 657 checks
  0 failures with the fix, 2 failures without it (the create returns
  0 and leaves a half-created link with reparse data but no index
  entry); plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all`
  265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
- 2026-11-26: `cbe287fd65f1` (quota removal) — ported: deleted
  `core/ntfs/quota.c`/`quota.h`, the `ntfs_mark_quotas_out_of_date()`
  call in `ntfs_reconfigure()`, `load_and_init_quota()` and its call
  site, `vol->quota_ino`/`quota_q_ino`, the `NV_QuotaOutOfDate` flag
  and bit ops, and the quota commit/iput sites in `ntfs_put_super()`
  and `ntfs_fill_super()`; kept the on-disk quota layout definitions
  in `layout.h`, as upstream did. The super_glue notes were updated:
  the rw path never called the marker (it always failed -- the $Q
  index was looked up under the wrong name "$I30"), so the glue and
  the kernel path now agree that there is no quota step. Regression
  test `test_quota_removal_remount_rw` in `core/tests/test_mount.c`
  drives the fs_context ops directly (read-only mount, reconfigure
  to read-write, back to read-only for a clean teardown; the VFS flag
  step is applied by the test, since `reconfigure()` itself never
  touches `sb->s_flags` -- the kernel applies `fc->sb_flags` after
  it returns 0). Evidence: test_mount 133 checks 0 failures with the
  fix, 2 failures without it (rw remount refused -EROFS, volume
  still read-only); plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1;
  ntfscli cross-check (rebuilt tree): rw mount, file created, reads
  back.
- 2026-11-26: `19cac7902a8a` + `fea9e4488f38` (mapping-pairs bounds)
  — ported together: `ntfs_cluster_alloc()` in `core/ntfs/lcnalloc.c`
  now clamps the $Bitmap scan to `min(i_size, (nr_clusters + 7) >> 3)`,
  and `ntfs_mapping_pairs_decompress()` in `core/ntfs/runlist.c`
  refuses a run whose starting LCN is at or beyond `nr_clusters`, and
  (with `check_add_overflow`) one whose LCN + length runs past
  `nr_clusters`. Both hunks verbatim from upstream. New fixture
  patcher `tools/mkfixtures/patch_oob_run.py` rewrites a file's first
  $DATA run out of the volume (3-byte length/offset fields) and
  appends 0x42 marker bytes past the volume end, so an escaping read
  returns known bytes instead of EOF. Regression test
  `test_mapping_pairs_bounds` in `core/tests/test_compress.c` (start
  mode: LCN nr_clusters+10, len 1; run-end mode: LCN nr_clusters-5,
  len 10) on a scratch `empty.img` (8191 clusters @ 4K). Evidence:
  test_compress 502 checks 0 failures with the fix, 2 failures
  without it (the two reads return the marker bytes instead of -EIO;
  verified independently with ntfscli before the test was written);
  plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1; ntfscli cross-check
  (rebuilt tree): both OOB reads fail EIO, clean-image read still
  works. The lcnalloc clamp hunk has no userspace-observable test
  (the overrun reads the heap `lcn_empty_bits_per_page` array,
  silent without ASan) and is carried verbatim, proven by the
  byte-identical diff.
- 2026-11-26: `766062a82e1c` (empty mapping pairs) — ported: both
  hunks verbatim in `ntfs_mapping_pairs_decompress()`
  (`core/ntfs/runlist.c`): an empty array is refused with -EIO
  unless it describes a zero-length attribute, and a genuinely
  empty attribute reuses the old runlist when one was passed in.
  New fixture patcher
  `tools/mkfixtures/patch_empty_mapping_pairs.py` zeroes the first
  mapping-pairs byte of a file's non-resident $DATA (sizes and
  highest_vcn left as the driver wrote them). Regression test
  `test_empty_mapping_pairs` in `core/tests/test_compress.c`:
  one-cluster file, remount, metadata still readable, read refused
  -EIO. Evidence: test_compress 526 checks 0 failures with the fix,
  1 failure without it (read returns 4096, expected -EIO; verified
  independently with ntfscli: the read succeeds and returns the
  file's bytes as zeroes); plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
  A truncate-to-zero round trip still works with the fix (the driver
  converts the attribute to resident rather than leaving an empty
  non-resident array).
  49 port items remain.
- 2026-11-26: `323751a604e7` + `cc9d09fef784` (boot-sector validation)
  — ported as one commit: `323751a604e7` replaces the old
  sectors_per_cluster range test in `is_boot_sector_ntfs()` with
  upstream's `is_power_of_2` check (`core/ntfs/super.c`, no new
  include needed — visible via ntfs.h -> kernel.h -> log2.h);
  `cc9d09fef784` decodes the MFT/MFTMirr LCNs into `u64 ll` instead
  of `s64 ll` in `parse_ntfs_boot_sector()`, so a high-bit LCN no
  longer wraps negative past the bounds check (`%lli` -> `%llu`).
  Tests in `core/tests/test_mount.c`: two new cases in
  `test_boot_sector_geometry_refusals` (mft_lcn / mftmirr_lcn top
  byte 0x80 — both mount without the fix, refused with it, verified
  with ntfscli first) and a new counting-bdev test
  `test_boot_sector_spc_refused_early` (spc=3 and spc=5 refused
  with zero reads past sector 0; a plain refusal test is vacuous
  here because both sides fail the mount with -EINVAL — without
  the fix the mount reads $MFT record 0 at offset 2048 first).
  Evidence: test_mount 141 checks 0 failures with the fix,
  4 failures without it (2 LCN cases mount, 2 spc cases read past
  sector 0: past0=1 maxoff=2048); plain ctest 15/15, UBSan ctest
  15/15, `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1. Note: `git checkout -- <file>` must never be
  used in this repo — HEAD on disk is stale upstream f61228f, so a
  checkout restores the upstream file (quota.h include, unbuildable)
  and destroys the port; the negative control was done by direct
  edit instead, and the file was verified byte-identical to its
  pre-control state afterwards.
  47 port items remain.
- 2026-11-26: `81684340963d` (validate usa_ofs before preserving the
  update sequence number) — ported: upstream hunk verbatim in
  `ntfs_mft_record_alloc()` (`core/ntfs/mft.c`): the old USN is only
  preserved when usa_ofs is even and usa_ofs+2 <= mft_record_size,
  otherwise usn stays 0. New test
  `test_mft_alloc_usa_ofs_validated` in `core/tests/test_links.c`:
  create A/C/D (records R/R+1/R+2, R discovered via getattr — 295 on
  basic-4k.img), delete C, unmount, patch C's usa_ofs to 1024,
  remount, create B reusing C's record; the reused record's USN must
  be a fresh small value. C's record is page-aligned (R+1 is a
  multiple of 4), so the unguarded read lands deterministically on
  the first two bytes of D's record, "FI" = 0x4946. Evidence:
  test_links 471 checks 0 failures with the fix; without it exactly
  1 failure, USN reads back 0x4948 (0x4946 + 2 fixup increments).
  A non-page-aligned target instead reads past the mapped folio
  (heap garbage 0x5382 observed on the first attempt) — the same bug,
  but nondeterministic, which is why the test asserts the target is
  page-aligned. Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
  46 port items remain.
- 2026-11-26: `d97a36bae86a` (sanitize MFT references from
  ntfs_lookup_inode_by_name) — ported: static inline
  `ntfs_check_mref()` added after I30 (`core/ntfs/dir.c`) and all four
  lookup return paths wrapped, so an error-marked on-disk reference
  normalizes to ERR_MREF(-EIO) instead of reaching callers, which
  decode failures with MREF_ERR(). New test
  `test_lookup_err_mref_normalized` in `core/tests/test_links.c` plus
  fixture patcher `tools/mkfixtures/patch_err_mref.py`: the $I30 entry
  for victim.txt is set to error-bit | 0x42. The fixture's root index
  is large, so the entry lives in an $INDEX_ALLOCATION INDX block,
  not the $INDEX_ROOT -- the patcher searches the root first, then
  every allocation block. Evidence: test_links 475 checks 0 failures
  with the fix; without it exactly 2 failures, ntfs_lookup returns 0
  with a bogus handle (MREF_ERR 66, which IS_ERR() misses). No
  fsck_clean for this test: the image is deliberately corrupt.
  Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265 passed
  0 failed with NTFS_REQUIRE_FSCK=1.
  45 port items remain.
- 2026-11-26: `a83e82b0ec3a` + `ffc755828de1` (non-resident attribute
  validation) — ported: the port has no
  `ntfs_non_resident_attr_value_is_valid()`; the inline validator in
  `ntfs_attr_find()` (`core/ntfs/attrib.c`) checked only
  mapping_pairs_offset bounds. It now also raises min_len from 64 to
  72 for sparse/compressed attrs (ffc755828de1), requires the name to
  lie within the attribute without overlapping the mapping pairs, and
  requires 8 bytes of room after mapping_pairs_offset for the
  compressed_size field a sparse conversion would add (a83e82b0ec3a).
  New test `test_nonres_attr_validation` in `core/tests/test_links.c`
  plus fixture patcher `tools/mkfixtures/patch_nonres_attr.py` with
  three cases, each on its own fixture copy: room64 (unnamed $DATA
  length := 64, room 0 < 8), sparse64 (same + ATTR_IS_SPARSE, isolates
  the min_len-72 check since the room check is skipped for sparse),
  mpinside (ADS mapping_pairs_offset inside the name, so
  name_end > mp_offset). The trigger must stop at the attribute find
  (lookup for the unnamed $DATA, getxattr size query for the ADS): a
  read is NOT discriminating, because the mapping-pairs decompressor
  bounds the pairs by the attribute length (so a length-64 $DATA fails
  there in both builds) and ntfs_attr_iget already refuses
  name_offset >= mp_offset (so mp_offset := 64 fails in both builds).
  Evidence: test_links 482 checks 0 failures with the fix; without it
  exactly 3 failures, one per case (operations return 0 instead of
  -EIO). No fsck_clean for this test: the images are deliberately
  corrupt. Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all`
  265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
  44 port items remain.
- 2026-11-26: `8bed376124ab` (harden runlist realloc size
  calculations) — ported: `ntfs_rl_realloc()` and
  `ntfs_rl_realloc_nofail()` in `core/ntfs/runlist.c` multiplied the
  element counts by sizeof(*rl) in int and stored the product back
  into an int, so a negative count wrapped to a huge size_t and the
  memcpy that follows used the wrapped size. Both now go through a
  shared static inline `ntfs_rl_realloc_gfp()` helper that rejects
  negative counts with -EINVAL before allocating anything and checks
  both multiplies with check_mul_overflow (already in the port's
  overflow.h, already used by the mapping-pairs decoder). New test
  `test_rl_realloc_overflow` in `core/tests/test_compress.c` calls
  `ntfs_rl_realloc()` directly (no fixture): positive control (4 -> 8
  elements preserves contents, same-size returns the original
  pointer) plus the discriminating negative counts.
  Note: the overflow check itself cannot be tripped through this
  interface on a 64-bit size_t -- an int count x 24 bytes always fits,
  verified with a scratch program (__builtin_mul_overflow(INT_MAX, 24,
  &size_t) reports no overflow) -- so no INT_MAX case: in the new code
  it would attempt kvzalloc(51 GiB), and in the old code it returns
  -ENOMEM either way. The dangerous case is a negative old_size with
  a live runlist (old code memcpys the wrapped size); it is covered
  by the same check (same helper, same first lines), not by crashing
  the test binary. Evidence: test_compress 491 checks 0 failures with
  the fix (rl_realloc_overflow 13 checks); without it exactly 2
  failures -- negative new_size returns -ENOMEM (-12) instead of
  -EINVAL (-22), and negative old_size succeeds (kvzalloc(0)).
  Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265 passed
  0 failed with NTFS_REQUIRE_FSCK=1.
  43 port items remain.
- 2026-11-26: `c8504fc1245f` (bound $AttrDef table walk to the loaded
  table size) — ported: `ntfs_attr_find_in_attrdef()` in
  `core/ntfs/attrib.c` bounded only the start of each 160-byte entry,
  so a table whose size is not a multiple of 160 let the loop read
  ad->type (offset 128) and the body fields of a trailing partial
  entry past the kvzalloc'd buffer; the loop guard now requires the
  whole entry to lie within attrdef_size (<= attrdef_size -
  sizeof(*ad)). `load_and_init_attrdef()` in `core/ntfs/super.c` also
  accepted any i_size > 0; it now rejects i_size < sizeof(struct
  attr_def) at mount. New test `test_attrdef_too_small_refused` in
  `core/tests/test_mount.c` plus fixture patcher
  `tools/mkfixtures/patch_attrdef_size.py` (shrinks record 4's $DATA
  data_size/initialized_size to 120 bytes; control mounts fine).
  Evidence: test_mount 143 checks 0 failures with the fix; without
  it exactly 1 failure, the 120-byte $AttrDef mounts. No fsck_clean
  for this test: the image is deliberately corrupt. Plain ctest 15/15,
  UBSan ctest 15/15, `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1.
  42 port items remain.
- 2026-11-26: `b8d6c528e9d5` (WARN_ON for resident attribute in
  ntfs_map_runlist_nolock) — SKIPPED, fix NOT landed. The fix is 3
  lines (WARN_ON -> -EIO return, verbatim upstream; the sibling
  branch already does the same) but no failing-without-fix test is
  reachable through the public harness, and a fix without that
  evidence does not land. The mount path (upstream's KASAN trigger
  via ntfs_empty_logfile) is unreachable: a resident $LogFile $DATA
  fails ntfs_check_logfile's minimum-size gate (40 KiB) before
  emptying, and the restart-page validation needs a full 4 KiB page
  that a ~950-byte resident value cannot hold. A white-box call needs
  struct ntfs_inode *, but the internal headers redefine public
  symbols and cannot coexist with ntfscore.h in one TU. The read path
  branches on NInoNonResident before any runlist mapping, so resident
  compressed files do not reach it either. Landing needs a test
  harness that can call internal functions (or an ASan fuzz seed
  reaching the path). Triage row updated with the full reason.
  42 port items remain (this item still counts as remaining).
- 2026-11-26: `650a2751c9ae` (validate final EA attribute size) —
  ported: `ntfs_set_ea()` in `core/ntfs/ea.c` checked only the NEW
  entry's size against the $AttrDef $EA limit (64 KiB), and only at
  the end, after a replacement had already removed the old entry and
  committed the shortened $EA stream and $EA_INFORMATION. The fix
  checks the FINAL size before mutating: `ea_info_qsize - ea_size +
  new_ea_size` on replace, `ea_info_qsize + new_ea_size` on add (both
  -> -EFBIG via `ntfs_attr_size_bounds_check` against the $EA max
  65536, verified from the fixture's $AttrDef with ntfscat); the
  trailing check keeps only `ea_packed > 0xffff`. New test
  `test_ea_final_size_validated` in `core/tests/test_links.c` drives
  $EA through the VFS xattr handler (`ntfs_xattr_handlers[0]->set/
  get`) because the public `ntfs_setxattr()` stores xattrs as ADS
  and never reaches `ntfs_set_ea()`; the handler is the
  upstream-representative path (and what WSL-inode creation and ACL
  sets use). Two cases, each on its own fixture copy (the old
  build's add case sets the volume error flag): add (7 x 8192-value
  EAs, then an 8040-value EA: final stream 65572 > 65536, final
  packed 65525 <= 65535) and replace (15 x 4096-value EAs, replace
  one with 8192: final stream 65896 > 65536, final packed 65776 >
  65535). The totals account for the three hidden WSL EAs
  ($LXUID/$LXGID/$LXMOD) `ntfs_create()` writes (+45 packed / +60
  stream); the packed-vs-stream window matters because the trailing
  packed check fires in both builds otherwise. Evidence: test_links
  486 checks 0 failures with the fix; without it exactly 2 failures
  -- add returns -EIO (-5) instead of -EFBIG (-27) (the $EA resize
  to 65572 trips the bounds check in the write path and sets the
  volume error flag), and replace returns -EFBIG in both builds but
  the old value is gone in the old build (get=-ENODATA) while intact
  in the new one. Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
  41 port items remain.
- 2026-11-26: `5b6eedd7cc29` (avoid calling post_write_mst_fixup() for
  invalid index_block) — ported: `ntfs_icx_ib_sync_write()` called
  `post_write_mst_fixup()` whenever `ntfs_ib_write()` returned an
  error, but `ntfs_ib_write()` also errors when `pre_write_mst_fixup()`
  validation fails on an invalid block, and `post_write` assumes the
  block is valid and does no boundary checks (with usa_count = 0 the
  count underflows to 0xffff and it walks ~33 MB past the 4 KiB
  buffer). The fix moves the `post_write` call into `ntfs_ib_write()`,
  after the pwrite, so it only runs when `pre_write` succeeded. New
  test `test_ib_sync_write_no_post_fixup` in `core/tests/test_links.c`
  plus fixture patcher `tools/mkfixtures/patch_ib_usa.py` (sets
  usa_count = 0 on the root's single INDX block). The trigger runs in
  a forked child (setattr the existing "ads" directory's crtime to
  its current value to set FileNameDirty, then fsync it, reaching
  `ntfs_inode_sync_filename` -> `ntfs_icx_ib_sync_write`); the parent
  CHECKs the wait status so the suite survives the negative control.
  Two dead ends are recorded in the test comment: creating a file
  first does not work (the create's own index write fails at pre_write
  and is swallowed, so the entry never lands on disk and the
  fsync-time lookup returns -ENOENT), and "links" cannot be the target
  (its mref spans sector end 1534, so the unrestored USN 0xa reads back
  instead of its true sequence 1 and the lookup fails stale-reference
  -EIO; "ads" at 1240 does not span one). Evidence: test_links 487
  checks 0 failures with the fix; without it exactly 1 failure, the
  child killed by signal 11 (SIGSEGV). Plain ctest 15/15, UBSan ctest
  15/15, `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1.
  40 port items remain.

- 2026-11-26: `56cc42a7b387` (return errors from ntfs_attr_readall) —
  ported: readall returned NULL on every failure, so callers lost
  the errno — ntfs_get_ea() reported -ENODATA for a failed $EA read,
  ntfs_listxattr() an empty list, and the reparse parser treated an
  allocation/I/O error as "no reparse data". All four failure paths
  now return ERR_PTR() (iget, invalid size, kvmalloc, pread) and all
  callers propagate: ntfs_get_ea / ntfs_set_ea / ntfs_listxattr in
  ea.c, ntfs_ibm_get_free in index.c (its -1 becomes the real errno;
  both callers already test < 0), the two reparse.c sites (IS_ERR
  normalised to NULL before the kvfree), plus the port-only readlink
  fallback in core/vfs/api.c (same normalisation, required because
  its out: path kvfrees the pointer). New test
  `test_readall_error_propagates` in `core/tests/test_links.c` plus
  fixture patcher `tools/mkfixtures/patch_oob_ea.py` (rewrites a
  non-resident $EA run out of the volume). Evidence: test_links 697
  checks 0 failures with the fix; without it exactly 1 failure, the
  get returning -ENODATA (-61) instead of -EIO. Plain ctest 15/15,
  UBSan ctest 15/15, `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1.
  39 port items remain.

- 2026-11-26: `8efe00b098b5` + `ba1b61ddaa76` (error codes collapsed
  to -EIO / literal -1) — ported: `ntfs_resident_attr_record_add()`
  returned -EIO for every failure at its put_err_out label, which
  defeated the -ENOSPC resident-to-non-resident fallback in
  `ntfs_attr_add()` — a big EA set on a file with no $EA could never
  convert to non-resident and always failed -EIO; and
  `ntfs_non_resident_attr_record_add()` returned the literal -1,
  propagated as -EPERM to userspace. Both now return err (one line
  each, verbatim upstream). New test `test_attr_add_enospc_fallback`
  in `core/tests/test_links.c`: sets an 8 KiB EA on /files/sz/0 in
  basic-4k.img, a fixture file with no $EA, so `ntfs_set_ea()` takes
  the `ntfs_attr_add()` path and the ~8.2 KiB resident record fits in
  no record — the fallback must convert it. Evidence: test_links 701
  checks 0 failures with the fix; without it exactly 2 failures, the
  set returning -EIO (-5). The non-resident hunk has no behavioural
  test (its put_err_out is unreachable through the public API: the
  non-resident record is ~80 bytes and `ntfs_attr_add()` only calls
  it on a record it already checked has room); proven by the verbatim
  diff. Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1.
  37 port items remain.

- 2026-11-26: **Found during porting** (not in the upstream drift —
  present in upstream v7.1, no fix in the v7.1→v7.3 drift):
  `ntfs_set_ea()` clears the HasEA flag when creating the first $EA
  on a file that has none. The local `ea_info_qsize` is never updated
  on the create path (only `p_ea_info->ea_query_length` is), so the
  out: label sees 0 and calls NInoClearHasEA even though the $EA was
  just created; a same-mount getxattr then returns -ENODATA until
  remount (the inode reload sets HasEA from the on-disk $EA). Found
  while writing `test_attr_add_enospc_fallback`: the set returns 0
  and the remount read-back is correct, but the same-mount get fails
  -ENODATA. **Fixed 2026-11-26** (one line, `ea_info_qsize +=
  new_ea_size;` in core/ntfs/ea.c): the increment sits AFTER the $EA
  write, because the append branch writes the new entry at offset
  `ea_info_qsize` — incrementing earlier lands the entry one entry
  too far and leaves a gap the lookup never crosses (caught by the
  new test's add-path get). Regression test
  `test_ea_first_set_same_mount_get` in `test_links` (488 checks, 0
  failures; 2 failures without the fix, both create-path same-mount
  gets returning -ENODATA); the deferred same-mount get in
  `test_attr_add_enospc_fallback` is un-deferred in the same commit.
  Related but different: `ac727d86fb84` (still in the queue) guards
  the flag update with `if (!err)` on failure paths; it does not fix
  this create-path staleness.

- 2026-11-26: `ada728801999` (-ERANGE for undersized xattr buffer)
  — ported: `ntfs_getxattr()` returned -ENODATA when the value buffer
  for system.dos_attrib / system.ntfs_attrib[_be] was smaller than
  the attribute value, telling userspace the attribute does not
  exist; the xattr API expects -ERANGE (and `ntfs_get_ea()` already
  returned -ERANGE for regular EAs). Two lines, verbatim upstream.
  New test `test_getxattr_erange` in `core/tests/test_links.c`,
  driven through the VFS xattr handler: 495 checks, 0 failures with
  the fix; 3 failures without it (all three undersized-buffer gets
  returning -ENODATA). Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
  36 port items remain.

- 2026-11-26: `1923eeffa63e` (propagate folio errors) — ported:
  `__ntfs_inode_non_resident_attr_pwrite()` replaced an
  `__filemap_get_folio()` failure with -ENOMEM instead of returning
  the folio's own error; the adjacent partial-page branch already
  propagates PTR_ERR correctly. One line, verbatim upstream
  (`ret = PTR_ERR(folio)` in the FGP_CREAT branch,
  core/vfs/inode.c). The failure itself is not reproducible through
  the public API (no allocation-failure hook in the platform; the
  only discriminating error is -EINVAL from a destroyed mapping,
  unreachable for a live attribute inode), so the propagation is
  proven by the verbatim diff per the round-8 precedent. New test
  `test_folio_fullpage_write` in `core/tests/test_links.c` proves
  the branch the fix sits in: a full-page (count == PAGE_SIZE)
  write to a non-resident attribute takes the FGP_CREAT path,
  driven through the EA append (first EA 8175-byte value ->
  8192-byte record makes $EA non-resident; second EA "user.pg"
  sized so its record is exactly one page, 4080-byte value);
  same-mount and remount gets return the exact bytes. 501 checks,
  0 failures. Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1.
  35 port items remain.

- 2026-11-26: **Found during porting** (not in the upstream drift —
  present in upstream v7.1, no fix in the v7.1→v7.3 drift):
  `__ntfs_inode_non_resident_attr_pwrite()` assumes count ==
  PAGE_SIZE implies a page-aligned pos and writes at folio offset 0
  unconditionally. A page-sized write at an unaligned offset (the EA
  append path: `ntfs_write_ea()` calls it with ea_off = stream size,
  essentially never page-aligned, and ea_size can be exactly
  PAGE_SIZE) lands the data at the folio start instead of at the
  offset, and the entry tail past the folio end is never written —
  the same-mount get then returns -ENODATA and the stream is
  corrupt (the write also overwrites the tail of the previous
  entry). Found while writing `test_folio_fullpage_write`: with the
  first EA 8192 bytes (stream 8212, append at pos 8212) the test
  fails 2 checks (same-mount and remount gets -ENODATA); padding
  the first EA to an 8192-byte record (append at pos 8192, aligned)
  makes it pass. Upstream v7.3 `fs/ntfs/ea.c` calls
  `ntfs_inode_attr_pwrite` identically, so upstream has the bug too.
  Other callers are safe (index/attrib/attrlist/reparse all write
  at pos 0 or with count 1). **Fixed 2026-11-26**: the FGP_CREAT
  full-page path is only taken when the write covers a whole folio
  (count == PAGE_SIZE and pos page-aligned); otherwise the write
  goes through `read_mapping_folio`, which brings in the existing
  page so bytes outside the write range are preserved, and the loop
  continues into the next folio where pos is aligned again
  (core/vfs/inode.c, `full_page` guard on both the folio-get and
  the offset computation). The same test now uses the unaligned
  case (first EA 8192 bytes): 501 checks, 0 failures with the fix;
  2 failures without it (same-mount and remount gets -ENODATA,
  stripped by line number and verified by diff).

- 2026-11-26: `3d3de2aee17d` (return DT_UNKNOWN on inode lookup
  failure in readdir) — ported: `ntfs_reparse_tag_dt_types()`
  returned PTR_ERR(vi) when `ntfs_iget()` failed, but its return
  type is unsigned int and the caller in dir.c passes the value
  straight to `dir_emit()` as d_type, so a stale or corrupt MFT
  reference made readdir report a garbage d_type to userspace.
  One line, verbatim upstream (`return DT_UNKNOWN;`,
  core/ntfs/reparse.c). New test
  `test_reparse_dt_unknown_on_lookup_failure` in
  `core/tests/test_links.c` calls the function directly: the
  readdir callback in the harness maps every unknown type to
  NTFS_ITEM_OTHER, so the garbage is not observable through
  `ntfs_readdir()`. A record number far past the fixture's $MFT
  (ntfs_iget fails) returns DT_UNKNOWN; a real symlink's record
  still returns DT_LNK. 504 checks, 0 failures with the fix;
  without it exactly 1 failure, the lookup-failure case returning
  4294967294 (0xFFFFFFFE, -ENOENT as unsigned). The positive
  control must run first: a failed `ntfs_iget` sets the volume
  error flag (`NVolSetErrors` in `ntfs_read_locked_inode()`'s
  err_out path), and later creates on the same mount fail.
  Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1.
  34 port items remain.

- 2026-11-26: `ea6a67ef6445` (reject unprivileged writes to reserved
  $LX* xattrs) — ported: `ntfs_setxattr()` now rejects the reserved
  $LXUID/$LXGID/$LXMOD/$LXDEV names with -EPERM (helper
  `ntfs_is_reserved_lxattr()`, verbatim upstream, core/ntfs/ea.c).
  The port has no credential plumbing in the VFS layer, so the
  privilege predicate (`ntfs_caller_is_privileged()`) returns
  false: every handler caller is unprivileged. Internal WSL
  creation bypasses the handler (`ntfs_ea_set_wsl_inode()` calls
  `ntfs_set_ea()` directly), so it is unaffected -- creation
  still writes $LXUID/$LXGID/$LXMOD on every new file, which the
  test accounts for: it records the stored values first and
  asserts a rejected set leaves them unchanged ($LXDEV has none
  on a regular file and stays absent). New test
  `test_reserved_lxattr_rejected` in `core/tests/test_links.c`:
  522 checks, 0 failures with the fix; without it 8 failures
  (the forged sets succeed and the values change). Plain ctest
  15/15, UBSan ctest 15/15, `run-tests.sh all` 265 passed 0
  failed with NTFS_REQUIRE_FSCK=1.
  33 port items remain.

- 2026-11-26: `0ebe8f625ab0` (fail attrlist updates when the
  superblock is inactive) — ported verbatim: SB_ACTIVE guard at
  the top of `ntfs_attrlist_update()` (core/ntfs/attrlist.c),
  returns -EIO once teardown has cleared SB_ACTIVE so the update
  never igets the $ATTRIBUTE_LIST fake inode while it is
  I_FREEING (self-deadlock in find_inode()). New test
  `test_attrlist_update_inactive_sb` (core/tests/test_links.c):
  grows an attribute list by hard-linking until the 1024-byte
  base MFT record overflows (verified on disk with ntfsinfo:
  $ATTRIBUTE_LIST present, $FILE_NAMEs in extension records),
  then active update returns 0, inactive returns -EIO,
  restored returns 0. Test needed two small wrappers
  (`ntfs_attrlist_update_vfs()` / `ntfs_attrlist_need_vfs()` in
  attrlist.c): the test TU cannot include inode.h (struct
  ntfs_attr collides with ntfscore.h), so the handle-to-
  ntfs_inode conversion lives there. Two fork observations from
  building the test: (1) ntfs_attrlist_update() segfaults when
  called on a file with no attribute list (pre-existing upstream
  behaviour -- its callers only call it when a list exists, and
  7 x 8192-byte EAs do NOT overflow the record); (2) the
  in-memory inode from ntfs_create() does not pick up a list the
  link path grows -- the test unmounts/remounts so the fresh
  lookup loads $ATTRIBUTE_LIST from disk. Evidence: test_links
  525 checks, 0 failures with the fix; without it exactly 1
  failure (inactive update returns 0, line-number strip,
  diff-verified). Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  32 port items remain.

- 2026-11-26: `41e1cf4ec10d` (rewrite EA stream before updating
  metadata) — ported: `ntfs_set_ea()` (core/ntfs/ea.c) now builds
  the complete $EA stream in memory and rewrites it from offset
  zero instead of committing a compacted stream and appending
  the new entry at ea_info_qsize; on removal the compacted $EA
  is written before $EA_INFORMATION, restoring the original pair
  if either write fails; when the last entry is removed the $EA /
  $EA_INFORMATION pair is torn down, restoring the original $EA
  if the $EA_INFORMATION removal fails. The port did not have
  the teardown block at all (the pair was left behind empty);
  restored here with the fix's restore logic. PORT: kvmemdup
  does not exist in the port, so the snapshot uses kvzalloc +
  memcpy. New tests in core/tests/test_links.c:
  `test_ea_stream_rewrite` (set/replace-with-8K/remove/add
  churn, every surviving entry verified after each step, then
  remount) and `test_ea_pair_teardown` (fixture file
  /nested/top.bin has no $LX* entries, so removing its only
  user EA must tear the pair down; pair presence via the new
  `ntfs_ea_pair_exists_vfs()` hook in ea.c). Evidence:
  test_links 571 checks, 0 failures with the fix; without it
  exactly 1 failure (the pair is left behind, value 3, after
  the last removal) -- negative control used the pre-fix ea.c
  from GitHub main plus only the test hook, diff-verified.
  The stream test passes on old code too (single-threaded end
  state is identical; the rewrite's benefit is the
  crash-consistency / fsstress layout class from generic/642),
  but it guards the rewrite's correctness (a misplaced entry
  would read back -ENODATA). Plain ctest 15/15, UBSan ctest
  15/15, `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  31 port items remain.

- 2026-11-26: `772e4c8f2555` (remove empty EA attribute pair) —
  already covered by the 41e1cf4ec10d port above: the teardown
  block it adds is a subset of what that port restored (the two
  $EA / $EA_INFORMATION removes, plus restore-on-failure), and
  test_ea_pair_teardown verifies it. No separate code change.

- 2026-11-26: `ac727d86fb84` (leave HasEA flag untouched on
  setxattr failure) — ported: the `out:` label in ntfs_set_ea()
  (core/ntfs/ea.c) now only updates HasEA on success. An error
  before ea_info_qsize was set (e.g. a failed $EA read) used to
  clear the flag and hide existing on-disk EAs until the inode
  was evicted. The fork-found HasEA comment in the same function
  is updated: ac727d86fb84 is now ported, so the failure hole it
  used to leave open is closed. New test
  test_hasea_untouched_on_set_failure (core/tests/test_links.c):
  OOB-$EA fixture (same patch as
  test_readall_error_propagates); a set fails with -EIO while
  ea_info_qsize is still 0, and the following get is the
  discriminator -- -ENODATA without the fix (flag cleared), -EIO
  with it (flag intact, read attempted). Evidence: test_links
  797 checks, 0 failures with the fix (under ctest); without it
  exactly 1 failure (get returns -61/-ENODATA). Plain ctest
  15/15, UBSan ctest 15/15, `run-tests.sh all` 265 passed 0
  failed with NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  30 port items remain.

- 2026-11-26: `60038154b40e` (do not update ctime when setxattr
  fails) — ported: the `out:` label in ntfs_setxattr()
  (core/ntfs/ea.c) now only updates ctime and marks the inode
  dirty on success. New test
  test_ctime_untouched_on_setxattr_failure (core/tests/test_links.c):
  XATTR_CREATE on an existing name fails with -EEXIST; ctime is
  read off the in-memory VFS inode before and after, with a 2s
  sleep between so a bump is observable (the port's
  current_time() is wall-clock). New ea_set_flags() helper in
  the test passes flags through to the handler. Evidence:
  test_links 799 checks, 0 failures with the fix (under ctest);
  without it exactly 1 failure (ctime moves by the 2s sleep).
  Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  29 port items remain.

- 2026-11-26: `4dc8f4ee2d46` (handle signal interruption in
  fallocate) — ported verbatim: both signal_pending() checks in
  ntfs_attr_fallocate() (core/ntfs/attrib.c) now jump to a new
  signal_out: label that sets err = -EINTR only when no other
  error is pending, preserving a completed allocation (err == 0
  after the loops) and other errors. NOTE: in this port
  signal_pending() is #defined to 0
  (platform/include/linux/kernel.h:110 -- the port has no
  task/signal model), so neither check can fire and no
  discriminating test is possible; the change is provably
  behavior-preserving here and is applied for upstream fidelity.
  The affected loops are exercised by the existing fallocate
  coverage (test_compress.c: 1MB keep_size fallocate, 531
  checks, 0 failures). Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  28 port items remain.

- 2026-11-26: `8c5dc7587fdd` (ignore interrupted inode reads as
  corruption) — ported: all five sites now skip the corruption
  log and/or the volume-error flag for -EINTR / -ERESTARTSYS --
  map_mft_record() (core/ntfs/mft.c), the err_out paths of
  ntfs_read_locked_inode(), ntfs_read_locked_attr_inode() and
  ntfs_read_locked_index_inode(), and the $BITMAP iget failure
  in ntfs_read_locked_index_inode() (core/vfs/inode.c; the err
  assignment moves above the log there so the guard can test
  it). NOTE: neither error can reach these sites in the port --
  the bdev layer retries EINTR (platform/src/bdev_file.c), the
  port has no task/signal model (signal_pending() is #defined
  0), and nothing produces -ERESTARTSYS -- so no discriminating
  test is possible and the change is provably
  behavior-preserving here; applied verbatim for upstream
  fidelity. Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  27 port items remain.

- 2026-11-26: `607a9478833d` (treat any nonzero dio zero-range
  return as an error) — ported: the check in
  ntfs_attr_fallocate() (core/ntfs/attrib.c) is now `if (err)`
  instead of `if (err > 0)`. New test
  test_fallocate_zero_failure (core/tests/test_faults.c): seed a
  1MB file, punch a cluster-aligned hole in the middle, arm a
  write fault on the whole device, fallocate over the hole -- the
  only device write during the call is the zeroing write, so it
  fails with -EIO. Evidence: faults suite 150 checks, 0
  failures with the fix (fallocate returns -EIO, 1 write
  refused); without it exactly 1 failure (fallocate returns 0,
  the failed zero silently ignored) -- negative control
  reverted the check by exact string. Also in this commit: the
  faults harness used `dd bs=1m`, which GNU dd rejects, so the
  whole faults suite was skipping here (0 checks); `bs=1M`
  works on GNU and BSD dd and the suite now runs for real
  locally. Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  26 port items remain.

- 2026-11-26: `88496c4ac5a6` (avoid stale runlist element
  dereference in fallocate) — ported: ntfs_attr_fallocate()
  (core/ntfs/attrib.c) now snapshots rl->lcn/length/vcn into
  scalars while the read lock is held and uses only the
  scalars after unlocking, and only calls ntfs_dio_zero_range()
  when map_cluster() reports balloc (new clusters allocated;
  the `if (err)` inside is the 607a9478833d port). NOTE: the
  race is not reachable in the port -- the upstream trigger
  needs a concurrent mmap page_mkwrite, which the port has no
  counterpart for, and every runlist-realloc path in the port
  runs under the same inode's i_rwsem (writes and fallocate
  take it exclusively, reads shared), while fallocate holds it
  exclusively; ntfs_attr_fallocate's only caller is
  ntfs_allocate_range via ntfs_fallocate. No discriminating
  test is possible in the single-threaded UBSan harness (the
  UAF needs a concurrent writer plus ASan to observe), so the
  fix is applied verbatim for upstream fidelity; the affected
  loop is exercised by the existing fallocate coverage
  (test_faults.c test_fallocate_zero_failure, test_compress.c
  1MB fallocate). Plain ctest 15/15, UBSan ctest 15/15,
  `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  25 port items remain.

- 2026-11-26: `81fe702ff176` (avoid stale runlist element
  dereference in MFT writeback) — ported:
  ntfs_write_mft_block() (core/ntfs/mft.c) now computes the
  remaining run length (rl_len) while the runlist lock is held
  and uses that scalar for the folio_sz decision after unlock,
  instead of carrying the borrowed runlist_element across the
  lock boundary. NOTE: unlike the fallocate twin
  (88496c4ac5a6), this race is theoretically reachable in the
  port -- writeback is an async path (sync/flush) while $MFT
  allocation extension runs on request threads -- but no
  discriminating test is possible in the single-threaded UBSan
  harness (it needs a concurrent writer plus ASan to observe);
  applied verbatim for upstream fidelity. The function is
  exercised by every test that creates files and syncs.
  Plain ctest 15/15, UBSan ctest 15/15, `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 8/8.
  (Pre-push initially failed at the darwin stage: a cell
  rebuild had wiped /tmp and removed clang; reinstalled clang
  18.1.3 via apt, darwin stage green again.)
  24 port items remain.

### 5.4 Homebrew cask not in homebrew-cask proper — open, needs stars
The cask lives in the maintainer's tap; homebrew-cask proper wants
~75 stars (notability bar). Nothing to code — it clears when the repo
does.

### 5.5 ASan ctest stage — done (2026-11-26); macOS 26 hang gone on current Xcode
The ctest suite now runs under AddressSanitizer: `tools/ci.sh --asan`
(stage_asan, own build-asan dir with -DNTFS_ASAN=ON -DNTFS_LOGFILE_ASAN=ON),
pre-push stage 9, and a dedicated `asan` job in .github/workflows/ci.yml
on macos-26. History: on macOS 26 ASan deadlocked inside
__asan::AsanInitInternal() before main() (confirmed 2026-09-14, every
test; one runaway burned a core for 23 h), so the stage was Linux-only
until 2026-11-26, when the user asked for the macOS hang to be fixed
rather than skipped. Re-test on the current runner image / Xcode,
2026-11-26: run 45785443966's asan job executed all 14 tests under
ASan in 35.56 s with no stall (geometry slowest at 17.56 s) -- the
hang is gone. That job still failed, but only on the stage's own
execution assertion: macOS ctest prints "100% tests passed out of N"
while the grep expected the Linux form "100% tests passed, 0 tests
failed out of N". The assertion now accepts both forms and also
requires a Passed line (d3fbebe3); run 45787227610's asan job went
green. The NTFS_ASAN / NTFS_LOGFILE_ASAN option descriptions no
longer say "hangs on macOS 26". The stage stays separately runnable
(dedicated `asan` job + `ci.sh --asan`), not in the default ci.sh
sequence: joining it would roughly double the checks job's macOS
time for a suite the asan job already covers. The stage asserts the
suite executed (ctest must report 100% passed over >= 1 test, with
at least one Passed line) so a run that executes nothing fails
loudly -- the 2026-09-14 failure mode, when the logfile suite's
243 checks silently did not execute for a day.

First run (2026-11-26, 12/15 pass) found three leaks, no memory errors,
each fixed in its own commit with a regression test and a negative control:
- Extent-inode $MFT folio (driver): ntfs_destroy_ext_inode() never put
  its pinned folio -- 122,728 B in 58 allocations via test_many_links
  (29 extension records). Upstream v7.1 omits the put identically; no fix
  in the v7.1->v7.3 drift.
- Pagecache test double-gets: test_invalidate() and test_reclaim() each
  called filemap_get_folio() once inside CHECK() and again inside
  folio_put() -- 8,464 B in 4 allocations.
- Logfile test buffers: lb_init()/vol_init() 256 KiB buffers never freed
  in four test functions -- 2,097,152 B in 8 allocations.
Full ASan ctest suite now 15/15.

## 6. What each CI check covers, and the fast local equivalent

CI (`tools/ci.sh`, run on macos-26) has six stages: toolchain
(ntfsprogs + ntfsprogs-plus), ctest, ctest under UBSan, fixtures
(`run-tests.sh all`, fsck required), Swift unit tests, app + extension
build. The mount-test job is manual, `continue-on-error`, and needs
signing (see 4.5). A seventh stage, ctest under ASan, runs on macos-26
too (dedicated `asan` job, §5.5) -- the re-test of the 2026-09-14 macOS
hang, not yet in the default sequence.

Local fast checks (`tools/pre-push.sh`, installed as the pre-push hook
by `tools/install-hooks.sh`) run everything CI runs that can run here,
plus the check CI cannot do for itself:

| CI stage | Local fast equivalent | Notes |
|---|---|---|
| toolchain, ctest, UBSan | `tools/ci.sh --quick` | same script, skips fixtures |
| ctest under ASan (macos-26 CI job) | `tools/ci.sh --asan`, pre-push stage 9 | 2026-09-14 macOS hang gone on current Xcode (see §5.5); the stage asserts the suite executed, so a run that executes nothing fails loudly |
| fixtures | run `tools/run-tests.sh all` explicitly before pushing core changes | slow; CI is the backstop |
| Swift tests, app build | none — need xcodebuild / macOS SDK | CI owns these |
| mount test | none — needs signing + real mount | manual, maintainer only |
| fuzz (weekly workflow, not `ci.sh`) | `tools/fuzz.sh check` (seeds + regression images) in `tools/pre-push.sh`; `tools/fuzz.sh regression` for the ASan+LSan leak checks | `.github/workflows/fuzz.yml`: weekly + dispatch, ubuntu-latest, own concurrency group, fails on any crash/leak; the mutation run itself is CI-only (300 s) |
| upstream drift (weekly workflow, not `ci.sh`) | `tools/upstream-drift.sh port-diff` in `tools/pre-push.sh` (informational); `tools/test-upstream-drift.sh` for the script itself | `.github/workflows/upstream-drift.yml`: weekly + dispatch, ubuntu-latest, own concurrency group, fails on drift; the network half (`upstream` vs latest tag) is CI-only |
| (no CI stage) | `tools/check-darwin.sh` | preprocesses the C tree for x86_64-apple-darwin; catches macOS-only breakage the Linux build cannot see (it caught the `__GLIBC_PREREQ` failure of run 45661009566 — verified by negative test) |
| (no CI stage) | shellcheck on `tools/*.sh`, `py_compile` on `tools/*.py`, YAML parse of `.github/workflows/*.yml` | no linters existed before 2026-11-25; these are the first ones |

There were no linters in the repo before this doc (no clang-format,
swiftlint, shellcheck, or pre-commit config). The pre-push script is
the lint layer for now. Add clang-format / swiftlint configs only when
someone will enforce them in CI; an unenforced config is clutter.

## 7. History (so the next person doesn't re-diagnose it)

- 2026-11-25, checks run 45661009566: failed on
  `platform/include/linux/string.h` — `__GLIBC_PREREQ` in a single-line
  `#if` is expanded by clang even in the short-circuited branch and is
  undefined on macOS. Fixed with a nested `#if` (commit 90eaae68).
- 2026-11-25, checks run 45671576917: failed on the toolchain stage —
  the first API push hardcoded mode 100644 for every file and stripped
  the `+x` bit from `tools/build-ntfsprogs.sh` ("Permission denied").
  Fixed mode-only (commit 27627562); `push_to_fork.py` now takes modes
  from the git index. A full tree audit (353 files) showed that was the
  only mismatch.
