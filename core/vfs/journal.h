/* SPDX-License-Identifier: GPL-2.0 */
/*
 * journal.h - journal glue for one user-visible operation.
 *
 * docs/JOURNAL-DESIGN.md Phase 2: the transaction API in core/logfile
 * stages records, but it knows nothing about the driver's in-memory
 * mutations. This layer bridges the two for file creation: it snapshots
 * each sub-mutation out of the mapped MFT records as __ntfs_create makes
 * it, stages the matching log record, and commits — which stamps MR_LSN
 * and opens the WAL gate — or aborts, which writes nothing.
 *
 * Lifetime: begin before the first mutation, stage as each sub-mutation
 * lands, commit after the last one, or abort on any error. Begin takes
 * vol->log_txn_lock and raises the WAL gate (vol->log_txn_open);
 * commit/abort lower the gate and release the lock. The lock serializes
 * whole transactions: the writer's append path and tables are shared
 * state with no internal lock, and two concurrent commits would
 * interleave appends.
 *
 * Only regular-file creates are journaled in Phase 2. Anything else —
 * journal inactive, mkdir/symlink/special — returns 0 with *out NULL
 * and proceeds unjournaled, exactly as before.
 */
#ifndef NTFS_JOURNAL_H
#define NTFS_JOURNAL_H

#include <linux/stat.h>

struct mft_record;
struct ntfs_attr_search_ctx;
struct ntfs_inode;
struct ntfs_volume;

struct ntfs_journal_txn;
struct ntfs_journal_unlink;

/* Begin a create transaction, or NULL (0) when there is nothing to
 * journal. On error the gate was never raised and the lock was never
 * taken; the caller proceeds unjournaled (or fails the create, its
 * choice — begin is called before any mutation, so either is safe). */
int ntfs_journal_create_begin(struct ntfs_volume *vol, mode_t mode,
			      struct ntfs_journal_txn **out);

/* Stage the Initialize record: the new record's bytes as they stand
 * after $STANDARD_INFORMATION was added (header + $SI, link 0). */
int ntfs_journal_create_init(struct ntfs_journal_txn *t,
			     struct ntfs_inode *ni,
			     const struct mft_record *mrec);

/* Stage a CreateAttribute record for the attribute just added to @ni's
 * record. Scans @mrec for (type, name, name_len) and stages it at its
 * true offset — attributes are inserted in type order, so the offset
 * must be read after the add, not assumed. For $FILE_NAME the staged
 * copy carries RESIDENT_ATTR_IS_INDEXED, which the live record does
 * not: the replay's link-count bump keys on it. Returns -ENOENT when
 * the attribute is not in the base record (it went to an extent);
 * the caller then aborts and proceeds unjournaled. */
int ntfs_journal_create_attr(struct ntfs_journal_txn *t,
			     struct ntfs_inode *ni,
			     const struct mft_record *mrec,
			     __le32 type, const __le16 *name, u8 name_len);

/* Stage CreateAttribute records for $EA_INFORMATION and $EA, net of
 * the three ntfs_set_ea() calls that built them: only the final values
 * are logged (the intermediate grows never reached the disk — the
 * flush happens after commit — so the redo lands the same final
 * state). Returns -ENOENT when either is missing; the caller then
 * aborts and proceeds unjournaled. */
int ntfs_journal_create_ea(struct ntfs_journal_txn *t,
			   struct ntfs_inode *ni,
			   const struct mft_record *mrec);

/* Stage the AddIndexEntryRoot record for the entry just inserted into
 * the parent directory @dir_ni's $INDEX_ROOT: scans the root for the
 * entry whose indexed_file is @child_mref and stages it. Returns
 * -ENOENT when the entry did not land in the root (directory root
 * full — the entry went to an $INDEX_ALLOCATION block, which Phase 2
 * cannot describe); the caller then aborts and proceeds unjournaled. */
int ntfs_journal_create_index(struct ntfs_journal_txn *t,
			      struct ntfs_inode *dir_ni,
			      const struct mft_record *dir_mrec,
			      u64 child_mref);

/* Stage the SetBits record for the allocated MFT record number. */
int ntfs_journal_create_bitmap(struct ntfs_journal_txn *t,
			       struct ntfs_volume *vol, u64 mft_no);

/* Stage the $MFT extension the record allocation just performed, if
 * any: a SetNewAttributeSizes record for record 0's $DATA (and, when
 * the allocation itself grew, an UpdateMappingPairs record before it),
 * plus a SetNewAttributeSizes record for record 0's $BITMAP when its
 * sizes grew. Called right after ntfs_mft_record_alloc, before any
 * other staging, so the redo order is: extend $MFT, then initialise
 * the new record. Returns 0 when nothing extended. Returns -ENOENT
 * when record 0 or either attribute cannot be read (the caller then
 * aborts and proceeds unjournaled — a create the log cannot describe
 * must never be half-journaled). */
int ntfs_journal_create_mft_sizes(struct ntfs_journal_txn *t);

/* Commit: append the staged records, the Commit record, a checkpoint
 * and the Forget record; stamp MR_LSN = the last LSN into the new
 * record and the parent record; lower the WAL gate; release the lock.
 * The stamp uses the transaction's last LSN for both records: it is >=
 * every record that touched either one, which is all the replay's
 * skip check (MR_LSN >= record LSN) needs, and it is the value the
 * next transaction's records will be skipped against.
 *
 * On a log-write failure the transaction stays ACTIVE in the log (its
 * records are on the log and the next replay will undo them — the
 * redo-then-undo sandwich: the redo pass applies the prefix first, so
 * an undo always sees the post-redo state, never the rolled-back disk
 * state) and the caller must roll the in-memory mutations back and
 * return the error. The gate is lowered and the lock released in all
 * cases, so the rollback writes can reach the disk. */
int ntfs_journal_create_commit(struct ntfs_journal_txn *t,
			       struct mft_record *mrec,
			       struct mft_record *parent_mrec);

/* Abort: write nothing, lower the WAL gate, release the lock. The
 * caller rolls the in-memory mutations back as before. */
void ntfs_journal_create_abort(struct ntfs_journal_txn *t);

/* ---- Unlink (Phase 3, first class) --------------------------------------
 *
 * An unlink is one transaction with two records per name removed, in
 * the order the driver makes the sub-mutations: DeleteIndexEntryRoot
 * for the entry in the parent's $INDEX_ROOT, then DeleteAttribute for
 * the $FILE_NAME in the target's record. The replay's DeleteAttribute
 * decrements the link count itself (it keys on the attribute's INDEXED
 * flag), so no separate record carries the count; its undo
 * (CreateAttribute) restores the attribute and the count, and the
 * index entry's undo (AddIndexEntryRoot) re-inserts it at the same
 * offset. A file with a DOS + WIN32 name pair stages two such pairs,
 * DOS first, exactly as ntfs_delete removes them.
 *
 * Lifetime mirrors the create transaction: begin before the first
 * mutation (it takes vol->log_txn_lock and raises the WAL gate),
 * stage at each sub-mutation inside ntfs_delete, commit after the
 * delete returns, or abort on any error. The staging helpers are
 * called from inside ntfs_delete, at the exact points where the
 * sub-mutation lands, so they see the same $FILE_NAME and search
 * context the delete itself uses — including its DOS/WIN32 pairing
 * and case handling — instead of a second search that could disagree
 * with it.
 *
 * Not journaled, by design: a target carrying a reparse point or an
 * object ID (ntfs_journal_unlink_check, -ENOENT) — ntfs_delete also
 * removes its $Extend index entry, which this class cannot describe
 * yet (that belongs to the reparse/EA class); the parent's $SI mtime
 * update after the delete (same known gap as the create path, the
 * setattr class's work — the WAL gate holds the directory record
 * until the commit, so the mtime can never reach the disk ahead of
 * the index removal); an index entry in an $INDEX_ALLOCATION block
 * or a $FILE_NAME outside the base record (-ENOENT from the staging
 * helpers). In every case the caller aborts and proceeds unjournaled,
 * exactly as the driver did before Phase 2.
 *
 * The record free IS part of this transaction: when the link count
 * hits 0, ntfs_delete stages a Deallocate record (redo clears
 * IN_USE and bumps the sequence number, undo restores the record
 * bytes) and a ClearBits record for the $MFT bitmap bit, for the
 * base record and each extent record, before the commit. The
 * physical free still runs at evict (iput, after the unlink
 * returns) — the log describes the intended end state, which the
 * replay applies only after a crash, when the file is no longer
 * open anywhere, so staging it at unlink time is safe for
 * unlink-while-open. Without it, a crash after the unlink commits
 * but before evict leaves a link-count-0 record still in use, and
 * the replay's DeleteAttribute leaves it with no $FILE_NAME —
 * ntfsck fails it ("Failed to find filename", 7 of 20 cut points
 * in test_journaled_unlink_cut_points even after the bitmap half
 * was journaled on its own). The data-cluster freeing stays
 * unjournaled (it belongs to the truncate/extend class).
 */
struct file_name_attr;

/* Begin an unlink transaction, or NULL (0) when there is nothing to
 * journal (no writer). On error the gate was never raised and the
 * lock was never taken; the caller proceeds unjournaled. */
int ntfs_journal_unlink_begin(struct ntfs_volume *vol,
			      struct ntfs_journal_unlink **out);

/* Abort: write nothing, lower the WAL gate, release the lock. */
void ntfs_journal_unlink_abort(struct ntfs_journal_unlink *u);

/* Gate check, called from inside ntfs_delete under the mrec locks
 * before anything mutates: 0 when the unlink can be journaled,
 * -ENOENT when the target carries a reparse point or an object ID
 * (its $Extend index removal is undescribable here) — the caller
 * aborts and proceeds unjournaled. Any other error is also -ENOENT
 * to the caller: a record that cannot be read cannot be journaled. */
int ntfs_journal_unlink_check(struct ntfs_journal_unlink *u,
			      struct ntfs_inode *ni);

/* Stage the DeleteIndexEntryRoot record for the index entry whose key
 * is the $FILE_NAME value @fn (@fn_len bytes) in @dir_ni's
 * $INDEX_ROOT. Returns -ENOENT when the entry is not in the root. */
int ntfs_journal_unlink_stage_index(struct ntfs_journal_unlink *u,
				    struct ntfs_inode *dir_ni,
				    const struct file_name_attr *fn,
				    u32 fn_len);

/* Stage the DeleteAttribute record for the attribute @actx points at
 * in @ni's record. Returns -ENOENT when it is not in the base record. */
int ntfs_journal_unlink_stage_attr(struct ntfs_journal_unlink *u,
				   struct ntfs_inode *ni,
				   struct ntfs_attr_search_ctx *actx);

/* Commit: append the staged records, the Commit record, a checkpoint
 * and the Forget record; stamp MR_LSN = the last LSN into the target
 * record and the parent record; lower the WAL gate; release the lock.
 * The stamp is the transaction's last LSN for both records, as in
 * the create commit.
 *
 * On a log-write failure the transaction stays ACTIVE in the log and
 * the caller must set the volume error flag (NVolSetErrors — the
 * established "metadata is suspect" signal) and return the error:
 * the in-memory unlink stands, but the next mount's replay undoes
 * the partial transaction and restores the file, which is the
 * correct outcome for a failed unlink. The gate is lowered and the
 * lock released in all cases. */
int ntfs_journal_unlink_commit(struct ntfs_journal_unlink *u,
			       struct ntfs_inode *ni,
			       struct ntfs_inode *dir_ni);

/* Stage the record free for the link-count-0 case: a Deallocate
 * record plus a ClearBits record for the $MFT bitmap bit, for the
 * base record (@mrec, in its post-delete state) and each extent
 * record. Call once, after the last $FILE_NAME removal, when the
 * link count has hit 0. Returns -ENOENT when a record cannot be
 * staged; the caller then aborts and proceeds unjournaled. */
int ntfs_journal_unlink_stage_free(struct ntfs_journal_unlink *u,
				   struct ntfs_inode *ni,
				   const struct mft_record *mrec);

/* ---- Rename (Phase 3, second class) -------------------------------------
 *
 * A rename is one transaction per __ntfs_vfs_rename call, in the order
 * the driver makes the sub-mutations: the target's removal (when
 * renaming over an existing file — the unlink records, including the
 * record free when its link count hits 0, staged by ntfs_delete
 * itself), then the link (a CreateAttribute for the new $FILE_NAME
 * plus an AddIndexEntryRoot for the new entry in the new parent's
 * $INDEX_ROOT), then the old name's removal (the unlink records
 * again). The replay's CreateAttribute bumps the link count on the
 * $FILE_NAME's INDEXED flag and its DeleteAttribute decrements it,
 * so the count goes 1 -> 2 -> 1 across the transaction and no
 * separate record carries it.
 *
 * The transaction reuses the unlink carrier (ntfs_journal_unlink
 * begin/abort) — it is just a log transaction with a WAL gate, and
 * the rename's delete steps ARE unlink steps, staged by ntfs_delete
 * through the same jtp. Only the link staging and the commit are
 * rename-specific: the commit stamps MR_LSN into every record the
 * rename touched (moved inode, old parent, new parent, replaced
 * target when there is one).
 *
 * The link staging matches by name and parent, not by first match:
 * a hard-linked inode carries several $FILE_NAME attributes, and a
 * cross-directory rename to the same name briefly holds two
 * $FILE_NAMEs with the same name and different parents — the new
 * one is the one with this name AND this parent. The $FILE_NAME is
 * found by its value's file_name and parent, and the index entry
 * by its key (the whole $FILE_NAME value), the same match the
 * unlink stage_index uses. Staging order is live order: the link
 * is staged after __ntfs_link, so its offsets are post-add
 * offsets, and the old-name delete is staged after that, so its
 * offsets are post-add too. The replay applies the records in log
 * order (add, then delete) and its Add/DeleteIndexEntryRoot apply
 * at the staged offset, so a same-directory rename replays to the
 * exact live layout — verified by the crash test, which sweeps the
 * same-directory case.
 *
 * Not journaled, by design: any step whose staging fails (-ENOENT
 * from a lookup miss — an entry in an $INDEX_ALLOCATION block, a
 * $FILE_NAME outside the base record, a reparse point or object ID
 * on either deleted name, via ntfs_delete's check) aborts the
 * transaction and the rename proceeds unjournaled, exactly as the
 * driver did before Phase 2. The $SI mtime/ctime updates the
 * rename makes (simple_rename_timestamp) are not journaled either
 * — the same known gap as the create path's parent $SI mtime, the
 * setattr class's work; the staged $FILE_NAME carries the
 * pre-rename timestamps, matching the live $FILE_NAME bytes, so
 * replay reproduces them exactly.
 *
 * One transaction per __ntfs_vfs_rename call: a case-only rename
 * goes through a temporary name in two calls, so it writes two
 * transactions. A crash between them leaves the file under the
 * temporary name — consistent and ntfsck-clean, and the same
 * outcome the unjournaled driver has always had there. Each step
 * is atomic; the two-step case is not yet one atomic rename.
 */

/* Stage the link half of a rename: a CreateAttribute for the new
 * $FILE_NAME (found by name in @ni's record) and an
 * AddIndexEntryRoot for the new entry (found by key in @dir_ni's
 * $INDEX_ROOT). Call after __ntfs_link succeeds. Returns -ENOENT
 * when either half cannot be found in the base record / root. */
int ntfs_journal_rename_stage_link(struct ntfs_journal_unlink *u,
				   struct ntfs_inode *ni,
				   struct ntfs_inode *dir_ni,
				   const __le16 *name, u8 name_len);

/* Commit: append the staged records, the Commit record, a checkpoint
 * and the Forget record; stamp MR_LSN = the last LSN into the moved
 * record, the old parent, the new parent and the replaced target
 * (@new_ni, NULL when there is none); lower the WAL gate; release
 * the lock. The stamp is the transaction's last LSN for every
 * record, as in the unlink commit.
 *
 * On a log-write failure the transaction stays ACTIVE in the log and
 * the caller must set the volume error flag (NVolSetErrors) and
 * return the error: the in-memory rename stands, but the next
 * mount's replay undoes the partial transaction and restores the
 * file under its old name, which is the correct outcome for a
 * failed rename. The gate is lowered and the lock released in all
 * cases. */
int ntfs_journal_rename_commit(struct ntfs_journal_unlink *u,
			       struct ntfs_inode *old_ni,
			       struct ntfs_inode *old_dir_ni,
			       struct ntfs_inode *new_dir_ni,
			       struct ntfs_inode *new_ni);

/* ---- Truncate/extend (Phase 3, fourth class) ----------------------------
 *
 * An explicit size change (truncate via setattr, one transaction per
 * ntfs_truncate_vfs call) is staged as the three things the design
 * doc names for an extend — mapping pairs plus sizes plus bitmap —
 * in redo order: the $Bitmap bit records first (the clusters the
 * resize freed or allocated), then the UpdateMappingPairs record
 * for the $DATA attribute's run list, then the SetNewAttributeSizes
 * record for its sizes. The undo side carries the old pairs, the
 * old sizes and the opposite bitmap bits, so a crash before the
 * commit restores the old size with the old clusters, and a crash
 * after it redoes the new size with the new clusters. File data is
 * never journaled, only metadata.
 *
 * Lifetime: begin before the resize (it takes vol->log_txn_lock and
 * raises the WAL gate, and snapshots the $DATA sizes, mapping pairs
 * and run list as they stand before the resize), stage once after
 * __ntfs_attr_truncate_vfs succeeds, commit, or abort on any error.
 * The snapshot is read from the live record at begin; the staging
 * reads the live record again after the resize and logs what moved
 * — nothing is reconstructed from a parallel model.
 *
 * Scope, step 1: non-resident $DATA of a regular file only.
 * Compressed and encrypted files are rejected before
 * ntfs_truncate_vfs is reached (ntfs_setattr_size), and a resident
 * $DATA, a $DATA in an extent record (attribute list), or a resize
 * that converts the attribute resident (shrink to zero calls
 * ntfs_attr_make_resident) aborts the transaction and proceeds
 * unjournaled — the resident path needs its own verification (the
 * replay's UpdateResidentValue handles length changes, but the
 * header fields it must also move have not been proven). The
 * parent index entry's duplicated $FILE_NAME (its data_size copy)
 * is NOT staged: it is refreshed by ntfs_inode_sync_filename at
 * the parent's writeback, outside this transaction's window — the
 * same class as the create path's parent $SI mtime gap, and the
 * WAL gate holds the file record until the commit, so the sizes
 * can never reach the disk ahead of the transaction. Extending
 * writes during writeback are also not journaled: they allocate
 * clusters without a user-visible transaction boundary, and
 * journaling them is a separate design problem (see GAPS.md §1.2).
 */

struct ntfs_journal_truncate;

/* Begin a truncate transaction, or NULL (0) when there is nothing to
 * journal: no writer, not a regular file, $DATA resident, in an
 * extent, compressed or encrypted. On error the gate was never raised
 * and the lock was never taken; the caller proceeds unjournaled. */
int ntfs_journal_truncate_begin(struct ntfs_volume *vol,
				struct ntfs_inode *ni,
				struct ntfs_journal_truncate **out);

/* Abort: write nothing, lower the WAL gate, release the lock. */
void ntfs_journal_truncate_abort(struct ntfs_journal_truncate *t);

/* Stage the resize's records after __ntfs_attr_truncate_vfs succeeds:
 * Set/ClearBits for the $Bitmap delta (old vs new run list, split at
 * bitmap-page boundaries), UpdateMappingPairs when the pairs changed,
 * SetNewAttributeSizes when the sizes changed. Returns -ENOENT when
 * the $DATA cannot be staged (it left the base record, or the resize
 * converted it resident); the caller then aborts and proceeds
 * unjournaled — the resize stands, it is just not logged. */
int ntfs_journal_truncate_stage(struct ntfs_journal_truncate *t,
				struct ntfs_inode *ni,
				const struct mft_record *mrec);

/* Commit: append the staged records, the Commit record, a checkpoint
 * and the Forget record; stamp MR_LSN = the last LSN into the file's
 * record; lower the WAL gate; release the lock. The stamp is the
 * transaction's last LSN, as in the other commits.
 *
 * On a log-write failure the transaction stays ACTIVE in the log and
 * the caller must set the volume error flag (NVolSetErrors) and
 * return the error: the in-memory resize stands, but the next
 * mount's replay undoes the partial transaction and restores the old
 * size with the old clusters, which is the correct outcome for a
 * failed truncate. The gate is lowered and the lock released in all
 * cases. */
int ntfs_journal_truncate_commit(struct ntfs_journal_truncate *t,
				 struct ntfs_inode *ni);

#endif /* NTFS_JOURNAL_H */
