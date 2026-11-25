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

### 1.2 No journal writing, only replay — open, large
The driver replays Windows' `$LogFile` on mount but never writes its
own records. Metadata is write-back, flushed within 1 s and on
sync/unmount, so a yank mid-write has no journal to recover from.
Full `$LogFile` journaling (writing records, not just replay) is the
item that makes yanks safe (`docs/PORTING.md` §6). Large; scope it
before starting.

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

### 1.6 8 KB xattr not checked — open
Xattrs round-trip as alternate data streams on Windows, but the 8 KB
`user.big` xattr (exceeds the inline limit, takes a different storage
path) was never checked (`docs/TODO.md`). Verify locally against
ntfsprogs, then on Windows.

## 2. Features Paragon / Tuxera have that this doesn't

### 2.1 Cannot read ntfs-3g Interix symlinks — open
Stock ntfs-3g is the most deployed third-party NTFS driver; its
symlinks (`IntxLNK` `$DATA` files) show to us as 1-cluster system
files with binary contents. Fix: parse the IntxLNK data and resolve
the target. Verify: create links with ntfs-3g locally (it is already
built in `tools/.local`), read them through `ntfscli` and the fixture
suite, round-trip through Windows.

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

### 2.5 `$EA` migration for pre-e045c2e symlinks — open, low priority
Symlinks created before commit e045c2e still carry `$EA`, and chkdsk
still rewrites their index entries. They still work on the Mac. Fix: a
migration pass that strips `$EA` from existing links. Verify with
`chkdsk` before/after on a fixture carrying such a link.

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

### 4.2 Arm64 only, no x86_64 app build — open
The C core now builds and passes all tests on x86_64 Linux (fork
commit 1d7f15ef), but the Xcode app never compiled for a second arch,
so Intel Macs can't run it. Fix: add the arch to the Xcode project;
CI's app-build stage (macos-26) verifies it compiles. No Intel
hardware needed to verify the build.

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

### 5.2 No fuzzing of the on-disk parser — open
NTFS-3G's 2026 CVEs were crafted-image heap overflows in its parser;
the same class of risk applies to any NTFS parser, including this
one. Fix: add a fuzz harness (libFuzzer target or a `ntfscli`-based
corpus runner), seed it with the fixture images, run it in CI on a
schedule, and fix what it finds. Verify: harness runs, corpus grows,
first findings triaged.

### 5.3 Upstream tracking has no automation — open
Tier 0/1 files must stay diffable against upstream Linux `fs/ntfs`
so kernel fixes can be merged. Today that is a manual discipline.
Fix: a script that diffs the vendored files against a pinned upstream
tag and reports drift, run in CI on a schedule.

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
