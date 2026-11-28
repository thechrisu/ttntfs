# Upstream triage: fork 0.4.2 → upstream 0.5.0 (dr-kbadawi/ttntfs)

Triage of every commit between the merge base `f61228f` (upstream 0.4.2)
and upstream main `92cc6267` (release 0.5.0, 2026-11-26). **38 commits.**
The fork is ahead 120 / behind 38 from that base. This is the §5.3 pin
move: the pin stays at Linux v7.1 until this triage's "port" items land.

## Verdicts

- **port** — the fork's code contains the same bug and the fix applies.
  12 items. These are the next rounds' work, in the priority order of
  §"Porting order" below.
- **with** — a test/docs companion that rides with its fix commit; ported
  as part of that item, not separately. 10 items.
- **already-covered** — the fork already contains the fix's substance
  (ported 2026-11-26 in the v7.1→v7.3 queue). 3 items.
- **needs-work** — a feature or fix that needs a design decision first.
  6 items (1 safety-policy feature + 5 macOS 27 items, deferred to §4.3).
- **not-applicable** — upstream app marketing, release mechanics, or
  trivial labels with no driver effect; the fork's `fskit/` stays
  byte-identical to upstream v0.4.2 by stance. 7 items.

## Method

Each commit's patch was read from a local fetch of upstream main
(`git fetch upstream main`, 2026-11-28) and checked against the fork
tree at `fb37640c` (Phase 4 complete). A first pass matched the
changed files and hunks against the fork files; every commit was then
classified by hand from the patch and the fork code at the cited lines.
Coverage check: all 38 commits appear exactly once below.

Note on the fork's test layout: upstream 0.5.0 added a new test
target, `core/tests/test_alloc.c` (commit `4d50b7d`), which never
existed in the fork — the fork's suite has no `alloc` target. Porting
the allocator items means creating that target, adapted to the fork's
harness. Upstream's test files are otherwise the same harness shapes
(`make_image`, `mount_rw`, `RW_OPTS`) as the fork's.

## Porting order (the 12 "port" items, highest first)

1. **Crash/corruption, allocator first.** `4d50b7d` (U13/U14: zone
   search restart + contiguous single run), `085e3eb` + `7dab337`
   (finding 29: zero a hole's new clusters, give back a failed write's
   clusters), `31eac87` (U20: roll back an allocation whose mapping
   pairs fail — after finding 29, whose `api.c` it builds on).
2. **Full-volume handling.** `dbde2ff` (finding 26: ENOSPC, not EPERM
   / remount-ro), `bfd3eb1` (finding 28: freed MFT records on a full
   volume), `b17eabb` (finding 27: failed create must evict its
   attribute inodes or the next file given the record is corrupted).
3. **Compressed writes.** `cb9995b` (U18/finding 30: write a block to
   every run), `ab78590` (U21/finding 32: keep the block when its
   rewrite fails), `d437eed` (U22: swap runs in one mapping-pairs
   update). All three touch `core/ntfs/compress.c`, which the fork's
   Phase 3 compressed-write journal code also touches (`do_write`
   hook) — adapt, do not apply blindly; the journal carrier snapshots
   sizes/pairs/runlist at begin, so a change to how runs are swapped
   must keep the staged undo consistent.
4. **Mount/unmount.** `4523d27` (finding 33/U23: write nothing to a
   read-only volume at unmount; persist the dirty flag while errors
   are set — the fork already has the `0e4c83990541` clear-guard in
   `ntfs_sync_fs`, the set-on-errors half and the teardown half are
   the gap), `3b43db9` (finding 34: stay read-only when discarding
   hibernation fails — the fork restores the errors flag but never
   switches the volume back itself).

## Table

| commit | subject | verdict | note |
|---|---|---|---|
| `dbde2ff` | fix: report a full volume as ENOSPC and keep it writable (finding 26) | **port** | U10: `ntfs_enlarge_attribute()` drops `-ENOSPC` to `-1` → EPERM on create; fork `core/vfs/inode.c` still has it. U11: `ntfs_error()` on MFT bitmap/data allocation failure remounts the volume read-only; fork `core/ntfs/mft.c:1338,1770` still calls it — and a third site the first port pass missed: `ntfs_mft_record_alloc()`'s caller of `ntfs_mft_data_extend_allocation_nolock()` also reports `-ENOSPC` via `ntfs_error()` (caught by `test_enospc_names`'s mft variant, 2026-11-28: volume went read-only, ro_reason 7; upstream guards it with `if (err != -ENOSPC)`). Backport of upstream `5cd6692db272` + the ENOSPC bypass. Test `test_enospc_names` in `core/tests/test_faults.c`. |
| `b17eabb` | fix: stop a failed create from losing the next file given its record (finding 27) | **port** | U12: a failed `__ntfs_create` leaves `$EA`/`$EA_INFORMATION`/`$SECURITY_DESCRIPTOR`/`$DATA` attribute inodes cached, holding a reference on the freed record; the next sync writes the freed copy over the next file given the record. Fork `core/vfs/namei.c` `err_out` (line 860) has the journal abort + rollbacks but no eviction block. The eviction goes after `ntfs_delete_reparse_index(ni)`, before `ntfs_mft_record_free`. Test in `core/tests/test_faults.c`. |
| `9379a53` | test, docs: rebuild finding 26's test on genuinely full volumes | **with `dbde2ff`** | Test/docs companion of finding 26. |
| `bfd3eb1` | fix: give out freed MFT records on a full volume (finding 28) | **port** | `ntfs_mft_bitmap_find_and_alloc_free_rec_nolock()` does not reuse records freed earlier in the same pass; fork `core/ntfs/mft.c` has no `mft_end` handling. Test in `core/tests/test_faults.c`. |
| `4d50b7d` | fix: backport upstream's two cluster allocator fixes (U13, U14) | **port** | U13: restart the zone search (pass, zone end, `has_guess`) when the allocation hint fails — fork `core/ntfs/lcnalloc.c` keeps the stale state and can fail `-ENOSPC` with clusters free (846 names on a 2 MB root vs 1076 with the fix). U14: a contiguous request gets a single run, not a second one from the zone position. Backports of mainline `18d4d2a3ce75` + `b05a67615b98`. New test target `core/tests/test_alloc.c` (810 lines) — does not exist in the fork; create it. |
| `2254d30` | fix: bound the cluster bitmap scan to the volume (U15) | **already-covered** | Ported 2026-11-26 (round 10, with `fea9e4488f38`): fork `core/ntfs/lcnalloc.c:316` clamps `i_size`, `core/ntfs/runlist.c:914` rejects `lcn >= nr_clusters`. |
| `70e4666` | fix: count only the clusters actually freed (U16) | **already-covered** | Ported 2026-11-26: fork `core/ntfs/lcnalloc.c:66` only adds a run to `nr_freed` on success. Upstream's discriminating test (`test_free_with_unreadable_bitmap`) was not ported — the triage note records no public-harness route to it; porting it is a follow-up, not a gap in the fix. |
| `07a869a` | fix: do not count a failed rollback as freed clusters (U17) | **already-covered** | Ported 2026-11-26: fork `core/ntfs/lcnalloc.c:1069` decrements only when the rollback succeeded. No test reaches it (upstream says so too). |
| `cba67a4` | test: U11 is in; say what the names wall did before it | **with `dbde2ff`** | Test-comment companion of finding 26. |
| `085e3eb` | fix: zero a hole's new clusters around a write into it (finding 29) | **landed** | A write into a hole reads back a deleted file's data: `alloc_range()` fills the hole with new clusters but never zeroes the edges the write does not cover. Adds `zero_cluster()` + `zero_new_edges()` in `core/vfs/api.c`, exports `ntfs_vfs_map_vcn()` from `core/vfs/aops.c` (`static` in the fork). Tests in `core/tests/test_geometry.c`. Landed on branch `upstream-0.5.0-prep` (stream F). |
| `cb9995b` | fix: write a compression block to every run it was given (U18, finding 30) | **port** | `ntfs_write_cb()` writes a block in one bio from the first allocated cluster, so a block spanning two runs is written short. Adds `ntfs_write_cb_runs()` in `core/ntfs/compress.c`. Fork has `cb_is_allocated()` and the `eb67671cd814` ordering fix but no per-run writer. Tests in `core/tests/test_compress.c`. |
| `7dab337` | fix: give back a failed write's clusters in holes (finding 29) | **landed** | A failed write into a hole leaks the clusters `alloc_range()` took; the hole reads back the deleted file's data on the same mount. Builds on `085e3eb`'s `api.c` shape — port after it. Tests in `core/tests/test_geometry.c` + `core/tests/test_faults.c`. |
| `31eac87` | fix: roll back a cluster allocation whose mapping pairs fail (U20) | **landed** | `ntfs_attr_map_cluster()` leaks the allocation when the mapping-pairs update fails. Backport of mainline `91709ba5d6d7`. Note the U14 interaction, recorded in the patch: without U14 the allocator can return a second run after the reported one, so the rollback frees `alloc_count` (all runs), not just the reported run — port U13/U14 first. Builds on finding 29's `api.c` comment shape — port after `085e3eb`. Test in `core/tests/test_faults.c`. |
| `9ef95bf` | test: let the U20 case remount read-only (finding 33) | **with `31eac87`** | Test companion of U20. |
| `552f962` | docs, test: say what U18 and U13 changed where notes still said otherwise | **with `cb9995b` / `4d50b7d`** | Docs/test companion of U18 + U13. |
| `ab78590` | fix: keep a compressed block when its rewrite fails (U21, finding 32) | **port** | A failed rewrite of a compressed block loses the old block: `ntfs_write_cb_runs()` takes the run list instead of the attribute, so the old clusters are not the fallback. Reworks `cb9995b`'s helper signature — port after U18. Tests in `core/tests/test_compress.c` (12 checks fail without it). |
| `d437eed` | fix: swap a compressed block's runs in one mapping-pairs update (U22) | **port** | Punch-then-merge writes the mapping pairs twice; a failure between them leaves a hole where the block was (reads back zeroes, and the next mapping-pairs write puts the hole on disk). Adds `ntfs_write_cb_swap()` — swap in memory, write once, restore from a copy on failure. Same shape as mainline's `ntfs_attr_map_cluster()` since `91709ba5d6d7` (U20). Port after U18/U21. Tests in `core/tests/test_compress.c` (12 checks fail without it). |
| `74ec94a` | test: accept a read-only remount after a failed U22 rewrite | **with `d437eed`** | Test companion of U22. |
| `c445b4f` | test: say what U13 changed in fill_volume()'s comment | **with `4d50b7d`** | Test-comment companion of U13. |
| `4523d27` | fix: write nothing to a read-only volume at unmount (finding 33, U23) | **port** | Two halves. (a) Teardown: unmount of a read-only volume clears `VOLUME_IS_DIRTY`, so Windows never runs chkdsk on it; the fix writes nothing once teardown begins (`core/vfs/super_glue.c`, `platform/src/inode.c` evict change). Fork has neither. (b) Sync: mainline no longer clears the flag in a sync at all and persists it while errors are set; the fork already has the clear-guard (`0e4c83990541`, `core/ntfs/super.c:2003`) — the set-on-errors half is the gap. Tests in `core/tests/test_mount.c` (691 lines). |
| `3b43db9` | fix: stay read-only when discarding hibernation fails (finding 34) | **port** | On `ntfs_glue_zero_hiberfil()` failure the fork restores the errors flag and relies on "the decision below" to keep the volume read-only — but that decision does not switch it back itself, so the volume can go read-write while still hibernated. Fix: set the errors flag back *and* the volume read-only. `core/vfs/super_glue.c`. Tests in `core/tests/test_mount.c`. |
| `0e89ccc` | core+app: Mount Read-Write Anyway for a disk marked dirty | **needs-work** | New mount flag: clear `VOLUME_IS_DIRTY` and mount read-write when that flag is the one thing keeping the volume read-only, on an explicit user override. This is a safety-policy change, not a bug fix: writing before chkdsk can make existing damage worse, and clearing the flag stops Windows checking the volume. It also interacts with the fork's journal — a dirty volume's `$LogFile` is replayed at mount, and the override's interplay with replay + the fork's dirty-volume read-only rule (`docs/PORTING.md` §6) needs a design first. Do not port blindly; the decision is the parent's. Swift UI + `core/vfs/super_glue.c` + `core/include/ntfscore.h`. |
| `84a62a6` | docs: record what stays open after findings 33 and 34 | **with `4523d27` / `3b43db9`** | Docs companion of findings 33 + 34. |
| `845c8e0` | docs: what finding 33 changed for the notes about U22 | **with `4523d27` / `d437eed`** | Docs companion of finding 33 + U22. |
| `2f9e578` | test: skip the override's cases when there is no fixture | **with `0e89ccc`** | Test companion of the RW-anyway override. |
| `28ce712` | app: one quiet request for a GitHub star, and a link in Settings | **not-applicable** | Upstream app marketing; no driver effect. |
| `152f2cf` | site, README: a Star on GitHub button, and one line asking for a star | **not-applicable** | Upstream site marketing; no driver effect. |
| `c067d86` | repo: a social preview card, drawn from the app icon | **not-applicable** | Upstream repo marketing; no driver effect. |
| `60086c0` | fix: let the app turn the extension on itself on macOS 27 | **needs-work** | macOS 27 system-extension enabling. The fork targets macOS 26 (§4.3 covers below-26, not 27). Deferred to §4.3 macOS-version work; not a driver fix. |
| `681b049` | fix: keep the menu's enable state current after a change in System Settings | **needs-work** | macOS 27 enabling follow-up. Deferred to §4.3. |
| `34c3020` | fix: say plainly when the module list has an unknown format | **needs-work** | macOS 27 enabling follow-up. Deferred to §4.3. |
| `cf18a3d` | test: pin the macOS 27 enabling rules with unit tests | **needs-work** | macOS 27 enabling tests. Deferred to §4.3. |
| `c28f86c` | app: a once-a-day check for new versions, with a switch to turn it off | **not-applicable** | Upstream app feature; no driver effect. The fork does not ship an update channel. |
| `faa4bfa` | docs: test counts for 0.5.0 | **not-applicable** | Upstream release bookkeeping. |
| `99f49fd` | docs, site: macOS 27 tries the app's own route first | **needs-work** | macOS 27 enabling docs. Deferred to §4.3. |
| `8dc8f08` | app: a label that fits for the dirty-disk action | **with `0e89ccc`** | Label companion of the RW-anyway override. |
| `63a05c3` | app: no ellipsis in button labels | **not-applicable** | Upstream UI polish; no driver effect. |
| `92cc626` | release: 0.5.0 | **not-applicable** | Upstream release mechanics; the fork versions separately. |

## What the pin move needs

The 12 "port" items, in the order above. The 3 "already-covered"
items need nothing. The 6 "needs-work" items need decisions/designs
(the RW-anyway safety call is the parent's; the macOS 27 items belong
to §4.3). The 7 "not-applicable" items are dropped. When the 12 land,
the fork has absorbed the whole 0.5.0 delta and the merge is done.
Note: this does not move the Linux pin in `upstream/UPSTREAM.md` —
the pin tracks torvalds/linux `fs/ntfs`, and this delta is the
maintainer's own findings on top of v0.4.2, not Linux drift. The pin
stays at v7.1 until the Linux drift is triaged separately.
