# `$LogFile` journal writing — design (GAPS.md §1.2)

Status: **scoped, not started**. This document is the scope: what the
writer must do, why it is the largest item in the gap list, and the
phased plan. No code lands from this round; the next round starts at
Phase 0/1 below.

## 0. Hard constraint — crash or disconnect at any instant leaves a good state

Set 2026-11-26 by the owner. This is a hard design constraint, not a
test to run at the end, and it has implications for the design — for
example the cache that is chosen. If the computer crashes or any
component fails, including the drive suddenly being disconnected,
the volume must always be left in a good state.

Stated plainly: **the current design does not meet this constraint.**
Today metadata is write-back, flushed within 1 s (background writer
tick 200 ms, folios dirty longer than 1000 ms flushed; §1 step 2
below), and journal writing is deferred. A crash or disconnect inside
that window loses up to a second of metadata writes with an empty
journal and nothing to replay — the volume is left dirty and recovery
is `chkdsk`, not the journal. That is not a good state. The journal
implementation in this document is what replaces that policy; until
it lands, the gap is open and recorded as such, not papered over.

What the constraint demands of every durability decision in this
document and in the code:

- **Cache policy must be justified against it**: what is cached,
  write-through vs write-back, write ordering, and exactly what can
  be lost on a sudden disconnect. A write-back cache is only
  acceptable where the journal makes the loss recoverable — i.e. the
  log record is on the device (synchronous log flush) before the
  cached metadata it describes is eligible for writeback (WAL,
  §3.5/§4). Anything cached without that ordering is data the driver
  has promised and cannot keep.
- **Flush ordering is part of correctness, not performance**: log
  before metadata, MFT-last, sync on commit — the orderings in §3.5
  exist because a crash between two flushes must still leave a
  replayable state. If an ordering cannot be enforced on the actual
  flush path (page cache, FSKit, device), the design does not ship
  as-is.
- **Replay behaviour must leave a good state too**: a torn or
  half-written log record is detected (checksums, LSN chain) and
  either replayed or cleanly rejected — never silently applied, never
  silently skipped (see §3.6/§3.7). A dirty volume mounts read-only
  until replay proves the state, or is refused; it is never mounted
  read-write on an unproven state.
- **Speed second**: if a design cannot prove crash/disconnect safety,
  it does not ship, however fast it is. Performance work on the write
  path is gated by §1.3 and by this constraint, not the other way
  round.
- **Commit-message rule**: any commit that touches caching, flushing,
  journaling or replay must state in its message what happens on a
  crash/disconnect at the worst instant.

## 1. Where we are

The driver replays `$LogFile` but never writes it. That is not an
oversight in one file — there is no writer anywhere in the ecosystem
this driver is built from:

- `core/ntfs/logfile.c` (vendored Linux 7.1): no journal writing.
  `ntfs_empty_logfile()` only wipes the log.
- `libntfs-3g` (`tools/build/src/ntfsprogs-plus-1.0.0/libntfs/logfile.c`):
  read-only clean check; no record writing.
- Paragon `ntfs3` `fslog.c`: replay only — and its replay is reported
  inoperative by the NTFSPLUS author (`docs/LOGFILE.md` §5).
- `core/logfile/` (this repo's own replay module): reads geometry, LSNs,
  restart pages, checkpoints, tables and all 37 redo/undo opcodes
  (`docs/LOGFILE.md` §2.9); writes exactly two things — replayed MFT
  records/clusters (`logfile_replay.c`) and clean restart pages
  (`ntfs_logfile_mark_clean()`, `logfile_repair.c`). `current_lsn` is
  read in `logfile_open.c:325` and never advanced by any writer.

What happens today, step by step:

1. On a read-write mount, `ntfs_glue_make_rw()` (`core/vfs/super_glue.c`)
   **retires the journal**: `ntfs_logfile_mark_clean_device()` writes
   two restart pages (v1.1, `VOLUME_IS_CLEAN`, log closed) — 2 writes,
   8 KiB. Only if that fails does it fall back to
   `ntfs_empty_logfile()`, the full-log erase. After this the log is
   empty and clean.
2. Every metadata write after that is unjournaled. Metadata policy
   (`docs/PORTING.md` §6): write-back, background writer tick 200 ms
   (`WRITER_TICK_NS`), folios dirty longer than 1000 ms
   (`NTFS_WRITEBACK_INTERVAL_MS`) flushed, `pagecache_sync_sb` ordering
   MFT-last; flush on sync/unmount. The first write of a session sets
   `VOLUME_IS_DIRTY` (`core/vfs/api.c`, `file.c`, `namei.c`); a clean
   unmount clears it (`core/ntfs/super.c`).
3. A yank after metadata has flushed but before unmount leaves the
   volume dirty with an **empty journal**. Windows has nothing to
   replay. Recovery is `chkdsk`, not the journal. That is the current
   crash story, and it is the reason this item is first in the gap list:
   everything else in §1 is about making replay correct; this one is
   about having anything to replay at all.

What the replay work buys the writer: a complete, Windows-verified
reader and applier for every record type the writer must emit, plus a
`mark_clean` that writes restart pages Windows accepts byte-for-byte
(`docs/LOGFILE.md` E1). The writer is the mirror image of that engine.
It is also the reason the writer cannot be a small patch: the replay
engine took a whole phase (phase 4) and is still only verified on four
scenarios plus synthetics.

## 2. What "journal writing" means

Write-ahead logging (WAL), the NTFS rule: **a metadata change reaches
the disk only after the log record describing it is durable**. In
concrete terms, for every metadata mutation the driver must:

1. Group the mutation into a **transaction** — one user-visible
   operation (create, rename, unlink, extend, …) — with redo records
   (what to re-apply) and undo records (what to roll back) for every
   MFT record, index block, bitmap range and mapping-pair change it
   makes.
2. Append those records to `$LogFile`'s circular area, allocate LSNs,
   maintain the restart area (current LSN, checkpoint tables), and
   **flush the log pages to the device** before the metadata itself is
   written.
3. On a clean unmount, close the log exactly as `mark_clean` does now
   (v1.1, `VOLUME_IS_CLEAN`). On a crash, the next mount — ours or
   Windows' — replays the records and the volume comes back consistent
   to the last committed transaction.

File *data* is never journaled in NTFS, only metadata. Data writes
stay exactly as they are. The journal covers MFT records, index
blocks, `$Bitmap`, `$MFT` mapping pairs and attribute headers — the
things `chkdsk` currently has to reconstruct after a yank.

## 3. Why this is the biggest item

### 3.1 There is no transaction model to hook into

Mutations today are ad-hoc sequences spread across `attrib.c`,
`mft.c`, `index.c`, `bitmap.c`, `lcnalloc.c`, `ea.c`, `reparse.c`,
`namei.c` and `file.c`, each writing MFT records, index blocks and
bitmap bits through the page cache with no grouping. Journaling needs
every one of those paths instrumented to emit records for what it just
changed, grouped per user-visible operation, with a commit point. That
is a cross-cutting change to the whole write path, not a module you
bolt on the side. The transaction boundaries have to be designed
operation by operation: a rename is two directory updates plus a
`$FILE_NAME` move; an extend is mapping pairs plus sizes plus bitmap;
a create is an MFT record init plus attributes plus index entries plus
bitmap. Nothing in the current code knows where one operation ends.

### 3.2 Record emission must be byte-exact NTFS

The writer must produce the same client records the replay engine
parses (`docs/LOGFILE.md` §2.5–2.9): LFS header (LSNs, transaction
id, record type, flags), NTFS client header (redo/undo opcodes,
offsets, lengths, `target_attr`, `lcns_to_follow`, `record_offset`,
`attribute_offset`, `cluster_block_offset`, `target_vcn`,
`page_lcns[]`), and redo/undo payloads in the exact layout each
opcode expects. Every opcode the driver can cause needs an emitter;
every emitter needs its payload validated against the replay engine
and against Windows. A record that is merely well-formed is not
enough — it must describe the change the metadata actually made, or
replay will "fix" the volume into a wrong state, which is worse than
no journal at all.

### 3.3 Windows is a co-reader from the first commit

There is no safe subset in which only our replay reads our records.
We retire the log on a clean unmount, so Windows never sees our
records after a clean session — but a yank leaves them in the log,
and the next Windows mount **will** replay them. So the records must
be Windows-acceptable from day one. The acceptance bar is not "our
replay agrees with itself" but "`chkdsk` clean and a Windows mount
replays them to the same state". That bar can only be cleared on a
Windows machine, per operation class (Phase 5), which is why the
phases below keep Windows validation as a gate rather than an
afterthought.

### 3.4 LFS machinery the reader never needed

The reader parses LSNs, pages, restart areas and checkpoints. The
writer must *maintain* them: allocate LSNs from `current_lsn` with
correct sequence numbers, lay records into pages (8-aligned, at
`log_page_data_offset`, multi-page continuation with
`LOG_RECORD_END` / `last_end_lsn`), write tail copies, keep the
restart area's `current_lsn` / `last_lsn_data_length` current, emit
checkpoint records (`NTFS_RESTART`, record type 2) carrying the four
table dumps (open-attribute, attribute-names, dirty-page,
transaction), and reuse the circular area when the log wraps — which
requires checkpointing everything older than the wrap point first.
None of that code exists; the reader's geometry and LSN math
(`logfile_open.c`, `logfile_pages.c`) are reusable, the maintenance
is new.

### 3.5 Ordering against the page cache

The background writer flushes dirty folios on its own schedule
(200 ms tick, 1000 ms age), with no knowledge of any log. WAL needs
log-before-metadata ordering, and the flush path must enforce it.
The design decision (see §4) is to log at mutation time, not at
flush time: the log write is part of the operation's commit, before
its folios are eligible for background writeback. That keeps the
page-cache writer untouched but puts a synchronous log flush on the
commit path of every metadata op — one log flush per op, which is
acceptable for metadata (already ~0.3–0.6 ms per op over FSKit) and
is what NTFS itself does (log writes are synchronous, metadata is
lazy). The existing `pagecache_sync_sb` MFT-last ordering is a
precedent for ordered flushes, not a substitute for WAL: it orders
metadata against metadata, where WAL orders log against metadata.

### 3.6 The writer makes replay atomicity a gate (GAPS.md §1.3)

Replay atomicity is proven at the engine level since 2026-11-26
(`test_replay_cut_points`, core/logfile/tests/test_logfile.c: 5/5
cut points converge to the uninterrupted state; see GAPS.md §1.3
for the boundary — synthetic logs, not a real-volume mount proof).
Once we write our own records, our replay must apply them after a
crash that may itself have interrupted a replay — so §1.3's
fault-injection proof was a prerequisite, not a parallel item, and
that prerequisite is now met. The writer also needs a round-trip
property the reader never had: records we write must replay
through our own engine to exactly the state the metadata already
has (idempotence via the per-record LSN skip the replay engine
already implements).

### 3.7 Torn writes and the log's own integrity

A yank can tear a log page mid-write. The format already defends
against this — every page carries update-sequence fixups and the
tail scan (H4, `docs/LOGFILE.md` §3) refuses a tail that could only
have been written after the page under examination — but the writer
must use it correctly: never emit a record that straddles a page
boundary except through the multi-page protocol, never update the
restart area to point past a page that is not yet durable, and never
let `current_lsn` advance past a torn page. The restart area is the
one structure whose corruption loses the whole log, so it is updated
last, after the record pages it describes are durable.

Known wart (noted 2026-11-26, round 18, not fixed): when the tail
scan stops at its very first page — a torn page the restart area
points at — `last_lsn` stays at the restart area's value and the
walk fails `-EINVAL` instead of truncating the log at that page.
The log is still safe (nothing is replayed from an untrusted tail),
but the failure mode is a refusal rather than a clean truncation.
Fix it when it fits the phase work: the scan should treat "first
page unreadable" as "log ends at the restart area" and let the walk
and replay proceed from the last good state.

## 4. Design

### 4.1 Modules

- **LFS writer** — new, in `core/logfile/` beside the reader
  (e.g. `logfile_write.c`): append records to the circular area, LSN
  allocation, page sequencing and fixups, tail copies, restart-area
  maintenance, checkpoint emission, log wrap. It shares the reader's
  geometry, LSN math and page layout; it does not know about inodes.
  Testable offline with `ntfslog` + the replay engine as oracle, no
  mount needed.
- **Transaction / record emission** — new, in `core/ntfs/` where the
  mutations live, behind a small API the mutation paths call:
  `ntfs_journal_begin(vol, &txn)`,
  `ntfs_journal_log_<op>(txn, …)` per operation class,
  `ntfs_journal_commit(txn)` / `ntfs_journal_abort(txn)`. Records are
  staged in memory per transaction and appended only at commit; abort
  discards the staging, so a failed operation writes no log at all.
  The emission functions live next to the code that performs each
  operation, because only that code knows what changed.
- **Commit / flush integration** — in the write path: commit appends
  the transaction's records to `$LogFile`, `pwrite`s the log pages and
  `sync`s the device, and only then marks the operation's folios
  eligible for writeback. The background writer needs no change: it
  only ever sees folios whose log records are already durable.
- **Close path** — the writer's unmount close replaces/augments
  `ntfs_logfile_mark_clean()`: same two restart pages, same v1.1
  clean state, but written by the writer module so the restart area
  it closes is the one the writer maintained. The existing
  `mark_clean` stays as the path for volumes whose log we did not
  write (retiring a Windows log on mount).

### 4.2 Version: 1.1 first

Write v1.1 records. The record format is version-independent; what
differs is the restart-area layout and the tail-copy area (v1.1: 2
tail pages; v2.0: 32). Our `mark_clean` already writes v1.1 restart
pages and Windows accepts them byte-for-byte (`docs/LOGFILE.md` E1),
and Windows itself downgrades to 1.1 on a clean dismount. 2.0 is what
Windows writes while mounted, and Windows upgrades the log to 2.0 at
mount itself — so writing 1.1 does not fight Windows, it hands it a
log in the form Windows produces at dismount. Revisit 2.0 only if
Windows rejects 1.1 records on replay (Phase 5 will tell us).

### 4.3 Checkpoint policy

Checkpoints (restart records with the four table dumps) are emitted
periodically, not per transaction: on log wrap, when the dirty-page
table grows past a bound, and at least every N transactions (N tuned
by measurement; start with 128). The checkpoint is what bounds
replay work — without it, a replay after a crash would walk the whole
log. The open-attribute and dirty-page tables are maintained
in-memory by the writer as transactions commit; the checkpoint
serialises them.

### 4.4 What the writer does not do

- No data journaling (NTFS never journals file data).
- No change to the dirty-flag policy: the flag is still set on first
  write and cleared on clean unmount; the journal is an additional
  recovery mechanism, not a replacement for the flag.
- No `$MFTMirr` special-casing beyond what NTFS does: mirror updates
  are MFT operations and are logged as such; the existing mirror sync
  in `write_inode` stays.
- No ACL mapping (§2.2) and no change to the replay opt-in policy:
  replay of *our* records after a crash follows the same
  per-volume-instruction rule as replay of Windows' records until
  Phase 5 proves otherwise.

## 5. Phased plan

| Phase | Work | Verifies with | Estimate |
|---|---|---|---|
| 0 | Gates: §1.3 fault-injection proof that replay is atomic; §1.5 real v2.0 image for the writer's version handling. Both are already scoped in GAPS.md; do them first because the writer depends on them. | Fault-injection test (kill mid-replay, volume still mounts/replays cleanly); v2.0 fixture in `tools/images`. | ~1 week |
| 1 | LFS writer infrastructure: append, LSN allocation, page sequencing + fixups, tail copies, restart-area maintenance, checkpoint emission, log wrap. No integration with mutation paths. Test tool writes synthetic transactions. | `ntfslog` + our replay engine as oracle on synthetic logs; `ntfsck` clean; writer-only unit tests in `core/logfile/tests/`. | 2–4 weeks |
| 2 | One operation class end-to-end: file creation (`InitializeFileRecordSegment` + `CreateAttribute` + `AddIndexEntryRoot/Allocation` + `$Bitmap`). Log-at-commit + WAL ordering wired into the create path; crash tests. | Kill -9 mid-create + replay → `chkdsk` clean; fixture suite still 265/0/0; new `test_crash` cases. | 2–3 weeks |
| 2 (step 2, landed 2026-11-27) | Create-path wiring: `core/vfs/journal.c` stages the transaction in `__ntfs_create`; WAL gate at `ntfs_write_mft_block` + `ntfs_write_folio` (`$MFT:$BITMAP` only) + `__ntfs_write_inode` — **not** `writeback_one`, which is not the single choke point (`ntfs_mft_writepages` and the inode path bypass it); mount opens the writer in `ntfs_glue_make_rw` and closes it in `ntfs_put_super`, retiring the journal on refusal; `MR_LSN` stamped on both touched records after a successful commit. Deliberate divergences from live bytes: the log copy of `$FILE_NAME` carries the `INDEXED` flag (live record does not); `$EA` is journaled as its net effect (final `$EA_INFORMATION` + `$EA`), not the intermediate updates. A mutation the log cannot describe (`-ENOENT`: entry in an `$INDEX_ALLOCATION` block, attribute in an extent) aborts the transaction and the create proceeds unjournaled — never fails the operation. Known Phase 2 gap: the parent's `$SI` mtime update at dir-inode writeback is not journaled; it belongs to the setattr class in Phase 3. | New `core/tests/test_journal.c`: 80 checks, 0 failures (journaled create in a driver-made directory: 13 records in order, replay redoes only the SetBits, twice identically; mkdir unjournaled; root create succeeds unjournaled). Logfile suite 4900/0. | — |
| 2 (step 3, landed 2026-11-27) | Crash tests for the create path: `test_journaled_create_cut_points` in `core/tests/test_crash.c` sweeps 19 sampled cut points through one journaled create (a write budget cuts the device writes; per cut pre-replay `ntfsck`, replay, post-replay `ntfsck` + remount lookup + second replay no-op). The first sweep found four real bugs, each fixed in its own commit: (1) the writer now emits `OpenNonresidentAttribute` records, so a first create is replayable before its commit checkpoint lands; (2) the create journals the `$MFT` `$DATA` size extension (`SetNewAttributeSizes`, staged first) plus `UpdateMappingPairs` when the runs change and the same sizes record for `$MFT:$BITMAP` — which exposed that a short-form attribute's offset 0x40 is the mapping pairs, not a `compressed_size` field, so the total is logged only when the pairs start after it; (3) the analysis skips transaction-table upkeep for table dumps, so a close checkpoint's dumps no longer resurrect a forgotten transaction as a phantom active; (4) the replay heals `$MFTMirr` for records 0–3 it did not itself touch, comparing and copying the raw on-disk bytes (a freshly protected copy would bump the USN and mismatch `$MFT`). A journaled create is 16 records. | `core/tests/test_crash.c`: 379 checks, 0 failures (19 cut points, 2 inconsistent before replay, 0 still bad after it; `j.txt` present 11 times, absent 8 times). Logfile suite 4989/0; driver journal suite 95/0. | — |
| 3 | Operation class by operation class: unlink, rename, mkdir, truncate/extend, setattr, xattr, symlink, EA, reparse, compressed-write. One commit per class, each with its own crash tests. | Per-class crash tests + `ntfsck`; Windows `chkdsk` per class (Phase 5). | 1–2 weeks per class, ~8 classes → 2–4 months |
| 3 (setattr design, 2026-11-27) | One transaction per `ntfs_setattr` call. `api.c` begins before the FLAGS/CRTIME mutations (they would otherwise destroy the old snapshot) and threads the carrier through `ntfs_vfs_setattr` → `ntfs_setattr_size` → `ntfs_truncate_vfs`, which joins via `ntfs_journal_truncate_begin_in` (snapshots only, stages into the setattr txn, no lock/gate/commit/stamp); stage + commit run in `api.c` after `ntfs_vfs_setattr` returns, so the CRTIME-only path is covered too; a no-op setattr aborts instead of committing (no log churn). `$SI`: one 36-byte `UpdateResidentValue` (4×u64 times + u32 flags, contiguous in `struct standard_information`), old and new both built from in-memory state at begin/stage — the mapped record lags behind the log, so a mapped-record snapshot would restore the pre-previous state on undo. `$EA`/`$EA_INFORMATION` (`$LXUID`/`$LXGID`/`$LXMOD` via `ntfs_ea_set_wsl_inode`, which writes straight into the mapped record): pure replace stages the whole value as `UpdateResidentValue`; absent at begin and present at stage stages `CreateAttribute` ×2 (the `create_ea` pattern); a length grow or a non-resident `$EA` aborts to unjournaled (the `UpdateResidentValue` resize path is unproven here). Parent `$FILE_NAME`: `UpdateFileNameRoot` via a shared `stage_filename_dup` helper factored from the truncate's tail — old sizes/flags from the begin snapshot, new from `ni` state (the same rule `ntfs_inode_sync_filename` applies), times from the entry (a utimens updates `$SI` mtime but NOT the file's own `$FILE_NAME` times nor the parent's entry times — probed, record 65: the live driver never updates `$FILE_NAME` times after create/link; fixing that staleness is a live-driver bug, not this class). The outer commit stamps `MR_LSN` into the file's record only (the `$FILE_NAME` redo is idempotent, as in truncate). Lock order: `inode_lock` → `log_txn_lock` → `mrec_lock`. Follow-ups, not this class: parent `$SI` mtime on create/unlink/rename (Phase 2 gap — apply the `$SI` staging machinery to the parent in those three transactions), `$SI` ctime on rename, `$EA` length-grow (needs a logfile-level test proving the resize path), extending writes during writeback (still unjournaled). | `core/tests/test_journal.c`: chmod+utimens walk + op sequence, `$SI` redo/undo times+flags, `$LXMOD` value, `$FILE_NAME` flags, replay redoes only the idempotent records; SIZE+MODE join in one transaction; root fallback unjournaled; negative control; `core/tests/test_crash.c`: chmod+utimens cut-point sweep. | — |
| 3 (xattr, landed 2026-11-27) | One transaction per `ntfs_setxattr` / `ntfs_removexattr` call. `api.c` begins after `inode_lock` + `VOLUME_IS_DIRTY`, before `ntfs_attr_iget`; stage + commit run under `mrec_lock` after the ctime bump. Begin snapshots the `$SI` 36 bytes (from in-memory state, setattr's form) + the ADS whole-record copy + the decoded runlist when non-resident, all from the mapped base record, before raising the WAL gate. Stage: `$SI` as one 36-byte `UpdateResidentValue` on change (shared `stage_si_36`); create → `CreateAttribute` + `SetBits` for the new clusters; remove → `DeleteAttribute` (begin-time bytes/offset) + `ClearBits` for the old runs; resident→resident replace → one `UpdateResidentValue` with the whole value at any length — this is the first class to exercise the replay's length-change path; flip either way → `DeleteAttribute` + `CreateAttribute` + bitmap delta; non-resident→non-resident → mapping pairs first, then sizes, then bitmap delta via `lcn_ranges_sub` — the pairs go through the dedicated `UpdateMappingPairs` op, not a raw `UpdateResidentValue` on the pairs bytes: redo order is extend the runs, then the sizes that name them (the truncate class stages the same way), and a raw value update leaves the pairs disagreeing with the sizes so the replay corrupts the attribute (the crash sweep caught it: `Corrupt mapping pairs array`, ADS unreadable). A header change (flags, name or pairs offset moved) aborts to unjournaled. Abort to unjournaled (never fail the op) on: attribute list, compressed/encrypted ADS at stage, ADS moved or pairs undecodable, `$SI` missing/too short, non-regular file/directory. No `$FILE_NAME` record: ADS sizes are not in the parent dup, and the live driver never updates `$FILE_NAME` times on setattr/xattr (a live-driver bug, probed — the journal reproduces the live bytes). A file in the volume root is journalable for xattr (only its own record + `$Bitmap` are touched). Bug fixed en route: `lcn_ranges_sub` returns the range COUNT (≥ 0), not an error — the first implementation did `err = lcn_ranges_sub(...); if (err) goto out_free;` and scanned the output for a zero terminator the function never writes, so every non-resident replace aborted unjournaled; fixed to the truncate pattern (`n_freed`/`n_allocd`, `< 0` → `-ENOSPC`, loop by count). Debugging lesson, same round: inserting a bare `fprintf` between an `if` and its single-statement body silently unbraces the body — the finish's `goto out_abort` ran unconditionally and every transaction aborted; probes must be standalone statements or braced blocks. Crash contract: before commit → undo (old value/absent, bitmap + `$SI` restored); after commit → redo (new value/presence, bitmap agrees, new `$SI` ctime); crash after commit before pagecache flush → non-resident ADS with new sizes and stale/zero value bytes — the same as a regular file write crash, metadata consistent, `ntfsck` clean; second replay no-op. | `core/tests/test_journal.c`: 899 checks, 0 failures (5 new tests: create, replace, big create, big replace+remove, root; walk counts + op sequence, `$SI` ctime redo/undo, replay redoes, values correct after replay + mount, `ntfsck` clean; negative control pending in the crash-test commit). `core/tests/test_crash.c`: cut-point sweep in the follow-up commit (5 sweeps: create-small, create-big, replace resident, replace to non-resident, remove; per cut pre-replay ntfsck, replay, post-replay ntfsck + remount getxattr all-old/all-new, second replay no-op). | — |
| 3 (symlink, landed 2026-11-27) | One transaction per `ntfs_symlink` call, reusing the create transaction: `ntfs_journal_create_begin` in `__ntfs_create` now also begins for `S_ISLNK` (devices still never begin — mknod is a separate class). A symlink create touches three MFT records — the new record, the parent dir (index entry) and `$Reparse` (its `$R` index entry) — plus `$MFT:$BITMAP` SetBits, all in the one transaction. The target lives in `$REPARSE_POINT`, not `$DATA`: `ntfs_reparse_set_win_symlink` / `ntfs_reparse_set_wsl_symlink` (via `ntfs_set_ntfs_reparse_data`, `reparse.c`) adds the attribute if absent, pwrite's the value into the mapped record, then inserts a `struct reparse_index` entry (key = tag u32 + file_id u64) into `$Reparse`'s `$R` index; a directory symlink (`dir_link`) stages `$INDEX_ROOT` instead of `$DATA`. Stage, in both `__ntfs_create` branches right after the reparse set, via one new `ntfs_journal_create_reparse(jt, vol, ni, ni_mrec)`: (1) `CreateAttribute` for `AT_REPARSE_POINT` (existing `create_attr`, reads the final value from the mapped record at stage time); (2) `AddIndexEntryRoot` for the `$R` entry — new staging that finds the entry by (tag, file_id) in `$Reparse`'s `$R` root (abort to unjournaled if the entry landed in an `$INDEX_ALLOCATION` block, the `create_index` pattern); (3) a 4-byte `UpdateResidentValue` for the `$SI` flags — the mapped `$SI` still holds the as-added flags at stage time (the WAL gate defers `ntfs_inode_sync_standard_information` while the transaction is open), so old = mapped flags, new = `ni->flags` (with `FILE_ATTR_REPARSE_POINT`), staged only on change, the truncate `$SI` pattern. `$FILE_NAME` is staged after the reparse set and already carries the tag — no change. `$DATA` staged empty is correct. No `$EA` on symlinks. Commit stamps `MR_LSN` into the new record and the parent (as before) and now also into `$Reparse`'s record (the txn carries `$Reparse`'s mft_no); without it, a post-commit flush + crash would duplicate the `$R` entry on replay, because `AddIndexEntryRoot` is not idempotent — the replay's skip check is per-record `MR_LSN`, so every record a transaction touches must be stamped. Crash contract: before commit → undo (no link, no `$R` entry, bitmap restored); after commit → redo (link + target + `$R` entry + flags, `ntfsck` clean); second replay no-op. A cut mid-transaction leaves the link all-old (absent) or all-new (readlink returns the target) — never a link without its `$R` entry or a `$R` entry without its link. Follow-ups, not this class: `$SI` ctime on rename; parent `$SI` mtime on create/unlink/rename (Phase 2 gap); mknod (devices also carry reparse data but never begin). | `core/tests/test_journal.c`: 1177 checks, 0 failures (4 new tests: win-tag, WSL-tag, dir-link, root fallback — each journaled link 17 records; walk + op sequence, `$SI` flags redo 0x400 / undo 0, `$R` entry exactly one after replay and after a second replay, readlink correct, `$SI` carries 0x400, `ntfsck` clean; root link writes no records). `core/tests/test_crash.c`: 5791 checks, 0 failures (3 sweeps, 177 cut points: win 59, wsl 59, dir 59; 6 inconsistent before replay each, 0 still bad after it; link all-old 24/24/23, all-new 35/35/36; per cut the `$R` entry count agrees with the link). | — |
| 3 (EA, landed 2026-11-27) | One transaction per VFS xattr-handler set on the $EA path (`ntfs_setxattr` in `core/ntfs/ea.c` — the handler the tests drive; the public `ntfs_setxattr` in `api.c` stores xattrs as ADS and is the xattr class). The hook is in the handler, NOT inside `ntfs_set_ea`: that function is also called from inside the ambient create/setattr transactions (which stage `$LX*` via `ntfs_journal_create_ea` / `stage_setattr_ea`), and a begin inside it would nest the WAL gate and the `log_txn_lock`. The handler's flags path (`system.ntfs_attrib` / `system.ntfs_dos_attrib`, via `ntfs_new_attr_flags`) is NOT journaled here: it changes $SI flags only, no $EA, and belongs to the setattr class's domain. ACL: `CONFIG_NTFS_FS_POSIX_ACL` is not defined in this port (no `ntfs_set_acl` symbols in the archive), so `ntfs_set_acl_ex` is compiled out and there is no ACL hook; when §2.2 enables ACLs its sets will need the same treatment (they call `ntfs_set_ea` directly, not the handler). Begin snapshots the $SI 36 bytes from in-memory state (the setattr form — the mapped record lags, so a mapped snapshot would restore the pre-previous state on undo) plus whole-record copies of $EA_INFORMATION and $EA with their offsets and presence; the $SI offsets come from the mapped record. Stage: $SI via the shared `stage_si_36` (the handler only bumps ctime and `ntfs_set_ea` sets/clears HasEA in `ni->flags`); the pair net effect — absent→present stages CreateAttribute ×2 ($EA_INFORMATION first), present→absent stages DeleteAttribute ×2 ($EA first, the live removal order, from the begin-time bytes), present→present stages one whole-value UpdateResidentValue per changed value, any length (the xattr-proven length-change path). A half-pair at either end, a non-resident $EA or $EA_INFORMATION at either end, or a moved attribute aborts to unjournaled. A half-pair at begin also never begins. Scope: regular files and directories; an attribute list never begins; the root directory itself is journalable and is where the create/teardown cases are tested (it is the only inode a driver-made volume guarantees has no $EA pair — `ntfs_create` writes the three $LX* entries into every file it makes). The commit stamps MR_LSN into the file's record only; no $FILE_NAME record (EA sizes are not in the parent dup). Lock order: `inode_lock` → `log_txn_lock` → `mrec_lock`. Crash contract: before commit → undo (old pair, old $SI); after commit → redo (new pair, new $SI); second replay no-op. Follow-ups, not this class: the xattr class snapshots $SI from the mapped record instead of in-memory state — a latent one-step-too-far undo on $SI ctime/flags when two xattr transactions run without a sync between them; the handler's flags path (`system.ntfs_attrib`) is still unjournaled. | `core/tests/test_journal.c`: 1418 checks, 0 failures (was 1177; 7 new tests: replace, replace-grow, remove, create on root, teardown on root, in-root, failed-set negative control; walk counts + op sequence, $EA stream redo/undo whole values, $EA_INFORMATION ea_query_length agreeing with the stream lengths, replay redoes only the idempotent records, values correct after replay + mount, `ntfsck` clean). `core/tests/test_crash.c`: 6 cut-point sweeps (create, replace-grow, replace-shrink, remove on a file; create + teardown on the root — the only inode exercising the CreateAttribute ×2 / DeleteAttribute ×2 paths), 229 cut points, 12 inconsistent before replay, 0 still bad after; EA all-old 48, all-new 181; $LXMOD asserted present after every file-sweep replay; test_crash 7641 checks, 0 failures (was 5791). No bug found. | — |
| 3 (reparse, landed 2026-11-27: unlink half + mknod half, both this round) | The reparse class finishes the $R index entry's lifecycle: the symlink class journaled its creation, this class journals its removal at unlink. One transaction per `ntfs_unlink` of a reparse point: the unlink's data records are DeleteIndexEntryRoot ($I30), DeleteAttribute ($FILE_NAME), DeleteIndexEntryRoot ($R), DeallocateFileRecordSegment and ClearBitsInNonresidentBitMap, in live order. The $R removal is staged by a new `ntfs_journal_unlink_stage_reparse` in the link-count-0 branch of `ntfs_delete`, before `ntfs_delete_reparse_index` removes the entry — the staging reads the live bytes, and the entry is gone afterwards. The tag comes from the file's own record (its $REPARSE_POINT value's first 4 bytes): `ni->flags` is the authority for whether the file IS a reparse point, but `ni->reparse_tag` is only populated by the readlink path (`ntfs_make_symlink`), so it cannot be trusted at unlink time. The $R entry is found by (tag, file_id) in $Reparse's $R root through a new shared `find_reparse_index_entry` helper, factored out of `ntfs_journal_create_reparse`'s scan (the symlink journal tests still pass, so the refactor is behaviour-identical). The $REPARSE_POINT attribute itself needs no record: the record free staged after the $R removal (stage_free) carries the whole post-delete record bytes, and the attribute dies with the record. `ntfs_journal_unlink_check` no longer refuses reparse points — only object IDs still refuse, since their $Extend removal is undescribable. A $R entry in an $INDEX_ALLOCATION block, a missing $REPARSE_POINT attribute, or a hard link whose count does not hit 0 (no $R removal happens) all abort to unjournaled. The unlink carrier gains its own `reparse_mft_no` (initialized to 0 at begin — kmalloc does not zero, round 25), and both the unlink and the rename commit stamp MR_LSN into $Reparse's record through a new shared `stamp_reparse_mft_lsn` helper (also used by the create commit now): without the stamp a post-commit flush + crash would replay the offset-based DeleteIndexEntryRoot onto a record that already lacks the entry and excise the wrong bytes. A rename of a reparse point becomes journaled as a side effect — the $R key is (tag, file_id), which a rename does not change, and the delete steps go through `ntfs_delete` with the shared carrier. No VFS operation sets reparse data on an existing file: the three setters run only inside `__ntfs_create`, so symlink create (done) plus device create (this class) cover every live reparse writer. `ntfs_remove_ntfs_reparse_data` was declared in `reparse.h` but defined only in libntfs-3g and never called in this port — the dead declaration is removed in this commit. Crash contract: before commit → undo (link + $R entry + record + bitmap restored); after commit → redo (link gone, $R entry gone); second replay no-op. The mknod half: `ntfs_journal_create_begin` begins for S_ISBLK/S_ISCHR/S_ISSOCK/S_ISFIFO, and the device branch of `__ntfs_create` stages `ntfs_journal_create_reparse` right after `ntfs_reparse_set_wsl_not_symlink` — the $REPARSE_POINT and the $R entry. The $SI flags are staged separately, by a new `ntfs_journal_create_si_flags`, after the $FILE_NAME is built: `ni->flags` is only final once `__ntfs_create` folds `fn->file_attributes` back into it, so the staged new value is `fn->file_attributes | (ni->flags & FILE_ATTRIBUTE_RECALL_ON_OPEN)` — the same formula as the fold. Staging from `ni->flags` at the reparse set (as the symlink class originally did) replayed a device's $SI without SYSTEM (0x40400, live 0x40404) and a dir-link's without DUP_FILE_NAME_INDEX_PRESENT (0x400, live 0x10000400): the $SI and $FILE_NAME copies disagreed after replay, depending on whether the record had been flushed. The old value is read from the mapped $SI (FILE_ATTR_SYSTEM for a device — the device branch's RECALL_ON_OPEN write goes to the $SI copy, not the record; no flags for a link). The commit path is mode-agnostic, and a device create already staged $DATA, $EA and $FILE_NAME, so begin + the staging calls close the class. Crash contract for mknod: before commit → undo (no device, no $R entry, record + bitmap restored); after commit → redo (device with its mode, tag and $R entry); second replay no-op. Follow-up, not this class: the mkdir class never stages a $SI flags update at all, so a replayed directory's $SI carries the as-added flags instead of the final DUP_FILE_NAME_INDEX_PRESENT — the same shape, unasserted there. | `core/tests/test_journal.c`: 1586 checks, 0 failures (was 1418; new test_unlink_reparse_journaled: walk 27 records — the create's 15 still-live plus the unlink's 12 (2 opens + DeleteIndexEntryRoot $I30 + DeleteAttribute + DeleteIndexEntryRoot $R + Deallocate + ClearBits + Commit + 3 dumps + Forget); $R delete targets $MFT:$DATA at vcn rp_ino/4; MR_LSN stamped into the file, the parent and $Reparse; replay redoes 3 (create's sizes + SetBits, unlink's ClearBits); link gone and $R count 0 after replay and on the live snapshot; `ntfsck` clean; new test_mknod_journaled: walk 19 records — 2 opens + 12 data ($MFT $DATA sizes, Initialize, $SD, $DATA, $REPARSE_POINT, $R entry, $EA_INFORMATION, $EA, $FILE_NAME, $SI flags, $I30 entry, SetBits) + Commit + 3 dumps + Forget; $SI redo 0x40404 undo 0x4, live and replayed $SI agree at 0x40404; MR_LSN stamped into the file, the parent and $Reparse; replay redoes 2; device reads back S_IFCHR\|0644 with REPARSE_POINT and RECALL_ON_OPEN on its $SI, tag 0x80000025 and $R count 1; second replay redoes 2 and the $R entry is still exactly one; `ntfsck` clean; the symlink and dir-link walks moved their $SI update after the $FILE_NAME create too — res_redo 0x400 for a plain/WSL link, 0x10000400 for a dir-link, live and replayed $SI agreeing in both cases). `core/tests/test_crash.c`: one cut-point sweep for the unlink half (46 cut points, every write sampled: 6 inconsistent before replay, 0 still bad after it; link all-old 13 times, all-new 33 times; per cut the $R entry count agrees with the link) and one for the mknod half (63 cut points, every write sampled: 6 inconsistent before replay, 0 still bad after it; device all-old 27 times, all-new 36 times; per cut the $R entry count agrees with the device — 1 with tag 0x80000025 when present, 0 when absent — and the $SI flags are exactly 0x40404 when present); test_crash 8519 checks, 0 failures (was 7641). The mknod sweep found one bug (the $SI staging, fixed in the follow-up commit of this round); the unlink sweep found none. | — |
| 3 (compressed-write, landed 2026-11-27) | The last Phase 3 class: one transaction per `do_write` on a compressed file that is ALREADY non-resident when the write begins. A new carrier `struct ntfs_journal_cwrite` (`core/vfs/journal.c`, API in `core/vfs/journal.h`), reusing the truncate patterns: begin snapshots the `$DATA` sizes + pairs + runlist (`snapshot_mft_attr`), the `$SI` 36 bytes from in-memory state (`si_build`), the `$SI` flags, `fn_alloc_old` (the compressed-size rule) and a runlist copy. Begin runs at the top of the compressed branch of `do_write`, BEFORE the `ntfs_attr_expand` — the expand itself changes sizes/pairs/bitmap and the WAL gate must be up first. Begin gate: regular file, non-resident `$DATA`, compressed, no attribute list, not encrypted, not an attribute inode (`NInoAttr` — which excludes the EA-set `do_write` calls on ADS inodes, journaled by the xattr class); a resident file at begin aborts to unjournaled, and a begin failure warns and proceeds unjournaled (the xattr class's rule), never failing the write for journal reasons. Stage runs in `done:`, after the mtime bump, under `mrec_lock`: the `$Bitmap` delta first via `lcn_ranges_sub` (old vs new runlists), then `UpdateMappingPairs` (pairs before sizes, the truncate order), `SetNewAttributeSizes` (alloc/initialized/data/compressed_size — the total only when the pairs start at or after 0x48), the `$SI` 36 bytes via the shared `stage_si_36`, and the parent `$FILE_NAME` via `stage_filename_dup` (mirroring `stage_truncate_filename_one`). An empty transaction aborts instead of committing. Commit stamps `MR_LSN` into the file's record. On a failed write the transaction aborts before the existing rollback runs; a commit failure sets `NVolSetErrors` and returns the error (the truncate/xattr rule). The `$SI` flags come from in-memory `ni->flags`, NOT from the mapped record (the truncate class's source): the flags handler (`ntfs_new_attr_flags`) updates `ni->flags` but never the `$SI` record bytes, so a handler-flagged file carries 0x800 in `ni->flags` and 0x0 in its mapped `$SI`, and the parent's entry is refreshed from `ni->flags` — staging the mapped value would clear COMPRESSED from the entry on replay. The first version did exactly that and the overwrite test caught it (its `$FILE_NAME` undo carried flags 0x0 against a live entry of 0x800); the setattr carrier sources its flags the same way. A compressed write never changes the flags, so old and new agree. Content is all-old/all-new on crash without journaling the data: `ntfs_write_cb` always allocates NEW clusters for a rewritten block, writes the compressed data there, then punches the old clusters (upstream port fix `eb67671cd814`) — undo restores the old pairs, whose clusters still hold the old data (the punch frees bitmap bits only, never erases), and redo applies the new pairs, whose clusters hold the new data. The all-zeroes shortcut punches only if the block is allocated; writing zeros to an already-hole block changes nothing (then only the `$SI` record stages). The resident→non-resident conversion inside a write is a documented limitation, not journaled: the attribute changes shape and every following attribute shifts, needing DeleteAttribute+CreateAttribute machinery that is unproven. The negative control reaches it through the `compressed-sparse.img` fixture's `/compressed/inherit` (a compressed directory, so a child created there is resident + compressed + `NInoCompressed`); the fixture is used because the test cannot flag a directory itself — `ntfs_attr_set_compression_flag` takes the raw `struct ntfs_inode *`, which the test API's opaque handle cannot reach, and the setxattr handler only updates `ni->flags`/`$SI` on a directory (never `$INDEX_ROOT`) and converts a file to non-resident eagerly. Crash contract: before commit → undo (old pairs/sizes/bitmap/`$SI`/`$FILE_NAME`); after commit → redo (new ones); second replay no-op. Follow-ups, not this class: the truncate class shares the flags-source shape for handler-flagged sparse files (its mapped-`$SI` undo would clear SPARSE from the entry after a refresh); the mkdir class never stages a `$SI` flags update (a replayed directory's `$SI` carries the as-added flags instead of the final DUP_FILE_NAME_INDEX_PRESENT). | `core/tests/test_journal.c`: 1816 checks, 0 failures (was 1586; three new tests: overwrite one 64 KiB block — walk 39 records, the create's 14 still-live plus the setup write's 12 plus the op's 13 (2 opens + ClearBits + SetBits + pairs + sizes + `$SI` + `$FILE_NAME` + Commit + 3 dumps + Forget); the `$FILE_NAME` undo/redo flags are both 0x800 and the `$SI` flags agree; replay redoes 7; append one block at EOF — walk 39, the old 8 KiB tail block frees 3 clusters, replay redoes 7; conversion — walk 16, the create only, replay redoes 2; content reads back whole after replay + mount in all three; `ntfsck` clean). Crash tests landed in the follow-up commit of this round: three cut-point sweeps in `core/tests/test_crash.c` — overwrite one 64 KiB block (49 cut points), append one block at EOF (50), overwrite two blocks crossing a boundary (50); per cut ntfsck + allocation coverage + replay + remount read-back + second replay no-op, with size, allocation and content all required to agree on the same side. `test_crash` 9869 checks, 0 failures (was 8519): 149 cut points, 15 inconsistent before replay, 0 still bad after it; content all-old 53 times, all-new 96 times, never mixed; old alloc 208896 bytes in all three sweeps, new alloc 147456 (overwrite), 212992 (append), 151552 (two-block). No bug found; this closes the compressed-write class and §1.2 Phase 3. | — |
| 4 | Log wrap + checkpoint under load; performance: write amplification per op, op latency vs the no-journal baseline, log-size behaviour on small sticks (5.5 MiB log). | Benchmarks against the phase-5 bar (metadata within 2× of Apple's exFAT); long-run soak with forced wraps. | 2–3 weeks |
| 5 | Windows round-trip per operation class: Windows mounts the volume, replays our records, `chkdsk /f` clean, byte-for-byte data check. | Physical Windows PC (the phase-4 capture setup in `docs/LOGFILE.md` §7). | Needs a Windows machine; ongoing per class |

Total: **several months of focused work** — comparable to the phase-4
replay work, which took weeks and is still only verified on four
scenarios plus synthetics. That scale is the finding, not a failure:
no shipping open-source NTFS writer writes `$LogFile` records
(ntfs-3g, ntfs3 and NTFSPLUS are all replay-only or nothing; the
ecosystem's recovery path is the dirty flag plus `chkdsk`). This
driver would be the first. The phased plan is what makes that tractable:
each phase lands independently verifiable value, and Phase 2 alone —
one operation class, journaled — already changes the crash story for
the most common metadata operation.

## 6. Test plan (applies from Phase 1 on)

- **Round-trip**: records written by the writer replay through our
  own engine (`ntfslog` + `core/logfile` apply path) to exactly the
  state the metadata already has; a second replay is a no-op (LSN
  skip).
- **Fault injection**: kill the process mid-commit, mid-flush and
  mid-checkpoint; corrupt/torn log pages; the volume must still mount
  (read-only at worst) and replay to a consistent state. This is the
  §1.3 test, extended to writer crashes.
- **Refusal, not corruption**: a malformed record (fuzzed payloads)
  must make replay refuse with `needs_chkdsk`, never restructure
  metadata. The §5.2 fuzz harness grows a writer variant.
- **Structural**: `ntfsck` (ntfsprogs-plus) clean after every test;
  fixture suite 265/0/0 throughout; no regression in ctest/UBSan.
- **Windows**: per-class `chkdsk /f` + Windows-mount replay, on the
  physical PC. Nothing in Phases 2–4 is "done" for an operation class
  until Phase 5 has cleared it.

## 7. Acceptance criteria

- Yank mid-write → next mount (ours or Windows') replays to the last
  committed transaction; `chkdsk` clean; no data loss beyond the
  in-flight operation.
- Clean unmount → log closed as today (v1.1, `VOLUME_IS_CLEAN`);
  Windows mounts the volume without complaint.
- No regression: fixture suite 265/0/0, ctest 15/15, UBSan 15/15,
  pre-push green, on every phase commit.
- Performance: metadata ops stay within the phase-5 bar (within 2×
  of Apple's exFAT); log write amplification measured and recorded
  per operation class.

## 8. Open questions

1. Does Windows accept 1.1 records written by a non-Windows writer
   while the volume was mounted by us (as opposed to only at
   dismount)? Testable only on Windows (Phase 5, file creation
   first). If it refuses, the fallback is v2.0 record writing — same
   record format, larger tail-copy area.
2. Checkpoint frequency vs log size on small sticks: a 5.5 MiB log
   with a checkpoint every 128 transactions may wrap too often under
   sustained metadata load. Measure in Phase 4; the bound is tunable.
3. Are `$MFTMirr` updates logged as separate records, or does NTFS
   rely on mirror sync alone? Read the answer out of a real Windows
   log (Phase 0 capture work) rather than guessing from fslog.c.
4. The writer's close must replace `mark_clean` for logs we wrote,
   not duplicate it — one close path, with `mark_clean` kept for
   retiring Windows logs on mount. The boundary between the two needs
   a state flag on the volume, not a heuristic.
5. How much of the transaction-table / dirty-page-table state must
   survive across a remount for replay of our own records to be
   fast? The checkpoint carries it; the question is how often the
   checkpoint must be written for replay time to stay acceptable on
   large logs (64 MiB).

## 9. Non-goals

Data journaling; moving the upstream pin; changing the dirty-flag
policy; ACL mapping (§2.2); changing the replay opt-in policy;
anything that writes to a volume Windows has hibernated (§1.4).
