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
  52 port items remain.

### 5.4 Homebrew cask not in homebrew-cask proper — open, needs stars
The cask lives in the maintainer's tap; homebrew-cask proper wants
~75 stars (notability bar). Nothing to code — it clears when the repo
does.

## 6. What each CI check covers, and the fast local equivalent

CI (`tools/ci.sh`, run on macos-26) has six stages: toolchain
(ntfsprogs + ntfsprogs-plus), ctest, ctest under UBSan, fixtures
(`run-tests.sh all`, fsck required), Swift unit tests, app + extension
build. The mount-test job is manual, `continue-on-error`, and needs
signing (see 4.5).

Local fast checks (`tools/pre-push.sh`, installed as the pre-push hook
by `tools/install-hooks.sh`) run everything CI runs that can run here,
plus the check CI cannot do for itself:

| CI stage | Local fast equivalent | Notes |
|---|---|---|
| toolchain, ctest, UBSan | `tools/ci.sh --quick` | same script, skips fixtures |
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
