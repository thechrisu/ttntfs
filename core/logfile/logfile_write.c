// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
/*
 * logfile_write.c - the $LogFile (LFS journal) WRITER.
 *
 * The mirror image of the replay engine: where logfile_open/pages/records/
 * tables parse restart pages, record pages and checkpoints, this file
 * produces them. Phase 1 of docs/JOURNAL-DESIGN.md: LFS infrastructure
 * only (append, LSN allocation, page sequencing, tail copies, restart-area
 * maintenance). No integration with the mutation paths yet (Phase 2) and
 * no checkpoint emission yet (a later commit in this phase).
 *
 * Layout the writer produces (v1.1 only; v2.0 logs are refused):
 * restart pages at 0 and 1, tail-copy pages at 2 and 3, record pages from
 * page 4, records as a 0x30-byte LFS header followed by client data that
 * may continue on the next page (from data_off) and wrap to the first
 * record page with the sequence number incremented. That is exactly the
 * layout the test builder in tests/test_logfile.c produces and the reader
 * accepts; the writer is held to the reader as oracle.
 *
 * Durability contract (docs/JOURNAL-DESIGN.md §0 — crash or disconnect at
 * any instant must leave a good state):
 *
 * - append() is synchronous: every page the record touches is written to
 *   the device before append() returns. The restart area is NOT flushed
 *   per record; it is flushed by sync() (and, later, checkpoint/close).
 *   A crash after append() but before sync() leaves the record on the
 *   device and a stale restart area, and the next open recovers the true
 *   end with the tail scan — that is what the tail scan is for.
 *   One exception, once per log lifetime: the FIRST record also flushes
 *   the restart area. The scan derives its expected sequence number from
 *   the restart area's current_lsn, and a current_lsn of 0 yields
 *   sequence 0 while real records carry sequence >= 1, so a log with
 *   records but a never-flushed restart area would be unreadable. The
 *   first append establishes the invariant "current_lsn == 0 iff no
 *   records exist"; later appends may lag, which the scan tolerates
 *   because the sequence still matches.
 * - Before a page that already holds durable record content is rewritten
 *   in place, a copy of the NEW page image is written to a tail-copy
 *   page. A crash between the two writes leaves either the old page
 *   (record absent, restart area does not point past it) or the tail
 *   copy (record present, the scan supersedes the page with it) — never
 *   a torn new page with no copy. A crash during the tail-copy write
 *   leaves a torn copy the scan ignores, and the old page stands.
 * - Pages are written single-page I/O (page_count = 1, page_position = 1,
 *   RESTART_SINGLE_PAGE_IO in the restart area), which is the layout the
 *   reader's tail scan validates; multi-page *records* still span pages
 *   through the continuation protocol, which is orthogonal to the
 *   transfer bookkeeping.
 * - A record is laid out and checked for wrap BEFORE any page is
 *   written, so a refused append (log full) writes nothing.
 * - Any io->write failure puts the writer into a failed state: further
 *   appends return -EIO. A log-write failure is a volume error, not a
 *   retryable operation failure — NTFS treats it the same way. The
 *   on-disk state is still either old-page or new-copy (see above), so
 *   the next open recovers cleanly; what the failure forbids is
 *   continuing to write on a device that would not take the log.
 * - Wrap is refused with -ENOSPC until a checkpoint exists: wrapping
 *   destroys records older than the wrap point, and without a checkpoint
 *   every record is live. Checkpoint emission (a later commit in this
 *   phase) lifts that refusal.
 *
 * What a crash at the worst instant leaves, per operation:
 * - mid-append, before the tail copy: old page, record absent.
 * - mid-append, after the tail copy, before the in-place page: tail
 *   copy, record present via the scan's override.
 * - mid-append, during the in-place page write: torn page; the scan
 *   stops there, the log ends at the previous record, the record is
 *   absent. (The tail copy, if it landed, supersedes instead.)
 * - mid-sync (restart pages): one restart page updated, one stale;
 *   both carry the same LSN, so either is a good state.
 */
#include "logfile_internal.h"

#define LW_PAGE_SIZE LFS_DEFAULT_PAGE_SIZE	/* 4096; v1.1 writer only */
#define LW_TAIL_SLOTS 2			/* v1.x keeps two tail copies */

struct ntfs_log_writer {
	ntfs_logfile_t *log;
	uint8_t *ra;			/* restart area copy, kept current */
	uint32_t ra_len;
	uint8_t *page_buf;		/* deprotected image of cur_page */
	uint32_t cur_page;		/* vbo of the page being appended to */
	uint32_t off;			/* next free byte offset in cur_page */
	uint64_t seq;			/* sequence number for new records */
	uint64_t prev_lsn;		/* client_prev_lsn for the next record */
	uint64_t last_lsn;		/* lsn of the last appended record */
	uint32_t last_len;		/* client_data_length of the last record */
	uint64_t first_lsn;		/* lsn of the oldest live record (0 none) */
	uint64_t checkpoint_lsn;	/* lsn of the last checkpoint (0 none) */
	uint16_t client_idx;
	uint16_t client_seq;
	uint32_t tail_slot;		/* next tail-copy page vbo */
	bool failed;
	bool checkpointed;
};

/* ---- helpers ----------------------------------------------------------- */

static uint32_t lw_bits(uint64_t v)
{
	uint32_t bits = 0;

	while (v) {
		v >>= 1;
		bits++;
	}
	return bits;
}

static void lw_page_init(uint8_t *p)
{
	memset(p, 0, LW_PAGE_SIZE);
	lf_put32(p + PG_MAGIC, LFS_MAGIC_RCRD);
	lf_put16(p + PG_USA_OFS, PG_USA);
	lf_put16(p + PG_USA_COUNT, LW_PAGE_SIZE / LFS_SECTOR_SIZE + 1);
	lf_put16(p + PG_PAGE_COUNT, 1);
	lf_put16(p + PG_PAGE_POSITION, 1);
	lf_put16(p + PG_NEXT_RECORD_OFFSET, (uint16_t)LF_ALIGN8(PG_USA + (LW_PAGE_SIZE / LFS_SECTOR_SIZE + 1) * 2));
}

/*
 * Build one restart page (deprotected) from @ra at @ra_ofs. The header
 * layout mirrors lb_restart() in the tests and mark_clean(): v1.1, XP+
 * client array at 0x40.
 */
static void lw_restart_page(uint8_t *page, const uint8_t *ra, uint32_t ra_ofs, uint16_t usn)
{
	memset(page, 0, LW_PAGE_SIZE);
	lf_put32(page + RP_MAGIC, LFS_MAGIC_RSTR);
	lf_put16(page + RP_USA_OFS, RP_HEADER_SIZE);
	lf_put16(page + RP_USA_COUNT, LW_PAGE_SIZE / LFS_SECTOR_SIZE + 1);
	lf_put32(page + RP_SYSTEM_PAGE_SIZE, LW_PAGE_SIZE);
	lf_put32(page + RP_LOG_PAGE_SIZE, LW_PAGE_SIZE);
	lf_put16(page + RP_RESTART_AREA_OFFSET, (uint16_t)ra_ofs);
	lf_put16(page + RP_MINOR_VER, 1);
	lf_put16(page + RP_MAJOR_VER, 1);
	memcpy(page + ra_ofs, ra, RA_SIZE_XP + CR_SIZE);
	lf_put16(page + RP_HEADER_SIZE, usn);
}

static int lw_write_page(ntfs_logfile_t *log, uint32_t vbo, const uint8_t *page)
{
	uint8_t *copy = malloc(log->page_size);
	int err;

	if (!copy)
		return -ENOMEM;
	memcpy(copy, page, log->page_size);
	err = lfs_fixup_pre_write(copy, log->page_size, LFS_SECTOR_SIZE);
	if (err) {
		free(copy);
		return err;
	}
	err = log->io.write(log->io.ctx, vbo, copy, log->page_size);
	free(copy);
	if (err)
		return err < 0 ? err : -EIO;
	return 0;
}

/* ---- format ------------------------------------------------------------ */

/*
 * Lay down two v1.1 restart pages (open, NTFS client, no records yet)
 * over an all-0xff $LogFile. The record area is left 0xff; the first
 * append fills it. Refuses a log that already has a restart page.
 */
int ntfs_log_writer_format(const struct ntfs_log_io *io, const struct ntfs_log_geometry *geom,
			   uint64_t size)
{
	uint8_t head[LFS_SECTOR_SIZE];
	uint8_t *page, *ra, *cr;
	uint32_t ra_ofs, ca_ofs = RA_SIZE_XP, seq_bits;
	int err, i;

	if (!io || !io->write || !geom || !geom->cluster_size || !geom->mft_record_size)
		return -EINVAL;
	if (!size || size > io->size || (size & (LW_PAGE_SIZE - 1)) ||
	    size < (LFS_MIN_RECORD_PAGES + 2) * LW_PAGE_SIZE)
		return -EINVAL;
	/* A restart page already present means this log is not fresh. */
	err = io->read(io->ctx, 0, head, sizeof(head));
	if (err)
		return err < 0 ? err : -EIO;
	if (lf_get32(head) == LFS_MAGIC_RSTR || lf_get32(head) == LFS_MAGIC_CHKD)
		return -EEXIST;

	page = calloc(1, LW_PAGE_SIZE);
	if (!page)
		return -ENOMEM;
	ra_ofs = LF_ALIGN8(RP_HEADER_SIZE + (LW_PAGE_SIZE / LFS_SECTOR_SIZE + 1) * 2);
	/* seq_number_bits = 67 - bits(file_size), as the reader validates. */
	seq_bits = 67 - lw_bits(size);

	ra = page + ra_ofs;
	lf_put64(ra + RA_CURRENT_LSN, 0);
	lf_put16(ra + RA_LOG_CLIENTS, 1);
	lf_put16(ra + RA_CLIENT_FREE_LIST, LFS_NO_CLIENT);
	lf_put16(ra + RA_CLIENT_IN_USE_LIST, 0);
	lf_put16(ra + RA_FLAGS, RESTART_SINGLE_PAGE_IO);
	lf_put32(ra + RA_SEQ_NUMBER_BITS, seq_bits);
	lf_put16(ra + RA_RESTART_AREA_LENGTH, (uint16_t)(ca_ofs + CR_SIZE));
	lf_put16(ra + RA_CLIENT_ARRAY_OFFSET, (uint16_t)ca_ofs);
	lf_put64(ra + RA_FILE_SIZE, size);
	lf_put32(ra + RA_LAST_LSN_DATA_LENGTH, 0);
	lf_put16(ra + RA_LOG_RECORD_HEADER_LENGTH, LR_HEADER_SIZE);
	lf_put16(ra + RA_LOG_PAGE_DATA_OFFSET, (uint16_t)LF_ALIGN8(PG_USA + (LW_PAGE_SIZE / LFS_SECTOR_SIZE + 1) * 2));
	lf_put32(ra + RA_RESTART_LOG_OPEN_COUNT, 1);

	cr = ra + ca_ofs;
	lf_put64(cr + CR_OLDEST_LSN, 0);
	lf_put64(cr + CR_CLIENT_RESTART_LSN, 0);
	lf_put16(cr + CR_PREV_CLIENT, LFS_NO_CLIENT);
	lf_put16(cr + CR_NEXT_CLIENT, LFS_NO_CLIENT);
	lf_put16(cr + CR_SEQ_NUMBER, 1);
	lf_put32(cr + CR_CLIENT_NAME_LENGTH, 8);
	lf_put16(cr + CR_CLIENT_NAME + 0, 'N');
	lf_put16(cr + CR_CLIENT_NAME + 2, 'T');
	lf_put16(cr + CR_CLIENT_NAME + 4, 'F');
	lf_put16(cr + CR_CLIENT_NAME + 6, 'S');

	for (i = 0; i < 2; i++) {
		uint8_t *copy = malloc(LW_PAGE_SIZE);

		if (!copy) {
			free(page);
			return -ENOMEM;
		}
		/* Fresh usa value per page, as mark_clean does. */
		lf_put32(page + RP_MAGIC, LFS_MAGIC_RSTR);
		lf_put16(page + RP_USA_OFS, RP_HEADER_SIZE);
		lf_put16(page + RP_USA_COUNT, LW_PAGE_SIZE / LFS_SECTOR_SIZE + 1);
		lf_put32(page + RP_SYSTEM_PAGE_SIZE, LW_PAGE_SIZE);
		lf_put32(page + RP_LOG_PAGE_SIZE, LW_PAGE_SIZE);
		lf_put16(page + RP_RESTART_AREA_OFFSET, (uint16_t)ra_ofs);
		lf_put16(page + RP_MINOR_VER, 1);
		lf_put16(page + RP_MAJOR_VER, 1);
		lf_put16(page + RP_HEADER_SIZE, (uint16_t)(0x100 + i));
		memcpy(copy, page, LW_PAGE_SIZE);
		err = lfs_fixup_pre_write(copy, LW_PAGE_SIZE, LFS_SECTOR_SIZE);
		if (!err)
			err = io->write(io->ctx, (uint64_t)i * LW_PAGE_SIZE, copy, LW_PAGE_SIZE);
		free(copy);
		if (err) {
			free(page);
			return err < 0 ? err : -EIO;
		}
	}
	free(page);
	return 0;
}

/* ---- open -------------------------------------------------------------- */

/* Find the NTFS client record in the restart-area client array by name. */
static int lw_find_client(ntfs_logfile_t *log, uint16_t *idx_out)
{
	const uint8_t *ra = log->ra;
	uint16_t ca_ofs = lf_get16(ra + RA_CLIENT_ARRAY_OFFSET);
	uint16_t nr = lf_get16(ra + RA_LOG_CLIENTS);
	uint16_t i;

	for (i = 0; i < nr; i++) {
		const uint8_t *cr = ra + ca_ofs + (size_t)i * CR_SIZE;

		if (lf_get32(cr + CR_CLIENT_NAME_LENGTH) == 8 &&
		    lf_get16(cr + CR_CLIENT_NAME + 0) == 'N' && lf_get16(cr + CR_CLIENT_NAME + 2) == 'T' &&
		    lf_get16(cr + CR_CLIENT_NAME + 4) == 'F' && lf_get16(cr + CR_CLIENT_NAME + 6) == 'S') {
			*idx_out = i;
			return 0;
		}
	}
	return -ENOENT;
}

int ntfs_log_writer_open(ntfs_logfile_t *log, ntfs_log_writer_t **out)
{
	ntfs_log_writer_t *w;
	int err;

	*out = NULL;
	if (!log || !log->io.write)
		return -ENOTSUP;
	if (log->major_ver >= 2)
		return -ENOTSUP;	/* v1.1 writer only in Phase 1 */
	if (log->state == NTFS_LOG_EMPTY || !log->ra)
		return -ENOENT;	/* format first */
	if (log->state == NTFS_LOG_CORRUPT || log->state == NTFS_LOG_UNSUPPORTED)
		return -EINVAL;
	if (log->page_size != LW_PAGE_SIZE)
		return -ENOTSUP;	/* writer assumes 4096-byte pages */

	/* Recover the true end first: the restart area may lag the pages. */
	err = lfs_tail_scan(log);
	if (err)
		return err;

	w = calloc(1, sizeof(*w));
	if (!w)
		return -ENOMEM;
	w->log = log;
	w->ra_len = log->ra_len;
	w->ra = malloc(w->ra_len);
	w->page_buf = malloc(log->page_size);
	if (!w->ra || !w->page_buf) {
		free(w->ra);
		free(w->page_buf);
		free(w);
		return -ENOMEM;
	}
	memcpy(w->ra, log->ra, w->ra_len);

	err = lw_find_client(log, &w->client_idx);
	if (err) {
		free(w->ra);
		free(w->page_buf);
		free(w);
		return err;
	}
	{
		uint16_t ca_ofs = lf_get16(w->ra + RA_CLIENT_ARRAY_OFFSET);
		const uint8_t *cr = w->ra + ca_ofs + (size_t)w->client_idx * CR_SIZE;

		w->client_seq = lf_get16(cr + CR_SEQ_NUMBER);
		w->checkpoint_lsn = lf_get64(cr + CR_CLIENT_RESTART_LSN);
		w->checkpointed = w->checkpoint_lsn != 0;
		w->first_lsn = lf_get64(cr + CR_OLDEST_LSN);
	}
	w->seq = log->seq_num;
	if (!log->current_lsn)
		w->seq = 1;	/* sequence 0 is never written */
	w->prev_lsn = log->last_lsn;
	w->last_lsn = log->last_lsn;
	w->last_len = lf_get32(w->ra + RA_LAST_LSN_DATA_LENGTH);
	w->tail_slot = 2 * log->page_size;

	/*
	 * Reopen a clean (closed) log: put the client back in use and clear
	 * the clean flag, so appends after this point belong to this session.
	 * The restart pages go out now, before any record, so a crash right
	 * after open leaves an open log with no new records — a good state.
	 */
	if (lf_get16(w->ra + RA_CLIENT_IN_USE_LIST) == LFS_NO_CLIENT) {
		lf_put16(w->ra + RA_CLIENT_IN_USE_LIST, w->client_idx);
		lf_put16(w->ra + RA_CLIENT_FREE_LIST, LFS_NO_CLIENT);
		lf_put16(w->ra + RA_FLAGS, lf_get16(w->ra + RA_FLAGS) & ~RESTART_VOLUME_IS_CLEAN);
		err = ntfs_log_writer_sync(w);
		if (err) {
			free(w->ra);
			free(w->page_buf);
			free(w);
			return err;
		}
	}

	/* Position the append cursor. */
	w->cur_page = log->next_page;
	if (log->reuse_tail) {
		bool torn = false;

		err = lfs_read_page(log, w->cur_page, w->page_buf, &torn);
		if (err || torn) {
			/* The partial page is unreadable: the scan already
			 * truncated the log before it, so start it fresh. */
			lw_page_init(w->page_buf);
			w->off = log->data_off;
		} else {
			w->off = lf_get16(w->page_buf + PG_NEXT_RECORD_OFFSET);
			if (w->off < log->data_off || w->off >= log->page_size) {
				lw_page_init(w->page_buf);
				w->off = log->data_off;
			}
		}
	} else {
		lw_page_init(w->page_buf);
		w->off = log->data_off;
	}
	*out = w;
	return 0;
}

void ntfs_log_writer_free(ntfs_log_writer_t *w)
{
	if (!w)
		return;
	free(w->ra);
	free(w->page_buf);
	free(w);
}

/* ---- sync: flush the restart area -------------------------------------- */

/*
 * Write both restart pages from the writer's in-memory restart area.
 * The area is updated last, after the record pages it describes are
 * durable: append() updates the in-memory copy only, sync() writes it.
 * Both pages carry the same LSN, so a crash between the two writes
 * leaves either page as a good state.
 */
int ntfs_log_writer_sync(ntfs_log_writer_t *w)
{
	ntfs_logfile_t *log = w->log;
	uint8_t *page;
	uint32_t ra_ofs = log->ra_ofs;
	int err, i;

	if (w->failed)
		return -EIO;
	lf_put64(w->ra + RA_CURRENT_LSN, w->last_lsn);
	lf_put32(w->ra + RA_LAST_LSN_DATA_LENGTH, w->last_len);
	{
		uint16_t ca_ofs = lf_get16(w->ra + RA_CLIENT_ARRAY_OFFSET);
		uint8_t *cr = w->ra + ca_ofs + (size_t)w->client_idx * CR_SIZE;

		lf_put64(cr + CR_OLDEST_LSN, w->first_lsn);
		lf_put64(cr + CR_CLIENT_RESTART_LSN, w->checkpoint_lsn);
		lf_put16(cr + CR_SEQ_NUMBER, w->client_seq);
	}

	page = malloc(log->page_size);
	if (!page)
		return -ENOMEM;
	for (i = 0; i < 2; i++) {
		lw_restart_page(page, w->ra, ra_ofs, (uint16_t)(0x1000 + i + (log->open_log_count & 0xff)));
		err = lw_write_page(log, (uint32_t)i * log->page_size, page);
		if (err) {
			w->failed = true;
			free(page);
			return err;
		}
	}
	free(page);
	log->current_lsn = w->last_lsn;
	log->last_lsn = w->last_lsn;
	return 0;
}

/* ---- append ------------------------------------------------------------ */

/*
 * Write the deprotected page image in @w->page_buf to @vbo. When the
 * on-disk page already holds record content, a copy of the NEW image
 * goes to a tail-copy page first (see the file header).
 */
static int lw_flush_page(ntfs_log_writer_t *w, uint32_t vbo)
{
	ntfs_logfile_t *log = w->log;
	uint8_t *disk = malloc(log->page_size);
	int err;

	if (!disk)
		return -ENOMEM;
	err = log->io.read(log->io.ctx, vbo, disk, log->page_size);
	if (!err && lf_get32(disk) == LFS_MAGIC_RCRD) {
		uint8_t *copy = malloc(log->page_size);

		if (!copy) {
			free(disk);
			return -ENOMEM;
		}
		memcpy(copy, w->page_buf, log->page_size);
		/* v1.x tail copies carry the duplicated page's file offset
		 * where last_lsn belongs; the scan only trusts copies that
		 * end a record, which this image does when a record ends
		 * on the page. */
		lf_put64(copy + PG_LAST_LSN, vbo);
		lf_put16(copy + PG_PAGE_COUNT, 1);
		lf_put16(copy + PG_PAGE_POSITION, 1);
		err = lw_write_page(log, w->tail_slot, copy);
		free(copy);
		if (err) {
			free(disk);
			return err;
		}
		w->tail_slot = w->tail_slot == 2 * log->page_size ?
			3 * log->page_size : 2 * log->page_size;
	}
	free(disk);
	if (err)
		return err;
	return lw_write_page(log, vbo, w->page_buf);
}

/* Does laying out @total bytes from (page, off) cross the end of file? */
static bool lw_would_wrap(ntfs_logfile_t *log, uint32_t page, uint32_t off, uint32_t total)
{
	uint32_t left = total;

	for (;;) {
		uint32_t room = log->page_size - off;

		if (left <= room)
			return false;
		left -= room;
		page = lfs_next_page_off(log, page);
		if (page == log->first_page)
			return true;
		off = log->data_off;
	}
}

int ntfs_log_writer_append(ntfs_log_writer_t *w, uint32_t record_type, uint32_t transaction_id,
			   uint64_t undo_next_lsn, const void *data, uint32_t data_len,
			   uint64_t *lsn_out)
{
	ntfs_logfile_t *log = w->log;
	uint8_t *stage;
	uint32_t total, left, pos, room, n;
	uint64_t lsn, seq = w->seq;
	uint32_t page = w->cur_page;
	uint32_t off = w->off;
	bool multi;
	int err;

	if (w->failed)
		return -EIO;
	if (record_type != LFS_RECORD_TYPE_CLIENT && record_type != LFS_RECORD_TYPE_CLIENT_RESTART)
		return -EINVAL;
	if (!data && data_len)
		return -EINVAL;
	if (data_len > 0x00ffffffu)
		return -EINVAL;
	total = LR_HEADER_SIZE + data_len;
	/* The record must fit in the log area with a page to spare, and must
	 * not wrap without a checkpoint (see the file header). */
	if ((uint64_t)total > (uint64_t)(log->l_size - log->first_page) - log->page_size)
		return -ENOSPC;
	if (off + LR_HEADER_SIZE > log->page_size) {
		page = lfs_next_page_off(log, page);
		if (page == log->first_page)
			seq++;
		off = log->data_off;
	}
	/* Pre-check the wrap with the adjusted start: a refused append
	 * writes nothing (the layout below only runs after this passes). */
	if (lw_would_wrap(log, page, off, total) && !w->checkpointed)
		return -ENOSPC;
	multi = off + total > log->page_size;

	stage = malloc(total);
	if (!stage)
		return -ENOMEM;
	lsn = lfs_vbo_to_lsn(log, page + off, seq);
	memset(stage, 0, LR_HEADER_SIZE);
	lf_put64(stage + LR_THIS_LSN, lsn);
	lf_put64(stage + LR_CLIENT_PREV_LSN, w->prev_lsn);
	lf_put64(stage + LR_CLIENT_UNDO_NEXT_LSN, undo_next_lsn);
	lf_put32(stage + LR_CLIENT_DATA_LENGTH, data_len);
	lf_put16(stage + LR_CLIENT_SEQ_NUMBER, w->client_seq);
	lf_put16(stage + LR_CLIENT_INDEX, w->client_idx);
	lf_put32(stage + LR_RECORD_TYPE, record_type);
	lf_put32(stage + LR_TRANSACTION_ID, transaction_id);
	lf_put16(stage + LR_FLAGS, multi ? LFS_RECORD_MULTI_PAGE : 0);
	if (data_len)
		memcpy(stage + LR_HEADER_SIZE, data, data_len);

	/* Lay the staged record out page by page, flushing as we go. */
	left = total;
	pos = off;
	for (;;) {
		room = log->page_size - pos;
		n = left < room ? left : room;
		memcpy(w->page_buf + pos, stage + (total - left), n);
		/* Every page the record touches carries its lsn as last_lsn. */
		lf_put64(w->page_buf + PG_LAST_LSN, lsn);
		pos += n;
		left -= n;
		if (!left) {
			uint32_t end = LF_ALIGN8(pos);

			lf_put32(w->page_buf + PG_FLAGS, LFS_PAGE_LOG_RECORD_END);
			lf_put64(w->page_buf + PG_LAST_END_LSN, lsn);
			lf_put16(w->page_buf + PG_NEXT_RECORD_OFFSET,
				 (uint16_t)(end > log->page_size ? log->page_size : end));
			err = lw_flush_page(w, page);
			if (err)
				break;
			w->cur_page = page;
			w->off = end > log->page_size ? log->page_size : end;
			break;
		}
		/* The record continues: this page is full, no record ends here. */
		lf_put32(w->page_buf + PG_FLAGS, 0);
		lf_put16(w->page_buf + PG_NEXT_RECORD_OFFSET, (uint16_t)log->page_size);
		err = lw_flush_page(w, page);
		if (err)
			break;
		page = lfs_next_page_off(log, page);
		if (page == log->first_page)
			seq++;
		lw_page_init(w->page_buf);
		pos = log->data_off;
	}
	free(stage);
	if (err) {
		w->failed = true;
		return err;
	}
	w->seq = seq;
	{
		bool first = w->last_lsn == 0;

		w->prev_lsn = lsn;
		w->last_lsn = lsn;
		w->last_len = data_len;
		if (!w->first_lsn)
			w->first_lsn = lsn;
		log->current_lsn = lsn;
		log->last_lsn = lsn;
		if (first) {
			/* Establish "current_lsn == 0 iff no records":
			 * the scan cannot recover records the restart area
			 * never heard of (see the file header). */
			err = ntfs_log_writer_sync(w);
			if (err)
				return err;
		}
	}
	if (lsn_out)
		*lsn_out = lsn;
	return 0;
}
