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
| 3 (xattr, landed 2026-11-27) | One transaction per `ntfs_setxattr` / `ntfs_removexattr` call. `api.c` begins after `inode_lock` + `VOLUME_IS_DIRTY`, before `ntfs_attr_iget`; stage + commit run under `mrec_lock` after the ctime bump. Begin snapshots the `$SI` 36 bytes (from in-memory state, setattr's form) + the ADS whole-record copy + the decoded runlist when non-resident, all from the mapped base record, before raising the WAL gate. Stage: `$SI` as one 36-byte `UpdateResidentValue` on change (shared `stage_si_36`); create → `CreateAttribute` + `SetBits` for the new clusters; remove → `DeleteAttribute` (begin-time bytes/offset) + `ClearBits` for the old runs; resident→resident replace → one `UpdateResidentValue` with the whole value at any length — this is the first class to exercise the replay's length-change path; flip either way → `DeleteAttribute` + `CreateAttribute` + bitmap delta; non-resident→non-resident → sizes + mapping pairs (`UpdateResidentValue` on the pairs bytes) + bitmap delta via `lcn_ranges_sub`. Abort to unjournaled (never fail the op) on: attribute list, compressed/encrypted ADS at stage, ADS moved or pairs undecodable, `$SI` missing/too short, non-regular file/directory. No `$FILE_NAME` record: ADS sizes are not in the parent dup, and the live driver never updates `$FILE_NAME` times on setattr/xattr (a live-driver bug, probed — the journal reproduces the live bytes). A file in the volume root is journalable for xattr (only its own record + `$Bitmap` are touched). Bug fixed en route: `lcn_ranges_sub` returns the range COUNT (≥ 0), not an error — the first implementation did `err = lcn_ranges_sub(...); if (err) goto out_free;` and scanned the output for a zero terminator the function never writes, so every non-resident replace aborted unjournaled; fixed to the truncate pattern (`n_freed`/`n_allocd`, `< 0` → `-ENOSPC`, loop by count). Debugging lesson, same round: inserting a bare `fprintf` between an `if` and its single-statement body silently unbraces the body — the finish's `goto out_abort` ran unconditionally and every transaction aborted; probes must be standalone statements or braced blocks. Crash contract: before commit → undo (old value/absent, bitmap + `$SI` restored); after commit → redo (new value/presence, bitmap agrees, new `$SI` ctime); crash after commit before pagecache flush → non-resident ADS with new sizes and stale/zero value bytes — the same as a regular file write crash, metadata consistent, `ntfsck` clean; second replay no-op. | `core/tests/test_journal.c`: 899 checks, 0 failures (5 new tests: create, replace, big create, big replace+remove, root; walk counts + op sequence, `$SI` ctime redo/undo, replay redoes, values correct after replay + mount, `ntfsck` clean; negative control pending in the crash-test commit). `core/tests/test_crash.c`: cut-point sweep in the follow-up commit. | — |
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
