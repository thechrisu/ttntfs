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

End state, set 2026-11-26 by the owner: keep working until this is
an industry-leading driver that does everything for all Macs
perfectly and is at its physical limit for how fast and correct it
can be — every supported Mac, every NTFS feature Paragon/Tuxera
cover, journal-correct, as fast as the hardware allows. That reopens
§4.3 (earlier macOS), §4.2-testing (Intel run), §3.1/§3.2
(performance to the physical limit) and §2.3/§2.4 (re-examine TRIM
and in-app fsck for any FSKit path) as active work after the port
queue. §4.1 and §5.1 stay blocked on Apple/signing; the paid signing
route was declined and is not re-proposed.

North star, set 2026-11-26 by the owner: data safety is the
overriding first priority, above speed and above everything else —
the worst possible outcome is the driver wiping, corrupting or
breaking user data. The target is a bulletproof, state-of-the-art
driver, best in class for correctness, aiming to be better than any
filesystem driver of any filesystem out there. In short: **a fast
driver that can lose data is a failure; a correct driver that is
slightly slower is not.** What that changes: §1.2 journal writing
moves to the top of the post-port-queue work, ahead of features and
performance; §1.3 gates §1.2 Phase 0 and any later performance work
that touches the write path; crash-consistency is a standing bar,
not a one-off test (power-loss / torn-write simulation on every
write-path change, fault injection at every flush point, and a torn
volume must always be detected, never silently papered over — the
4188d3b1 lesson); fuzzing (§5.2) gets more coverage, not less; every
write-path change is verified against Windows chkdsk where a round
trip is possible and by ntfsck + remount read-back locally where it
is not; and no performance optimisation may trade away a safety
margin — a perf change touching the write path needs the same
crash-consistency evidence as a correctness fix.

Hard design constraint, set 2026-11-26 by the owner and standing
for all future work: **if the computer crashes or any component
fails, including the drive suddenly being disconnected, the volume
must always be left in a good state.** This is a design constraint,
not a test to run at the end — it decides the cache that is chosen
(what is cached, write-through vs write-back, write ordering, and
exactly what can be lost on a sudden disconnect). Every design
decision that touches durability — cache policy, flush ordering,
$LogFile record layout, replay behaviour, dirty-volume handling —
is evaluated against "crash or disconnect at any instant leaves a
good state" first, speed second. If a design cannot prove that, it
does not ship. Standing rule: any commit that touches caching,
flushing, journaling or replay must state in its message what
happens on a crash/disconnect at the worst instant. See
docs/JOURNAL-DESIGN.md §0 for the full statement.

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
Phase 5 Windows round-trip per class. Top of the post-port-queue
work since 2026-11-26 (owner's data-safety-first direction): the
port queue finishes first, then §1.2 starts at Phase 0/1, ahead of
§2.2/§4.4 and ahead of all performance work. Stated explicitly
(2026-11-26 hard constraint): the current metadata write policy —
write-back, flushed within 1 s, journal writing deferred — does
NOT meet "crash or disconnect at any instant leaves a good state".
A crash inside the flush window loses up to a second of metadata
with an empty journal and nothing to replay. This implementation is
what replaces that policy; until it lands the gap is open and
recorded as such. See docs/JOURNAL-DESIGN.md §0.

Phase 1, step 1 landed 2026-11-26 (commit, this round): the LFS
writer core — `core/logfile/logfile_write.c` (`format`, `open`,
`append`, `sync`): v1.1 records, LSN allocation, page sequencing
with fixups, tail-copy-before-rewrite, single-page I/O, restart-area
maintenance, first-record restart flush (the scan derives its
expected sequence from the restart area, so a log with records but a
never-flushed restart area is unreadable — proven by negative
control: without the flush the walk fails `-EINVAL`). Crash contract,
per append: crash before the tail copy → record absent; after the
tail copy, before the in-place page → record present via the scan's
override; during the in-place page → torn page, log ends at the
previous record. A log-write failure fails the writer (volume-error
semantics). Wrap is refused (`-ENOSPC`) until a checkpoint exists.
Tests: `test_writer_roundtrip` (incl. a 6 KiB multi-page record,
byte-exact read-back through the reader), `test_writer_reopen_append`
(write counts pinned: 7 then 11), `test_writer_tail_copy_crash`
(crash between copy and page → record present; refuse the copy →
only the earlier record), `test_writer_torn_tail` (torn last page →
log ends at the previous record). Negative controls: no tail copy →
crash test fails 4 checks; no first-record flush → walk fails
`-EINVAL`.

Phase 1, step 2 landed 2026-11-26 (commit, this round): checkpoint
emission — `ntfs_log_writer_checkpoint()` dumps each non-empty
restart table as a client record (dump opcodes 0x1d–0x20, tid 0x18:
the replay analysis allocates an entry for every client record's
tid and rejects tid 0), then an NTFS_RESTART record naming them,
then syncs the restart area so the new checkpoint is referenced
before any later wrap can destroy the one it supersedes. Oldest
live LSN advances to the checkpoint's start (first dump's LSN, or
the restart record's own LSN when every table is empty); an active
transaction's first record is never passed over. Also:
`ntfs_log_writer_open_attr()` registers an open attribute (+ name
in the attribute-names blob, keyed by table offset); `writer_open()`
adopts the last checkpoint's tables so a second session's checkpoint
does not drop them; appends keep a checkpoint reserve (one page per
non-empty dump, one for the restart record, one of margin) so a full
log can still be checkpointed — checkpoint appends are exempt. A
record is refused when it would overwrite the oldest live page; a
wrap around the end of the file is allowed when it stays inside the
free pages, which a checkpoint makes possible by freeing the early
ones. The wrap itself needs no `first_lsn` advance — the pages it
overwrites held only dispensable records. Crash contract: a crash
at any instant leaves the old checkpoint or the new one; a crash
mid-checkpoint leaves a partial checkpoint that is never referenced.
Tests: `test_writer_checkpoint_basic` (1 attr + name, dumps and
restart record walked from the checkpoint, pre-checkpoint records
now outside `[oldest, last]`), `test_writer_checkpoint_reopen`
(second session keeps the attribute), `test_writer_checkpoint_reserve`
(fill to `-ENOSPC`, checkpoint, fill again, checkpoint again).
Negative controls: stubbing `checkpoint()` fails all three tests;
dropping the table adoption fails the reopen test (attribute lost).
Bug fixed en route: `lw_pages_free()` returned 0 when the cursor
sat on the oldest page — every other page is free then, and the old
code refused the next append on a nearly empty log.
Phase 1, step 3 landed 2026-11-26 (commit, this round): wrap
read-back — `test_writer_wrap_readback` fills the log, checkpoints
near the end, fills again so the appends wrap onto the first pages
(sequence 2), reopens, and walks from the checkpoint: the restart
record and all 56 second-fill records come back in order,
byte-exact, and a walk from a pre-checkpoint LSN fails `-EINVAL`.
Bug found by this test, fixed here: the append layout loop's
"record continues" branch unconditionally cleared `PG_FLAGS` when
a record spilled to the next page, destroying the end marker of
an earlier record that ended on the same page — the reader checks
`PG_FLAGS`/`LAST_END_LSN` when it finishes a multi-page record,
so that record became unreadable. The branch now preserves the
END flag when a record has already ended on the page. The
roundtrip test never caught it: the only record ending on a page
shared with a continuing record there was single-page, and
single-page reads never consult the flag. Crash contract for a
wrap: only dispensable records are overwritten, and the
tail-copy-before-rewrite protocol still applies, so a crash
mid-wrap leaves the old page or its tail copy; a stale tail copy
cannot supersede the new page (older sequence, earlier end LSN).
Negative control: reverting the `PG_FLAGS` fix fails the new test
(walk `-EINVAL`, 1 record instead of 57).

Phase 1, step 4 landed 2026-11-26 (commit, this round): the
writer-owned clean close — `ntfs_log_writer_close()` emits a
final checkpoint, then marks the volume clean
(`RESTART_VOLUME_IS_CLEAN`) and frees the NTFS client in the
restart area, and syncs. Appends, checkpoints and open-attribute
registrations after the close return `-EINVAL`, as does a second
close; reopening the writer on the closed log starts a new
(dirty) session. Reader fixes this close exposed
(`core/logfile/logfile_open.c`): the open-time client lookup
walked only the in-use list, so a cleanly closed log's checkpoint
was invisible (`load_checkpoint` `-ENOENT`) — it now falls back
to the free list; and the oldest-LSN bound was the last record
when no client was in use, which put the checkpoint's own dump
records outside `[oldest, last]` — the NTFS client's own
`CR_OLDEST_LSN` now counts in the minimum. Crash contract: the
checkpoint lands first, so a crash before or during the close
leaves a dirty log whose latest checkpoint is durable; a crash
during the final sync leaves one restart page clean-closed and
one dirty, either a good state. Tests: `test_writer_clean_close`
(clean reopen with tables loading, post-close appends refused,
writer reopen goes dirty again). Negative controls: dropping the
reader free-list fallback fails the new test (4 checks); stubbing
`close()` fails it (8 checks).

Phase 1 is now complete: writer core, checkpoint emission, wrap
read-back, clean close. No integration with the mutation paths
yet — the driver still retires the journal on mount and writes
metadata unjournaled, so the crash story is unchanged until
Phase 2.

Phase 2, step 1 landed 2026-11-26 (commit, this round): the
transaction API — `ntfs_log_txn_begin/commit/abort` plus builders
`init_record` (redo Initialize / undo Deallocate),
`create_attr` (Create / Delete), `add_index_root` (Add / Delete
IndexEntryRoot), `update_resident` (UpdateResidentValue both
ways), `set_bitmap` (SetBits / ClearBits), in
`core/logfile/logfile_write.c` + `core/logfile/ntfs_logfile.h`.
Records are staged in memory (length-prefixed) and appended at
commit in order with the undo chain (`LR_CLIENT_UNDO_NEXT_LSN`
= previous record's LSN), then a CommitTransaction record, then
a checkpoint (per-commit policy for Phase 2 — replay needs a
checkpoint to see the transaction; Phase 4 may relax this to
periodic), then a ForgetTransaction record with the in-memory
entry freed. Abort writes nothing. The commit also populates
the dirty-page table (target attribute, page-aligned VCN,
`page_lcns`, oldest LSN = first touching record's LSN) and the
open attributes are registered through the existing
`ntfs_log_writer_open_attr` (`$MFT:$DATA` → 0x18,
`$MFT:$BITMAP` → 0x40). Crash contract, per commit: crash
before the first data record → transaction absent, volume
unchanged; crash mid-append → the durable prefix is an ACTIVE
transaction and the next replay undoes it (redo-then-undo: the
redo pass applies the prefix first, so an undo always sees the
post-redo state); crash after the Commit record but before the
checkpoint → transaction committed, redo applies it; crash
after the checkpoint → same, via the checkpoint's tables. A
torn spanning record at the tail is simply absent (its tail
never landed), so the transaction never happened — also a good
state. A log-write failure fails the writer (volume-error
semantics) and the transaction stays ACTIVE when any record
was appended.
Bug fixed en route (found by the new commit test, Phase 1
checkpoint code): `lw_checkpoint_inner()` advanced the restart
area's oldest LSN to the first dump's LSN, but the checkpoint's
own dirty-page table still referenced the pre-checkpoint data
records, so replay's redo pass could not read them
(`lfs_lsn_in_file` refused) and the whole log failed `-EINVAL`.
Oldest is now the minimum of the first dump, every transaction
table entry's first LSN (any state — a committed transaction's
records are still needed for redo until its pages flush), and
every dirty-page entry's oldest LSN. With no live transactions
or dirty pages the old behaviour is unchanged (pre-checkpoint
records stay outside `[oldest, last]`, as
`test_writer_checkpoint_basic` pins). Three facts the tests
established about record construction: a CreateAttribute
inserts at the END marker (`init_len - 8`), not after it
(`check_if_attr` requires an attribute boundary); the
Initialize redo image carries `MFT_RECORD_IN_USE` from the
start (the replay's `check_file_record` requires it for the
next record; Deallocate clears it on undo); and the SetBits
op re-applies on a second replay (clusters carry no LSN to
skip on — setting an already-set bit is a no-op in effect),
so the second replay redoes 1 record and writes 1 cluster
while the volume is unchanged.
Tests: `test_journal_create_commit` (full 7-record create
transaction: walk shows 7 data + Commit + 3 dumps + Forget in
order; dry-run redoes exactly 7 with 1 committed; real run
writes 2 MFT records + 1 bitmap cluster; record 40 and record
32 byte-exact including the `MR_LSN` the replay stamps from
the last touching record; bitmap bit 40 set and nothing else),
`test_journal_create_abort` (nothing written, volume
byte-identical), `test_journal_create_uncommitted`
(fault-injected mid-commit: the budget is found adaptively,
1–16 writes, because per-append cost depends on tail copies
and page spanning — the first budget leaving ≥1 record durable
is used, so the test can never pass vacuously; here budget 3
lands exactly 1 record, the spanning Initialize, and replay
redoes 7 + 1 then undoes exactly that 1, first file present,
second file rolled back, bit 41 clear),
`test_journal_create_bad_vcn` (negative control: Initialize
naming the wrong `$MFT` vcn commits fine but replay refuses
the whole log, H1, and writes nothing). Logfile suite now
4900 checks, 0 failures.
Phase 2, step 2 landed 2026-11-27 (commit, this round): the
create path is journaled end to end. New glue
`core/vfs/journal.c` + `core/vfs/journal.h` stages one create
transaction — `ntfs_journal_create_begin` (regular files only,
writer present; a volume-level `log_txn_lock` serialises
transactions and an atomic `log_txn_open` counter drives the
WAL gate), `create_init` (copies the record right after the
`$SI` add, validates the first attribute is `$SI` and the
second is `AT_END`), `create_attr` (captures each attribute
at its own insertion offset — attributes are type-sorted, not
appended — and sets `RESIDENT_ATTR_IS_INDEXED` on the log
copy of the `$FILE_NAME` only, never on the live record, so
replay's CreateAttribute bumps the link count), `create_ea`
(net effect: one CreateAttribute each for `$EA_INFORMATION`
and `$EA` with their final values after all three `set_ea`
calls, not the intermediate updates), `create_index` (scans
the parent's `$INDEX_ROOT` `$I30` for the child's mref;
`aoff` is the entry offset from the attribute start;
`index_length` is measured from the **index header** start,
not from `entries_offset` — a bug in the first version
overshot the value end and failed the create), `create_bitmap`
(the `$MFT:$BITMAP` bit), and `create_commit` (commit, then
stamp the transaction's last LSN — the Commit record's LSN —
into both touched MFT records' `MR_LSN`, so every record of
the transaction skips on replay). `__ntfs_create`
(`core/vfs/namei.c`) begins the transaction before the
parent-mtime dirty and before `ntfs_mft_record_alloc`,
stages after each mutation, and commits after
`ntfs_index_add_filename`; `err_out` aborts first, before
the rollback writes (the gate must be down before they run).
A stage failure of `-ENOENT` means the sub-mutation cannot be
described in the log — the attribute went to an extent, the
index entry went to an `$INDEX_ALLOCATION` block — and is
**not** a create failure: `journal_stage` aborts the
transaction and the create proceeds unjournaled, exactly as
the driver did before Phase 2. The mkntfs/Windows volume root
already has an `$INDEX_ALLOCATION` block and its END entry
points down at it, so a create in the root is always
unjournaled on such images; a create in a driver-made
subdirectory (fresh `$INDEX_ROOT`, no allocation block) is
journaled. Any other stage error fails the create, as before.
The WAL gate (corrected from the plan: `writeback_one` is
**not** the single choke point — `ntfs_mft_writepages` calls
`ntfs_write_mft_block` directly and `__ntfs_write_inode`
writes the MFT record straight to the device via bio, so the
gate sits at three sites: `ntfs_write_mft_block` (defer by
`folio_redirty_for_writepage` + unlock + return 0),
`ntfs_write_folio` in `core/vfs/aops.c` (only the
`$MFT:$BITMAP` mapping; resident-attr folios only copy bytes
into the in-memory MFT record), and `__ntfs_write_inode`
(return early, leaving `NInoDirty` set); nothing goes in the
filesystem-agnostic `pagecache.c`) closes two windows the
locking alone does not: the bare-record window between
`ntfs_mft_record_alloc` and `insert_inode_hash` (the inode
is not in icache yet, so `ntfs_may_write_mft_record` returns
true and the folio path would flush the fresh bare record
with no log coverage) and the dir-mtime window between
`mark_inode_dirty(dir)` and taking `dir_ni->mrec_lock`.
Mount lifecycle: `ntfs_glue_make_rw` opens the writer via
`ntfs_log_writer_open_device` (open image → log io/geometry
→ `ntfs_logfile_open`; on failure format a v1.1 log and retry;
version check v1.1 only), registers `$MFT:$DATA` then
`$MFT:$BITMAP` with the logfile module's plain-u32 constants
(`ATTR_TYPE_DATA` 0x80 / `ATTR_TYPE_BITMAP` 0xB0 — the replay
compares the table entry against them, not the driver's
`__le32` `AT_DATA`/`AT_BITMAP`), and falls back to retiring
the journal with a logged warning on any refusal; `ntfs_put_super`
closes the writer after the first inode-commit block (all
metadata the log describes is durable before the log is
marked clean — `ntfs_commit_inode` is synchronous — and a
close failure sets the volume-error flag so the flag clear is
skipped and the next mount replays). The parent's on-disk
`$SI` mtime update happens at dir-inode writeback and is
**not** journaled in Phase 2 — it belongs to the setattr
class in Phase 3 (recorded gap). `$MFT` extension during a
create is unjournaled but self-consistent (growing `$MFT`
with empty records is valid; the gate keeps it off disk
before commit). Commit-failure semantics: a mid-append
failure leaves an ACTIVE transaction in the log and the
error path rolls the in-memory mutations back; the next
replay does redo-then-undo, so the undo sees the post-redo
state, not the rolled-back disk state — consistent, and no
`MR_LSN` stamp happened (the stamp is after a successful
commit), so the redo is not skipped. Crash contract, per
commit: crash before the first data record → transaction
absent, volume unchanged; crash mid-append → durable prefix
is ACTIVE, next replay undoes it; crash after the Commit
record but before the checkpoint → committed, redo applies
it; crash after the checkpoint → same, via the checkpoint's
tables; a torn spanning record at the tail is simply absent
— the transaction never happened; a log-write failure fails
the writer (volume-error semantics), the transaction stays
ACTIVE, and the rollback path runs with the gate down. Bugs
fixed en route: a fresh mkntfs `$LogFile` is all `0xFF`
(`LFS_MAGIC_EMPTY`); `ntfs_logfile_open` returns 0 with state
`NTFS_LOG_EMPTY` and version 0.0, so the ladder's version
check refused with `-ENOTSUP` before the writer-open
`-ENOENT` format branch could run and the mount fell back to
unjournaled — the ladder now formats an empty log before the
version check (a zeroed all-0x00 log is still refused as
present-but-unparseable, by design); and the `index_length`
semantics above. The test walks the log from the oldest LSN,
not 0 — 0 starts at the current checkpoint, which the commit's
own checkpoint moved past the data records, so the walk saw
only the dumps and the Forget.
Tests: new `core/tests/test_journal.c` (84 checks, 0
failures) — `test_create_journaled` (create `sub/j.txt` in a
driver-made directory: walk shows exactly 15 records — 2
open, 8 data in order, Commit, 3 dumps, Forget; `$FILE_NAME` create
carries "j.txt" in UTF-16LE; SetBits bit is the file's MFT
number, count 1; both touched records carry an `MR_LSN` at
or past their touching record; replay redoes exactly 1
record — the SetBits, which carries no LSN to skip on — and
writes 0 MFT records and 1 cluster, twice identically; the
file is present after replay + mount; `ntfsck -n` clean),
`test_mkdir_unjournaled` (no client records, directory
present, clean), `test_create_in_root_unjournaled` (create
in the root succeeds with no client records — pins the
`-ENOENT` fallback — file present after remount, clean).
The journaled test fails without the wiring (forced
unjournaled: walk 0 records, `MR_LSN` 0, replay redoes 0 —
10 failures), so it is live, not vacuous. Logfile suite
stays 4900 checks, 0 failures.
Phase 2, step 3 (crash tests, in progress 2026-11-27): new
`test_journaled_create_cut_points` in `core/tests/test_crash.c`
sweeps 17 sampled cut points through one journaled create
(`d/j.txt`, `d` a driver-made subdirectory — root creates are
unjournaled by design): per cut it records the pre-replay
`ntfsck` verdict, replays via `ntfs_logfile_replay_device()`,
then checks post-replay `ntfsck`, remount lookup of the file,
and that a second replay is a no-op. The first sweep found
three real bugs (24 failures). Fix 1, landed 2026-11-27
(commit, this round): the writer now emits one
`OpenNonresidentAttribute` record per distinct attribute the
transaction touches, before its data records
(`txn_append_open_records` in `core/logfile/logfile_write.c`,
appended directly — not via `txn_append_one` — so it touches
no dirty page, but it does set the transaction's
`first_lsn`). Replay's analysis learns the open-attribute
table from these records (LOGFILE.md §3 step 4); without
them the first create after a mount was unreplayable when
its commit checkpoint never landed — the two attributes were
opened at writer-open (new slots `0x68`/`0x90`, the
checkpoint's `0x18`/`0x40` are stale duplicates from the
previous session) and only the commit's checkpoint would
have registered them, so analysis failed "open attribute
entry `0x68` not allocated" and refused the volume. The redo
payload is the writer's open-attribute table entry
(`OA1_SIZE` bytes; the reader copies bytes 4..`OA1_SIZE`),
the undo payload is the attribute name if it has one, and
the `OA1_OPEN_RECORD_LSN` field keeps its open-time value
(informational only — replay never decides on it — and the
checkpoint dump carries the same value, so the two paths
agree). A journaled create is now 15 records (was 13), a
synthetic create 14 (was 12); the redo pass skips the open
records (`lcns == 0`), so `records_redone` counts data
records only. New unit test
`test_journal_create_first_txn_uncommitted` (logfile suite
4930 checks, 0 failures): a first transaction faulted
mid-commit before any checkpoint, over a checkpoint that
predates the attributes — without the fix replay fails
`-EINVAL`, with it replay redoes the landed records and
undoes exactly those and the file never appears (negative
control: disabling the emission fails the new test with
`-EINVAL`, plus the count assertions that depend on the open
records). Crash sweep after fix 1: 379 checks, 8 failures —
cuts 22/25 now replay (were refused) but still fail `ntfsck`
with "Failed to open inode(65)", which is fix 2. Fix 2, landed 2026-11-27 (commit, this round): the create
transaction now journals the `$MFT` `$DATA` size extension as a
`SetNewAttributeSizes` record, staged first so the redo extends
`$MFT` before it initialises the new record, plus an
`UpdateMappingPairs` record when the runs change (once per 16
records of growth) and the same sizes record for `$MFT:$BITMAP`
when its sizes grow (every 8 records). New builders
`ntfs_log_txn_set_attr_sizes` /
`ntfs_log_txn_update_mapping_pairs` in
`core/logfile/logfile_write.c`; `ntfs_journal_create_begin`
snapshots record 0's `$DATA` and `$BITMAP` before the allocation
and `ntfs_journal_create_mft_sizes` stages after it
(`core/vfs/journal.c`, `core/vfs/namei.c`); a snapshot failure or
an unreadable/moved attribute aborts to unjournaled, never fails
the create. The sizes record is idempotent (absolute sizes) and
the replay apply stamps record 0's `MR_LSN` itself, so record 0
needs no commit-time stamp. The sweep exposed a second bug in
the same work: for a short-form attribute (the `$MFT` `$DATA` is
one — pairs at 0x40, length exactly 0x48) offset 0x40 IS the
mapping pairs, not a `compressed_size` field, so logging the
pairs bytes as the record's total and writing them back on
replay clobbers the pairs whenever they change. Fixed in three
places — the snapshot and the staging comparison in
`core/vfs/journal.c` now read the total from `compressed_size`
only when the pairs start after it (`pairs_off >= 0x48`, else 0),
and the replay apply in `core/logfile/logfile_replay.c` writes
`NAS_TOTAL` only on the same condition (the old length-only
check was true for every short-form attribute). Without the
snapshot side the staging comparison disagreed with the snapshot
and staged a spurious `$BITMAP` sizes record on every create. A
journaled create is now 16 records (was 15). New unit test
`test_journal_create_mft_sizes` (logfile suite 4977 checks, 0
failures): pairs + sizes replay to a grown `$DATA` with the pairs
intact, second replay redoes nothing; negative control (disabling
the replay fix) fails the pairs check and the second replay then
fails `-EINVAL` on the undecodable pairs. Driver test
`test_journal.c` updated to 16 records with sizes assertions (95
checks, 0 failures), including that the logged total is 0, not
the pairs bytes. Crash sweep after fix 2: 379 checks, 2 failures
— only cut 46 remains (see below). Fix 3, landed 2026-11-27 (commit, this round): the analysis pass
skips transaction-table upkeep for table dumps. The dumps carry the
last transaction's tid by construction (`lw_dump_table` appends with
0x18) but they are snapshots of the tables, not transaction
activity; a dump that lands after its transaction was forgotten —
the close checkpoint's dumps, when the crash cut the close before
its restart record — re-allocated the freed entry as ACTIVE and
reported a phantom active transaction with nothing to undo
(`transactions_active=1`, `records_undone=0` at cuts 43–49). New
helper `op_is_table_dump` in `core/logfile/logfile_replay.c`; the
upkeep block is skipped for the four dump ops (the dumps hit the
switch's default case and are handled by the checkpoint-load path,
so they need no upkeep). New unit test
`test_journal_create_close_dumps_no_phantom` (logfile suite 4989
checks, 0 failures): a dump appended after the Forget with the
transaction's tid, no checkpoint after it — the crash-mid-close
state — replays with `transactions_active=0`, `records_undone=0`;
negative control (guard forced true) fails with
`transactions_active=1`. The crash sweep's cuts 43–49 now report
`active 0`. Fix 4, landed 2026-11-27 (commit, this round): the replay's flush
now heals `$MFTMirr` for records 0–3 it did not itself touch. A crash
between a record 0–3 `$MFT` writeback and its mirror bio leaves the
mirror stale by that write — the unmount's dirty-flag clear in
record 3 is the routine case — and no log record ever touches
record 3, so the redo/undo passes never put it in the overlay and
the replay never healed it; `ntfsck` then reports
"`$MFT`/`$MFTMirr` records do not match" (cut 46: one flag byte in
record 3). After the overlay flush, `flush_overlay` in
`core/logfile/logfile_replay.c` re-checks every record 0–3: it
reads the raw on-disk `$MFT` bytes, validates them on a deprotected
copy (`check_file_record`), and copies them into the mirror cluster
when they differ (new counter `mirror_records_healed` in
`struct ntfs_log_replay_result`). The comparison and the copy use
the raw bytes deliberately: protecting a copy bumps the USN
(`lfs_fixup_pre_write` increments it on every call), so a freshly
protected record never matches the bytes on disk and the "healed"
mirror would then mismatch `$MFT` — the first version of this fix
did exactly that (driver journal suite 3 failures, `ntfsck` exit
4). A record that fails validation is left alone (the mirror stays
the better copy; chkdsk owns it) and a mirror write failure is
counted, not returned (the volume is consistent, only its backup
is stale). The crash sweep is now fully green: 379 checks, 0
failures — 19 cut points, 2 inconsistent before replay, 0 still bad
after it. Step 3 landed 2026-11-27 (commit, this round): the crash test
itself — `test_journaled_create_cut_points` in
`core/tests/test_crash.c`, 379 checks, 0 failures. It sweeps 19
sampled cut points through one journaled create (`d/j.txt`): per
cut pre-replay `ntfsck` + allocation coverage, replay via
`ntfs_logfile_replay_device()`, post-replay `ntfsck` + coverage +
remount lookup of the file + second replay no-op. The sweep is
green end to end: 2 cuts inconsistent before replay, 0 still bad
after it; `j.txt` present 11 times, absent 8 times. The four bugs
it found are fixed in the four commits above (open records,
`$MFT` size extension + the short-form `compressed_size` overlap,
phantom active from table dumps, `$MFTMirr` heal). Phase 2 step 3
is complete; a journaled create is 16 records (2 open + 9 data +
Commit + 3 dumps + Forget).
Phase 3, unlink — builders landed 2026-11-27 (commit, this
round): the two transaction builders the unlink class needs, in
`core/logfile/logfile_write.c` with declarations in
`core/logfile/ntfs_logfile.h`. `ntfs_log_txn_delete_attr`
stages a `LOP_DeleteAttribute` redo (empty payload — the
replay removes the attribute at the recorded offset) with a
`LOP_CreateAttribute` undo carrying the attribute bytes, and
`ntfs_log_txn_delete_index_root` stages a
`LOP_DeleteIndexEntryRoot` redo (empty) with a
`LOP_AddIndexEntryRoot` undo carrying the entry bytes.
Deleting an attribute decrements the link count itself in the
replay — the $FILE_NAME's `RESIDENT_ATTR_IS_INDEXED` flag keys
the bump, and the live record carries it (attrib.c sets it for
every `AT_FILE_NAME` it adds), so no separate record carries
the count. New unit tests `test_journal_unlink_commit`,
`test_journal_unlink_abort`, `test_journal_unlink_bad_offset`
(logfile suite 5084 checks, 0 failures): the commit test walks
the log and checks the record sequence and the undo bytes; the
abort test checks the undo restores the record byte-identical;
the bad-offset test checks staging rejects an out-of-range
attribute offset. Negative control (delete_attr stubbed to
stage nothing): 8 failures — the tests are live, not vacuous.
Two logfile-suite learnings worth pinning: walking from an
older transaction's first LSN fails `-EINVAL` once a later
transaction's checkpoint lands (oldest advances past it), and
`txn_append_open_records` emits one open record per
transaction, not per writer lifetime (an unlink transaction is
1 open + 2 data + Commit + 3 dumps + Forget = 8 records).
Driver glue landed 2026-11-27 (commit, this round):
`ntfs_journal_unlink_begin/abort/commit/check/stage_index/
stage_attr` in `core/vfs/journal.c` (+ API in
`core/vfs/journal.h`), wired into `ntfs_vfs_unlink`
(`core/vfs/namei.c`, which gains a `jtp` parameter; the three
rename call sites pass NULL). The begin runs before the mrec
locks (lock order `log_txn_lock` → `mrec_lock`, as in
`__ntfs_create`); the staging calls sit before
`ntfs_index_remove` and `ntfs_attr_record_rm`, so the logged
bytes are the bytes the driver actually removes. On commit
failure the caller sets `NVolSetErrors` and returns the error —
the next mount's replay undoes the partial transaction and
restores the file, the correct outcome for a failed unlink.
New driver tests `test_unlink_journaled`,
`test_unlink_in_root_unjournaled`, `test_rmdir_journaled`
(`core/tests/test_journal.c`, suite 171 checks, 0 failures at
that commit; 191 checks, 0 failures after the free-in-unlink
fix below): the journaled unlink writes exactly one
transaction — 8 records before the free-in-unlink fix, 11
after (2 open + 4 data + Commit + 3 dumps + Forget); the walk
from the oldest LSN holds 22 records before the fix and 25
after — the create's 14 still-live ones (its 2 open records
are already dispensable) plus the unlink's — because the MFT
pages the create dirtied were never flushed and no checkpoint
is emitted after a sync, so the dirty-page table still names
the create's LSNs as the oldest for those pages (correct NTFS
behaviour: a crash before those pages flush needs the create's
redo records). The unlink's data records are skipped on replay
by their `MR_LSN` stamps (the Deallocate by its effect, see
below); the create's two idempotent records are redone (sizes
+ SetBits), plus the unlink's ClearBits after the fix. Two bugs found by the tests and fixed
in this commit: `stage_attr` tested `actx->mrec !=
actx->base_mrec`, but the search context only fills
`base_mrec` on the external (attribute-list) path, so it
rejected every attribute of a plain file — the test is now
`actx->ntfs_ino == ni` (the base inode while the search stays
in the base record); and the `journal.c` comments claiming the
live $FILE_NAME carries no `INDEXED` flag were stale —
attrib.c sets `RESIDENT_ATTR_IS_INDEXED` for every
`AT_FILE_NAME` it adds, so the copy's explicit flag set is a
no-op on live-created names. The root-unlink and root-rmdir
cases stay unjournaled by design (their entries live in the
root's `$INDEX_ALLOCATION` blocks) and still succeed.
Free-in-unlink-txn (evict-deallocation gap, closed, commit this
round): the unlink transaction now stages the record's
deallocation itself — Deallocate and ClearBits records in the
unlink commit — so a crash after unlink-commit but before
evict still leaves a good state: the record is freed by replay.
New builder `ntfs_log_txn_deallocate_record` (redo
LOP_DeallocateFileRecordSegment, undo
LOP_InitializeFileRecordSegment carrying the record bytes after
the $FILE_NAME removal, so it composes with the DeleteAttribute
undo instead of duplicating the $FILE_NAME), staging helper
`ntfs_journal_unlink_stage_free`, called from `ntfs_delete` for
the base record and each extent record when `link_count`
reaches 0, gated on a journaled unlink (rename passes NULL, so
it never stages it). The physical free still runs at evict —
the log describes the intended end state, applied by replay
only after a crash, when the file is no longer open anywhere,
so staging at unlink time is safe for unlink-while-open. The
data clusters' free stays unjournaled (the truncate/extend
class). The replay's LOP_DeallocateFileRecordSegment skip is now
effect-based, not LSN-based: skip only when the record's
IN_USE flag is already clear. An LSN skip is wrong because the
commit stamps MR_LSN = last_lsn, which is past the Deallocate's
LSN, but the flag clear happens at evict after the commit — a
record flushed after the commit carries an MR_LSN claiming the
effect while the flag is still set, and an LSN skip would leave
the flag set and ntfsck would fail the record for having no
$FILE_NAME. A second replay is still a no-op: the first one
cleared the flag. The unlink transaction is now 11 records (2
open — $MFT:$DATA 0x18 and $MFT:$BITMAP 0x40 — + 4 data +
Commit + 3 dumps + Forget); the walk from oldest holds 25
records (14 still-live create records + 11 unlink); replay
redoes 3 (create sizes + create SetBits + unlink ClearBits; the
Deallocate is skipped on its effect). Driver test updated to
the new shape (test_journal 191 checks, 0 failures); logfile
test `stage_unlink` now stages the free records too
(test_journal_unlink_commit expects the 11-record walk, the
free record with no $FILE_NAME, and the clear bitmap bit).
Crash test landed (commit, this round):
`test_journaled_unlink_cut_points` (`core/tests/test_crash.c`,
modelled on `test_journaled_create_cut_points`): a base image
with `d/j.txt`, a write-budget cut sweep over the journaled
unlink, per cut pre-replay ntfsck + coverage, replay,
post-replay ntfsck + coverage + remount lookup + second replay
no-op. 22 cut points: 3 inconsistent before replay, 0 still
bad after it; j.txt present 6 times (crash before the unlink
commit — replay undoes it), absent 16 times (crash after —
replay redoes it). Presence is counted, never asserted: either
outcome is correct depending on where the crash landed. Whole
test_crash binary: 536 checks, 0 failures. The sweep is what
drove the free-in-unlink fix above: before the fix it was 16
bad (resurrected bitmap bit) and then 7 bad (link-count-0
record with no $FILE_NAME).
Phase 3, rename — glue landed 2026-11-27 (commit, this round):
one transaction per `__ntfs_vfs_rename` call, in live order —
the target's removal (when renaming over an existing file, the
unlink records including its record free, staged by
`ntfs_delete` itself), then the link
(`ntfs_journal_rename_stage_link`: a CreateAttribute for the
new $FILE_NAME plus an AddIndexEntryRoot for the new entry),
then the old name's removal (the unlink records again). The
transaction reuses the unlink carrier
(`ntfs_journal_unlink_begin`/`abort`); only the link staging
and the commit are rename-specific
(`ntfs_journal_rename_commit` stamps MR_LSN into the moved
record, both parents and the replaced target). The link staging
matches by name AND parent — a cross-directory rename to the
same name briefly carries two $FILE_NAMEs with the same name
and different parents, and the first version matched by name
alone, staged the old $FILE_NAME, failed the index lookup and
fell back to unjournaled (caught by the cross-dir driver test:
walk 16, MR_LSN of the new parent 0). The replay's
Add/DeleteIndexEntryRoot apply at the staged offset, and
staging order is live order, so a same-directory rename
replays to the exact live layout. A case-only rename goes
through a temporary name in two calls, so it writes two
transactions — a crash between them leaves the file under the
temporary name, consistent and ntfsck-clean, the same outcome
the unjournaled driver has always had there. The $SI
mtime/ctime updates the rename makes are not journaled (the
setattr class's work); the staged $FILE_NAME carries the
pre-rename timestamps, matching the live $FILE_NAME bytes.
Driver tests (`core/tests/test_journal.c`, suite 358 checks, 0
failures): `test_rename_journaled` (one 10-record transaction,
walk 24, replay redoes only the create's 2 idempotent records),
`test_rename_in_root_unjournaled` (no client records),
`test_rename_over_journaled` (15-record transaction, walk 45 —
the second create's opens are still live, the first's precede
oldest; replay redoes 5), `test_rename_cross_dir_journaled`
(same four data records split across two parents, walk 24).
Negative control (link staging stubbed to -ENOENT): 6 failures
across the three journaled tests, including an ntfsck exit 4
on the rename-over replay — the target's freed record bit
resurrected by the create's SetBits redo, the same failure mode
the unlink's free-in-transaction fix closed.
Crash tests (`core/tests/test_crash.c`, this round, landed as
their own commit): `test_journaled_rename_cut_points`
(same-directory rename, 42 cut points sampled at every write —
the 21-sample sweep missed the torn window entirely, 0
pre-bad; 3 inconsistent before replay, 0 still bad after it;
j.txt present 10 times, k.txt 32) and
`test_journaled_rename_over_cut_points` (54 cut points, 6
inconsistent before replay, 0 still bad after it; j.txt
present 21 times, k.txt present all 54 — the target name must
always survive, and both names present is the correct undo
outcome). Whole test_crash binary: 1406 checks, 0 failures.
Phase 3, mkdir — glue landed 2026-11-27 (commit, this round):
two changes, both in the create path, because a mkdir IS a
create with an $INDEX_ROOT instead of a $DATA attribute. First,
`ntfs_journal_create_begin` now begins for S_ISDIR as well as
S_ISREG (symlinks, devices and other non-regular, non-directory
files still never begin — their reparse staging belongs to their
own classes). Second, `__ntfs_create` stages the $INDEX_ROOT
attribute add (`ntfs_journal_create_attr` with AT_INDEX_ROOT,
I30, 4) — staged after the inherited-compression flag set, so
the logged bytes are the final ones; staging before it would
replay an $INDEX_ROOT without the COMPRESSED flag the live
record carries. Everything else was already staged for
directories: the $MFT sizes, the Initialize record, $SECURITY_
DESCRIPTOR, $EA_INFORMATION + $EA, $FILE_NAME, the index entry
and the bitmap bit. A journaled mkdir is the same 16 records as
a journaled create (2 open + 9 data + Commit + 3 dumps +
Forget), the five CreateAttributes being $SECURITY_DESCRIPTOR,
$INDEX_ROOT, $EA_INFORMATION, $EA and $FILE_NAME — the
$INDEX_ROOT sitting exactly where the file's $DATA sits. A mkdir
in the root still falls back to unjournaled, as before: the
root's entry lands in its $INDEX_ALLOCATION block, which the
journal cannot describe.
Driver tests (`core/tests/test_journal.c`, suite 442 checks, 0
failures): new `test_mkdir_journaled` (walk 16, same op sequence
as a create; replay redoes only the create's 2 idempotent
records; after replay a file can be created inside the replayed
directory — that is what proves the replayed record carries a
usable $INDEX_ROOT, not just the name). The $FILE_NAME name scan
in the walk harness is now a settable pattern (`set_fn_name`,
default "j.txt", reset after each walk) so a test that creates
another name can scan for it — the mkdir test scans for "jdir".
`test_rmdir_journaled` updated for the new world: its setup
mkdir of sub/sub2 now journals, so the walk holds the mkdir's 14
still-live records plus the rmdir's 11 (25), and replay redoes 3
(the mkdir's sizes + SetBits and the rmdir's ClearBits).
Negative control ($INDEX_ROOT staging stubbed to -ENOENT): 18
failures across the mkdir and rmdir tests, and the mkdirs
themselves still succeed — the abort-to-unjournaled path works
as designed. (A first stub that replaced the whole
`journal_stage` call instead made every mkdir fail with
-ENOENT — the abort-and-swallow lives in `journal_stage`
itself, so a control must stub the staging call inside it.)
Crash tests (`core/tests/test_crash.c`, this round, landed as
their own commit): `test_journaled_mkdir_cut_points`
(mkdir d/e, 56 cut points sampled at every write, 6
inconsistent before replay, 0 still bad after it; d/e present
34 times, absent 22). Whole test_crash binary: 1808 checks, 0
failures.
Phase 3, truncate/extend — glue landed 2026-11-27 (commit, this
round): explicit truncate via setattr only (`ntfs_truncate_vfs`,
sole caller `ntfs_setattr_size`), non-resident $DATA of a regular
file only. The begin snapshots the $DATA sizes, mapping pairs and
run list before the resize; the stage runs after the resize and
stages the $Bitmap delta first (set difference of old vs new
run lists, split at bitmap-page boundaries), then
UpdateMappingPairs, then SetNewAttributeSizes; the commit stamps
MR_LSN into the file's record. A staging failure aborts and the
resize stands unjournaled; a commit failure marks the volume, and
the next mount's replay undoes the partial transaction.
Two things the design did not foresee. First, an extend takes the
hole path (`HOLES_OK`): it allocates nothing, appends a hole run
and flips the file sparse — which inserts the 8-byte
compressed_size field and moves name_offset and
mapping_pairs_offset 64 -> 72. The pairs record therefore carries
the attribute header it depends on: both payloads are prefixed
with an 8-byte header (flags, name_off, pairs_off), the replay
applies the header before writing the pairs, and the undo payload
is the only copy of the old header (landed as its own commit
first, with a logfile-level redo/undo test). A journaled shrink
is 10 records (2 open + ClearBits + pairs + sizes + Commit + 3
dumps + Forget); a journaled extend is 8 (1 open + pairs + sizes
+ Commit + 3 dumps + Forget — no $Bitmap record, so $BITMAP
never opens). fallocate and extending writes during writeback are not
in this class.
Crash-test fix (2026-11-27, this round): the truncate did not
journal the parent's $FILE_NAME. The parent directory's $FILE_NAME
(in sub/d's $INDEX_ROOT) still carried the old sizes, because the
live driver refreshes it via ntfs_inode_sync_filename at parent
writeback — which the torn sync can cut. The WAL gate defers the
parent's record writeback while the transaction is open; the
commit lands, then the parent's writeback proceeds with the new
$FILE_NAME — a crash between the commit and the parent's
writeback leaves file = new size (committed, replayed) but parent
= old size (stale), and ntfsck reports "Allocated size is
different with IDX/$FN(262144), MFT/$DATA(65536)". A crash before
the commit is consistent (parent still old, undo restores file to
old). The fix stages an UpdateFileNameRoot record (new builder
ntfs_log_txn_update_filename_root, op 0x13, FN_DUP_SIZE bytes
each way; the replay already handled the op) for each resident
$FILE_NAME in the file's record, addressed through $MFT:$DATA
with the parent's mft_coords — no new open attribute needed.
Two subtleties the first version got wrong. First, the entry's
dup bytes cannot be the old payload: the parent's record in
memory lags behind the log (only refreshed at parent writeback),
so the entry still holds the values from before the previous
transaction — using them as the undo payload would make an undo
restore the pre-previous state (a committed shrink followed by a
loser's extend leaves the entry at the pre-shrink size). The old
dup takes its sizes/flags from the begin snapshot (fn_alloc_old,
snap.data, si_flags_old); only the times come from the entry.
Second, for a sparse file the parent's $FILE_NAME alloc must be
ni->itype.compressed.size (the real clusters, 64K), not the
$DATA's allocated_size (which counts the hole, 256K) — the same
rule ntfs_inode_sync_filename applies. The record is staged only
when the new dup differs from the old (a no-op truncate stages
nothing); any staging failure aborts the whole transaction to
unjournaled. The fix also changed find_index_entry_by_key to
match by NAME (bytes + type), not the whole $FILE_NAME value:
the file's record and the index entry legitimately disagree in
exactly one byte — value offset 60 (0x3c), the ea_size/reparse
union, which the create writes as the packed EA size (45, the
packed size of $LXUID/$LXGID/$LXMOD) while the index copy holds
the real reparse tag (0x00). The name is what the $I30 index is
ordered by; the rest is payload. This affects the unlink and
rename callers too, but they pass the $FILE_NAME from the VFS
lookup (the entry's own key), so they match the same entry as
before.
Crash-test fix (2026-11-27, this round): the truncate did not
journal the file's $STANDARD_INFORMATION flags. The live driver
sets BOTH the $DATA flag and the $SI flags when a resize flips
the file sparse (`core/ntfs/attrib.c`: `NInoSetSparse(ni);
ni->flags |= FILE_ATTR_SPARSE_FILE; a->flags |= ATTR_IS_SPARSE;`
— and the reverse clears both), but the transaction staged only
the $DATA flag (via the pairs header). The $SI record copy is
written at inode sync AFTER the commit, under the WAL gate; a
crash after the commit but before that writeback flushes leaves
$DATA sparse + $SI non-sparse, and libntfs-3g refuses the
attribute: "Inode 65 has corrupt attribute flags (0x8000 <>
0x0)". The check is one-directional (a file may be sparse
though its unnamed data is not), and the replayed record's
bytes are otherwise identical to the live driver's. The fix
snapshots the $SI flags at begin (from the mapped record, via
find_attr; a missing or too-short $SI aborts the transaction)
and stages an UpdateResidentValue record for the 4-byte
file_attributes field when le32_to_cpu(ni->flags) differs from
the snapshot, re-finding the $SI in the mapped record at stage
and verifying it still sits where begin found it. A shrink of a
sparse file flips the bit the other way — same fix, same record.
Crash-test fix (2026-11-27, this round): the truncate's $Bitmap
delta was staged against the wrong bitmap. `stage_bitmap_range`
used `vol->log_mft_bitmap_attr` ($MFT:$BITMAP, the MFT-record
allocation bitmap, cluster LCN 2) — correct for the create's
SetBits (bit 65 = MFT record 65) and the unlink's ClearBits
(MFT-record bits), wrong for cluster bits. The volume $Bitmap
had no open attribute at all. The replayed shrink image showed
it: the freed 48 clusters stayed marked in the volume $Bitmap
(record 6, LCN 519) while the ClearBits redo changed nothing on
disk — the $Bitmap was byte-identical between the crashed and
replayed images. The driver test had encoded the bug (it only
asserted the target attribute was 0x40) and passed anyway,
because on a cleanly-synced volume the live sync had already
cleared the real bitmap and the wrong-cluster writes were no-ops.
The fix registers a third open attribute, `log_lcn_bitmap_attr`
($Bitmap:$DATA, record 6), in `ntfs_glue_make_rw`, and
`stage_bitmap_range` now uses it with a new `cluster_bitmap_coords`
(`NTFS_I(vol->lcnbmp_ino)`). If record 6 cannot be opened the
journal retires and the resize proceeds unjournaled. The driver
test now asserts the ClearBits targets the attribute the truncate
opened first and NOT the create's SetBits attribute, and the
replayed shrink writes two clusters (the $MFT:$BITMAP cluster
for the create's SetBits and the volume $Bitmap's cluster for the
truncate's ClearBits), not one.
Driver tests (`core/tests/test_journal.c`, suite 563 checks, 0
failures): `test_truncate_shrink_journaled` (walk 25: create 14
+ shrink 11 — the shrink gains a $FILE_NAME record for the
parent's $FILE_NAME; replay redoes 4 — the create's sizes +
SetBits, the shrink's ClearBits and the shrink's $FILE_NAME
(the $FILE_NAME record redoes because the commit stamps MR_LSN
only into the file's record, not the parent's; the redo is
idempotent); the shrink's pairs and sizes are skipped by the
file record's MR_LSN stamp) and
`test_truncate_extend_journaled` (walk 35: create 14 + shrink 11
+ extend 10 — the extend gains a $SI flags record and a
$FILE_NAME record; replay redoes 5 — the create's sizes +
SetBits, the shrink's ClearBits and $FILE_NAME, and the extend's
$FILE_NAME; the extend's pairs, sizes and $SI are skipped by the
same stamp and it has no $Bitmap record; the file is 256 KiB
after replay and ntfsck-clean). Negative control
(truncate staging stubbed to -ENOENT): the truncates still
succeed, sizes right, ntfsck clean — only the walk counts and
redo counts fail, as designed. One nuance the control exposed:
with the truncates aborted, the walk starts at the create's
first Open record (16, not 14) — oldest_lsn lands on the first
record of the oldest transaction when no later transaction
dirties; the opens are dispensable and the replay skips them,
so this is benign.
Crash tests (`core/tests/test_crash.c`, suite 2579 checks, 0
failures, landed as their own commit 2026-11-27): the truncate
class is crash-clean. `test_journaled_truncate_shrink_cut_points`
(256 KiB -> 64 KiB, 43 cuts sampled at every write; 5
inconsistent before replay, 0 still bad after it; j.txt 64 KiB
33 times, 256 KiB 10 times) and
`test_journaled_truncate_extend_cut_points` (64 KiB -> 256 KiB,
38 cuts; 4 inconsistent before replay, 0 still bad after it;
j.txt 64 KiB 6 times, 256 KiB 32 times). Per cut: pre-replay
ntfsck + coverage, replay, post-replay ntfsck + coverage +
remount size/alloc lookup + second replay no-op. The sweeps are
what found the three crash-test fixes above (wrong bitmap
target, missing $SI flags, missing parent $FILE_NAME) — the
first sweep run had 21 shrink cuts and 20 extend cuts still bad
after replay; after the three fixes, 0.
Setattr class — landed 2026-11-27 (fork commit, this round): one
transaction per `ntfs_setattr` call, begin in `api.c` before the
FLAGS/CRTIME mutations, carrier threaded through
`ntfs_vfs_setattr` → `ntfs_setattr_size` → `ntfs_truncate_vfs`
(joins via `ntfs_journal_truncate_begin_in`), stage + commit in
`api.c`; `$SI` as one 36-byte UpdateResidentValue built from
in-memory state; `$EA` as whole-value UpdateResidentValue on pure
replace, CreateAttribute ×2 when absent at begin, abort on
length-grow or non-resident; parent `$FILE_NAME` via a shared
`stage_filename_dup` helper. `ntfs_truncate` routes through
`ntfs_setattr`, so a SIZE-only call also gets a setattr transaction
(the truncate's mtime/ctime update is real): the truncate walks move
shrink 25 → 27, extend 35 → 39, and each setattr transaction stages
its own `$SI` record plus a second `$FILE_NAME` dup identical to the
truncate's. A resize that cannot be journaled (resident `$DATA`,
attribute list, compressed/encrypted) marks the outer transaction
unjournaled — the first implementation missed this (begin_in returns
0 with a NULL carrier for an unjournalable resize, which the join
caller did not check), and the resident test caught it: the setattr
was committing the parent's `$FILE_NAME` with the new sizes while
the resize was unlogged. A failing resize also now aborts/drops its
transaction instead of leaking the lock and gate (pre-existing in
the standalone truncate path, fixed in the same change). Evidence:
`test_journal.c` 707 checks, 0 failures (new `test_setattr_journaled`,
`test_setattr_size_joins`, `test_setattr_in_root_unjournaled`; `$SI`
36-byte redo/undo capture, `$LXMOD` capture, `attr_after_mount`);
`test_crash.c` 2958 checks, 0 failures (`test_journaled_setattr_cut_points`,
38 cuts; 2 inconsistent before replay, 0 still bad after it; j.txt
all-old 8 times, all-new 30 times). Negative control (staging stubbed
to `-ENOENT`): the setattr and join tests fail on walk/replay counts
only (16 vs 23/27/28/39, replay 2 vs 3/5/7) while the values stay
right — the op succeeds unjournaled. Follow-ups recorded in the
design row, not this class: `$FILE_NAME` times never update on
setattr (live-driver bug, probed), parent `$SI` mtime on
create/unlink/rename (Phase 2 gap), `$SI` ctime on rename, `$EA`
length-grow (needs a logfile-level resize-path test), extending writes
during writeback (still unjournaled). Pre-existing edge, not fixed
here: a same-size standalone truncate can commit an empty transaction
and stamp MR_LSN = 0, clearing the record's stamp; the setattr
finish avoids it via the empty-transaction check.
Xattr class — landed 2026-11-27 (fork commit, this round): one
transaction per `ntfs_setxattr` / `ntfs_removexattr` call, begin in
`api.c` after `inode_lock`, stage + commit under `mrec_lock` after the
ctime bump; begin snapshots `$SI` 36 bytes + ADS whole-record copy +
decoded runlist when non-resident. Create stages `CreateAttribute` +
`SetBits`; remove stages `DeleteAttribute` (begin-time bytes/offset) +
`ClearBits`; resident→resident replace stages one whole-value
`UpdateResidentValue` (first class to prove the replay length-change
path); non-resident→non-resident stages the mapping pairs first
(dedicated `UpdateMappingPairs` op, redo order is extend the runs
then the sizes that name them) then sizes + bitmap delta via
`lcn_ranges_sub`; flip either way stages Delete+Create+bitmap
delta. The first version staged the pairs as a raw
`UpdateResidentValue` after the sizes — the crash sweep caught it:
18 cuts still bad after replay, `Corrupt mapping pairs array`,
ADS unreadable; fixed to the truncate class's pairs op + order. Aborts to unjournaled on attribute list,
compressed/encrypted ADS, moved ADS, `$SI` missing/too short,
non-regular file/directory. No `$FILE_NAME` record (ADS sizes are not
in the parent dup). Bug fixed en route: `lcn_ranges_sub` returns the
range COUNT (>= 0) — the first implementation treated any nonzero
return as an error and scanned for a zero terminator the function
never writes, so every non-resident replace aborted unjournaled;
fixed to the truncate pattern (n_freed/n_allocd, < 0 -> -ENOSPC,
loop by count). Evidence: `test_journal.c` 899 checks, 0 failures
(5 new tests: create, replace, big create, big replace+remove, root;
walk 45 records in the big test: create 14, big-create 10, replace
11, remove 10; replay redoes 5, 2 dirty overlay clusters written
back). Crash tests in the follow-up commit: 5 sweeps, 199 cut points
(create-small 37, create-big 41, replace-res 36, replace-big 45,
remove-big 40), 0 still bad after replay; `test_crash.c` 4366 checks,
0 failures (was 2958). Follow-ups, not this
class: the `$EA` class reuses the resident-replace pattern proven
here; `$FILE_NAME` times never update on setattr/xattr (live-driver
bug); extending writes during writeback (still unjournaled).
Phase 3, symlink — glue landed 2026-11-27 (commit, this round):
one transaction per `ntfs_symlink`, reusing the create transaction.
`ntfs_journal_create_begin` now begins for S_ISLNK as well as
S_ISREG/S_ISDIR (devices still never begin — mknod is a separate
class). `__ntfs_create` stages, in both branches right after the
reparse set, via one new `ntfs_journal_create_reparse`: a
CreateAttribute for $REPARSE_POINT (the value as it stands after
the reparse set), an AddIndexEntryRoot for the $R entry in $Reparse
(found by (tag, file_id), the create_index pattern with a different
key), and a 4-byte UpdateResidentValue for the $SI flags (old = the
as-added flags from the mapped $SI, which the WAL gate holds
un-synced; new = ni->flags with FILE_ATTR_REPARSE_POINT).
$FILE_NAME is staged after the reparse set and already carries the
tag; $DATA staged empty is correct; no $EA on symlinks. The commit
stamps MR_LSN into $Reparse's record too (the txn carries its
mft_no) — without it the replay would duplicate the $R entry,
because AddIndexEntryRoot inserts unconditionally. Bug fixed en
route: the new txn field `reparse_mft_no` was not initialized at
begin (kmalloc does not zero) — garbage made the commit iget a
wrong record on every create, corrupting the volume and remounting
it read-only (the first pre-push run failed 5 suites with EROFS on
the op after the create); fixed by initializing it to 0. Also
`struct reparse_index` moved from reparse.c to reparse.h, which now
includes layout.h, so the type is visible wherever the header is
included. Evidence: `test_journal.c` 1177 checks, 0 failures (was
899; 4 new tests: win-tag, WSL-tag, dir-link, root fallback — each
journaled link is 17 records; replay redoes only the 2 idempotent
records; readlink correct after replay + mount; $SI carries 0x400;
the $R entry is exactly one after replay and after a second
replay; the root link writes no records and still works). Crash
tests: 3 sweeps, 177 cut points (win 59, wsl 59, dir 59; 6
inconsistent before replay each, 0 still bad after it; link
all-old 24/24/23, all-new 35/35/36); per cut the $R entry count
agrees with the link (0 when absent, 1 when present);
`test_crash.c` 5791 checks, 0 failures (was 4366). Follow-ups, not
this class: $SI ctime on rename; parent $SI mtime on
create/unlink/rename (Phase 2 gap); mknod (devices also carry
reparse data but never begin).
Phase 3, EA — glue landed 2026-11-27 (commit, this round): one
transaction per VFS xattr-handler set on the $EA path
(`ntfs_setxattr` in core/ntfs/ea.c). The hook is in the handler,
NOT inside `ntfs_set_ea`: that function is also called from inside
the ambient create/setattr transactions, which stage $LX* through
their own carriers, and a begin inside it would nest the WAL gate
and the `log_txn_lock`. The handler's flags path
(system.ntfs_attrib / system.ntfs_dos_attrib) is NOT journaled
here — it changes $SI flags only, no $EA, and belongs to the
setattr class. ACL is compiled out (`CONFIG_NTFS_FS_POSIX_ACL`
never defined, no `ntfs_set_acl` symbols in the archive), so there
is no ACL hook; §2.2 will need one when ACLs are enabled. Begin
snapshots the $SI 36 bytes from in-memory state (the setattr form,
not the xattr class's mapped-record snapshot) plus whole-record
copies of $EA_INFORMATION and $EA; stage logs the pair's net
effect — CreateAttribute ×2, DeleteAttribute ×2, or one
whole-value UpdateResidentValue per changed value at any length
(the xattr-proven length-change path). A half-pair, a non-resident
$EA, or a moved attribute aborts to unjournaled. The commit stamps
MR_LSN into the file's record only. Evidence: `test_journal.c`
1418 checks, 0 failures (was 1177; 7 new tests: replace,
replace-grow, remove, create on root, teardown on root, in-root,
failed-set negative control; walk counts + op sequence, $EA stream
redo/undo whole values, $EA_INFORMATION ea_query_length agreeing
with the stream lengths, replay redoes only the idempotent
records; values correct after replay + mount; `ntfsck` clean).
Crash tests landed in the follow-up commit (this round): six
cut-point sweeps in core/tests/test_crash.c, one per EA op shape —
create, replace-grow, replace-shrink and remove on a file, plus
create and teardown on the root directory (the only inode that
exercises the CreateAttribute ×2 / DeleteAttribute ×2 paths, since
a driver-made file already carries $LX*). Per cut: pre-replay
ntfsck + allocation coverage, replay, post-replay ntfsck +
coverage + remount EA read + second replay no-op. The sweeps
found no bug. Evidence: `test_crash` 7641 checks, 0 failures (was
5791): 229 cut points, 12 inconsistent before replay, 0 still bad
after it; EA all-old 48 times, all-new 181 times. The $LXMOD entry
is asserted present after every replay on the file sweeps — the
whole $EA stream redoes/undoes as one value, so a torn stream
would lose it. Follow-ups, not this class:
the xattr class's $SI snapshot comes from the mapped record, a
latent one-step-too-far undo on $SI ctime/flags when two xattr
transactions run without a sync between them; the handler's flags
path is still unjournaled.
Phase 3, reparse (unlink half) — glue landed 2026-11-27 (commit,
this round): unlinking a reparse point is now one journaled
transaction. `ntfs_journal_unlink_check` no longer refuses reparse
points (only object IDs still refuse); a new
`ntfs_journal_unlink_stage_reparse` stages a DeleteIndexEntryRoot
record for the $R entry in $Reparse's $INDEX_ROOT — found by (tag,
file_id) through a shared `find_reparse_index_entry` helper factored
out of `ntfs_journal_create_reparse`'s scan — in the link-count-0
branch of `ntfs_delete`, before `ntfs_delete_reparse_index` removes
the entry. The tag is read from the file's own record (its
$REPARSE_POINT value): `ni->reparse_tag` is only populated by the
readlink path and cannot be trusted at unlink time. The $REPARSE_POINT
attribute itself needs no record: the record free staged after the
$R removal carries the whole post-delete record bytes. The unlink
carrier gains its own `reparse_mft_no` (initialized to 0 at begin),
and the unlink and rename commits stamp MR_LSN into $Reparse's
record through a shared `stamp_reparse_mft_lsn` helper (the create
commit uses it now too): without the stamp a post-commit flush +
crash would replay the offset-based DeleteIndexEntryRoot onto a
record that already lacks the entry and excise the wrong bytes. A
rename of a reparse point becomes journaled as a side effect (the $R
key is (tag, file_id), unchanged by rename). `ntfs_remove_ntfs_reparse_data`
was declared in `reparse.h` but defined only in libntfs-3g and never
called in this port — the dead declaration is removed. Evidence:
`test_journal.c` 1480 checks, 0 failures (was 1418; new
test_unlink_reparse_journaled: walk 27 records — the create's 15
still-live plus the unlink's 12; $R delete targets $MFT:$DATA at vcn
rp_ino/4; MR_LSN stamped into the file, the parent and $Reparse;
replay redoes 3; link gone and $R count 0 after replay and on the
live snapshot; `ntfsck` clean). Crash tests landed in the follow-up
commit (this round): one cut-point sweep in core/tests/test_crash.c
— 46 cut points, every write sampled, 6 inconsistent before replay,
0 still bad after it; link all-old 13 times, all-new 33 times; per
cut the $R entry count agrees with the link (1 when present, 0 when
gone). Evidence: `test_crash` 8012 checks, 0 failures (was 7641).
The sweep found no bug. Phase 3, reparse (mknod half) — glue landed
2026-11-27 (commit, this round): creating a device node is now one
journaled transaction. `ntfs_journal_create_begin` begins for
S_ISBLK/S_ISCHR/S_ISSOCK/S_ISFIFO, and the device branch of
`__ntfs_create` stages `ntfs_journal_create_reparse` right after
`ntfs_reparse_set_wsl_not_symlink` — the $REPARSE_POINT and the $R
entry. It works as-is because the reparse set populates
`ni->reparse_tag` (through `ntfs_set_ntfs_reparse_data`) and
`ni->flags` gains FILE_ATTR_REPARSE_POINT there. The $SI flags are
staged separately, by a new `ntfs_journal_create_si_flags`, after
the $FILE_NAME is built: `ni->flags` is only final once
`__ntfs_create` folds `fn->file_attributes` back into it, so the
staged new value is `fn->file_attributes | (ni->flags &
FILE_ATTRIBUTE_RECALL_ON_OPEN)` — the same formula as the fold. The
first version staged from `ni->flags` at the reparse set and the
mknod crash sweep caught it: it replayed a device's $SI without
SYSTEM (0x40400, live 0x40404) and a dir-link's without
DUP_FILE_NAME_INDEX_PRESENT (0x400, live 0x10000400) — the $SI and
$FILE_NAME copies disagreed after replay, depending on whether the
record had been flushed. The fix (this round, follow-up commit)
moved the staging after the $FILE_NAME for all reparse creates.
The commit path is mode-agnostic (commit + MR_LSN stamps into the
file, the parent and $Reparse), and a device create already staged
$DATA, $EA and $FILE_NAME, so begin + the staging calls close the
class. Evidence: `test_journal.c` 1586 checks, 0 failures (was 1480;
new test_mknod_journaled: walk 19 records — 2 opens + 12 data ($MFT
$DATA sizes, Initialize, $SD, $DATA, $REPARSE_POINT, $R entry,
$EA_INFORMATION, $EA, $FILE_NAME, $SI flags, $I30 entry, SetBits) +
Commit + 3 dumps + Forget; $SI redo 0x40404 undo 0x4, live and
replayed $SI agree at 0x40404; MR_LSN stamped into the file, the
parent and $Reparse; replay redoes 2; device reads back S_IFCHR|0644
with REPARSE_POINT and RECALL_ON_OPEN on its $SI, tag 0x80000025
and $R count 1; second replay redoes 2 and the $R entry is still
exactly one; `ntfsck` clean; the symlink and dir-link walks moved
their $SI update after the $FILE_NAME create too — res_redo 0x400
for a plain/WSL link, 0x10000400 for a dir-link, live and replayed
$SI agreeing in both cases). Crash tests landed in the follow-up
commit (this round): one cut-point sweep for the mknod half in
core/tests/test_crash.c — 63 cut points, every write sampled, 6
inconsistent before replay, 0 still bad after it; device all-old 27
times, all-new 36 times; per cut the $R entry count agrees with the
device and the $SI flags are exactly 0x40404 when present.
Evidence: `test_crash` 8519 checks, 0 failures (was 7641). The
sweep found one bug — the $SI staging from a not-yet-final
ni->flags, fixed in the follow-up commit of this round before this
sweep landed. Follow-ups, not this class: no VFS operation sets
reparse data on an existing file, so there is no standalone
reparse-set class to journal; the mkdir class never stages a $SI
flags update at all, so a replayed directory's $SI carries the
as-added flags instead of the final DUP_FILE_NAME_INDEX_PRESENT —
the same shape, unasserted there.
Phase 3, compressed-write — glue landed 2026-11-27 (commit, this
round): the last Phase 3 class. One transaction per `do_write` on a
compressed file that is already non-resident when the write begins,
through a new `struct ntfs_journal_cwrite` carrier reusing the
truncate patterns. Begin runs at the top of the compressed branch of
`do_write`, before `ntfs_attr_expand`, and snapshots the `$DATA`
sizes + pairs + runlist, the `$SI` 36 bytes from in-memory state, the
`$SI` flags, `fn_alloc_old` and a runlist copy. Stage runs in
`done:`, after the mtime bump, under `mrec_lock`: the `$Bitmap`
delta first (`lcn_ranges_sub`), then `UpdateMappingPairs`, then
`SetNewAttributeSizes`, then the `$SI` 36 bytes, then the parent
`$FILE_NAME`; an empty transaction aborts instead of committing,
and the commit stamps `MR_LSN` into the file's record. Begin gate:
regular file, non-resident `$DATA`, compressed, no attribute list,
not encrypted, not an attribute inode; a resident file at begin
aborts to unjournaled, and a begin failure warns and proceeds
unjournaled, never failing the write for journal reasons. The `$SI`
flags come from in-memory `ni->flags`, not the mapped record: the
flags handler updates `ni->flags` but never the `$SI` record bytes,
so the first version staged a mapped 0x0 and the overwrite test
caught the undo clearing COMPRESSED from the parent's entry (live
0x800). Content is all-old/all-new on crash without journaling the
data: the writer allocates NEW clusters for a rewritten block,
writes the compressed data there, then punches the old clusters, so
undo restores old pairs over old data and redo applies new pairs
over new data. The resident-to-non-resident conversion inside a
write is a documented limitation, not journaled: the attribute
changes shape and every following attribute shifts. Evidence:
`test_journal.c` 1816 checks, 0 failures (was 1586; three new
tests: overwrite one 64 KiB block — walk 39 records, replay redoes
7, `$FILE_NAME` undo/redo flags both 0x800; append one block at EOF
— walk 39, the old 8 KiB tail block frees 3 clusters, replay redoes
7; conversion — walk 16, the create only, replay redoes 2; content
reads back whole after replay + mount in all three; `ntfsck`
clean). Crash tests landed in the follow-up commit of this round
(this commit): three cut-point sweeps in `core/tests/test_crash.c`
— overwrite one 64 KiB block (49 cut points), append one block at
EOF (50), overwrite two blocks crossing a boundary (50); per cut
ntfsck + allocation coverage + replay + remount read-back + second
replay no-op, with the size, the allocation and the content all
required to agree on the same side. Evidence: `test_crash` 9869
checks, 0 failures (was 8519): 149 cut points, 15 inconsistent
before replay, 0 still bad after it; content all-old 53 times,
all-new 96 times, never mixed; old alloc 208896 bytes in all three
sweeps, new alloc 147456 (overwrite), 212992 (append), 151552
(two-block). No bug found: the writer's allocate-new-before-punch
order holds even for the two-block rewrite, whose mixed blocks go
to new clusters. This closes the compressed-write class and §1.2
Phase 3.
Follow-ups, not this class: the truncate class shares the
flags-source shape for handler-flagged sparse files (its mapped-`$SI`
undo would clear SPARSE from the entry after a refresh); the mkdir
class never stages a `$SI` flags update (see above).
Phase 4, step 1 — periodic checkpoints, landed 2026-11-28 (commit,
this round): the mechanism, not the flip. `ntfs_log_writer_set_
checkpoint_every()` sets how many commits run between checkpoints
(0 clamps to 1, the Phase 2 behaviour); the first commit always
checkpoints, and a successful checkpoint resets the counter.
`ntfs_log_txn_commit` checkpoints iff no checkpoint exists yet or
the counter reached the policy. A refused append is side-effect-free
(the reserve/wrap pre-check runs before any staging), so the commit
path retries once after checkpointing when the log fills to the
reserve with uncheckpointed commits pending (`txn_append_retry`, at
the four commit-path append sites); a checkpoint never passes an
active transaction's first record or a dirty page's oldest LSN, so
records already appended for the transaction stay live, and a second
failure fails the commit to unjournaled. The Phase 2 comment that a
committed-but-uncheckpointed transaction is invisible to replay was
wrong in general: replay walks forward from the last checkpoint
through every later record, building the transaction and dirty-page
tables as it goes — only a log with no checkpoint at all replays
nothing, which the first-commit rule prevents. The default stays 1;
flipping it to 128 (the design §4.3 start) needs the driver
walk-count updates and is step 3. Finding, not this step: a
checkpoint alone does not free log pages in-session — the writer
never removes dirty-page entries and no `DeleteDirtyClusters`
emitter exists — so on a long-lived mount the log eventually fills
and journaling degrades to unjournaled (safe, but the journal stops
working). The flush notification that retires dirty pages when their
clusters reach the volume is step 2; the retry here is the safety
valve, not the wrap fix. Evidence: `test_writer_periodic_checkpoint`
in `core/logfile/tests/test_logfile.c` — five creates at every=2 and
at the default, crash-simulated: the walk from `oldest_lsn` sees 62
client records / 9 dumps at every=2 and 68 / 15 at the default; the
first walked record is commit 1's `InitializeFileRecordSegment` in
both runs (commit 1's 2 open records sit before `oldest_lsn`); dry
and real replays redo all 35 data records in both runs, records
40..44 in use, bitmap bits 40..44 set, five entries in the parent's
`$INDEX_ROOT`, second replay redoes 5. Logfile suite 13460 checks,
0 failures. The test itself caught two things en route, both fixed
in the test, not the driver: an unbounded index-entry walk (now
bounded by the record size) and the walk starting at the first
commit's first LSN, which later checkpoints have correctly moved
past `oldest_lsn` (the reader refuses anything before oldest).
Phase 4, step 2 — flush notification / dirty-page retirement, design
landed 2026-11-28 (round 31, docs only; implementation follows in
the same round): the flush notification retires dirty pages when
their clusters reach the volume, so a checkpointed page frees log
space in-session and the log can wrap. Design in
docs/JOURNAL-DESIGN.md §4.3: the notification is applied
synchronously at flush time, in the writeback path, under the folio
lock — never deferred to the next commit, because a deferred drain
cannot tell a flush that predates a re-dirty from one that follows
it (the dirty-page entry carries no newest-LSN, and staging
re-populates a zeroed slot with the same LCN values). The writer
gains a recursive pthread mutex (lock order `log_txn_lock` ->
writer lock, folio lock -> writer lock, never the reverse) and a
`ntfs_log_writer_delete_dirty_clusters()` emitter that appends one
`DeleteDirtyClusters` record (redo payload `{u64 lcn, u64 len}`
ranges, tid `0x18`) and zeroes the matching slots in the writer's
dirty-page table, dropping entries that become all-zero; it appends
nothing when no slot matches and never checkpoints or retries (an
`-ENOSPC` is dropped, the entry stays, a later flush retries). The
analysis pass skips transaction-table upkeep for
`DeleteDirtyClusters` (log-client bookkeeping, like the table
dumps — otherwise a phantom ACTIVE transaction; the redo pass
already shortens by zeroed clusters). Driver hook
`ntfs_journal_notify_flushed()` from `ntfs_write_mft_block` (per
written record, only when no record overlapping its clusters is
still dirty), `write_mft_record_nolock` (the inode path), and
`ntfs_write_folio` for `AT_BITMAP` / `AT_INDEX_ALLOCATION` folios
(`$Bitmap:$DATA` arrives as `AT_DATA` with `mft_no == FILE_Bitmap`
and is caught by that check, not by `AT_BITMAP`); file data never
notifies, compressed-data flush is a follow-up, and entries for
attributes that no longer exist linger (slow leak, a
checkpoint-time reaper is the follow-up). In this port `submit_bio`
performs the I/O immediately, so the bytes are durable when the
writeback function returns; on a real async kernel the notify must
move to bio completion. Implementation landed 2026-11-28 (round 31,
commits this round: writer half, then driver hook + tests): the
emitter in `core/logfile/logfile_write.c` with the upkeep skip in
`core/logfile/logfile_replay.c` and the recursive writer mutex; the
hook in `core/vfs/journal.c` with call sites in `core/ntfs/mft.c`
and `core/vfs/aops.c`. Evidence: logfile suite 13531 checks, 0
failures (new `test_writer_delete_dirty_clusters`, 71 checks —
with the upkeep skip removed the emitter case reports a phantom
ACTIVE transaction at tid 0x18, so the test fails without the
fix); journal suite 2074 checks, 0 failures (new
`test_flush_notify_retires_dirty_pages` and
`test_flush_notify_no_sync_no_retire` — the walk holds only the
second create's 16 records plus 3 `DeleteDirtyClusters`, and
`oldest_lsn` advances past the first create; the no-sync control
still holds the first create and `oldest_lsn` stays at its first
data record — plus the walk-count rewrite that zeroed all 90
non-zero replay assertions, since after a sync replay is a no-op);
crash sweep `test_journaled_flush_notify_cut_points`: 24 cut
points step 1 across the sync tail (writes 38–61), 10 inconsistent
before replay, 0 still bad after it, zero ACTIVE transactions at
every cut, j.txt present 24 times; full crash suite 10560 checks,
0 failures (was 9869 — the notify's extra log writes lengthen
every sweep's sync tail). Finding, not this step: aborted
transactions leak dirty-page entries (`ntfs_log_txn_abort` frees
only the transaction-table entry; the stale entries carry
`oldest_lsn == 0` and do not pin `first_lsn`, but are dumped at
every checkpoint until a flush notifies them away) — belongs with
the checkpoint-time reaper follow-up, not this commit.
Next: Phase 4 step 3 (flip the default to 128 + driver walk-count
updates + write-amplification and latency measurement).

### 1.3 Replay atomicity — proven at the engine level (2026-11-26)
Gate for §1.2 Phase 0 (as before) and, since 2026-11-26, also a
gate for any later performance work that touches the write path.
Proven by `test_replay_cut_points` (core/logfile/tests/
test_logfile.c): one committed transaction dirtying two MFT records
and one bitmap cluster (3 flush writes + barrier); a write budget
on the apply callbacks cuts the flush after 0/1/2/3 writes, plus a
barrier-refusal cut; every cut run must report failure and leave
the log untouched, and a second replay with the device restored
must return 0 and land byte-identical to an uninterrupted replay.
5/5 cut points converged. Negative control (second replay
skipped): cuts 0/1/2 leave a partial disk that fails the memcmp,
2/5 converged — the test is live, not vacuous. The cut is a write
refusal through the apply callbacks, which are the device writes
(flush_overlay is their only caller): a SIGKILL mid-flush leaves
the same on-disk state, only the reporting differs.
Boundary, stated honestly: this proves the replay ENGINE is
atomic on synthetic logs. It does not prove a real dirty volume
mounts — that needs a real $LogFile with a dirty journal, which
only Windows can produce (the port never writes $LogFile records;
no tool materializes a synthetic log into a real image). The
mount-level proof belongs to §1.2 Phase 5 (Windows round trip).
Replay stays opt-in per volume until then.

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

### 2.3 No TRIM for SSDs — open, re-examine (2026-11-26)
Was blocked: no FSKit path to issue discards. Reopened by the
owner's industry-leading bar — re-examine whether any FSKit path
exists now (newer SDKs, `FSDiscard` or equivalent, discard via the
block device). If genuinely impossible, keep the reason recorded
here and move on; do not ship a fake TRIM.

### 2.4 No verify / repair in the app — open, re-examine (2026-11-26)
Was wontfix (stance): chkdsk is the tool and the project will not
pretend otherwise. Reopened by the owner's industry-leading bar —
Paragon ships Disk Utility verify/repair, so re-examine whether a
safe subset belongs in the app after all (read-only verify first;
repair only where the driver can prove the fix). Journal recovery
is the safe subset and it is done; anything beyond it needs a
design decision first, not a half-built fsck.

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

### 3.1 Userspace costs what a kernel driver doesn't — open, to the physical limit
Paragon runs in the kernel. This runs in userspace over FSKit, so
every metadata op pays an XPC round trip (~0.3–0.6 ms, 10–20× a kernel
`stat`). The bar is now the physical limit, not "mitigate only":
measure every metadata op against Apple's exFAT and Paragon's
published figures where reachable, then squeeze what FSKit allows
(warm inode cache, batching, fewer round trips per op) and record
the measured numbers per release. Every perf claim needs measured
numbers in the commit message, not assertions. Safety constraint
(2026-11-26): no performance optimisation may trade away a safety
margin — a perf change touching the write path needs the same
crash-consistency evidence as a correctness fix (§1.3 gates it).

### 3.2 Random 4 KiB writes miss the bar — open, to the physical limit
Phase 5 bar is "within 2× of Apple's exFAT"; random 4 KiB writes still
miss it. Attributed to the macOS buffer cache rather than the driver —
prove that attribution with a benchmark before changing code, then
close the gap as far as the hardware allows (write coalescing,
alignment, fewer syncs per write). Same rule as §3.1: measured
numbers in the commit message, not assertions — and the same safety
constraint: random-write work touches the write path, so §1.3 gates
it and every change needs torn-write / power-loss evidence, not
just a faster number.

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

Runner-image regression (2026-11-27): the macos-26-arm64 image
update 20261117.0526.1 -> 20261123.0526.1 started producing
arm64-only app binaries — `** BUILD SUCCEEDED **` then the
universal check failed `missing x86_64 slice`, three runs in a
row (push attempts 1–3 of ff859eab, plus the mount-test dispatch
run), with identical project settings and the same Xcode 26.6.
All test stages stayed green in every run (fixtures 265/0/0,
Swift 0 failures, ASan green), so this is a CI-environment
break, not a code break. Fix (this commit): `ARCHS="arm64
x86_64" ONLY_ACTIVE_ARCH=NO` is now also explicit on the
`stage_build_app` xcodebuild command line in `tools/ci.sh` —
command-line settings override any image-default arch
resolution; the xcconfig stays the single source of truth for
the project itself. Validated only by CI (no macOS here).

Still open: nothing has ever *run* on Intel hardware — the universal
build is verified by compilation and `lipo` on the arm64 runner only.
Reopened as active work by the owner's industry-leading bar (2026-11-26):
get the universal build actually tested on Intel — a GitHub Actions
Intel runner if one is available, otherwise record exactly what is
needed (real Intel Mac, or a Rosetta-less x86_64 VM) and why it is
blocked. No Intel hardware is available on this VM.

### 4.3 macOS 26 only — open (2026-11-26)
Was wontfix unless demand appears; reopened by the owner's
industry-leading bar ("everything for all Macs"). Extend support to
earlier macOS versions as far as FSKit allows, and record exactly
where the floor is and why: FSKit arrived in macOS 15.4, so the
floor is somewhere in 15.4–26 depending on which FSKit APIs the
driver needs. Deployment target is 26.0 today, one FSKit API
generation; every API the driver uses needs a per-version
availability check before the target can move down.

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

Expanded coverage since 2026-11-26 (owner's data-safety-first
direction): dirty-$LogFile seeds, a read-only-mount fuzz variant,
longer runs, and any new write path gets a fuzz target before it
lands. Fuzzing is a standing bar alongside crash-consistency, not
a one-off harness.

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

- 2026-11-26: `9cc5761b8f28` + `67aded1da114` + `5c32f0b8d791`
  (invalidate_lock / size extension) — ported as one commit (same
  two functions): `ntfs_setattr_size()` now takes
  filemap_invalidate_lock around the whole resize (unlocked on
  error too, restoring i_size first), and `ntfs_vfs_fallocate()`
  takes it unconditionally before the punch/allocate switch (the
  compressed/encrypted check returns -EOPNOTSUPP before the lock;
  the old map_locked flag is gone); lock order i_rwsem ->
  invalidate_lock. New `pagecache_isize_extended()` in
  platform/pagecache/pagecache.c (zeroes [from, to) in the folio
  straddling the old EOF and marks it dirty; no-op when from >= to
  or the folio is not cached), and both extension branches now call
  truncate_pagecache(old_size) + pagecache_isize_extended(old, new)
  instead of the old ntfs_vfs_zero_range() epilogue (which only ran
  when old_size % PAGE_SIZE != 0). NOTE: the lock half is
  unobservable in this port — no mmap fault path, no shared taker
  of invalidate_lock anywhere, no inode_dio_wait — so no
  discriminating test is possible for it (same precedent as
  4dc8f4ee2d46 / 8c5dc7587fdd / 88496c4ac5a6 / 81fe702ff176);
  applied for upstream fidelity. The zeroing half is
  behavior-preserving here (the old zero_range also dirtied the
  folio; the read path zeroes past initialized_size on fill;
  ntfs_truncate_vfs never raises initialized_size on extension),
  and it is pinned by the discriminating unit test
  `test_isize_extended()` in
  platform/pagecache/tests/test_pagecache.c — negative controls:
  stripping the zero fails it at the zeroed-bytes assertion,
  stripping the dirty mark fails it at the dirty assertion
  (the folio starts clean with stale bytes planted by memset, so
  the setup cannot mask either). `test_size_extension_zeroes()` in
  core/tests/test_links.c pins the end-to-end behaviour (setattr
  extension and the fallocate gap, in memory and after a remount)
  as a characterization test: it passes with the old code too,
  verified empirically by stripping the new extension branch to a
  bare i_size_write. Evidence: plain ctest 15/15, UBSan ctest
  15/15, ASan ctest PASS, `run-tests.sh all` 265 passed 0 failed
  with NTFS_REQUIRE_FSCK=1, pre-push 9/9 (202s).
  19 port items remain (triage-table count; the running GAPS
  count had drifted from the table by two rows).

- 2026-11-26: `4e646ecd4475` (drop stale page-cache when shrinking
  a non-resident attr) — ported: `ntfs_non_resident_attr_shrink()`
  (core/ntfs/attrib.c) now calls
  `truncate_inode_pages(VFS_I(ni)->i_mapping, newsize)` right
  after updating data/initialized size, verbatim upstream. NOTE: no
  discriminating test is possible through the public harness — the
  reachable shrink-without-cache-drop path is the $EA replace
  (`ntfs_write_ea()` -> `ntfs_attr_truncate()`; the $DATA path
  already drops the cache in `truncate_setsize` before
  `ntfs_truncate_vfs`, and the ADS replace drops it explicitly in
  core/vfs/api.c:1578), but the port's
  `ntfs_write_folio_non_resident()` returns 0 without writing for
  any folio at or past `allocated_size`, so a stale $EA folio is
  silently skipped at writeback: no -ENOENT, no write to freed
  clusters, and EA reads go through `ntfs_attr_readall` (direct
  pread), so the stale bytes are never visible. The stale folios
  are real (they linger dirty until writeback) and this drops them
  at the source; one verbatim call, exercised by the existing EA
  coverage. Same "no new test, port note explains why" precedent
  as 88496c4ac5a6 / 81fe702ff176.
- 2026-11-26: ASan per-test timeout 60 s -> 180 s (tools/ci.sh
  stage_asan, tools/pre-push.sh comments) — NOT a port item, but
  it blocked this commit's pre-push: six consecutive ASan runs
  failed with `links` timing out at 60 s and no assertion or ASan
  error. Diagnosis, not a hang: two live /proc dumps of the
  "hung" process showed the main thread RUNNING (state R,
  progressing through the suite) or in normal kernel I/O wait
  (state D, folio_wait_bit_common during fsync-heavy teardown),
  writer/bg-io threads idle — no deadlock signature. A/B
  measurement settled it: WITHOUT the 4e646ecd4475 change,
  `test_links` under ASan takes 43-93 s (six runs: 43, 79, 92, 78,
  78, 93 s); WITH it, the same 18-93 s range. The 60 s bound was
  set when the slowest test was ~20 s; on this shared 1.5-CPU box
  the host load decides whether links fits in 60 s. 180 s is ~2x
  headroom over the slowest observed; a true hang still fails,
  never runs away (job timeout is 30 min). On the macos-26 CI
  runner the whole ASan suite takes ~36 s, so this changes
  nothing there.
  Evidence (this commit): plain ctest 15/15, UBSan ctest 15/15,
  ASan ctest PASS (159 s), `run-tests.sh all` 265 passed 0 failed
  with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (489 s).
  18 port items remain.

- 2026-11-26: `5f2a22b36fe3` + `be9e89ccb8e5` (free-cluster
  accounting pair) — ported, one commit, both in
  core/ntfs/lcnalloc.c, verbatim upstream. `5f2a22b36fe3`:
  `__ntfs_cluster_free()`'s err_out now calls
  `ntfs_dec_free_clusters(vol, delta)` only when the rollback
  succeeded (else branch); a failed rollback used to subtract the
  negative errno, adding bogus clusters to `vol->free_clusters`.
  `be9e89ccb8e5`: `ntfs_cluster_free_from_rl_nolock()` now only
  adds a run's length to `nr_freed` when
  `ntfs_bitmap_clear_run()` succeeded; the old if/else inflated
  the counter on a second run failing with the same errno as an
  earlier one, or any failure after a non-ENOMEM error was
  recorded. NOTE: no discriminating test is possible through the
  public harness for either half. For `5f2a22b36fe3`, a failing
  rollback needs a bitmap-set failure on a run whose clear just
  succeeded (same bits, same folio, identical I/O path) or a
  runlist-map failure in the recursive call that the first call
  did not hit — reachable only on a corrupt multi-extent file or
  failing disk. For `be9e89ccb8e5`, a failing bitmap clear needs a
  $Bitmap read failure, which a crafted runlist cannot produce:
  the mapping-pairs decoder validates every run's LCN against
  `nr_clusters` at map time (core/ntfs/runlist.c:912,939), so an
  out-of-volume run never reaches the free code, and
  `__ntfs_bitmap_set_bits_in_run()` has no range check of its own
  (it succeeds by dirtying a past-EOF folio). The allocation-failure
  rollback (the only from_rl_nolock caller in lcnalloc.c) cannot
  fail either: the allocator only hands out in-volume runs. The
  fixes are 2-3 lines each in error paths, exercised by the
  existing allocation/free coverage. Same "no new test, port note
  explains why" precedent as 4e646ecd4475 / 88496c4ac5a6.
  Evidence (this commit): plain ctest 15/15, UBSan ctest 15/15,
  ASan ctest PASS (67 s), `run-tests.sh all` 265 passed 0 failed
  with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (488 s).
  16 port items remain.

- 2026-11-26: `631946431ddc` (account for MFT records added
  during allocation) — ported, six hunks: (1)
  `ntfs_mft_bitmap_extend_initialized_nolock()` no longer credits a
  fixed 64; (2) `ntfs_mft_record_alloc()` credits the records
  actually added, `nr_new_mft_records = (i_size - old_data_size) >>
  mft_record_size_bits`, before dropping the lock; (3)
  `ntfs_mft_record_free()` credits the record only when
  `ntfs_bitmap_clear_bit()` succeeded; (4)
  `__get_nr_free_mft_records()` always stores the clamped scan
  result; (5) `ntfs_statfs()` clamps `f_ffree` to `[0, f_files]`
  with a warning; (6) the `NVolFreeClusterKnown` gate is removed
  from `inc/dec_free_mft_records`. New discriminating test
  `test_mft_free_records_track_extension()` (core/tests/test_links.c):
  creates files until $MFT grows, then requires the live counter to
  equal the bitmap rescan after a remount. Verified: fails without
  the fix (free 29 vs true 45 after 16 files — the $MFT data grows
  one record per file while the bitmap was already initialized past
  the new end, so the old code credited nothing) and passes with it.
  The check runs every 16 creates: checking only every 64 would
  miss the drift, because 64 creates happen to grow the $MFT by
  exactly one 64-record bitmap chunk. (Diagnostic note: the
  remount rescan was briefly suspected of reading a stale bitmap —
  disproved by reading the on-disk $MFT/$BITMAP directly with
  python: 266 set bits in the first 311, i.e. the true free count
  45; the in-memory counter was the wrong one.)
  Evidence (this commit): plain ctest 15/15, UBSan ctest 15/15,
  ASan ctest PASS (175 s), `run-tests.sh all` 265 passed 0 failed
  with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (363 s).
  15 port items remain.

- 2026-11-26: `0e4c83990541` (do not mark the volume clean in
  sync_fs when errors were recorded) — ported, one line in
  core/ntfs/super.c: `ntfs_sync_fs()` now clears VOLUME_IS_DIRTY
  only when `!NVolErrors(vol)`, matching `put_super` and the
  remount path. Discriminating test: `test_write_fail_midtransfer()`
  in core/tests/test_faults.c now ends with
  `check_torn_volume_left_dirty()` instead of `check_image()`. The
  torn transfer is the $Volume record write (the fault writes the
  first 512 bytes, new USN, and fails on the second half), so $MFT
  record 3 is left with mismatched sector USNs; before the fix the
  sync rewrote the record while clearing the dirty bit, repairing
  the tear and hiding the error, so ntfsck called the image clean.
  With the fix the volume stays dirty with the torn record visible,
  asserted three ways: the dirty bit still set on disk, the torn
  USA, and ntfsck reporting record 3 — all three fail without the
  fix. (Proven the hard way: the first full ladder ran against a
  tree where the fix had been reverted for the negative control and
  never re-applied — caught by the push dry-run listing only the
  docs files; the fix was re-applied and the ladder re-run.)
  Note: the upstream reproducer (a $MFTMirr mismatch leaving a
  read-write mount with errors set) is NOT the route used — it is
  unreachable in this port, because the glue forces
  `errors=remount-ro` on every mount (core/vfs/super_glue.c:146),
  `ntfs_glue_make_rw()` refuses RW whenever `NVolErrors` is set,
  and a mirror-mismatch mount fails with `-EROFS` (verified
  empirically with a python patcher that flips one byte in the
  mirror's copy of record 1; patcher deleted).
  Evidence (this commit): plain ctest 15/15, UBSan ctest 15/15,
  ASan ctest PASS (123 s), `run-tests.sh all` 265 passed 0 failed
  with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (385 s).
  14 port items remain.

- 2026-11-26: `4d730a4dd78c` (respect per-file chmod mode over
  mount masks) — ported, upstream's five hunks plus the
  port-necessary `mode_of()` half: `ntfs_ea_get_wsl_inode()` takes
  a `bool *has_lxmod` out-param and loads $LXUID/$LXGID
  best-effort (a missing optional EA no longer returns -EIO before
  $LXMOD is read); `ntfs_read_locked_inode()` applies dmask/fmask
  only when `!has_lxmod` (all three sites incl. the port's symlink
  branch); `ntfs_setattr()` no longer remasks in memory after
  persisting and propagates `ntfs_ea_set_wsl_inode()` errors;
  `__ntfs_create()` no longer masks at create. PORT-EXTRA: the
  mode users see comes from `mode_of()` in core/vfs/api.c, which
  computed perm from the masks unconditionally — it now returns
  `vi->i_mode & 07777` when the inode carries $LXMOD. That needs
  the flag to survive on the inode, so `NI_HasLXMOD` was added
  (core/ntfs/inode.h): set at load from `has_lxmod`, set in
  `ntfs_ea_set_wsl_inode()` on a successful $LXMOD store (covers
  create/setattr/xattr-set), cleared in `ntfs_set_ea()` when
  $LXMOD is removed and in `ntfs_migrate_symlink_ea()` ($EA
  stripped entirely). Symlinks keep the unconditional 0777 in
  `mode_of()`. Discriminating test: `test_permissions()` in
  core/tests/test_links.c — the old contract (create 0600 -> 0755,
  chmod 0644 -> 0755, chmod 0400 -> 0555, remount under new masks
  changes modes) is replaced by the new one (0600/0700/0400/0644
  preserved, remount under fmask 077/dmask 027 leaves them
  unchanged, root without $LXMOD still follows the mask at 0750).
  Verified: 6 failures with the fix against the old expectations,
  0 failures with the new ones; the new expectations fail on the
  old code at the same six points (measured values, no revert
  round-trip — the 0e4c83990541 lesson: verify the fix is present
  before the ladder, and the dry-run file list before pushing).
  13 port items remain.

- 2026-11-26: `71feaa7686fb` (apply Windows name checks only
  with windows_names) — ported, upstream's reorder verbatim in
  `ntfs_check_bad_windows_name()` (core/vfs/namei.c): the
  `NVolCheckWindowsNames` early return now precedes
  `ntfs_check_bad_char()`, so `:` etc. are no longer rejected on
  a mount without `windows_names`. No new test: no discriminating
  test is possible through the public harness — the only caller
  is `ntfs_vfs_validate_uname()`, which already returns 0 when
  the flag is clear (core/vfs/namei.c:1091), so the upstream bug
  was already masked at every reachable path, and the function
  takes the internal `struct ntfs_volume *`, which tests cannot
  construct (opaque handle). Verified empirically both ways:
  test_names 471 checks, 0 failures with the fix and with the
  fix reverted; the existing `test_windows_illegal_allowed`
  coverage (`co:lon.txt` created on an allowillegal mount) passes
  on old code too. Applied for upstream fidelity, same "no new
  test, port note explains why" precedent as 4e646ecd4475 /
  88496c4ac5a6.
  Evidence (this commit): plain ctest 15/15, UBSan ctest 15/15,
  ASan ctest PASS (120 s), `run-tests.sh all` 265 passed 0 failed
  with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (367 s).
  12 port items remain.

- 2026-11-26: `f72df3a4c33b` (make system files immutable to
  prevent corruption) — ported. Upstream's verbatim block in
  `ntfs_read_locked_inode()` (core/vfs/inode.c): `mft_no <
  FILE_first_user && S_ISREG(vi->i_mode)` sets `S_IMMUTABLE`,
  directories skipped so the root and $Extend stay usable.
  PORT-EXTRA enforcement half, same precedent as 4d730a4dd78c
  (mode_of): the flag alone is unobservable in this port — no VFS
  enforces `IS_IMMUTABLE`, the only consumer is the delete gate in
  `ntfs_vfs_unlink` (core/vfs/namei.c:1291), itself gated on
  `NVolSysImmutable`, and nothing in the port ever passes the
  `sys_immutable` option (no glue flag, no FSKit option). So
  `ntfs_write`, `ntfs_setattr` (truncate/mode/times),
  `ntfs_fallocate`, `ntfs_setxattr`, `ntfs_removexattr`
  (core/vfs/api.c) now refuse immutable inodes with -EPERM,
  `ntfs_vfs_unlink` no longer requires `NVolSysImmutable`, and
  `__ntfs_vfs_rename` refuses an immutable source or existing
  target (checked before any lock, direct return). New
  discriminating test `test_system_files_immutable()`
  (core/tests/test_names.c, mounted with
  `NTFS_MOUNT_SHOW_SYSTEM`): positive controls on `plain.txt`
  first, then $Bitmap/$MFT refuse write/truncate/fallocate/
  chmod/setxattr/removexattr/rename-away/rename-over/unlink and
  read-back bytes are unchanged. Negative control: with only the
  flag block reverted, 492 checks / 10 failures (all 10
  mutation-refusal assertions), and the run exits cleanly under
  `timeout 120` — the test's breach guard skips the unlink block
  and `structural_check` on breach, because unlinking $MFT out of
  the corrupted volume spins the driver at 77% CPU instead of
  failing cleanly (first negative-control run had to be killed
  after 3+ minutes).
  Evidence (this commit): test_names 498 checks, 0 failures;
  plain ctest 15/15 (172 s), UBSan ctest 15/15 (166 s), ASan ctest
  15/15 (213 s), `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (401 s).
  11 port items remain.

- 2026-11-26: `06769b8f23b4` (fix hole runlist memory leak in
  insert range error path) — ported. One line verbatim:
  `kfree(hole_rl)` added to the `ntfs_attr_map_whole_runlist`
  error path in `ntfs_non_resident_attr_insert_range()`
  (core/ntfs/attrib.c), matching the later paths in the same
  function that already free it. No new test, honestly: the
  function has zero callers in the port — upstream's caller is
  `ntfs_insert_range()` (FALLOC_FL_INSERT_RANGE) in
  fs/ntfs/file.c, which the port never ported (its fallocate is
  `ntfs_attr_fallocate`, with no insert-range path) — so the leak
  is unreachable here, and the error path needs
  `map_whole_runlist` to fail on internal runlist state tests
  cannot construct. Ported for upstream fidelity, same "no new
  test, port note explains why" precedent as 71feaa7686fb.
  Evidence (this commit): plain ctest 15/15 (115 s), UBSan ctest
  15/15 (115 s), ASan ctest 15/15 (123 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (430 s).
  10 port items remain.

- 2026-11-26: `cf06dcd57284` (fix kmap_local leak in
  write_mft_record_nolock() error paths) — ported. Upstream's two
  changes verbatim in `write_mft_record_nolock()`
  (core/ntfs/mft.c): the `pre_write_mst_fixup` failure now goes to
  the new `unmap_err_out` label instead of `err_out`, and
  `unmap_err_out` runs `kunmap_local(kaddr)` before `err_out`,
  covering both that path and the `put_bio_out` path
  (`bio_add_folio` failure). No new test, honestly: in this port
  `kunmap_local` is a no-op shim
  (platform/include/linux/highmem.h:14) and `kmap_local_folio`
  just returns a pointer into folio data, so the leak has no
  observable effect here; the fix is upstream fidelity in case
  the shim ever becomes real. Same "no new test, port note
  explains why" precedent as 71feaa7686fb.
  Evidence (this commit): plain ctest 15/15 (114 s), UBSan ctest
  15/15 (113 s), ASan ctest 15/15 (151 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (403 s; first pre-push run failed stage 5 transiently under load
  — an isolated `ci.sh --quick` re-run passed all three Linux
  stages, and the full re-run went 9/9).
  9 port items remain.

- 2026-11-26: `98716c9fce21` (fix off-by-one page overflow in
  ntfs_decompress()) — ported. One character verbatim:
  `dp_addr > dp_sb_end` to `dp_addr >= dp_sb_end` in the token-loop
  range check in `ntfs_decompress()` (core/ntfs/compress.c:356);
  the pre-tag check at :343 keeps `>` exactly as upstream. When a
  sub-block decompresses to exactly 4096 bytes with tags remaining,
  the old code parsed one more token and wrote past the sub-block
  end. No new test, honestly: `ntfs_decompress()` is static,
  reachable only through the full read path, and the boundary needs
  a byte-exact hand-crafted LZ77 stream — no compressed-stream
  fixture helper exists in the repo. Ported for upstream fidelity,
  same "no new test, port note explains why" precedent as
  71feaa7686fb.
  Evidence (this commit): plain ctest 15/15 (116 s), UBSan ctest
  15/15 (122 s), ASan ctest 15/15 (141 s, re-run — the first run
  hung 12:33 in `test_links` under ASan: the forked child of
  `test_ib_sync_write_no_post_fixup` sat in a futex wait with 0 CPU,
  i.e. blocked from the start, not looping; `test_links` alone under
  ASan then passed 827 checks / 0 failures in 44 s and the full
  suite re-ran 15/15, so it was a one-off ASan+fork flake, not this
  one-character change — the child never touches compressed data),
  `run-tests.sh all` 265 passed 0 failed with NTFS_REQUIRE_FSCK=1,
  pre-push 9/9 PASS (379 s).
  8 port items remain.

- 2026-11-26: `b54559689a00` (fix initialized size and page state
  after compressed writes) — ported. Two of upstream's three hunks
  in `ntfs_compress_write()` (core/ntfs/compress.c): the `!copied`
  guard returns -EFAULT via the new `release_pages:` label instead
  of calling `ntfs_write_cb` with zero bytes copied, and the
  page-state loop is now `if (!err) { clear_dirty; mark_uptodate; }
  else { clear_uptodate; }` instead of `if (i < ip)` marking pages
  uptodate unconditionally even on error. The initialized-size hunk
  was deliberately NOT ported per-block: the port already publishes
  initialized_size once at the end of the function, only for a write
  that ran to completion (PORT comment at :1633) — a more
  conservative choice than upstream's per-block publication, kept on
  purpose. New discriminating test: a pre-unmount read-back
  assertion in `test_compressed_block_survives_failed_rewrite()`
  (core/tests/test_compress.c) — after the ENOSPC failed overwrite
  of a compressed block, the OLD block must read back, not the new
  bytes sitting in the cache. Negative control (page-state hunk only
  reverted, `!copied` guard kept): test_compress 493 checks /
  1 failure, exactly the new assertion (`first_diff` at
  test_compress.c:1729; the failed write had copied 65536 bytes,
  ip=15, then `ntfs_write_cb` failed -ENOSPC); with the fix 493 / 0.
  Lesson from this item, recorded so it is not re-learned: manual
  test runs must execute from the repo root — from `build/` the
  relative tool paths (`tools/.local/sbin/mkntfs`) break,
  `make_image()` SKIPs silently, and the test is vacuous (0 checks,
  0 failures, nothing tested). The first "positive" and "negative"
  runs of this item were both vacuous for exactly that reason; the
  real negative control only ran once the binary was executed from
  the repo root.
  Evidence (this commit): test_compress 493 checks, 0 failures;
  plain ctest 15/15 (125 s), UBSan ctest 15/15 (137 s), ASan ctest
  15/15 (153 s), `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (697 s, slow under load).
  7 port items remain.

- 2026-11-26: `3e497af63725` (propagate compression context
  allocation errors) — ported, verbatim. `ntfs_compress_block()`
  returned `unsigned int`, so its `return -ENOMEM` on kvzalloc
  failure truncated to a huge positive size, and the caller
  (`ntfs_write_cb()`, core/ntfs/compress.c) only tested `!sz` — a
  real allocation failure under memory pressure would have been
  read as a successful giant compression and fed into the size
  math. Now the function returns `int` (doc: "A negative error code
  if an error has been met"), the caller declares `int sz;` and
  returns `err = sz; goto out;` when `sz < 0`. No new test,
  honestly: the error path needs kvzalloc() to fail inside
  `ntfs_compress_block()`, and the harness has no
  allocation-failure injection (test_faults.c injects device I/O
  faults only); the function is static with a single caller, so
  nothing reachable can force the path.
  Crash/disconnect at the worst instant: on allocation failure the
  write now fails cleanly with -ENOMEM instead of writing garbage
  sized by a truncated error code; a crash mid-write still leaves
  either the old block or nothing new (the old block is not punched
  out until the new block is safe — upstream eb67671cd814 ordering,
  already in the tree), so no torn mixture is ever presented as
  good data.
  Evidence (this commit): plain ctest 15/15 (70 s), UBSan ctest
  15/15 (65 s), ASan ctest 15/15 (68 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (266 s).
  6 port items remain.

- 2026-11-26: `523307b8516f` (preserve RECALL_ON_OPEN on WSL
  special-file reparse points) — ported, verbatim. In
  `__ntfs_create()` (core/vfs/namei.c:663) the reload
  `ni->flags = fn->file_attributes` dropped the
  FILE_ATTRIBUTE_RECALL_ON_OPEN set just before for WSL special
  files, because the $FILE_NAME copy had been overwritten with
  FILE_ATTR_SYSTEM: on-disk file_attributes came out 0x00000404
  instead of 0x00040404, and after a remount
  `valid_reparse_data()` rejected the reparse point while fsck
  reported "$REPARSE_POINT data is corrupted". Now
  `ni->flags = fn->file_attributes | (ni->flags &
  FILE_ATTRIBUTE_RECALL_ON_OPEN)`. New discriminating test
  `test_wsl_special_file_survives_remount()` (core/tests/
  test_links.c): a fifo and a socket, created via a new public
  `ntfs_mknod()` wrapper (core/vfs/api.c, core/include/ntfscore.h
  — `ntfs_create()` forces S_IFREG, so the WSL path was
  unreachable from the public API), must keep S_IFIFO/S_IFSOCK
  and the RECALL_ON_OPEN bit across a remount. Negative control
  (fix only reverted): test_links 620 checks / 5 failures —
  on-disk file_attributes 0x404 instead of 0x40404 at create, the
  fifo back as the invalid mode 0110644 (S_IFREG OR'd onto
  S_IFIFO) after remount, file_attributes 0x4; with the fix 620 / 0.
  Notes: the socket's mode check alone cannot discriminate
  (S_IFSOCK already contains the S_IFREG bit, so the load
  fallback leaves it looking like a socket even when validation
  failed); the socket is discriminated by its file_attributes
  check. The `vi->i_mode |= S_IFREG` fallback in inode.c produces
  the invalid two-type-bits mode on a genuinely corrupt reparse
  point — a pre-existing load-path quirk, out of scope for this
  port, recorded here so it is not re-discovered.
  Crash/disconnect at the worst instant: this commit touches inode
  creation, so per the standing rule — a crash between the reparse
  write and the $FILE_NAME write leaves a file whose reparse point
  is either fully present or absent; either way the volume mounts
  and the file is readable as a regular file, never as a torn
  reparse point presented as good data. The fix itself only
  preserves one attribute bit across an in-memory reload; it does
  not change what is written to the device or the write order.
  Evidence (this commit): test_links 620 checks, 0 failures;
  plain ctest 15/15 (68 s), UBSan ctest 15/15 (68 s), ASan ctest
  15/15 (201 s), `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (542 s).
  5 port items remain.

- 2026-11-26: `b8d6c528e9d5` (fix WARN_ON for resident attribute
  in ntfs_map_runlist_nolock()) — ported, verbatim. The
  WARN_ON(!ctx->attr->non_resident) in the ctx_needs_reset
  re-lookup of `ntfs_map_runlist_nolock()` (core/ntfs/attrib.c:177)
  is now `if (unlikely(!ctx->attr->non_resident)) { err = -EIO;
  goto err_out; }`, matching the sibling branch at :120 that
  already returns -EIO the same way. No new test, honestly (this
  supersedes the earlier BLOCKED call in the triage row, which
  held the fix back for want of a test — the coordinator rule is
  to land with the reason stated): no failing-without-fix test is
  reachable through the public harness. The mount path
  (upstream's KASAN trigger via ntfs_empty_logfile) is
  unreachable: a resident $LogFile $DATA fails
  ntfs_check_logfile's minimum-size gate (40 KiB) before emptying,
  and the restart-page validation needs a full 4 KiB page that a
  ~950-byte resident value cannot hold. A white-box call needs
  struct ntfs_inode *, but the internal headers redefine public
  symbols and cannot coexist with ntfscore.h in one TU. The read
  path branches on NInoNonResident before any runlist mapping.
  The path needs an $ATTRIBUTE_LIST whose re-lookup lands on a
  resident extent — a candidate for an ASan fuzz seed in the §5.2
  follow-up. In this port WARN_ON only logs and continues, so
  without the fix a crafted image would read garbage mapping
  pairs from the resident attr's union after the warning.
  Crash/disconnect at the worst instant: this commit touches only
  an error return in runlist mapping — a crafted or torn image
  that hits it now fails the mount/read with -EIO instead of
  proceeding on garbage runlist data, so a torn volume is
  detected, never silently papered over.
  Evidence (this commit): plain ctest 15/15 (62 s), UBSan ctest
  15/15 (163 s), ASan ctest 15/15 (201 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (211 s; the first pre-push run was interrupted by a VM restart
  mid-run with its log lost — the re-run after the restart, with
  clang 18.1.3 restored by the os-intent replay, went 9/9).
  4 port items remain.

- 2026-11-26: `33200693eb22` (reuse the compression context
  during writes) — ported. `ntfs_compress_block()` allocated and
  initialised a ~40 KiB match-finder context for every 4 KiB
  sub-block — sixteen large allocations per 64 KiB compression
  unit. Now it takes the context as a parameter, `ntfs_write_cb()`
  takes and passes it, and `ntfs_compress_write()` allocates one
  context per write request and frees it in `out:`. Port
  adaptations: the port has no kvzalloc_obj, so the plain
  kvzalloc(sizeof(struct compress_context), GFP_NOFS) the old code
  used is kept, once per request; the port's ntfs_write_cb has no
  page_offset parameter, so ctx is its 5th argument. The
  context's hash chains are reset per sub-block exactly as before
  (the memset in ntfs_compress_block is untouched).
  Measured, not asserted: scratch 256 MiB image, 16 MiB
  compressible write (repeated 8-byte pattern, 1 MiB chunks),
  5 reps each on this VM, fresh image per rep — old median 216 ms
  (212.1 / 216.1 / 318.9 / 396.4 / 187.8), new median 92 ms
  (137.2 / 89.6 / 90.7 / 114.5 / 91.6), ~2.4x faster, ranges do
  not overlap (new max 137 < old min 188). The benchmark
  (/tmp/bench_compress_write.c, not part of the suite) verifies
  byte-exact read-back after a remount on every rep.
  Crash/disconnect at the worst instant: this is a pure allocation
  change on the write path — no write ordering, flush, or on-disk
  layout changes, so the crash story is unchanged: a crash
  mid-write leaves either the old block or nothing new (the old
  block is not punched out until the new block is safe), and a
  failed allocation now fails the whole write request with -ENOMEM
  before any page is copied, instead of failing one sub-block in.
  Evidence (this commit): plain ctest 15/15 (61 s), UBSan ctest
  15/15 (62 s), ASan ctest 15/15 (70 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (211 s).
  3 port items remain.

- 2026-11-26: `5b676a77f3e4` (skip reads for full compression
  unit overwrites) — ported. `ntfs_compress_write()` read every
  page in a compression unit before copying new data into it. The
  read is unnecessary when an aligned write replaces every byte
  covered by the page-cache folios, so a full page-aligned unit
  overwrite now grabs locked cache folios without reading them;
  partial units and units covering only part of a page keep the
  read-modify-write path. Port adaptations: the port has no
  filemap_grab_folio, so the primitive was added to the platform
  (platform/pagecache/pagecache.c +
  platform/include/linux/pagemap.h — __filemap_get_folio with
  FGP_LOCK | FGP_CREAT | FGP_ACCESSED, which never calls
  read_folio); the port hard-codes 4K pages, so upstream's
  !page_offset and !(cb_size & (PAGE_SIZE - 1)) terms hold
  unconditionally and only !off && bytes == cb_size is tested
  (PORT comment at the site).
  Measured, not asserted: same scratch benchmark as 33200693eb22
  (256 MiB image, 16 MiB compressible write, 5 reps each) —
  without the skip median 92.6 ms (92.4 / 100.7 / 105.2 / 92.6 /
  88.9), with it median 85.1 ms (85.2 / 84.7 / 85.1 / 85.2 /
  84.5), ~8% faster, ranges do not overlap (new max 85.2 < old
  min 88.9) and variance collapses. Correctness: every byte of
  every grabbed folio is overwritten by the copy loop before
  ntfs_write_cb reads it, and the page-state loop marks folios
  uptodate only on success (item b54559689a00); partial units
  still take the read path.
  Crash/disconnect at the worst instant: this is a read-path
  change only — what is written to the device, and in what order,
  is unchanged, so the crash story is unchanged: a crash mid-write
  leaves either the old block or nothing new (the old block is
  not punched out until the new block is safe). A grabbed folio
  that was never on the disk holds only bytes the copy loop just
  wrote; if the write then fails, the item-6 page-state fix clears
  its uptodate flag, so no stale data is ever served.
  Evidence (this commit): plain ctest 15/15 (59 s), UBSan ctest
  15/15 (59 s), ASan ctest 15/15 (71 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (203 s).
  2 port items remain.

- 2026-11-26: `8cd2bf1d7af9` (reuse compression output
  workspace across write units) — ported. `ntfs_write_cb()`
  allocated output pages and created input and output vmaps for
  every compression unit. Now one output workspace (output pages
  + one vmap) is allocated per write request, input sub-blocks
  are read straight from their folios via kmap_local_page(), and
  the incompressible path copies the unit into the workspace with
  `ntfs_copy_cb()`; the bdev write always writes ws->outbuf.
  Port adaptations (upstream's version is bio-based, the port
  writes via ntfs_bdev_write): no page_offset in the port, so
  ntfs_copy_cb takes no offset and the input offset is just p;
  the port's vmap is a copy, so the old code paid a full 64 KiB
  copy-in per unit plus a copy-back on vunmap — the new code
  avoids both, which is why the gain here is larger than
  upstream's. The workspace is freed in ntfs_compress_write()'s
  out:, never by ntfs_write_cb; the all-zeroes punch-hole path is
  untouched.
  Measured, not asserted: same scratch benchmark (256 MiB image,
  16 MiB writes, 5 reps each, fresh image per rep) —
  incompressible (random data, exercises ntfs_copy_cb) old median
  664 ms (664.4 / 706.4 / 791.2 / 610.2 / 569.4), new median
  358 ms (357.5 / 389.9 / 350.7 / 346.4 / 452.6), ~1.9x faster,
  ranges do not overlap; compressible old median 110 ms (102.9 /
  112.0 / 110.2 / 124.0 / 105.5), new median 73 ms (94.1 / 71.7 /
  72.6 / 74.5 / 70.8), ~1.5x on medians — with the caveat that
  this shared box's load swings ±15% between windows, so the
  compressible figure is stated with that caveat (an interleaved
  A/B run in a loaded window still favoured the new code 2 of 3
  pairs). The benchmark verifies byte-exact read-back after a
  remount on every rep, both modes.
  Crash/disconnect at the worst instant: the write order and
  on-disk layout are unchanged — the same bytes are written to
  the same clusters in the same order; only where the bytes sit
  in memory before the write changed (workspace buffer instead
  of two per-unit vmaps). A crash mid-write still leaves either
  the old block or nothing new (the old block is not punched out
  until the new block is safe). A workspace allocation failure
  fails the whole write request with -ENOMEM before any unit is
  touched.
  Evidence (this commit): plain ctest 15/15 (60 s), UBSan ctest
  15/15 (61 s), ASan ctest 15/15 (70 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (196 s).
  1 port item remains.

- 2026-11-26: `f2b2aeeaadce` (support large pages in compressed
  writes) — ported. `ntfs_compress_write()` derived its page
  count by shifting the compression block size and assumed every
  block begins at a page boundary — a zero page count for small
  blocks on large-page systems, and an ignored in-page offset.
  Now every page covering the block is mapped (pages_per_cb via
  DIV_ROUND_UP(offset_in_page + cb_size, PAGE_SIZE), once from
  pos and recomputed per unit), the in-page offset is passed to
  `ntfs_write_cb()` and `ntfs_copy_cb()`, insz is the block size
  rather than pages_per_cb << PAGE_SHIFT, and the full_cb
  condition is upstream-verbatim (including !page_offset and
  !(cb_size & (PAGE_SIZE - 1))). The upstream else-branch
  memcpy/pages assignment and the bio sector math are already
  covered in better form by the item-12 port (ntfs_copy_cb +
  workspace; the port has no bio). Behaviour-neutral at PAGE_SIZE
  4096 — every new computation reduces to the old value — so no
  new test can discriminate it here; the value is structural: the
  4K assumption is gone from the compressed-write path, so a
  future move to host page sizes (macOS arm64 is 16K) cannot
  silently yield pages_per_cb = 0.
  Measured, honestly: same scratch benchmark (256 MiB image, 16
  MiB writes, 5 reps each) — compressible median 69 ms (69.1 /
  84.6 / 69.4 / 65.2 / 65.1) vs 73 ms before, random median
  341 ms (325.5 / 341.1 / 331.3 / 356.3 / 341.6) vs 358 ms
  before — both inside this box's ±15% load swing, i.e. no
  measurable change, which is the correct result for this port.
  No perf gain is claimed.
  Crash/disconnect at the worst instant: the write order, flush
  points, and on-disk layout are unchanged — this commit only
  changes how the in-memory page count and offsets are computed,
  so the crash story is unchanged.
  Evidence (this commit): plain ctest 15/15 (60 s), UBSan ctest
  15/15 (59 s), ASan ctest 15/15 (74 s), `run-tests.sh all` 265
  passed 0 failed with NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS
  (201 s).
  0 port items remain — the port queue is done.

- 2026-11-26: §1.3 replay atomicity proven at the engine level.
  New test `test_replay_cut_points` (core/logfile/tests/
  test_logfile.c) + a write-budget fault hook on the fake volume's
  flush callbacks. One committed transaction dirties two MFT
  records (InitializeFileRecordSegment record 40,
  UpdateResidentValue record 30) and one bitmap cluster (SetBits
  BITMAP_LCN bits 100..102): 3 flush writes + barrier. Cuts after
  0/1/2/3 writes plus a barrier refusal; every cut run must report
  failure and leave the log untouched, and a second replay with
  the device restored must return 0 and land byte-identical to
  an uninterrupted replay. 5/5 cut points converged
  (test_logfile 341 checks, 0 failures). Negative control
  (second replay skipped): cuts 0/1/2 leave a partial disk that
  fails the memcmp, 2/5 converged. Boundary: engine-level proof
  on synthetic logs, not a real-volume mount proof — that needs a
  real dirty $LogFile from Windows (§1.2 Phase 5). §1.3 no longer
  gates §1.2 Phase 0.
  Crash/disconnect at the worst instant: a replay interrupted at
  any flush write leaves the volume dirty with its log intact, so
  the next attempt redoes the full plan and converges to the
  uninterrupted state — a torn volume is detected (the cut run
  reports failure, the log is never marked clean), never silently
  papered over.
  Evidence (this commit): test_logfile 341 checks, 0 failures;
  plain ctest 15/15 (62 s), UBSan ctest 15/15 (61 s), ASan ctest
  15/15 (71 s), `run-tests.sh all` 265 passed 0 failed with
  NTFS_REQUIRE_FSCK=1, pre-push 9/9 PASS (196 s).
  Next: §1.2 journal writing, Phase 0/1.

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
