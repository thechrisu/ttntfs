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
 * The evict-time deallocation (freeing the data clusters, clearing
 * the $MFT bitmap bit, deallocating the record) is NOT part of this
 * transaction: it runs at iput, after the unlink returns, and stays
 * unjournaled for now. A crash after the unlink commits but before
 * evict leaves a link-count-0 record still in use — the crash tests
 * pin down whether ntfsck accepts that state; if it does not, the
 * deallocation gets its own transaction in a follow-up commit.
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

#endif /* NTFS_JOURNAL_H */
