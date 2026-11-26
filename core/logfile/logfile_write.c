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
 * - A record is refused with -ENOSPC when it would overwrite the
 *   oldest live page: the layout may use only the free pages, those
 *   strictly after the cursor up to (not including) the oldest live
 *   page. A wrap around the end of the file is allowed when it stays
 *   inside that range — which is what a checkpoint makes possible, by
 *   advancing the oldest live LSN past the records it covers (before
 *   any checkpoint the oldest record sits on the first page, so no
 *   wrap is possible). The wrap itself needs no first_lsn advance:
 *   the pages it overwrites held only dispensable records.
 * - Appends keep a checkpoint reserve: a record is refused with
 *   -ENOSPC if it would leave fewer free pages than a checkpoint needs
 *   (one page per non-empty table dump, one for the restart record,
 *   one of margin). Without the reserve a full log could not be
 *   checkpointed at all, and the checkpoint is what frees the log.
 *   Checkpoint appends themselves are exempt from the reserve.
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
	bool in_checkpoint;		/* checkpoint appends: reserve-exempt */

	/* Restart tables, maintained in memory and dumped by checkpoint().
	 * Phase 1: only the open-attribute table is populated (by
	 * ntfs_log_writer_open_attr()); the transaction and dirty-page
	 * tables gain entries in Phase 2. A table with no allocated
	 * entries is not dumped at all (the reader skips a zero length). */
	struct lfs_table trtbl, dptbl, oatbl;
	uint8_t *attr_names;		/* attribute-names dump blob */
	uint32_t attr_names_len, attr_names_cap;
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

/* Copy the last checkpoint's tables into the writer, if one exists. */
static int lw_adopt_checkpoint(ntfs_log_writer_t *w, ntfs_logfile_t *log)
{
	int err = lfs_load_checkpoint(log);

	if (err && err != -ENOENT)
		return err;
	if (err)
		return 0;
	if (log->trtbl.buf) {
		err = lfs_table_copy(&w->trtbl, log->trtbl.buf, log->trtbl.bytes);
		if (err)
			return err;
	}
	if (log->dptbl.buf) {
		err = lfs_table_copy(&w->dptbl, log->dptbl.buf, log->dptbl.bytes);
		if (err)
			return err;
	}
	if (log->oatbl.buf) {
		err = lfs_table_copy(&w->oatbl, log->oatbl.buf, log->oatbl.bytes);
		if (err)
			return err;
	}
	if (log->attr_names_len) {
		w->attr_names = malloc(log->attr_names_len);
		if (!w->attr_names)
			return -ENOMEM;
		memcpy(w->attr_names, log->attr_names, log->attr_names_len);
		w->attr_names_len = log->attr_names_len;
		w->attr_names_cap = log->attr_names_len;
	}
	return 0;
}

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

	/*
	 * Adopt the last checkpoint's tables, so a checkpoint written by
	 * this session does not drop what an earlier session registered.
	 * No checkpoint yet is fine (-ENOENT); a corrupt one refuses the
	 * open rather than writing on top of it.
	 */
	err = lw_adopt_checkpoint(w, log);
	if (err) {
		ntfs_log_writer_free(w);
		return err;
	}

	err = lw_find_client(log, &w->client_idx);
	if (err) {
		ntfs_log_writer_free(w);
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
			ntfs_log_writer_free(w);
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
	lfs_table_free(&w->trtbl);
	lfs_table_free(&w->dptbl);
	lfs_table_free(&w->oatbl);
	free(w->attr_names);
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

/* Pages a @total-byte record occupies starting at offset @off. */
static uint32_t lw_pages_for(ntfs_logfile_t *log, uint32_t off, uint32_t total)
{
	uint32_t cap = log->page_size - log->data_off;
	uint32_t left = total, n = 1;

	if (left <= log->page_size - off)
		return 1;
	left -= log->page_size - off;
	while (left > cap) {
		left -= cap;
		n++;
	}
	return n + 1;
}

/* Number of record pages in the log area. */
static uint32_t lw_page_count(ntfs_logfile_t *log)
{
	return (log->l_size - log->first_page) / log->page_size;
}

/* Pages strictly after @page, up to (not including) the oldest live page. */
static uint32_t lw_pages_free(ntfs_log_writer_t *w, uint32_t page)
{
	ntfs_logfile_t *log = w->log;
	uint32_t oldest, p = page, n = 0, total = lw_page_count(log);

	if (!w->first_lsn)
		return total;		/* empty log: everything is free */
	oldest = lfs_lsn_to_vbo(log, w->first_lsn) & ~log->page_mask;
	/* Advance before comparing: when the cursor sits on the oldest
	 * page, every other page is free (the live records are all on
	 * this page, ahead of the cursor). */
	p = lfs_next_page_off(log, p);
	while (p != oldest) {
		n++;
		p = lfs_next_page_off(log, p);
		if (n > total)
			break;		/* cannot happen */
	}
	return n;
}

/* Pages a checkpoint needs: one per non-empty table dump, one for the
 * restart record, one of margin. */
static uint32_t lw_reserve_pages(ntfs_log_writer_t *w)
{
	ntfs_logfile_t *log = w->log;
	uint32_t p = 2;		/* restart record + margin */

	if (w->trtbl.buf && lfs_table_total(&w->trtbl))
		p += lw_pages_for(log, log->data_off, LR_HEADER_SIZE + NR_HEADER_SIZE + 8 + LF_ALIGN8(w->trtbl.bytes));
	if (w->dptbl.buf && lfs_table_total(&w->dptbl))
		p += lw_pages_for(log, log->data_off, LR_HEADER_SIZE + NR_HEADER_SIZE + 8 + LF_ALIGN8(w->dptbl.bytes));
	if (w->attr_names_len)
		p += lw_pages_for(log, log->data_off, LR_HEADER_SIZE + NR_HEADER_SIZE + 8 + LF_ALIGN8(w->attr_names_len));
	if (w->oatbl.buf && lfs_table_total(&w->oatbl))
		p += lw_pages_for(log, log->data_off, LR_HEADER_SIZE + NR_HEADER_SIZE + 8 + LF_ALIGN8(w->oatbl.bytes));
	return p;
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
	/* Pre-check the wrap and the checkpoint reserve with the adjusted
	 * start: a refused append writes nothing (the layout below only
	 * runs after this passes). The wrap is refused even with a
	 * checkpoint until the wrap commit advances first_lsn past
	 * overwritten pages; the reserve keeps a checkpoint emittable. */
	{
		uint32_t need = lw_pages_for(log, off, total);
		uint32_t free_after = lw_pages_free(w, page);

		if (need - 1 > free_after)
			return -ENOSPC;
		if (!w->in_checkpoint &&
		    free_after - (need - 1) < lw_reserve_pages(w))
			return -ENOSPC;
	}
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
		/* The record continues: this page is full, so the next record
		 * starts on the following page — but an earlier record may
		 * already have ended on this page, and its end marker must
		 * survive: the reader checks PG_FLAGS/LAST_END_LSN when it
		 * finishes reading that (multi-page) record. Only clear the
		 * flag when no record has ended here yet. */
		if (!(lf_get32(w->page_buf + PG_FLAGS) & LFS_PAGE_LOG_RECORD_END))
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

/* ---- open-attribute registration --------------------------------------- */

/*
 * Register an open attribute in the writer's in-memory table. Phase 2
 * calls this when a transaction opens an attribute; the checkpoint
 * dumps the table, so the attribute survives a crash and a reopen.
 * The name, if any, is appended to the attribute-names blob with the
 * entry's table offset, which is how the reader pairs names to
 * entries. Returns the entry's table offset in *attr_id_out.
 */
int ntfs_log_writer_open_attr(ntfs_log_writer_t *w, uint32_t type,
			      uint64_t mft_no, uint16_t mft_seq,
			      const uint16_t *name, uint16_t name_len,
			      uint32_t *attr_id_out)
{
	uint8_t *e;
	uint32_t off;
	int err;

	if (w->failed)
		return -EIO;
	if (!w->oatbl.buf) {
		err = lfs_table_init(&w->oatbl, OA1_SIZE, 16);
		if (err)
			return err;
	}
	e = lfs_table_alloc(&w->oatbl);
	if (!e)
		return -ENOMEM;
	off = lfs_table_off(&w->oatbl, e);
	lf_put32(e + OA1_TYPE, type);
	lf_put64(e + OA1_FILE_REF,
		 (mft_no & 0xffffffffffffull) | ((uint64_t)mft_seq << 48));
	lf_put64(e + OA1_OPEN_RECORD_LSN, w->last_lsn);
	if (name_len) {
		uint32_t need = w->attr_names_len + 4 + (uint32_t)name_len * 2;
		uint8_t *nb = w->attr_names;

		if (need > w->attr_names_cap) {
			uint32_t cap = w->attr_names_cap ? w->attr_names_cap : 64;

			while (cap < need)
				cap *= 2;
			nb = realloc(w->attr_names, cap);
			if (!nb)
				return -ENOMEM;
			w->attr_names = nb;
			w->attr_names_cap = cap;
		}
		lf_put16(w->attr_names + w->attr_names_len, (uint16_t)off);
		lf_put16(w->attr_names + w->attr_names_len + 2, (uint16_t)(name_len * 2));
		memcpy(w->attr_names + w->attr_names_len + 4, name, (size_t)name_len * 2);
		w->attr_names_len = need;
	}
	if (attr_id_out)
		*attr_id_out = off;
	return 0;
}

/* ---- checkpoint emission ----------------------------------------------- */

/*
 * Append one restart table as a client record with a dump opcode.
 * The redo payload is the table bytes; there is no undo. The record
 * uses transaction id 0x18 (the first transaction-table entry): the
 * replay analysis pass allocates an entry for every client record's
 * tid, and tid 0 is rejected, so a dump must not use it.
 */
static int lw_dump_table(ntfs_log_writer_t *w, uint16_t op,
			 const uint8_t *data, uint32_t len, uint64_t *lsn_out)
{
	uint32_t hdr = NR_HEADER_SIZE + 8;	/* lcns = 0 */
	uint32_t total = hdr + LF_ALIGN8(len);
	uint8_t *cdata;
	int err;

	if (len > 0xffff)
		return -E2BIG;
	cdata = malloc(total);
	if (!cdata)
		return -ENOMEM;
	memset(cdata, 0, total);
	lf_put16(cdata + NR_REDO_OP, op);
	lf_put16(cdata + NR_UNDO_OP, LOP_Noop);
	lf_put16(cdata + NR_REDO_OFFSET, (uint16_t)hdr);
	lf_put16(cdata + NR_REDO_LENGTH, (uint16_t)len);
	lf_put16(cdata + NR_UNDO_OFFSET, (uint16_t)total);
	lf_put16(cdata + NR_UNDO_LENGTH, 0);
	memcpy(cdata + hdr, data, len);
	err = ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0,
				     cdata, total, lsn_out);
	free(cdata);
	return err;
}

/*
 * Emit a checkpoint: dump each non-empty restart table, then an
 * NTFS_RESTART record naming them, then sync the restart area so the
 * new checkpoint is referenced before any later wrap can destroy the
 * checkpoint it supersedes.
 *
 * The checkpoint's start_of_checkpoint is the first dump's LSN (or 0,
 * which the reader takes as the restart record's own LSN, when every
 * table is empty). Records before it become dispensable: the tables
 * carry the state, and the oldest live LSN advances to it — which is
 * what makes wraps possible: append() may use only the free pages,
 * and before any checkpoint the oldest record sits on the first page,
 * so no wrap fits. An active transaction's first record is never
 * passed over (Phase 2 populates the transaction table; none exist
 * yet).
 *
 * Crash contract: every dump and the restart record are appended
 * synchronously, and the final sync is part of the call, so a crash
 * at any instant leaves either the old checkpoint (restart area
 * still points at it, its pages intact) or the new one (restart area
 * updated, dumps durable). A crash mid-checkpoint leaves a partial
 * checkpoint that is never referenced — the reader loads the old one.
 */
static int lw_checkpoint_inner(ntfs_log_writer_t *w, uint64_t *lsn_out)
{
	uint64_t oa_lsn = 0, an_lsn = 0, dp_lsn = 0, tr_lsn = 0;
	uint32_t oa_len = 0, an_len = 0, dp_len = 0, tr_len = 0;
	uint64_t first_dump = 0, rlsn;
	uint8_t crst[CRST_SIZE];
	uint8_t *e;
	int err;
	if (w->trtbl.buf && lfs_table_total(&w->trtbl)) {
		err = lw_dump_table(w, LOP_TransactionTableDump,
				    w->trtbl.buf, w->trtbl.bytes, &tr_lsn);
		if (err)
			return err;
		tr_len = w->trtbl.bytes;
		if (!first_dump)
			first_dump = tr_lsn;
	}
	if (w->dptbl.buf && lfs_table_total(&w->dptbl)) {
		err = lw_dump_table(w, LOP_DirtyPageTableDump,
				    w->dptbl.buf, w->dptbl.bytes, &dp_lsn);
		if (err)
			return err;
		dp_len = w->dptbl.bytes;
		if (!first_dump)
			first_dump = dp_lsn;
	}
	if (w->attr_names_len) {
		err = lw_dump_table(w, LOP_AttributeNamesDump,
				    w->attr_names, w->attr_names_len, &an_lsn);
		if (err)
			return err;
		an_len = w->attr_names_len;
		if (!first_dump)
			first_dump = an_lsn;
	}
	if (w->oatbl.buf && lfs_table_total(&w->oatbl)) {
		err = lw_dump_table(w, LOP_OpenAttributeTableDump,
				    w->oatbl.buf, w->oatbl.bytes, &oa_lsn);
		if (err)
			return err;
		oa_len = w->oatbl.bytes;
		if (!first_dump)
			first_dump = oa_lsn;
	}

	memset(crst, 0, sizeof(crst));
	lf_put32(crst + CRST_MAJOR_VER, 1);
	lf_put32(crst + CRST_MINOR_VER, 1);
	lf_put64(crst + CRST_START_OF_CHECKPOINT, first_dump);
	lf_put64(crst + CRST_OPEN_ATTR_TABLE_LSN, oa_lsn);
	lf_put32(crst + CRST_OPEN_ATTR_TABLE_LEN, oa_len);
	lf_put64(crst + CRST_ATTR_NAMES_LSN, an_lsn);
	lf_put32(crst + CRST_ATTR_NAMES_LEN, an_len);
	lf_put64(crst + CRST_DIRTY_PAGE_TABLE_LSN, dp_lsn);
	lf_put32(crst + CRST_DIRTY_PAGE_TABLE_LEN, dp_len);
	lf_put64(crst + CRST_TRANSACTION_TABLE_LSN, tr_lsn);
	lf_put32(crst + CRST_TRANSACTION_TABLE_LEN, tr_len);
	err = ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT_RESTART, 0, 0,
				     crst, sizeof(crst), &rlsn);
	if (err)
		return err;

	w->checkpoint_lsn = rlsn;
	w->checkpointed = true;
	w->first_lsn = first_dump ? first_dump : rlsn;
	e = NULL;
	while ((e = lfs_table_next(&w->trtbl, e))) {
		uint8_t st = e[TR_STATE];

		if (st == TRANSACTION_ACTIVE || st == TRANSACTION_PREPARED) {
			uint64_t fl = lf_get64(e + TR_FIRST_LSN);

			if (fl && fl < w->first_lsn)
				w->first_lsn = fl;
		}
	}
	err = ntfs_log_writer_sync(w);
	if (err)
		return err;
	if (lsn_out)
		*lsn_out = rlsn;
	return 0;
}

int ntfs_log_writer_checkpoint(ntfs_log_writer_t *w, uint64_t *lsn_out)
{
	int err;

	if (w->failed)
		return -EIO;
	w->in_checkpoint = true;
	err = lw_checkpoint_inner(w, lsn_out);
	w->in_checkpoint = false;
	return err;
}
