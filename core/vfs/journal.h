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
struct ntfs_inode;
struct ntfs_volume;

struct ntfs_journal_txn;

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

#endif /* NTFS_JOURNAL_H */
