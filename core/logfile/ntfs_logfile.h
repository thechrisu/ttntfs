/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * ntfs_logfile.h - NTFS $LogFile (LFS journal) parser, analyzer and replay.
 *
 * Standalone module: depends only on libc. The caller supplies the bytes of
 * $LogFile through ntfs_log_io and, for replay, access to MFT records and
 * clusters through ntfs_log_apply. The module never touches a device.
 *
 * Typical use at mount:
 *
 *   ntfs_logfile_open(&io, &geom, &ctx)      -> parse restart pages
 *   if (ntfs_logfile_is_clean(ctx)) { done }
 *   ntfs_logfile_replay(ctx, &apply, true)   -> dry run: analysis + plan
 *   if plan coherent: ntfs_logfile_replay(ctx, &apply, false)
 *   ntfs_logfile_mark_clean(ctx)             -> write clean restart pages
 *   ntfs_logfile_close(ctx)
 *
 * All functions return 0 or a negative errno. -EINVAL means the journal is
 * inconsistent (never apply), -ENOTSUP an unsupported version/feature.
 */
#ifndef NTFS_LOGFILE_H
#define NTFS_LOGFILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Input: the $LogFile bytes ---------------------------------------- */

struct ntfs_log_io {
	void *ctx;
	uint64_t size;			/* data size of $LogFile in bytes */
	/* Read @len bytes at @off. Must fill the buffer completely. */
	int (*read)(void *ctx, uint64_t off, void *buf, size_t len);
	/* Optional; needed by ntfs_logfile_mark_clean(). */
	int (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
};

/* Volume geometry the log math depends on. */
struct ntfs_log_geometry {
	uint32_t cluster_size;
	uint32_t mft_record_size;
	uint32_t sector_size;		/* 512 or 4096; log fixups always use 512 */
};

typedef struct ntfs_logfile ntfs_logfile_t;

enum ntfs_logfile_state {
	NTFS_LOG_EMPTY = 0,		/* all 0xff: freshly formatted, clean */
	NTFS_LOG_CLEAN,			/* closed, or open with VOLUME_IS_CLEAN */
	NTFS_LOG_DIRTY,			/* open, not clean: replay required */
	NTFS_LOG_CHKDSK,		/* restart page written by chkdsk */
	NTFS_LOG_CORRUPT,		/* no usable restart page */
	NTFS_LOG_UNSUPPORTED,		/* version we refuse to touch */
};

struct ntfs_logfile_info {
	int state;			/* enum ntfs_logfile_state */
	uint16_t major_ver, minor_ver;
	uint32_t system_page_size, log_page_size;
	uint64_t file_size;		/* usable size from the restart area */
	uint64_t current_lsn;		/* restart area's current lsn */
	uint64_t last_lsn;		/* true end of log after the tail scan (replay) */
	uint64_t oldest_lsn;
	uint64_t client_restart_lsn;	/* NTFS client's checkpoint record lsn */
	uint64_t checkpoint_lsn;	/* start of checkpoint from NTFS_RESTART */
	uint32_t seq_number_bits;
	uint32_t open_log_count;
	uint16_t restart_area_flags;
	uint16_t client_in_use, client_free;
	bool restart_page_used[2];	/* which of the two restart pages is current */
	bool volume_is_clean_flag;
	uint32_t table_transactions, table_dirty_pages, table_open_attrs;
	bool tail_scanned;		/* last_lsn/next_page reflect the page scan */
	uint32_t tail_overrides;	/* tail-copy pages that supersede on-disk pages */
	uint32_t next_page;		/* file offset of the next page the log would write */
	bool reuse_tail;		/* next_page still has room */
	uint32_t client_restart_version;/* NTFS_RESTART major version (0 or 1) */
	uint32_t attr_names_bytes;
};

int ntfs_logfile_open(const struct ntfs_log_io *io,
		      const struct ntfs_log_geometry *geom,
		      ntfs_logfile_t **out);
void ntfs_logfile_close(ntfs_logfile_t *log);
void ntfs_logfile_get_info(const ntfs_logfile_t *log, struct ntfs_logfile_info *info);

/* Same decision the Linux drivers make: true if the volume can be mounted
 * read-write without replay. Empty, closed, or open-with-clean-flag. */
bool ntfs_logfile_is_clean(const ntfs_logfile_t *log);

/* Last diagnostic message (static buffer inside @log). */
const char *ntfs_logfile_last_error(const ntfs_logfile_t *log);

/* Diagnostics sink. level: 0 error, 1 warn, 2 info, 3 debug. */
typedef void (*ntfs_log_msg_fn)(int level, const char *msg, void *ctx);
void ntfs_logfile_set_logger(ntfs_logfile_t *log, ntfs_log_msg_fn fn, void *ctx);

/* ---- Analysis --------------------------------------------------------- */

struct ntfs_log_record {
	uint64_t lsn;
	uint64_t client_prev_lsn;
	uint64_t client_undo_next_lsn;
	uint32_t transaction_id;	/* offset into transaction table */
	uint32_t record_type;		/* LFS_RECORD_TYPE_* */
	uint16_t flags;			/* LFS record header flags */
	uint32_t data_len;		/* client data length */
	/* NTFS client record (valid when record_type == client): */
	uint16_t redo_op, undo_op;
	uint16_t target_attr;		/* offset into open attribute table */
	uint16_t lcns_follow;
	uint16_t record_off, attr_off, cluster_off;
	uint64_t target_vcn;
	const uint64_t *page_lcns;	/* lcns_follow entries (little-endian) */
	const void *redo, *undo;
	uint16_t redo_len, undo_len;
	const void *raw;		/* client data; valid during callback */
	uint16_t client_seq, client_idx;/* LFS client id in the record header */
	uint32_t hdr_len;		/* NTFS client header incl. page_lcns */
};

/* Return 0 to continue, nonzero to stop enumeration. */
typedef int (*ntfs_log_record_cb)(const struct ntfs_log_record *rec, void *ctx);

/* Walk every record from @from_lsn (0 = the checkpoint / client restart
 * lsn) to the end of the log. Does not modify anything. Runs the tail scan
 * first so records written after the last restart-area flush are seen. */
int ntfs_logfile_walk(ntfs_logfile_t *log, uint64_t from_lsn,
		      ntfs_log_record_cb cb, void *ctx);

/* Human-readable op name. */
const char *ntfs_log_op_name(unsigned op);

/* Multi-sector transfer protection helpers (update sequence array), for
 * callers that hand raw MFT records / index blocks to the module. @sector
 * is the fixup stride (512 for NTFS). post_read returns -EINVAL for a
 * malformed array and sets *torn when a sector tail mismatches. */
int ntfs_log_fixup_post_read(void *rec, uint32_t bytes, uint32_t sector, bool *torn);
int ntfs_log_fixup_pre_write(void *rec, uint32_t bytes, uint32_t sector);

/*
 * Load the NTFS client restart record (checkpoint) and the three restart
 * tables it references, without walking the records after it. Runs the
 * tail scan first. Returns 0, -ENOENT when the client has no checkpoint
 * (nothing was ever logged), -EINVAL when the checkpoint is inconsistent.
 * ntfs_logfile_walk/replay call this implicitly.
 */
int ntfs_logfile_load_checkpoint(ntfs_logfile_t *log);

/* Decoded restart table entries. @id is the entry's byte offset inside its
 * table, which is how log records refer to it (transaction_id /
 * target_attr). Pointers are valid during the callback only. */
struct ntfs_log_transaction {
	uint32_t id;
	uint8_t state;			/* TRANSACTION_* (0 uninit, 1 active, 2 prepared, 3 committed) */
	uint64_t first_lsn, prev_lsn, undo_next_lsn;
	uint32_t undo_records, undo_bytes;
};
struct ntfs_log_dirty_page {
	uint32_t id;
	uint32_t target_attr, transfer_len, lcns_follow;
	uint64_t vcn, oldest_lsn;
	const uint64_t *page_lcns;	/* lcns_follow entries, little-endian */
};
struct ntfs_log_open_attr {
	uint32_t id;
	uint32_t type;			/* attribute type code */
	uint64_t mft_no;
	uint16_t mft_seq;
	uint32_t bytes_per_index;
	uint64_t open_record_lsn;
	uint8_t dirty_pages;
	uint16_t name_len;		/* UTF-16 units */
	const uint16_t *name;		/* from the attribute names dump; may be NULL */
};
struct ntfs_log_table_cbs {
	int (*transaction)(const struct ntfs_log_transaction *t, void *ctx);
	int (*dirty_page)(const struct ntfs_log_dirty_page *d, void *ctx);
	int (*open_attr)(const struct ntfs_log_open_attr *a, void *ctx);
};
/* Enumerate the tables as currently held: after
 * ntfs_logfile_load_checkpoint() these are the checkpoint dumps; after
 * ntfs_logfile_replay() they reflect the analysis pass. Any callback may be
 * NULL. A callback returning nonzero stops enumeration (returned). */
int ntfs_logfile_tables(ntfs_logfile_t *log, const struct ntfs_log_table_cbs *cbs, void *ctx);

/* ---- Replay ----------------------------------------------------------- */

/*
 * Access to the volume. Buffers carry raw on-disk bytes: MFT records and
 * index blocks are returned with their update sequence arrays applied
 * (protected), and must be written back protected. The module handles the
 * fixups. This keeps the in-memory test volume a flat byte array.
 */
struct ntfs_log_apply {
	void *ctx;
	int (*read_mft_record)(void *ctx, uint64_t mft_no, void *buf);
	int (*write_mft_record)(void *ctx, uint64_t mft_no, const void *buf);
	int (*read_clusters)(void *ctx, uint64_t lcn, uint32_t count, void *buf);
	int (*write_clusters)(void *ctx, uint64_t lcn, uint32_t count, const void *buf);
	/* Optional: called once after all writes. */
	int (*sync)(void *ctx);
};

enum ntfs_log_plan_kind {
	NTFS_PLAN_MFT_RECORD = 1,	/* write of one MFT record */
	NTFS_PLAN_CLUSTERS,		/* write of a cluster range */
	NTFS_PLAN_SKIPPED,		/* record examined, nothing to do */
};

struct ntfs_log_plan_entry {
	int kind;
	bool undo;			/* applied during the undo pass */
	uint64_t lsn;			/* log record that caused it */
	uint16_t op;
	uint64_t mft_no;		/* NTFS_PLAN_MFT_RECORD */
	uint64_t lcn;			/* NTFS_PLAN_CLUSTERS */
	uint32_t count;
	uint32_t target_attr;
	uint64_t target_vcn;
	char why[64];			/* for NTFS_PLAN_SKIPPED */
};

struct ntfs_log_replay_result {
	uint32_t records_analyzed;
	uint32_t records_redone;
	uint32_t records_undone;
	uint32_t transactions_active;	/* rolled back */
	uint32_t transactions_committed;
	uint32_t dirty_pages;
	uint32_t mft_records_written;
	uint32_t clusters_written;
	uint32_t mirror_records_healed;	/* $MFTMirr records 0-3 brought
					 * into agreement with $MFT by the
					 * flush (a crash between a record
					 * 0-3 writeback and its mirror bio
					 * leaves the mirror stale) */
	uint64_t redo_lsn;
	bool needs_chkdsk;		/* a record could not be applied safely */
	/*
	 * Every write landed and only the final device barrier failed. The two
	 * are very different: a failed write can leave the volume half-applied,
	 * while a failed barrier only means the device would not confirm what it
	 * has already accepted. The journal is deliberately NOT marked clean in
	 * either case, so the work is retried on the next attempt or by Windows.
	 * Added 2026-09-14, when a successful replay reported itself as possible
	 * corruption on a USB device whose every flush fails.
	 */
	bool flush_failed;
	/* Plan (dry run and real run). Owned by @log until close. */
	const struct ntfs_log_plan_entry *plan;
	uint32_t plan_len;
};

/*
 * Analysis pass, redo pass, undo pass. With @dry_run nothing is written:
 * all modifications go to an in-memory overlay and the plan describes them.
 * Without @dry_run the same happens and, only if every step succeeded and
 * needs_chkdsk is false, the overlay is flushed through @apply in order
 * (MFT records, then clusters), then sync().
 */
int ntfs_logfile_replay(ntfs_logfile_t *log, const struct ntfs_log_apply *apply,
			bool dry_run, struct ntfs_log_replay_result *result);

/*
 * Write both restart pages marking the log closed and clean (what Windows
 * writes at dismount), so the next mount needs no replay. Requires io->write.
 * Also rewrites any tail-copy page the scan decided supersedes an on-disk
 * page. Does not zero the log records (the driver may do that separately).
 */
int ntfs_logfile_mark_clean(ntfs_logfile_t *log);

/* ---- Writing (Phase 1: LFS writer infrastructure, docs/JOURNAL-DESIGN.md) ----
 *
 * The mirror image of the reader above: appends LFS records (v1.1 only)
 * to a $LogFile. A writer attaches to an opened log with
 * ntfs_log_writer_open(); append() writes a record's pages synchronously
 * (tail copy first when the page already holds durable content — see
 * logfile_write.c for the crash contract); sync() flushes the restart
 * area, which append() deliberately does not do per record (the tail
 * scan recovers the true end when it lags). The writer refuses to wrap
 * the circular area until a checkpoint exists (checkpoint emission is a
 * later commit in this phase) and enters a failed state on any io->write
 * error — further appends then return -EIO.
 *
 * ntfs_log_writer_format() lays two open v1.1 restart pages over an
 * all-0xff $LogFile (refuses one that already has a restart page);
 * a fresh log is then opened with ntfs_logfile_open() and written.
 * A clean (closed) log is reopened by writer_open(), which puts the
 * NTFS client back in use and clears the clean flag before any record.
 */
typedef struct ntfs_log_writer ntfs_log_writer_t;

int ntfs_log_writer_format(const struct ntfs_log_io *io,
			   const struct ntfs_log_geometry *geom, uint64_t size);
int ntfs_log_writer_open(ntfs_logfile_t *log, ntfs_log_writer_t **out);
int ntfs_log_writer_append(ntfs_log_writer_t *w, uint32_t record_type,
			   uint32_t transaction_id, uint64_t undo_next_lsn,
			   const void *data, uint32_t data_len, uint64_t *lsn_out);

/* ---- Live-volume handle (Phase 2: journal writing, docs/JOURNAL-DESIGN.md) ----
 *
 * A writer attached to a mounted volume: the image, the opened log and the
 * writer travel together, because the log io reads/writes through the image
 * (io->ctx) — the image must outlive every append. The device open/close
 * live in logfile_repair.c (next to ntfs_logfile_mark_clean_device); the
 * struct is here so the volume can hold it by pointer. The image is a
 * forward declaration only: this header stays libc-only, and the image
 * type is completed by ntfs_image.h where it is used.
 */
struct ntfs_image;
typedef struct ntfs_log_handle ntfs_log_handle_t;
struct ntfs_log_handle {
	struct ntfs_image *img;
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
};
int ntfs_log_writer_open_attr(ntfs_log_writer_t *w, uint32_t type,
			      uint64_t mft_no, uint16_t mft_seq,
			      const uint16_t *name, uint16_t name_len,
			      uint32_t *attr_id_out);
int ntfs_log_writer_checkpoint(ntfs_log_writer_t *w, uint64_t *lsn_out);
/* Phase 4 step 3: the default checkpoint policy — 128 commits between
 * checkpoints (docs/JOURNAL-DESIGN.md §4.3). The first commit always
 * checkpoints regardless, so a log never has no checkpoint to walk
 * from. Measured (test_checkpoint_policy_amplification): 6 creates
 * write 806,912 bytes at every=1 vs 585,728 at every=128 (27% less),
 * replay redoes all 42 data records at both. */
#define NTFS_LOG_CHECKPOINT_EVERY_DEFAULT 128
/* Phase 4 step 1: how often a commit carries a checkpoint. n == 0 clamps
 * to 1 (every commit, the Phase 2 behaviour). The first commit
 * always checkpoints regardless of this setting. */
int ntfs_log_writer_set_checkpoint_every(ntfs_log_writer_t *w, uint32_t n);
/* A flushed cluster range for ntfs_log_writer_delete_dirty_clusters(). */
struct ntfs_log_range { uint64_t lcn, len; };
/* Phase 4 step 2: retire dirty pages whose clusters reached the volume
 * (the flush notification, docs/JOURNAL-DESIGN.md §4.3). Appends one
 * DeleteDirtyClusters record carrying @ranges and zeroes those slots in
 * the writer's dirty-page table, dropping entries that become all-zero,
 * so the next checkpoint's dump omits them and first_lsn can advance.
 * Appends nothing when no slot matches (returns 0); never checkpoints
 * or retries — a refused append (-ENOSPC) leaves the table untouched
 * and the caller drops the notification. Caller contract: the ranges
 * are the clusters of one synchronous write that has completed, and a
 * range's lcn + len does not overflow. Takes only the writer lock. */
int ntfs_log_writer_delete_dirty_clusters(ntfs_log_writer_t *w,
					  const struct ntfs_log_range *ranges,
					  uint32_t nranges);
int ntfs_log_writer_close(ntfs_log_writer_t *w, uint64_t *lsn_out);
int ntfs_log_writer_sync(ntfs_log_writer_t *w);
void ntfs_log_writer_free(ntfs_log_writer_t *w);

/* ---- Transactions (Phase 2: journal writing, docs/JOURNAL-DESIGN.md) ----
 *
 * One user-visible operation (a file creation) as one transaction: the
 * ntfs_log_txn_*() builders stage NTFS client records in memory — each
 * describing one sub-mutation with its redo and its undo — and commit()
 * appends them in order, then a CommitTransaction record, then a
 * checkpoint when the periodic policy says so (Phase 4: every N commits,
 * and always on the first commit — replay walks forward from the last
 * checkpoint through every later record, so a committed transaction
 * without its own checkpoint still redoes; only a log with no checkpoint
 * at all replays nothing), then a ForgetTransaction record. abort()
 * writes nothing at all.
 *
 * Crash contract: the records are appended synchronously, one by one,
 * so a crash mid-commit leaves a transaction with records but no
 * CommitTransaction record — ACTIVE in the log — and the next replay's
 * undo pass rolls it back through the undo ops, leaving the volume as
 * if the operation never happened. A crash after the Commit record
 * leaves a committed transaction, and replay redoes it. A crash after
 * the checkpoint leaves the same, reached through the checkpoint's
 * tables. There is no instant at which a half-applied transaction
 * survives: every record carries its undo, and the commit point is
 * the single CommitTransaction record.
 *
 * The builders take the on-disk coordinates the replay engine checks
 * (docs/LOGFILE.md H1): @mft_attr/@bitmap_attr is the open-attribute
 * table offset of $MFT:$DATA or the bitmap attribute (registered with
 * ntfs_log_writer_open_attr()), @target_vcn the cluster vcn the
 * record's bytes live at, @cluster_off the byte offset of the record
 * inside that cluster in 512-byte units, and @page_lcn the LCN of the
 * cluster (from the attribute's run list). The builders register the
 * dirty page in the writer's dirty-page table, so the checkpoint
 * covers it. Commit and abort free the transaction; on commit failure
 * the transaction stays ACTIVE in the writer's table when any record
 * was appended (its records are on the log and will be rolled back by
 * the next replay), and its entry is freed when nothing was appended.
 */
typedef struct ntfs_log_txn ntfs_log_txn_t;

int ntfs_log_txn_begin(ntfs_log_writer_t *w, ntfs_log_txn_t **out);
int ntfs_log_txn_init_record(ntfs_log_txn_t *t, uint32_t mft_attr,
			     uint64_t target_vcn, uint16_t cluster_off,
			     uint64_t page_lcn, const void *rec, uint32_t rec_len);
int ntfs_log_txn_deallocate_record(ntfs_log_txn_t *t, uint32_t mft_attr,
				   uint64_t target_vcn, uint16_t cluster_off,
				   uint64_t page_lcn, const void *rec,
				   uint32_t rec_len);
int ntfs_log_txn_create_attr(ntfs_log_txn_t *t, uint32_t mft_attr,
			     uint64_t target_vcn, uint16_t cluster_off,
			     uint64_t page_lcn, uint16_t record_off,
			     const void *attr, uint32_t attr_len);
int ntfs_log_txn_add_index_root(ntfs_log_txn_t *t, uint32_t mft_attr,
				uint64_t target_vcn, uint16_t cluster_off,
				uint64_t page_lcn, uint16_t record_off,
				uint16_t attr_off, const void *entry,
				uint32_t entry_len);
/*
 * DeleteAttribute: the attribute at @record_off in the MFT record at
 * @target_vcn is removed on redo; the undo is CreateAttribute with the
 * attribute bytes in @attr, so an uncommitted unlink rolls the
 * $FILE_NAME back and the replay's link-count bump restores the count
 * the redo's DeleteAttribute decremented (it keys on the attribute's
 * INDEXED flag, which $FILE_NAME attributes carry). The redo payload
 * is empty: the replay reads the attribute's length from the record.
 */
int ntfs_log_txn_delete_attr(ntfs_log_txn_t *t, uint32_t mft_attr,
			     uint64_t target_vcn, uint16_t cluster_off,
			     uint64_t page_lcn, uint16_t record_off,
			     const void *attr, uint32_t attr_len);
/*
 * DeleteIndexEntryRoot: the index entry at @attr_off in the $INDEX_ROOT
 * attribute at @record_off is removed on redo; the undo is
 * AddIndexEntryRoot with the entry bytes in @entry, re-inserted at the
 * same offset. The redo payload is empty: the replay reads the entry's
 * length from the record.
 */
int ntfs_log_txn_delete_index_root(ntfs_log_txn_t *t, uint32_t mft_attr,
				   uint64_t target_vcn, uint16_t cluster_off,
				   uint64_t page_lcn, uint16_t record_off,
				   uint16_t attr_off, const void *entry,
				   uint32_t entry_len);
int ntfs_log_txn_update_resident(ntfs_log_txn_t *t, uint32_t mft_attr,
				 uint64_t target_vcn, uint16_t cluster_off,
				 uint64_t page_lcn, uint16_t record_off,
				 uint16_t attr_off, const void *new_val,
				 uint16_t new_len, const void *old_val,
				 uint16_t old_len);
int ntfs_log_txn_update_filename_root(ntfs_log_txn_t *t, uint32_t mft_attr,
				      uint64_t target_vcn, uint16_t cluster_off,
				      uint64_t page_lcn, uint16_t record_off,
				      uint16_t attr_off, const void *new_dup,
				      const void *old_dup);
int ntfs_log_txn_set_bitmap(ntfs_log_txn_t *t, uint32_t bitmap_attr,
			    uint64_t target_vcn, uint16_t cluster_off,
			    uint64_t page_lcn, uint32_t bit, uint32_t nbits,
			    bool set);
int ntfs_log_txn_set_attr_sizes(ntfs_log_txn_t *t, uint32_t mft_attr,
				uint64_t target_vcn, uint16_t cluster_off,
				uint64_t page_lcn, uint16_t record_off,
				uint64_t new_alloc, uint64_t new_valid,
				uint64_t new_data, uint64_t new_total,
				uint64_t old_alloc, uint64_t old_valid,
				uint64_t old_data, uint64_t old_total);
int ntfs_log_txn_update_mapping_pairs(ntfs_log_txn_t *t, uint32_t mft_attr,
				      uint64_t target_vcn, uint16_t cluster_off,
				      uint64_t page_lcn, uint16_t record_off,
				      uint16_t attr_off,
				      uint16_t new_flags, uint16_t new_name_off,
				      const void *new_pairs, uint16_t new_len,
				      uint16_t old_flags, uint16_t old_name_off,
				      uint16_t old_attr_off,
				      const void *old_pairs, uint16_t old_len);
int ntfs_log_txn_commit(ntfs_log_txn_t *t, uint64_t *first_lsn_out,
			uint64_t *last_lsn_out);
int ntfs_log_txn_abort(ntfs_log_txn_t *t);

/* True when no record was ever staged into @t. The setattr class uses
 * it for its no-op decision: a transaction that staged nothing is
 * aborted, not committed — committing is a log no-op (it writes
 * nothing and returns last_lsn 0), but a caller that stamped that 0
 * as MR_LSN would clear the record's stamp, so the empty case must
 * never reach a commit. */
bool ntfs_log_txn_is_empty(ntfs_log_txn_t *t);

#ifdef __cplusplus
}
#endif
#endif /* NTFS_LOGFILE_H */
