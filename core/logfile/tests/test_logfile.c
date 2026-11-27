// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
/*
 * Unit tests for the $LogFile module, built from synthetic pages.
 *
 * The builder writes a version 1.1 log the way the LFS lays it out:
 * restart pages at 0 and 1, tail-copy pages at 2 and 3, record pages from
 * page 4, records made of a 0x30-byte LFS header followed by client data
 * that may continue on the next page (from data_off) and wrap to the first
 * record page with the sequence number incremented. See docs/LOGFILE.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include "ntfs_logfile.h"
#include "logfile_layout.h"
#include "ntfs_image.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { checks++; unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
	if (_a != _b) { failures++; fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", \
	__FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static void logger(int level, const char *m, void *ctx)
{
	if (getenv("LOGTEST_VERBOSE") || level == 0)
		fprintf(stderr, "    [%d] %s\n", level, m);
}

/* ---- Synthetic log builder --------------------------------------------- */

#define PS 4096u
#define NPAGES 64u			/* 256 KiB: file_data_bits = 16 */
#define LSIZE (PS * NPAGES)
#define DATA_OFF 0x40u
#define FDB 16u				/* file data bits */
#define SEQ_BITS (64 - FDB)

/* Log version the builder emits. v1.x: record pages from page 4, tail
 * copies at 2 and 3. v2.0: record pages from page 0x22, tail copies at
 * 0x02.. and 0x12.., page header carries file_off at 0x3c. */
static uint16_t log_major = 1, log_minor = 1;
static uint32_t first_page = 4 * PS;
#define FIRST_PAGE first_page

static void lb_set_version(uint16_t major, uint16_t minor)
{
	log_major = major;
	log_minor = minor;
	first_page = (major >= 2 ? 0x22u : 4u) * PS;
}

struct lb {
	uint8_t *buf;
	uint32_t page;			/* current page offset */
	uint32_t off;			/* next free byte in page */
	uint64_t seq;
	uint64_t last_lsn;		/* lsn of last record appended */
	uint32_t last_len;
	uint64_t prev_lsn;		/* for client_prev_lsn chaining */
	bool wrapped;
};

static uint64_t mk_lsn(uint32_t vbo, uint64_t seq) { return (vbo >> 3) + (seq << FDB); }

static void lb_page_init(struct lb *b, uint32_t page)
{
	uint8_t *p = b->buf + page;
	memset(p, 0, PS);
	lf_put32(p + PG_MAGIC, LFS_MAGIC_RCRD);
	lf_put16(p + PG_USA_OFS, PG_USA);
	lf_put16(p + PG_USA_COUNT, PS / 512 + 1);
	lf_put16(p + PG_PAGE_COUNT, 1);
	lf_put16(p + PG_PAGE_POSITION, 1);
	lf_put16(p + PG_NEXT_RECORD_OFFSET, DATA_OFF);
}

static void lb_init(struct lb *b, uint32_t start_page, uint64_t seq)
{
	memset(b, 0, sizeof(*b));
	b->buf = malloc(LSIZE);
	memset(b->buf, 0xff, LSIZE);
	b->page = start_page;
	b->off = DATA_OFF;
	b->seq = seq;
	lb_page_init(b, b->page);
}

static void lb_next_page(struct lb *b)
{
	b->page += PS;
	if (b->page >= LSIZE) {
		b->page = FIRST_PAGE;
		b->seq++;
		b->wrapped = true;
	}
	lb_page_init(b, b->page);
	b->off = DATA_OFF;
}

/* Append a record; returns its lsn. hdr: prev/undo_next/type/tid/client. */
static uint64_t lb_append(struct lb *b, uint32_t type, uint32_t tid, uint64_t undo_next,
			  const void *data, uint32_t len)
{
	uint8_t *p;
	uint64_t lsn;
	uint32_t left = len, first_page;
	const uint8_t *d = data;
	bool multi;

	if (b->off + LR_HEADER_SIZE > PS)
		lb_next_page(b);
	first_page = b->page;
	lsn = mk_lsn(b->page + b->off, b->seq);
	p = b->buf + b->page + b->off;
	memset(p, 0, LR_HEADER_SIZE);
	lf_put64(p + LR_THIS_LSN, lsn);
	lf_put64(p + LR_CLIENT_PREV_LSN, b->prev_lsn);
	lf_put64(p + LR_CLIENT_UNDO_NEXT_LSN, undo_next);
	lf_put32(p + LR_CLIENT_DATA_LENGTH, len);
	lf_put16(p + LR_CLIENT_SEQ_NUMBER, 0);
	lf_put16(p + LR_CLIENT_INDEX, 0);
	lf_put32(p + LR_RECORD_TYPE, type);
	lf_put32(p + LR_TRANSACTION_ID, tid);
	multi = b->off + LR_HEADER_SIZE + len > PS;
	lf_put16(p + LR_FLAGS, multi ? LFS_RECORD_MULTI_PAGE : 0);
	lf_put64(b->buf + b->page + PG_LAST_LSN, lsn);
	b->off += LR_HEADER_SIZE;
	for (;;) {
		uint32_t n = PS - b->off;
		if (n > left)
			n = left;
		memcpy(b->buf + b->page + b->off, d, n);
		d += n;
		left -= n;
		b->off += n;
		if (!left)
			break;
		/* continue on the next page; that page has no record header yet */
		lf_put16(b->buf + b->page + PG_NEXT_RECORD_OFFSET, (uint16_t)PS);
		lb_next_page(b);
		/* pages without a record of their own still carry the spanning
		 * record's lsn as last_lsn (the reader checks lsn <= last_lsn) */
		lf_put64(b->buf + b->page + PG_LAST_LSN, lsn);
	}
	/* record ends on b->page */
	lf_put32(b->buf + b->page + PG_FLAGS, LFS_PAGE_LOG_RECORD_END);
	lf_put64(b->buf + b->page + PG_LAST_END_LSN, lsn);
	b->off = LF_ALIGN8(b->off);
	lf_put16(b->buf + b->page + PG_NEXT_RECORD_OFFSET, (uint16_t)(b->off > PS ? PS : b->off));
	(void)first_page;
	b->prev_lsn = lsn;
	b->last_lsn = lsn;
	b->last_len = len;
	return lsn;
}

/* NTFS client record. Redo/undo data appended after the header + lcns. */
struct crec {
	uint16_t redo_op, undo_op, target_attr, lcns;
	uint16_t record_off, attr_off, cluster_off;
	uint64_t target_vcn;
	uint64_t lcn[4];
	const void *redo; uint16_t redo_len;
	const void *undo; uint16_t undo_len;
};

static uint32_t build_crec(const struct crec *c, uint8_t *out)
{
	uint32_t hdr = NR_HEADER_SIZE + 8u * (c->lcns ? c->lcns : 1), o = hdr, i;

	memset(out, 0, hdr);
	lf_put16(out + NR_REDO_OP, c->redo_op);
	lf_put16(out + NR_UNDO_OP, c->undo_op);
	lf_put16(out + NR_TARGET_ATTRIBUTE, c->target_attr);
	lf_put16(out + NR_LCNS_TO_FOLLOW, c->lcns);
	lf_put16(out + NR_RECORD_OFFSET, c->record_off);
	lf_put16(out + NR_ATTRIBUTE_OFFSET, c->attr_off);
	lf_put16(out + NR_CLUSTER_BLOCK_OFFSET, c->cluster_off);
	lf_put64(out + NR_TARGET_VCN, c->target_vcn);
	for (i = 0; i < c->lcns; i++)
		lf_put64(out + NR_PAGE_LCNS + 8 * i, c->lcn[i]);
	if (c->redo_len) {
		lf_put16(out + NR_REDO_OFFSET, (uint16_t)o);
		lf_put16(out + NR_REDO_LENGTH, c->redo_len);
		memcpy(out + o, c->redo, c->redo_len);
		o = LF_ALIGN8(o + c->redo_len);
	} else {
		lf_put16(out + NR_REDO_OFFSET, (uint16_t)o);
	}
	if (c->undo_len) {
		lf_put16(out + NR_UNDO_OFFSET, (uint16_t)o);
		lf_put16(out + NR_UNDO_LENGTH, c->undo_len);
		memcpy(out + o, c->undo, c->undo_len);
		o = LF_ALIGN8(o + c->undo_len);
	} else {
		lf_put16(out + NR_UNDO_OFFSET, (uint16_t)o);
	}
	return o;
}

static uint64_t lb_client(struct lb *b, uint32_t tid, uint64_t undo_next, const struct crec *c)
{
	uint8_t tmp[8192];
	uint32_t n = build_crec(c, tmp);
	return lb_append(b, LFS_RECORD_TYPE_CLIENT, tid, undo_next, tmp, n);
}

static uint64_t lb_simple(struct lb *b, uint32_t tid, uint16_t redo_op, uint16_t undo_op)
{
	struct crec c;
	memset(&c, 0, sizeof(c));
	c.redo_op = redo_op;
	c.undo_op = undo_op;
	return lb_client(b, tid, 0, &c);
}

/* Restart record (checkpoint). Returns its lsn. */
static uint64_t lb_checkpoint(struct lb *b, uint64_t start, uint64_t oatbl_lsn, uint32_t oatbl_len,
			      uint64_t dptbl_lsn, uint32_t dptbl_len, uint64_t trtbl_lsn, uint32_t trtbl_len)
{
	uint8_t c[CRST_SIZE];
	memset(c, 0, sizeof(c));
	lf_put32(c + CRST_MAJOR_VER, 1);
	lf_put64(c + CRST_START_OF_CHECKPOINT, start);
	lf_put64(c + CRST_OPEN_ATTR_TABLE_LSN, oatbl_lsn);
	lf_put32(c + CRST_OPEN_ATTR_TABLE_LEN, oatbl_len);
	lf_put64(c + CRST_DIRTY_PAGE_TABLE_LSN, dptbl_lsn);
	lf_put32(c + CRST_DIRTY_PAGE_TABLE_LEN, dptbl_len);
	lf_put64(c + CRST_TRANSACTION_TABLE_LSN, trtbl_lsn);
	lf_put32(c + CRST_TRANSACTION_TABLE_LEN, trtbl_len);
	return lb_append(b, LFS_RECORD_TYPE_CLIENT_RESTART, 0, 0, c, sizeof(c));
}

/* Open attribute table with two v1 entries: 0x18 $MFT:$DATA, 0x40 target. */
static uint32_t build_oatbl(uint8_t *t, uint64_t mft2, uint32_t type2)
{
	uint32_t n = RT_HEADER_SIZE + 2 * OA1_SIZE;
	uint8_t *e;
	memset(t, 0, n);
	lf_put16(t + RT_ENTRY_SIZE, OA1_SIZE);
	lf_put16(t + RT_NUMBER_ENTRIES, 2);
	lf_put16(t + RT_NUMBER_ALLOCATED, 2);
	lf_put32(t + RT_FREE_GOAL, 0xffffffffu);
	e = t + RT_HEADER_SIZE;
	lf_put32(e + OA1_NEXT, RT_ENTRY_ALLOCATED);
	lf_put32(e + OA1_TYPE, ATTR_TYPE_DATA);
	lf_put32(e + OA1_FILE_REF, 0);
	lf_put16(e + OA1_FILE_REF + 6, 1);
	e += OA1_SIZE;
	lf_put32(e + OA1_NEXT, RT_ENTRY_ALLOCATED);
	lf_put32(e + OA1_TYPE, type2);
	lf_put32(e + OA1_FILE_REF, (uint32_t)mft2);
	lf_put16(e + OA1_FILE_REF + 6, 1);
	return n;
}

static uint64_t lb_table_dump(struct lb *b, uint16_t op, const uint8_t *t, uint32_t n)
{
	struct crec c;
	memset(&c, 0, sizeof(c));
	c.redo_op = op;
	c.undo_op = LOP_Noop;
	c.redo = t;
	c.redo_len = (uint16_t)n;
	return lb_client(b, 0x18, 0, &c);
}

/* Write the two restart pages. */
static void lb_restart(struct lb *b, uint64_t current_lsn, uint32_t last_len, uint64_t oldest,
		       uint64_t client_restart, uint16_t flags, bool closed)
{
	int i;
	for (i = 0; i < 2; i++) {
		uint8_t *p = b->buf + i * PS, *ra, *cr;
		memset(p, 0, PS);
		lf_put32(p + RP_MAGIC, LFS_MAGIC_RSTR);
		lf_put16(p + RP_USA_OFS, RP_HEADER_SIZE);
		lf_put16(p + RP_USA_COUNT, PS / 512 + 1);
		lf_put32(p + RP_SYSTEM_PAGE_SIZE, PS);
		lf_put32(p + RP_LOG_PAGE_SIZE, PS);
		lf_put16(p + RP_RESTART_AREA_OFFSET, 0x30);
		lf_put16(p + RP_MINOR_VER, log_minor);
		lf_put16(p + RP_MAJOR_VER, log_major);
		ra = p + 0x30;
		lf_put64(ra + RA_CURRENT_LSN, current_lsn);
		lf_put16(ra + RA_LOG_CLIENTS, 1);
		lf_put16(ra + RA_CLIENT_FREE_LIST, closed ? 0 : LFS_NO_CLIENT);
		lf_put16(ra + RA_CLIENT_IN_USE_LIST, closed ? LFS_NO_CLIENT : 0);
		lf_put16(ra + RA_FLAGS, flags);
		lf_put32(ra + RA_SEQ_NUMBER_BITS, SEQ_BITS);	/* 67 - 19 */
		lf_put16(ra + RA_RESTART_AREA_LENGTH, RA_SIZE_XP + CR_SIZE);
		lf_put16(ra + RA_CLIENT_ARRAY_OFFSET, RA_SIZE_XP);
		lf_put64(ra + RA_FILE_SIZE, LSIZE);
		lf_put32(ra + RA_LAST_LSN_DATA_LENGTH, last_len);
		lf_put16(ra + RA_LOG_RECORD_HEADER_LENGTH, LR_HEADER_SIZE);
		lf_put16(ra + RA_LOG_PAGE_DATA_OFFSET, DATA_OFF);
		lf_put32(ra + RA_RESTART_LOG_OPEN_COUNT, 7);
		cr = ra + RA_SIZE_XP;
		lf_put64(cr + CR_OLDEST_LSN, oldest);
		lf_put64(cr + CR_CLIENT_RESTART_LSN, client_restart);
		lf_put16(cr + CR_PREV_CLIENT, LFS_NO_CLIENT);
		lf_put16(cr + CR_NEXT_CLIENT, LFS_NO_CLIENT);
		lf_put32(cr + CR_CLIENT_NAME_LENGTH, 8);
		lf_put16(cr + CR_CLIENT_NAME + 0, 'N');
		lf_put16(cr + CR_CLIENT_NAME + 2, 'T');
		lf_put16(cr + CR_CLIENT_NAME + 4, 'F');
		lf_put16(cr + CR_CLIENT_NAME + 6, 'S');
		lf_put16(p + RP_HEADER_SIZE, (uint16_t)(0x100 + i));
		ntfs_log_fixup_pre_write(p, PS, 512);
	}
}

/* Protect all record pages (in place, once, after building). */
static void lb_protect(struct lb *b)
{
	uint32_t pg;
	for (pg = 2 * PS; pg < LSIZE; pg += PS) {
		uint8_t *p = b->buf + pg;
		if (lf_get32(p) == LFS_MAGIC_RCRD) {
			lf_put16(p + PG_USA, (uint16_t)(1 + pg / PS));
			CHECK(ntfs_log_fixup_pre_write(p, PS, 512) == 0);
		}
	}
}

/* Make a (deprotected) copy of record page @page, append one Noop record
 * to it and store it as a v1 tail copy at page 2. Returns the new lsn. */
static uint64_t lb_make_tail_copy(struct lb *b, uint32_t page, uint64_t seq)
{
	uint8_t *copy = b->buf + 2 * PS, *src = b->buf + page;
	uint8_t tmp[64];
	struct crec c;
	uint32_t n, off;
	uint64_t lsn;

	memcpy(copy, src, PS);
	off = lf_get16(copy + PG_NEXT_RECORD_OFFSET);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_Noop;
	c.undo_op = LOP_Noop;
	n = build_crec(&c, tmp);
	lsn = mk_lsn(page + off, seq);
	memset(copy + off, 0, LR_HEADER_SIZE);
	lf_put64(copy + off + LR_THIS_LSN, lsn);
	lf_put64(copy + off + LR_CLIENT_PREV_LSN, lf_get64(copy + PG_LAST_END_LSN));
	lf_put32(copy + off + LR_CLIENT_DATA_LENGTH, n);
	lf_put32(copy + off + LR_RECORD_TYPE, LFS_RECORD_TYPE_CLIENT);
	lf_put32(copy + off + LR_TRANSACTION_ID, 0x18);
	memcpy(copy + off + LR_HEADER_SIZE, tmp, n);
	lf_put64(copy + PG_LAST_END_LSN, lsn);
	lf_put16(copy + PG_NEXT_RECORD_OFFSET, (uint16_t)LF_ALIGN8(off + LR_HEADER_SIZE + n));
	if (log_major >= 2) {
		/* v2 tail copies keep last_lsn and name the page in file_off */
		lf_put64(copy + PG_LAST_LSN, lsn);
		lf_put32(copy + PG_FILE_OFFSET, page);
	} else {
		/* v1 tail copies carry the page's file offset in the last_lsn field */
		lf_put64(copy + PG_LAST_LSN, page);
	}
	return lsn;
}

/* ---- Memory-backed io ------------------------------------------------- */

struct memio {
	uint8_t *buf;
	uint64_t size;
	int writes;
};

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct memio *m = ctx;
	if (off + len > m->size)
		return -EIO;
	memcpy(buf, m->buf + off, len);
	return 0;
}

static int mem_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct memio *m = ctx;
	if (off + len > m->size)
		return -EIO;
	memcpy(m->buf + off, buf, len);
	m->writes++;
	return 0;
}

/* Fault-injecting wrapper around a memio: once the budget is set (>= 0),
 * each write consumes one unit and the write at budget 0 fails with -EIO,
 * as if the device died mid-commit. Budget -1 means unlimited. */
struct fault_memio {
	struct memio *m;
	int limit;
	int writes;
};

static int fault_mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct fault_memio *f = ctx;
	return mem_read(f->m, off, buf, len);
}

static int fault_mem_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct fault_memio *f = ctx;
	if (f->limit == 0)
		return -EIO;
	if (f->limit > 0)
		f->limit--;
	f->writes++;
	return mem_write(f->m, off, buf, len);
}

/* Open @io as already configured (used when the io carries fault
 * injection; open_log resets it to the plain memio). */
static ntfs_logfile_t *open_log_keep_io(struct ntfs_log_io *io)
{
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log = NULL;

	CHECK_EQ(ntfs_logfile_open(io, &g, &log), 0);
	if (log) {
		struct ntfs_logfile_info info;
		ntfs_logfile_set_logger(log, logger, NULL);
		ntfs_logfile_get_info(log, &info);
		if (info.state == NTFS_LOG_CORRUPT || info.state == NTFS_LOG_UNSUPPORTED)
			fprintf(stderr, "    open: state %d: %s\n", info.state, ntfs_logfile_last_error(log));
	}
	return log;
}

static ntfs_logfile_t *open_log(struct memio *m, struct ntfs_log_io *io)
{
	memset(io, 0, sizeof(*io));
	io->ctx = m;
	io->size = m->size;
	io->read = mem_read;
	io->write = mem_write;
	return open_log_keep_io(io);
}

/* ---- Fake volume -------------------------------------------------------- */

#define VCS 4096u
#define VRS 1024u
#define MFT_LCN 16u			/* 16 clusters = 64 records */
#define MFT_CLUSTERS 16u
#define BITMAP_LCN 40u
/* An index allocation on its own cluster, left zeroed by vol_init. That is the
 * state finding 19 was about: a cluster allocated but never written, into which
 * a redo record lays down the first INDX header this volume has ever seen. */
#define INDEX_LCN  44u
#define MFTBMP_LCN 41u			/* $MFT:$BITMAP lives on its own cluster */
#define DIR_REC 32u			/* parent directory for the create tests */
#define NEW_REC 40u			/* record created by the first create txn */
#define NEW_REC2 41u			/* record created by the second create txn */
#define VOL_CLUSTERS 64u

struct vol {
	uint8_t *disk;
	int mft_writes, clu_writes, syncs;
	int sync_errno;		/* non-zero: the device refuses the barrier */
	int write_errno;	/* non-zero: the device refuses the writes themselves */
	/* Fault-injection budget for the flush (§1.3): >= 0 counts down one
	 * per flush write (MFT record or cluster run) and refuses with
	 * -EIO at zero, without writing; -1 is unlimited. Reads never
	 * consume it — only flush_overlay writes through these callbacks. */
	int write_limit;
	uint64_t last_mft_no;
};

static void put_nonres_attr(uint8_t *rec, uint32_t *off, uint32_t type, uint64_t lcn, uint64_t clusters, uint16_t id)
{
	uint8_t *a = rec + *off;
	uint8_t mp[16];
	uint32_t mplen = 0, len;

	mp[mplen++] = 0x41;	/* 1 byte length, 4 byte lcn */
	mp[mplen++] = (uint8_t)clusters;
	mp[mplen++] = (uint8_t)lcn;
	mp[mplen++] = (uint8_t)(lcn >> 8);
	mp[mplen++] = (uint8_t)(lcn >> 16);
	mp[mplen++] = (uint8_t)(lcn >> 24);
	mp[mplen++] = 0;
	len = LF_ALIGN8(AT_NONRESIDENT_SIZE + mplen);
	memset(a, 0, len);
	lf_put32(a + AT_TYPE, type);
	lf_put32(a + AT_LENGTH, len);
	a[AT_NON_RESIDENT] = 1;
	lf_put16(a + AT_NAME_OFFSET, AT_NONRESIDENT_SIZE);
	lf_put16(a + AT_INSTANCE, id);
	lf_put64(a + AT_HIGHEST_VCN, clusters - 1);
	lf_put16(a + AT_MAPPING_PAIRS_OFFSET, AT_NONRESIDENT_SIZE);
	lf_put64(a + AT_ALLOCATED_SIZE, clusters * VCS);
	lf_put64(a + AT_DATA_SIZE, clusters * VCS);
	lf_put64(a + AT_INITIALIZED_SIZE, clusters * VCS);
	memcpy(a + AT_NONRESIDENT_SIZE, mp, mplen);
	*off += len;
}

static void put_res_attr(uint8_t *rec, uint32_t *off, uint32_t type, const void *val, uint32_t vlen, uint16_t id)
{
	uint8_t *a = rec + *off;
	uint32_t len = LF_ALIGN8(AT_RESIDENT_SIZE + vlen);
	memset(a, 0, len);
	lf_put32(a + AT_TYPE, type);
	lf_put32(a + AT_LENGTH, len);
	lf_put16(a + AT_NAME_OFFSET, AT_RESIDENT_SIZE);
	lf_put16(a + AT_INSTANCE, id);
	lf_put32(a + AT_VALUE_LENGTH, vlen);
	lf_put16(a + AT_VALUE_OFFSET, AT_RESIDENT_SIZE);
	memcpy(a + AT_RESIDENT_SIZE, val, vlen);
	*off += len;
}

/* Build a deprotected FILE record with the given attributes appended by @fill. */
static void build_record(uint8_t *rec, uint64_t no, uint16_t flags, void (*fill)(uint8_t *, uint32_t *))
{
	uint32_t off = MR_FIXUP_OFFSET_3 + 8;	/* usa 0x30..0x36, attrs at 0x38 */
	memset(rec, 0, VRS);
	lf_put32(rec + MR_MAGIC, LFS_MAGIC_FILE);
	lf_put16(rec + MR_USA_OFS, MR_FIXUP_OFFSET_3);
	lf_put16(rec + MR_USA_COUNT, VRS / 512 + 1);
	lf_put16(rec + MR_SEQUENCE, 1);
	lf_put16(rec + MR_LINK_COUNT, 1);
	lf_put16(rec + MR_ATTRS_OFFSET, (uint16_t)off);
	lf_put16(rec + MR_FLAGS, flags);
	lf_put32(rec + MR_BYTES_ALLOCATED, VRS);
	lf_put32(rec + MR_RECORD_NUMBER, (uint32_t)no);
	lf_put16(rec + MR_FIXUP_OFFSET_3, 0x0001);
	fill(rec, &off);
	lf_put32(rec + off, ATTR_TYPE_END);
	off += 8;
	lf_put32(rec + MR_BYTES_IN_USE, off);
	lf_put16(rec + MR_NEXT_ATTR_ID, 4);
}

static void fill_mft(uint8_t *rec, uint32_t *off)
{
	put_nonres_attr(rec, off, ATTR_TYPE_DATA, MFT_LCN, MFT_CLUSTERS, 1);
	put_nonres_attr(rec, off, ATTR_TYPE_BITMAP, MFTBMP_LCN, 1, 2);
}
static void fill_bitmap(uint8_t *rec, uint32_t *off) { put_nonres_attr(rec, off, ATTR_TYPE_DATA, BITMAP_LCN, 1, 1); }
static void fill_index(uint8_t *rec, uint32_t *off)
{
	put_nonres_attr(rec, off, ATTR_TYPE_INDEX_ALLOCATION, INDEX_LCN, 1, 1);
}

static void fill_plain(uint8_t *rec, uint32_t *off)
{
	static const uint8_t si[0x30] = { 1, 2, 3 };
	put_res_attr(rec, off, ATTR_TYPE_STANDARD_INFORMATION, si, sizeof(si), 1);
}

/* Directory record 32: $STANDARD_INFORMATION with a distinct byte pattern
 * (value[i] = 0x10 + i, so the parent-mtime undo has a real old value) and
 * an empty $INDEX_ROOT $I30: root header (type $FILE_NAME, collation
 * FILE_NAME, 4096-byte blocks), index header (entries at 16, length 32),
 * one END entry. */
static void fill_dir(uint8_t *rec, uint32_t *off)
{
	uint8_t si[0x30];
	uint8_t ir[48];
	int i;

	for (i = 0; i < 0x30; i++)
		si[i] = (uint8_t)(0x10 + i);
	put_res_attr(rec, off, ATTR_TYPE_STANDARD_INFORMATION, si, sizeof(si), 0);
	memset(ir, 0, sizeof(ir));
	lf_put32(ir + 0, ATTR_TYPE_FILE_NAME);	/* index root: attribute type */
	lf_put32(ir + 4, 1);			/* collation rule FILE_NAME */
	lf_put32(ir + 8, 4096);			/* index block size */
	lf_put32(ir + 12, 1);			/* clusters per index block */
	lf_put32(ir + 16, 16);			/* index header: entries offset */
	lf_put32(ir + 20, 32);			/* index length */
	lf_put32(ir + 24, 32);			/* index allocated size */
	lf_put32(ir + 28, 0);			/* flags */
	lf_put16(ir + 32, 16);			/* END entry: length */
	lf_put16(ir + 34, 0);			/* key length */
	lf_put16(ir + 36, 2);			/* flags: INDEX_ENTRY_END */
	put_res_attr(rec, off, ATTR_TYPE_INDEX_ROOT, ir, sizeof(ir), 1);
}

static void vol_put_record(struct vol *v, uint64_t no, const uint8_t *deprot)
{
	uint8_t *dst = v->disk + MFT_LCN * VCS + no * VRS;
	memcpy(dst, deprot, VRS);
	ntfs_log_fixup_pre_write(dst, VRS, 512);
}

static void vol_init(struct vol *v)
{
	uint8_t rec[VRS];
	memset(v, 0, sizeof(*v));
	v->write_limit = -1;
	v->disk = calloc(VOL_CLUSTERS, VCS);
	build_record(rec, 0, MFT_RECORD_IN_USE, fill_mft);
	vol_put_record(v, 0, rec);
	build_record(rec, 6, MFT_RECORD_IN_USE, fill_bitmap);
	vol_put_record(v, 6, rec);
	build_record(rec, 30, MFT_RECORD_IN_USE, fill_plain);
	vol_put_record(v, 30, rec);
	build_record(rec, 31, MFT_RECORD_IN_USE, fill_index);
	vol_put_record(v, 31, rec);
	build_record(rec, DIR_REC, MFT_RECORD_IN_USE | MFT_RECORD_IS_DIRECTORY,
		     fill_dir);
	vol_put_record(v, DIR_REC, rec);
}

static int v_read_mft(void *ctx, uint64_t no, void *buf)
{
	struct vol *v = ctx;
	if (no >= MFT_CLUSTERS * VCS / VRS)
		return -ENOENT;
	memcpy(buf, v->disk + MFT_LCN * VCS + no * VRS, VRS);
	return 0;
}
static int v_write_mft(void *ctx, uint64_t no, const void *buf)
{
	struct vol *v = ctx;
	if (v->write_limit == 0)
		return -EIO;
	if (v->write_limit > 0)
		v->write_limit--;
	if (no >= MFT_CLUSTERS * VCS / VRS)
		return -ENOENT;
	memcpy(v->disk + MFT_LCN * VCS + no * VRS, buf, VRS);
	v->mft_writes++;
	v->last_mft_no = no;
	return 0;
}
static int v_read_clu(void *ctx, uint64_t lcn, uint32_t n, void *buf)
{
	struct vol *v = ctx;
	if (v->write_errno)
		return v->write_errno;
	if (lcn + n > VOL_CLUSTERS)
		return -EIO;
	memcpy(buf, v->disk + lcn * VCS, (size_t)n * VCS);
	return 0;
}
static int v_write_clu(void *ctx, uint64_t lcn, uint32_t n, const void *buf)
{
	struct vol *v = ctx;
	if (v->write_limit == 0)
		return -EIO;
	if (v->write_limit > 0)
		v->write_limit--;
	if (lcn + n > VOL_CLUSTERS)
		return -EIO;
	memcpy(v->disk + lcn * VCS, buf, (size_t)n * VCS);
	v->clu_writes += (int)n;
	return 0;
}
static int v_sync(void *ctx)
{
	struct vol *v = ctx;
	v->syncs++;
	return v->sync_errno;
}

static void vol_apply(struct vol *v, struct ntfs_log_apply *ap)
{
	memset(ap, 0, sizeof(*ap));
	ap->ctx = v;
	ap->read_mft_record = v_read_mft;
	ap->write_mft_record = v_write_mft;
	ap->read_clusters = v_read_clu;
	ap->write_clusters = v_write_clu;
	ap->sync = v_sync;
}

/* ---- Tests -------------------------------------------------------------- */

struct rec_list { uint64_t lsn[256]; uint16_t redo[256]; uint32_t len[256]; unsigned n; uint32_t last_data_crc; };

static int collect(const struct ntfs_log_record *r, void *ctx)
{
	struct rec_list *l = ctx;
	if (l->n < 256) {
		l->lsn[l->n] = r->lsn;
		l->redo[l->n] = r->redo_op;
		l->len[l->n] = r->data_len;
		if (r->record_type == LFS_RECORD_TYPE_CLIENT && r->redo && r->redo_len) {
			uint32_t i, c = 0;
			for (i = 0; i < r->redo_len; i++)
				c = c * 31 + ((const uint8_t *)r->redo)[i];
			l->last_data_crc = c;
		}
	}
	l->n++;
	return 0;
}

static void test_states(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;

	printf("test_states\n");
	/* Empty file. */
	lb_init(&b, FIRST_PAGE, 2);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	memset(b.buf, 0xff, LSIZE);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_EMPTY);
	ntfs_logfile_close(log);

	/* Open, flag clear: dirty. */
	/* ASan (2026-11-26): lb_init() mallocs a fresh buffer; re-init
	 * without freeing the first one leaked 256 KiB. */
	free(b.buf);
	lb_init(&b, FIRST_PAGE, 2);
	{
		uint64_t l = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
		lb_restart(&b, l, b.last_len, l, 0, 0, false);
		lb_protect(&b);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	CHECK(log && !ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.major_ver, 1);
	CHECK_EQ(info.minor_ver, 1);
	CHECK_EQ(info.log_page_size, PS);
	CHECK_EQ(info.seq_number_bits, SEQ_BITS);
	/* Open with VOLUME_IS_CLEAN: clean. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, RESTART_VOLUME_IS_CLEAN, false);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	/* Closed: clean. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, 0, true);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	/* Unsupported version. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, 0, false);
	lf_put16(b.buf + RP_MAJOR_VER, 3);
	lf_put16(b.buf + PS + RP_MAJOR_VER, 3);
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_UNSUPPORTED);
	CHECK(!ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	/* Both restart pages garbage: corrupt. */
	memset(b.buf, 0x5a, 2 * PS);
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_CORRUPT);
	CHECK(!ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	/* Second page newer than first: it is used. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, 0, false);
	{
		uint8_t *p = b.buf;
		ntfs_log_fixup_post_read(p, PS, 512, NULL);
		lf_put64(p + 0x30 + RA_CURRENT_LSN, b.last_lsn - 8);
		ntfs_log_fixup_pre_write(p, PS, 512);
	}
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK(info.restart_page_used[1]);
	CHECK_EQ(info.current_lsn, b.last_lsn);
	ntfs_logfile_close(log);
	free(b.buf);
}

static void test_walk_wrap_multipage(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;
	struct rec_list l;
	uint64_t ckpt, restart_lsn, lsn_at_ra = 0, big_lsn = 0, last;
	uint32_t len_at_ra = 0;
	uint8_t big[6000];
	unsigned i, expect = 0;
	uint32_t crc = 0;

	printf("test_walk_wrap_multipage\n");
	for (i = 0; i < sizeof(big); i++) {
		big[i] = (uint8_t)(i * 7 + 3);
		crc = crc * 31 + big[i];
	}
	/* Start near the end so the records wrap to the first record page. */
	lb_init(&b, LSIZE - 6 * PS, 5);
	ckpt = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);		/* start of checkpoint */
	restart_lsn = lb_checkpoint(&b, ckpt, 0, 0, 0, 0, 0, 0);
	/* Filler records of 1500 bytes each; 25 of them cross the wrap. */
	for (i = 0; i < 25; i++) {
		uint8_t pad[1500];
		struct crec c;
		memset(pad, (int)i, sizeof(pad));
		memset(&c, 0, sizeof(c));
		c.redo_op = LOP_Noop;
		c.undo_op = LOP_Noop;
		c.redo = pad;
		c.redo_len = sizeof(pad);
		last = lb_client(&b, 0x18, 0, &c);
		expect++;
		if (i == 10) {
			lsn_at_ra = last;
			len_at_ra = b.last_len;
		}
	}
	CHECK(b.wrapped);
	/* A multi-page record. */
	{
		struct crec c;
		memset(&c, 0, sizeof(c));
		c.redo_op = LOP_Noop;
		c.undo_op = LOP_Noop;
		c.redo = big;
		c.redo_len = sizeof(big);
		big_lsn = lb_client(&b, 0x18, 0, &c);
		expect++;
	}
	last = lb_simple(&b, 0x18, LOP_CommitTransaction, LOP_Noop);
	expect++;
	/* Restart area lags behind: it knows only record 10. */
	lb_restart(&b, lsn_at_ra, len_at_ra, ckpt, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	CHECK(log);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.current_lsn, lsn_at_ra);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK(info.tail_scanned);
	CHECK_EQ(info.last_lsn, last);
	CHECK_EQ(info.checkpoint_lsn, ckpt);
	/* walk yields the checkpoint record itself first, then everything after */
	CHECK_EQ(l.n, expect + 2);
	if (l.n < 2 || l.n > 256) {
		ntfs_logfile_close(log);
		free(b.buf);
		return;
	}
	CHECK_EQ(l.lsn[0], ckpt);
	CHECK_EQ(l.lsn[1], restart_lsn);
	CHECK_EQ(l.lsn[l.n - 1], last);
	CHECK_EQ(l.lsn[l.n - 2], big_lsn);
	CHECK_EQ(l.len[l.n - 2], NR_HEADER_SIZE + 8 + sizeof(big));
	CHECK_EQ(l.redo[l.n - 1], LOP_CommitTransaction);
	/* multi-page payload intact: recompute crc over the big record only */
	{
		struct rec_list l2;
		memset(&l2, 0, sizeof(l2));
		CHECK_EQ(ntfs_logfile_walk(log, big_lsn, collect, &l2), 0);
		CHECK_EQ(l2.n, 2);
		l2.n = 0;
		/* first record yielded is big_lsn; crc captured when yielded */
		memset(&l2, 0, sizeof(l2));
		ntfs_logfile_walk(log, big_lsn, collect, &l2);
	}
	/* Walk from an explicit lsn. */
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, big_lsn, collect, &l), 0);
	CHECK_EQ(l.n, 2);
	CHECK_EQ(l.lsn[0], big_lsn);
	/* crc of the last client record with redo data seen in that walk = big */
	CHECK_EQ(l.last_data_crc, crc);
	/* Replay: nothing dirty, one committed-but-forgotten? No: transaction 0x18
	 * was committed, has no dirty pages -> no plan, no error. */
	{
		struct vol v;
		struct ntfs_log_apply ap;
		struct ntfs_log_replay_result res;
		vol_init(&v);
		vol_apply(&v, &ap);
		CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
		CHECK_EQ(res.records_analyzed, expect + 1);	/* + the restart record */
		CHECK_EQ(res.transactions_committed, 1);
		CHECK_EQ(res.plan_len, 0);
		CHECK(!res.needs_chkdsk);
		free(v.disk);
	}
	ntfs_logfile_close(log);
	free(b.buf);
}

/* Build the standard replay scenario. @commit selects committed vs active.
 * *@bits_lsn (optional) receives the lsn of the SetBits record. */
static uint64_t build_bitmap_scenario(struct lb *b, bool commit, bool bogus_attr, uint64_t *bits_lsn)
{
	uint8_t oat[256];
	uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
	uint64_t start, oa_lsn, restart_lsn, l;
	struct crec c;
	uint8_t br[BR_SIZE];

	lb_init(b, FIRST_PAGE, 2);
	start = lb_simple(b, 0x18, LOP_Noop, LOP_Noop);
	oa_lsn = lb_table_dump(b, LOP_OpenAttributeTableDump, oat, oatn);
	restart_lsn = lb_checkpoint(b, 0, oa_lsn, oatn, 0, 0, 0, 0);

	lf_put32(br + BR_BITMAP_OFF, 100);
	lf_put32(br + BR_BITS, 3);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_SetBitsInNonresidentBitMap;
	c.undo_op = LOP_ClearBitsInNonresidentBitMap;
	c.target_attr = bogus_attr ? 0x68 : 0x40;
	c.lcns = 1;
	c.lcn[0] = BITMAP_LCN;
	c.redo = br; c.redo_len = BR_SIZE;
	c.undo = br; c.undo_len = BR_SIZE;
	l = lb_client(b, 0x40, 0, &c);
	if (bits_lsn)
		*bits_lsn = l;
	if (commit) {
		lb_simple(b, 0x40, LOP_CommitTransaction, LOP_Noop);
		l = lb_simple(b, 0x40, LOP_ForgetTransaction, LOP_Noop);
	}
	lb_restart(b, b->last_lsn, b->last_len, start, restart_lsn, 0, false);
	lb_protect(b);
	return l;
}

static void test_redo_bitmap(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	struct ntfs_logfile_info info;

	printf("test_redo_bitmap\n");
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	/* dry run */
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_analyzed, 3);	/* SetBits, Commit, Forget */
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.records_undone, 0);
	CHECK_EQ(res.dirty_pages, 1);
	CHECK_EQ(res.transactions_active, 0);
	CHECK_EQ(res.plan_len, 1);
	if (res.plan_len == 1) {
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_CLUSTERS);
		CHECK_EQ(res.plan[0].lcn, BITMAP_LCN);
		CHECK_EQ(res.plan[0].count, 1);
		CHECK_EQ(res.plan[0].op, LOP_SetBitsInNonresidentBitMap);
		CHECK(!res.plan[0].undo);
	}
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(v.mft_writes, 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.table_open_attrs, 2);
	CHECK_EQ(info.table_dirty_pages, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0);
	/* real run */
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(v.mft_writes, 0);
	CHECK_EQ(v.syncs, 1);
	CHECK_EQ(res.clusters_written, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);	/* bits 100..102 */
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 13], 0);
	/* mark clean and reopen */
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	CHECK_EQ(m.writes, 2);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_CLEAN);
	CHECK_EQ(info.open_log_count, 8);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_undo_bitmap(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_undo_bitmap\n");
	build_bitmap_scenario(&b, false, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	/* the page was flushed before the crash: bits already set */
	v.disk[BITMAP_LCN * VCS + 12] = 0x70;
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.records_undone, 1);
	CHECK_EQ(res.transactions_active, 1);
	CHECK_EQ(res.plan_len, 2);
	if (res.plan_len == 2) {
		CHECK(!res.plan[0].undo);
		CHECK(res.plan[1].undo);
		CHECK_EQ(res.plan[1].op, LOP_ClearBitsInNonresidentBitMap);
	}
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_refuse_incoherent(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_refuse_incoherent\n");
	/* Target attribute not in the open attribute table. */
	build_bitmap_scenario(&b, true, true, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(v.mft_writes, 0);
	CHECK_EQ(v.syncs, 0);
	ntfs_logfile_close(log);
	free(b.buf);

	/*
	 * Bitmap record whose logged lcn is unreadable (beyond the volume).
	 * The page LCN in the log record is what replay reads and writes
	 * (dirty page table semantics), so that is what must be corrupted;
	 * the read fails and nothing is written.
	 */
	{
		uint64_t bits_lsn;
		uint32_t vbo;
		uint8_t *p;
		build_bitmap_scenario(&b, true, false, &bits_lsn);
		vbo = (uint32_t)(bits_lsn << 3) & (LSIZE - 1);
		p = b.buf + (vbo & ~(PS - 1));
		ntfs_log_fixup_post_read(p, PS, 512, NULL);
		lf_put64(b.buf + vbo + LR_HEADER_SIZE + NR_PAGE_LCNS, VOL_CLUSTERS + 5);
		ntfs_log_fixup_pre_write(p, PS, 512);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_replay(log, &ap, false, &res) < 0);
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(v.mft_writes, 0);
	ntfs_logfile_close(log);
	free(b.buf);

	/*
	 * Stale run list: record 6's $DATA says the bitmap lives at lcn 41,
	 * the log says the dirty page is at lcn 40. The LCN logged by NTFS at
	 * the time of the operation wins (that is why the LFS records LCNs at
	 * all: the owning record may not have been flushed yet), so replay
	 * writes cluster 40 and leaves 41 alone. Refusing here would break the
	 * file-extension case the module exists for.
	 */
	build_bitmap_scenario(&b, true, false, NULL);
	{
		uint8_t rec[VRS];
		uint32_t off;
		memcpy(rec, v.disk + MFT_LCN * VCS + 6 * VRS, VRS);
		ntfs_log_fixup_post_read(rec, VRS, 512, NULL);
		off = lf_get16(rec + MR_ATTRS_OFFSET) + AT_NONRESIDENT_SIZE;
		rec[off + 2] = (uint8_t)(BITMAP_LCN + 1);
		vol_put_record(&v, 6, rec);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(res.plan_len, 1);
	if (res.plan_len == 1)
		CHECK_EQ(res.plan[0].lcn, BITMAP_LCN);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);
	CHECK_EQ(v.disk[(BITMAP_LCN + 1) * VCS + 12], 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);

	/*
	 * MFT op whose target vcn lies outside $MFT's run list: $MFT never
	 * shrinks, so the log and record 0 disagree; refused, not skipped.
	 */
	{
		uint8_t oat[256], newrec[VRS];
		uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
		uint64_t start, oa_lsn, restart_lsn;
		struct crec c;
		lb_init(&b, FIRST_PAGE, 2);
		start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
		oa_lsn = lb_table_dump(&b, LOP_OpenAttributeTableDump, oat, oatn);
		restart_lsn = lb_checkpoint(&b, 0, oa_lsn, oatn, 0, 0, 0, 0);
		build_record(newrec, 4 * MFT_CLUSTERS, MFT_RECORD_IN_USE, fill_plain);
		memset(&c, 0, sizeof(c));
		c.redo_op = LOP_InitializeFileRecordSegment;
		c.undo_op = LOP_Noop;
		c.target_attr = 0x18;
		c.lcns = 1;
		c.lcn[0] = MFT_LCN + MFT_CLUSTERS;	/* one past the end of $MFT */
		c.target_vcn = MFT_CLUSTERS;
		c.redo = newrec;
		c.redo_len = (uint16_t)lf_get32(newrec + MR_BYTES_IN_USE);
		lb_client(&b, 0x40, 0, &c);
		lb_simple(&b, 0x40, LOP_CommitTransaction, LOP_Noop);
		lb_simple(&b, 0x40, LOP_ForgetTransaction, LOP_Noop);
		lb_restart(&b, b.last_lsn, b.last_len, start, restart_lsn, 0, false);
		lb_protect(&b);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	vol_init(&v);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(v.mft_writes + v.clu_writes, 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_mft_record_ops(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	uint8_t oat[256], newrec[VRS], got[VRS];
	uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
	uint64_t start, oa_lsn, restart_lsn, init_lsn;
	struct crec c;
	static const uint8_t newsi[0x30] = { 9, 9, 9, 9 };

	printf("test_mft_record_ops\n");
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	oa_lsn = lb_table_dump(&b, LOP_OpenAttributeTableDump, oat, oatn);
	restart_lsn = lb_checkpoint(&b, 0, oa_lsn, oatn, 0, 0, 0, 0);

	/* InitializeFileRecordSegment of record 40 (page: vcn 10 -> lcn 26). */
	build_record(newrec, 40, MFT_RECORD_IN_USE, fill_plain);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_InitializeFileRecordSegment;
	c.undo_op = LOP_Noop;
	c.target_attr = 0x18;
	c.lcns = 1;
	c.lcn[0] = MFT_LCN + 10;
	c.target_vcn = 10;
	c.cluster_off = 0;		/* record 40 = byte 40960 = vcn 10 + 0 */
	c.redo = newrec;
	c.redo_len = (uint16_t)lf_get32(newrec + MR_BYTES_IN_USE);
	init_lsn = lb_client(&b, 0x40, 0, &c);
	/* UpdateResidentValue on record 30: $STANDARD_INFORMATION value. */
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_UpdateResidentValue;
	c.undo_op = LOP_UpdateResidentValue;
	c.target_attr = 0x18;
	c.lcns = 1;
	c.lcn[0] = MFT_LCN + 7;			/* record 30 = byte 30720 = vcn 7 + 2048 */
	c.target_vcn = 7;
	c.cluster_off = 2048 / 512;
	c.record_off = MR_FIXUP_OFFSET_3 + 8;	/* first attribute */
	c.attr_off = AT_RESIDENT_SIZE;		/* value */
	c.redo = newsi; c.redo_len = sizeof(newsi);
	c.undo = newsi; c.undo_len = sizeof(newsi);
	lb_client(&b, 0x40, 0, &c);
	lb_simple(&b, 0x40, LOP_CommitTransaction, LOP_Noop);
	lb_simple(&b, 0x40, LOP_ForgetTransaction, LOP_Noop);
	lb_restart(&b, b.last_lsn, b.last_len, start, restart_lsn, 0, false);
	lb_protect(&b);

	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_redone, 2);
	CHECK_EQ(res.plan_len, 2);
	if (res.plan_len == 2) {
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_MFT_RECORD);
		CHECK_EQ(res.plan[0].mft_no, 40);
		CHECK_EQ(res.plan[1].kind, NTFS_PLAN_MFT_RECORD);
		CHECK_EQ(res.plan[1].mft_no, 30);
	}
	CHECK_EQ(v.mft_writes, 0);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(v.mft_writes, 2);
	CHECK_EQ(v.clu_writes, 0);
	/* record 40 now exists, protected, with the record lsn stamped */
	memcpy(got, v.disk + MFT_LCN * VCS + 40 * VRS, VRS);
	CHECK_EQ(lf_get32(got), LFS_MAGIC_FILE);
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	CHECK_EQ(lf_get64(got + MR_LSN), init_lsn);
	CHECK_EQ(lf_get32(got + MR_RECORD_NUMBER), 40);
	CHECK(!memcmp(got + MR_ATTRS_OFFSET, newrec + MR_ATTRS_OFFSET, 2));
	/* record 30's value updated */
	memcpy(got, v.disk + MFT_LCN * VCS + 30 * VRS, VRS);
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	CHECK(!memcmp(got + MR_FIXUP_OFFSET_3 + 8 + AT_RESIDENT_SIZE, newsi, sizeof(newsi)));
	/* Replaying again is idempotent: records carry lsn >= record lsn. */
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK_EQ(res.records_redone, 0);
	CHECK_EQ(res.plan_len, 2);
	if (res.plan_len == 2)
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_SKIPPED);
	ntfs_logfile_close(log);

	/* Wrong logged lcn for the MFT page: refused, nothing written. */
	{
		uint8_t *p = b.buf + FIRST_PAGE;
		uint32_t vbo = (uint32_t)(init_lsn << 3) & (LSIZE - 1);
		ntfs_log_fixup_post_read(p, PS, 512, NULL);
		lf_put64(b.buf + vbo + LR_HEADER_SIZE + NR_PAGE_LCNS, MFT_LCN + 11);
		ntfs_log_fixup_pre_write(p, PS, 512);
		/* ASan (2026-11-26): vol_init() callocs a fresh disk; re-init
		 * without freeing the first one leaked 256 KiB. */
		free(v.disk);
		vol_init(&v);
	}
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(v.mft_writes, 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_tail_copy_and_torn(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;
	struct rec_list l;
	uint64_t start, restart_lsn, last, tail_lsn;
	uint32_t last_page;
	unsigned i;

	printf("test_tail_copy_and_torn\n");
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < 3; i++)
		last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	/* Tail copy at page 2 holds one more record than the in-place page. */
	tail_lsn = lb_make_tail_copy(&b, last_page, b.seq);
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.tail_overrides, 1);
	CHECK_EQ(info.last_lsn, tail_lsn);
	CHECK_EQ(l.n, 6);
	CHECK_EQ(l.lsn[l.n - 1], tail_lsn);
	CHECK_EQ(l.lsn[l.n - 2], last);
	/* mark_clean writes the superseding copy back, then both restart pages */
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	CHECK_EQ(m.writes, 3);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	{
		uint8_t pg[PS];
		memcpy(pg, b.buf + last_page, PS);
		CHECK_EQ(ntfs_log_fixup_post_read(pg, PS, 512, NULL), 0);
		CHECK_EQ(lf_get64(pg + PG_LAST_END_LSN), tail_lsn);
		CHECK_EQ(lf_get64(pg + PG_LAST_LSN), tail_lsn);
	}
	free(b.buf);

	/* Torn last page without tail copy: log ends at the previous page. */
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < 3; i++)
		last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	lb_next_page(&b);
	lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	b.buf[last_page + 512 - 2] ^= 0xff;	/* sector 0 tail mismatch */
	m.buf = b.buf;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.last_lsn, last);
	CHECK_EQ(l.n, 5);
	ntfs_logfile_close(log);
	/* ... but a valid page from the expected sequence after the torn one is corruption. */
	{
		struct lb b2;
		lb_init(&b2, FIRST_PAGE, 2);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_next_page(&b2);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_next_page(&b2);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_protect(&b2);
		/* page after the torn one, with the same (expected) sequence */
		memcpy(b.buf + last_page + PS, b2.buf + FIRST_PAGE + 2 * PS, PS);
		free(b2.buf);
	}
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), -EINVAL);
	{
		struct vol v;
		struct ntfs_log_apply ap;
		struct ntfs_log_replay_result res;
		vol_init(&v);
		vol_apply(&v, &ap);
		CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
		CHECK(res.needs_chkdsk);
		CHECK_EQ(v.mft_writes + v.clu_writes, 0);
		free(v.disk);
	}
	ntfs_logfile_close(log);
	free(b.buf);
}

static int oa_cb(const struct ntfs_log_open_attr *a, void *ctx)
{
	int *n = ctx;
	(*n)++;
	if (a->id == 0x18) {
		CHECK_EQ(a->mft_no, 0);
		CHECK_EQ(a->type, ATTR_TYPE_DATA);
	} else {
		CHECK_EQ(a->id, 0x40);
		CHECK_EQ(a->mft_no, 6);
	}
	return 0;
}

static void test_tables_api(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	int n_oa = 0;
	struct ntfs_log_table_cbs cbs;

	printf("test_tables_api\n");
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_load_checkpoint(log), 0);
	memset(&cbs, 0, sizeof(cbs));
	cbs.open_attr = oa_cb;
	CHECK_EQ(ntfs_logfile_tables(log, &cbs, &n_oa), 0);
	CHECK_EQ(n_oa, 2);
	ntfs_logfile_close(log);
	free(b.buf);
}

/*
 * Version 2.0 log (Windows 8+ layout as inferred from fslog.c): restart
 * pages at 0/1, 32 tail-copy slots at 0x02..0x21, record pages from 0x22,
 * file_off at 0x3c of the page header. Same scenario as test_redo_bitmap
 * plus a v2 tail copy that supersedes its in-place page. No real 2.0 log
 * has been seen by this code: this pins the inferred layout, nothing more.
 */
static void test_v2_log(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	struct rec_list l;
	uint64_t start, restart_lsn, last = 0, tail_lsn;
	uint32_t last_page;
	unsigned i;

	printf("test_v2_log\n");
	lb_set_version(2, 0);
	CHECK_EQ(FIRST_PAGE, 0x22 * PS);

	/* Replay of the standard scenario. */
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	CHECK(log);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.major_ver, 2);
	CHECK_EQ(info.minor_ver, 0);
	CHECK(!ntfs_logfile_is_clean(log));
	vol_init(&v);
	vol_apply(&v, &ap);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.plan_len, 1);
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);

	/* Tail copy at slot 0x02 with file_off naming the last record page. */
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < 3; i++)
		last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	tail_lsn = lb_make_tail_copy(&b, last_page, b.seq);
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf; m.writes = 0;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.tail_overrides, 1);
	CHECK_EQ(info.last_lsn, tail_lsn);
	CHECK_EQ(l.n, 6);
	CHECK_EQ(l.lsn[l.n - 1], tail_lsn);
	CHECK_EQ(l.lsn[l.n - 2], last);
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	CHECK_EQ(m.writes, 3);
	ntfs_logfile_close(log);
	{
		uint8_t pg[PS];
		memcpy(pg, b.buf + last_page, PS);
		CHECK_EQ(ntfs_log_fixup_post_read(pg, PS, 512, NULL), 0);
		CHECK_EQ(lf_get64(pg + PG_LAST_END_LSN), tail_lsn);
		CHECK_EQ(lf_get32(pg + PG_FILE_OFFSET), 0);	/* in-place page again */
	}
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	free(b.buf);

	/* A stray tail copy sitting in the record area is not a log page. */
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	lb_next_page(&b);
	lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	lf_put32(b.buf + b.page + PG_FILE_OFFSET, last_page);	/* claims to be a copy of the previous page */
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.last_lsn, last);
	CHECK_EQ(l.n, 3);
	ntfs_logfile_close(log);
	free(b.buf);

	lb_set_version(1, 1);
}

/* Fixture images from the tools stream: freshly formatted, must be clean. */
static void test_images(void)
{
	const char *dir = getenv("NTFS_IMAGES");
	DIR *d;
	struct dirent *de;
	int seen = 0;

	if (!dir || !(d = opendir(dir))) {
		printf("test_images: NTFS_IMAGES not set or missing, skipped\n");
		return;
	}
	while ((de = readdir(d))) {
		char path[1024];
		struct ntfs_image img;
		struct ntfs_log_io io;
		struct ntfs_log_geometry g;
		ntfs_logfile_t *log = NULL;
		struct ntfs_logfile_info info;
		size_t n = strlen(de->d_name);
		if (n < 4 || strcmp(de->d_name + n - 4, ".img"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (ntfs_image_open(&img, path, false, 0, 0)) {
			printf("test_images: %s: %s (skipped)\n", de->d_name, img.err);
			ntfs_image_close(&img);
			continue;
		}
		seen++;
		ntfs_image_log_io(&img, &io);
		ntfs_image_geometry(&img, &g);
		CHECK_EQ(ntfs_logfile_open(&io, &g, &log), 0);
		if (log) {
			ntfs_logfile_get_info(log, &info);
			printf("test_images: %s: state %d clean=%d\n", de->d_name, info.state,
			       ntfs_logfile_is_clean(log));
			CHECK(ntfs_logfile_is_clean(log));
			ntfs_logfile_close(log);
		}
		ntfs_image_close(&img);
	}
	closedir(d);
	printf("test_images: %d image(s)\n", seen);
}

/*
 * A device that accepts every write and then refuses the barrier.
 *
 * Not hypothetical: macOS FSKit's metadataFlush fails on every USB device on
 * the development machine (20 failures in 45 minutes on one stick), and a real
 * replay on a real Windows 10 volume reported itself as "Replay failed part-way.
 * The volume may be inconsistent" when every write had in fact landed and chkdsk
 * afterwards found no problems. The distinction matters: a failed write can
 * leave a volume half-applied, a failed barrier only means the device would not
 * confirm what it has already taken.
 */
static void test_flush_failure_is_not_a_partial_write(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_flush_failure_is_not_a_partial_write\n");

	/* barrier refused, writes fine */
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	v.sync_errno = -EIO;
	memset(&res, 0, sizeof(res));
	CHECK(ntfs_logfile_replay(log, &ap, false, &res) != 0);
	CHECK(res.flush_failed);
	CHECK_EQ(v.syncs, 1);
	CHECK_EQ(v.clu_writes, 1);	/* the write still happened */
	CHECK_EQ(res.clusters_written, 1);
	ntfs_logfile_close(log);
	/* ASan (2026-11-26): free both buffers before the second block
	 * re-inits them, or 2 x 256 KiB leak per block. */
	free(b.buf);
	free(v.disk);

	/* a genuine write failure must NOT be reported as a flush failure */
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	v.write_errno = -EIO;
	memset(&res, 0, sizeof(res));
	CHECK(ntfs_logfile_replay(log, &ap, false, &res) != 0);
	CHECK(!res.flush_failed);
	ntfs_logfile_close(log);
	free(b.buf);
	free(v.disk);
}

/* ---- §1.3: replay atomicity ------------------------------------------- */

/*
 * Build the cut-point scenario: one committed transaction whose redo
 * dirties two MFT records and one bitmap cluster, so the flush has
 * three writes plus the barrier. The MFT half is test_mft_record_ops'
 * (InitializeFileRecordSegment of record 40, UpdateResidentValue on
 * record 30); the cluster half is build_bitmap_scenario's SetBits on
 * BITMAP_LCN, bits 100..102. The plan must be [MFT 40, MFT 30,
 * clusters BITMAP_LCN] — the flush writes MFT records first, then
 * clusters, then the barrier.
 */
static void build_replay_cut_scenario(struct lb *b)
{
	uint8_t oat[256], newrec[VRS], br[BR_SIZE];
	uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
	uint64_t start, oa_lsn, restart_lsn;
	struct crec c;
	static const uint8_t newsi[0x30] = { 9, 9, 9, 9 };

	lb_init(b, FIRST_PAGE, 2);
	start = lb_simple(b, 0x18, LOP_Noop, LOP_Noop);
	oa_lsn = lb_table_dump(b, LOP_OpenAttributeTableDump, oat, oatn);
	restart_lsn = lb_checkpoint(b, 0, oa_lsn, oatn, 0, 0, 0, 0);

	/* InitializeFileRecordSegment of record 40 (page: vcn 10 -> lcn 26). */
	build_record(newrec, 40, MFT_RECORD_IN_USE, fill_plain);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_InitializeFileRecordSegment;
	c.undo_op = LOP_Noop;
	c.target_attr = 0x18;
	c.lcns = 1;
	c.lcn[0] = MFT_LCN + 10;
	c.target_vcn = 10;
	c.cluster_off = 0;
	c.redo = newrec;
	c.redo_len = (uint16_t)lf_get32(newrec + MR_BYTES_IN_USE);
	lb_client(b, 0x40, 0, &c);
	/* UpdateResidentValue on record 30: $STANDARD_INFORMATION value. */
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_UpdateResidentValue;
	c.undo_op = LOP_UpdateResidentValue;
	c.target_attr = 0x18;
	c.lcns = 1;
	c.lcn[0] = MFT_LCN + 7;
	c.target_vcn = 7;
	c.cluster_off = 2048 / 512;
	c.record_off = MR_FIXUP_OFFSET_3 + 8;
	c.attr_off = AT_RESIDENT_SIZE;
	c.redo = newsi; c.redo_len = sizeof(newsi);
	c.undo = newsi; c.undo_len = sizeof(newsi);
	lb_client(b, 0x40, 0, &c);
	/* SetBits in the bitmap cluster, bits 100..102. */
	lf_put32(br + BR_BITMAP_OFF, 100);
	lf_put32(br + BR_BITS, 3);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_SetBitsInNonresidentBitMap;
	c.undo_op = LOP_ClearBitsInNonresidentBitMap;
	c.target_attr = 0x40;
	c.lcns = 1;
	c.lcn[0] = BITMAP_LCN;
	c.redo = br; c.redo_len = BR_SIZE;
	c.undo = br; c.undo_len = BR_SIZE;
	lb_client(b, 0x40, 0, &c);
	lb_simple(b, 0x40, LOP_CommitTransaction, LOP_Noop);
	lb_simple(b, 0x40, LOP_ForgetTransaction, LOP_Noop);
	lb_restart(b, b->last_lsn, b->last_len, start, restart_lsn, 0, false);
	lb_protect(b);
}

/*
 * GAPS.md §1.3 — replay atomicity, the gate for §1.2 Phase 0 (and for
 * any later performance work that touches the write path). Finding 17b:
 * flush writes records one at a time and the overlay protects planning
 * only, so nothing demonstrated a partial flush and nothing ruled one
 * out. This test is the proof, in the same cut-point shape as
 * test_crash.c's table: run the replay against a device that stops
 * accepting writes after N of them — the fault-injection model of a
 * process killed mid-flush, where the writes before the cut landed and
 * the writes after it did not — then run the replay again with the
 * device restored and require the volume to come out byte-identical
 * to an uninterrupted replay. A replay that is atomic must converge:
 * whatever the cut, the second run reaches exactly the state the
 * uninterrupted run reached, never a torn mixture, and never claims
 * success for the cut run.
 *
 * The cut is a write refusal through the apply callbacks, which are
 * the device writes: flush_overlay is their only caller. A SIGKILL
 * mid-flush leaves the same on-disk state (the writes before the cut
 * landed, the rest did not); only the reporting differs, and the cut
 * run is required to report failure, never success.
 */
static void test_replay_cut_points(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	uint8_t *ref_disk, *log_copy;
	int ref_mft, ref_clu, ref_syncs, total, cut;
	int n_converged = 0, n_cuts = 0;

	printf("test_replay_cut_points\n");

	/* Reference run: uninterrupted replay, snapshot the final disk. */
	build_replay_cut_scenario(&b);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	memset(&res, 0, sizeof(res));
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK_EQ(res.plan_len, 3);
	if (res.plan_len == 3) {
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_MFT_RECORD);
		CHECK_EQ(res.plan[0].mft_no, 40);
		CHECK_EQ(res.plan[1].kind, NTFS_PLAN_MFT_RECORD);
		CHECK_EQ(res.plan[1].mft_no, 30);
		CHECK_EQ(res.plan[2].kind, NTFS_PLAN_CLUSTERS);
		CHECK_EQ(res.plan[2].lcn, BITMAP_LCN);
	}
	memset(&res, 0, sizeof(res));
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_redone, 3);
	ref_mft = v.mft_writes;
	ref_clu = v.clu_writes;
	ref_syncs = v.syncs;
	CHECK_EQ(ref_mft, 2);
	CHECK_EQ(ref_clu, 1);
	CHECK_EQ(ref_syncs, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);
	ref_disk = malloc(VOL_CLUSTERS * VCS);
	CHECK(ref_disk != NULL);
	if (!ref_disk) { ntfs_logfile_close(log); free(b.buf); free(v.disk); return; }
	memcpy(ref_disk, v.disk, VOL_CLUSTERS * VCS);
	log_copy = malloc(LSIZE);
	CHECK(log_copy != NULL);
	if (!log_copy) { ntfs_logfile_close(log); free(b.buf); free(v.disk); free(ref_disk); return; }
	memcpy(log_copy, b.buf, LSIZE);
	ntfs_logfile_close(log);
	free(b.buf);
	free(v.disk);
	total = ref_mft + ref_clu;

	printf("  cut  landed  replay2  disk==ref  log untouched\n");
	printf("  ---  ------  -------  ----------  -------------\n");

	/* Write cuts 0..total: cut writes land, the next is refused. */
	for (cut = 0; cut <= total; cut++) {
		int landed, rc1, rc2, disk_ok, log_ok;
		int mft_before, clu_before;

		build_replay_cut_scenario(&b);
		m.buf = b.buf; m.size = LSIZE; m.writes = 0;
		log = open_log(&m, &io);
		vol_init(&v);
		vol_apply(&v, &ap);
		v.write_limit = cut;
		memset(&res, 0, sizeof(res));
		rc1 = ntfs_logfile_replay(log, &ap, false, &res);
		landed = v.mft_writes + v.clu_writes;
		/* The cut run must not claim success; the log must be
		 * untouched by it, or the second run could not converge. */
		if (cut < total) {
			CHECK(rc1 != 0);
			CHECK_EQ(landed, cut);
		} else {
			CHECK_EQ(rc1, 0);
			CHECK_EQ(landed, total);
		}
		CHECK(memcmp(b.buf, log_copy, LSIZE) == 0);
		CHECK_EQ(m.writes, 0);
		log_ok = memcmp(b.buf, log_copy, LSIZE) == 0 && m.writes == 0;
		/* Restore the device and replay again on the same log. */
		v.write_limit = -1;
		mft_before = v.mft_writes;
		clu_before = v.clu_writes;
		memset(&res, 0, sizeof(res));
		rc2 = ntfs_logfile_replay(log, &ap, false, &res);
		CHECK_EQ(rc2, 0);
		CHECK(!res.needs_chkdsk);
		disk_ok = memcmp(v.disk, ref_disk, VOL_CLUSTERS * VCS) == 0;
		CHECK(disk_ok);
		/* The second run rewrites what the cut missed; pages the
		 * cut already landed are skipped by LSN, so its write
		 * count varies by cut — only the final disk is asserted. */
		(void)mft_before; (void)clu_before;
		printf("  %3d  %2d/%d    %s      %s         %s\n", cut, landed, total,
		       rc2 == 0 ? "ok" : "FAIL", disk_ok ? "yes" : "NO",
		       log_ok ? "yes" : "NO");
		if (rc2 == 0 && disk_ok && log_ok)
			n_converged++;
		n_cuts++;
		ntfs_logfile_close(log);
		free(b.buf);
		free(v.disk);
	}

	/* Barrier cut: every write lands, only the sync is refused. */
	{
		int rc1, rc2, disk_ok;

		build_replay_cut_scenario(&b);
		m.buf = b.buf; m.size = LSIZE; m.writes = 0;
		log = open_log(&m, &io);
		vol_init(&v);
		vol_apply(&v, &ap);
		v.sync_errno = -EIO;
		memset(&res, 0, sizeof(res));
		rc1 = ntfs_logfile_replay(log, &ap, false, &res);
		CHECK(rc1 != 0);
		CHECK(res.flush_failed);
		CHECK_EQ(v.mft_writes + v.clu_writes, total);
		CHECK_EQ(v.syncs, 1);
		v.sync_errno = 0;
		memset(&res, 0, sizeof(res));
		rc2 = ntfs_logfile_replay(log, &ap, false, &res);
		CHECK_EQ(rc2, 0);
		disk_ok = memcmp(v.disk, ref_disk, VOL_CLUSTERS * VCS) == 0;
		CHECK(disk_ok);
		printf("  sync %2d/%d    %s      %s         %s\n", total, total,
		       rc2 == 0 ? "ok" : "FAIL", disk_ok ? "yes" : "NO", "yes");
		if (rc2 == 0 && disk_ok)
			n_converged++;
		n_cuts++;
		ntfs_logfile_close(log);
		free(b.buf);
		free(v.disk);
	}

	printf("  %d/%d cut points converged to the uninterrupted state\n",
	       n_converged, n_cuts);
	CHECK_EQ(n_converged, n_cuts);
	free(ref_disk);
	free(log_copy);
	printf("test_replay_cut_points\n");
}

/*
 * Finding 19: a record that is the first thing ever written into a freshly
 * allocated cluster must still leave a PROTECTED index block.
 *
 * From a real Windows 10 crash capture, 2026-09-17. The block at attr 0x68 vcn 8
 * was all zeros on disk -- allocated, never written -- and an
 * UpdateNonresidentValue laid down its INDX header. Because the block had not
 * ARRIVED as INDX, the outgoing update sequence array was skipped, so the next
 * record to read it found no USNs, called it "index block torn" and refused the
 * whole replay. Windows completed all 50 operations on the same journal.
 *
 * The fix protects on the result rather than the origin. Revert it and the
 * second half of this test fails: the stored block deprotects as torn.
 */
static void test_first_write_into_fresh_cluster_is_protected(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	struct ntfs_logfile_info info;
	struct crec c;
	uint8_t oat[256], indx[VCS];
	uint32_t oatn = build_oatbl(oat, 31, ATTR_TYPE_INDEX_ALLOCATION);
	uint64_t start, oa_lsn, restart_lsn;
	uint8_t *stored;
	bool torn = false;

	printf("test_first_write_into_fresh_cluster_is_protected\n");

	/* The payload: an INDX header and nothing else, exactly as a first write
	 * into an empty cluster looks. usa_count covers every 512-byte sector. */
	/* Filled with a recognisable pattern, NOT zeros. An all-zero payload makes
	 * this test vacuous: the update sequence number would be zero too, every
	 * sector tail would trivially match it, and an unprotected block would pass
	 * the check it is supposed to fail. Caught by reverting the fix and seeing
	 * no failure. */
	for (size_t i = 0; i < sizeof(indx); i++)
		indx[i] = (uint8_t)(0xA5 ^ (i & 0xff));
	lf_put32(indx + 0, LFS_MAGIC_INDX);
	lf_put16(indx + 4, 0x28);
	lf_put16(indx + 6, VCS / 512 + 1);

	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	oa_lsn = lb_table_dump(&b, LOP_OpenAttributeTableDump, oat, oatn);
	restart_lsn = lb_checkpoint(&b, start, oa_lsn, oatn, 0, 0, 0, 0);

	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_UpdateNonresidentValue;
	c.undo_op = LOP_Noop;
	/* Entry 1 of the open-attribute table: RT_HEADER_SIZE + OA1_SIZE. Entry 0
	 * is $MFT's own $DATA, and aiming there writes inside the MFT. */
	c.target_attr = RT_HEADER_SIZE + OA1_SIZE;
	c.lcns = 1;
	c.lcn[0] = INDEX_LCN;
	c.target_vcn = 0;
	c.redo = indx;
	c.redo_len = (uint16_t)sizeof(indx);
	(void)lb_client(&b, 0x40, 0, &c);
	(void)lb_simple(&b, 0x40, LOP_ForgetTransaction, LOP_Noop);
	lb_restart(&b, b.last_lsn, b.last_len, start, restart_lsn, 0, false);
	lb_protect(&b);

	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);

	vol_init(&v);
	vol_apply(&v, &ap);
	memset(&res, 0, sizeof(res));
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK(v.clu_writes >= 1);

	stored = v.disk + INDEX_LCN * VCS;
	CHECK_EQ(lf_get32(stored), LFS_MAGIC_INDX);
	/* The point of the test: it must carry an update sequence array, so that
	 * reading it back succeeds. Without the fix this deprotects as torn. */
	CHECK_EQ(ntfs_log_fixup_post_read(stored, VCS, LFS_SECTOR_SIZE, &torn), 0);
	CHECK(!torn);
	ntfs_logfile_close(log);
	/* ASan (2026-11-26): neither buffer was freed -- 2 x 256 KiB. */
	free(v.disk);
	free(b.buf);
}


/* ---- $LogFile writer (Phase 1, docs/JOURNAL-DESIGN.md) ------------------ */

struct wseen {
	uint64_t lsn[16];
	uint64_t prev[16];
	uint32_t type[16];
	uint32_t tid[16];
	uint8_t *data[16];
	uint32_t len[16];
	int n;
};

static int wcollect(const struct ntfs_log_record *r, void *ctx)
{
	struct wseen *s = ctx;

	if (s->n < 16) {
		int i = s->n;

		s->lsn[i] = r->lsn;
		s->prev[i] = r->client_prev_lsn;
		s->type[i] = r->record_type;
		s->tid[i] = r->transaction_id;
		s->len[i] = r->data_len;
		s->data[i] = malloc(r->data_len ? r->data_len : 1);
		if (s->data[i] && r->data_len)
			memcpy(s->data[i], r->raw, r->data_len);
	}
	s->n++;
	return 0;
}

static void wseen_free(struct wseen *s)
{
	int i;

	for (i = 0; i < s->n && i < 16; i++)
		free(s->data[i]);
}

/* io whose writes fail after @budget successful ones (reads unaffected). */
struct faultio {
	struct memio *m;
	int budget;
	int writes;
};

static int fault_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	return mem_read(((struct faultio *)ctx)->m, off, buf, len);
}

static int fault_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct faultio *f = ctx;

	f->writes++;
	if (f->budget >= 0 && f->writes > f->budget)
		return -EIO;
	return mem_write(f->m, off, buf, len);
}

static ntfs_logfile_t *open_with_io(struct ntfs_log_io *io)
{
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log = NULL;

	CHECK_EQ(ntfs_logfile_open(io, &g, &log), 0);
	if (log)
		ntfs_logfile_set_logger(log, logger, NULL);
	return log;
}

/* Build a Noop client record carrying @plen bytes of pattern @pat. */
static uint32_t build_noop(uint8_t *out, const uint8_t *pat, uint32_t plen)
{
	struct crec c;

	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_Noop;
	c.undo_op = LOP_Noop;
	c.redo = pat;
	c.redo_len = (uint16_t)plen;
	return build_crec(&c, out);
}

static void test_writer_roundtrip(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p1[64], p2[6000], p3[40], c1[512], c2[8192], c3[512];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct wseen s;
	uint32_t n1, n2, n3;
	uint64_t lsn1, lsn2, lsn3;
	int i;

	printf("test_writer_roundtrip\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	for (i = 0; i < 6000; i++)
		p2[i] = (uint8_t)(0xA5 ^ (i & 0xff));
	for (i = 0; i < 40; i++)
		p3[i] = (uint8_t)(0x5A + i);
	n1 = build_noop(c1, p1, sizeof(p1));
	n2 = build_noop(c2, p2, sizeof(p2));
	n3 = build_noop(c3, p3, sizeof(p3));

	/* Format refuses a log that already has restart pages. */
	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), -EEXIST);

	log = open_log(&m, &io);
	CHECK(log != NULL);
	if (!log) {
		free(buf);
		return;
	}
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	/* 6 KiB payload: spans two pages, so this is the multi-page path. */
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x40, 0, c3, n3, &lsn3), 0);
	CHECK(lsn1 < lsn2 && lsn2 < lsn3);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Reopen: the reader is the oracle for what the writer wrote. */
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.current_lsn, lsn3);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, lsn1, wcollect, &s), 0);
	CHECK_EQ(s.n, 3);
	if (s.n == 3) {
		CHECK_EQ(s.lsn[0], lsn1);
		CHECK_EQ(s.lsn[1], lsn2);
		CHECK_EQ(s.lsn[2], lsn3);
		CHECK_EQ(s.prev[0], 0);
		CHECK_EQ(s.prev[1], lsn1);
		CHECK_EQ(s.prev[2], lsn2);
		CHECK_EQ(s.tid[0], 0x18);
		CHECK_EQ(s.tid[1], 0x18);
		CHECK_EQ(s.tid[2], 0x40);
		CHECK_EQ(s.type[0], LFS_RECORD_TYPE_CLIENT);
		CHECK_EQ(s.len[0], n1);
		CHECK_EQ(s.len[1], n2);
		CHECK_EQ(s.len[2], n3);
		CHECK(memcmp(s.data[0], c1, n1) == 0);
		CHECK(memcmp(s.data[1], c2, n2) == 0);
		CHECK(memcmp(s.data[2], c3, n3) == 0);
	}
	wseen_free(&s);
	ntfs_logfile_close(log);
	free(buf);
}

static void test_writer_reopen_append(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p1[64], p2[40], c1[512], c2[512];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct wseen s;
	uint32_t n1, n2;
	uint64_t lsn1, lsn2;
	int i;

	printf("test_writer_reopen_append\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	for (i = 0; i < 40; i++)
		p2[i] = (uint8_t)(0x5A + i);
	n1 = build_noop(c1, p1, sizeof(p1));
	n2 = build_noop(c2, p2, sizeof(p2));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	/* format 2 + first-record page 1 + first-record restart 2 + sync 2 */
	CHECK_EQ(m.writes, 7);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Second session appends to the same partial page. */
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), 0);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	/* + tail copy 1 + page 1 + sync 2 */
	CHECK_EQ(m.writes, 11);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, lsn1, wcollect, &s), 0);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.lsn[0], lsn1);
		CHECK_EQ(s.lsn[1], lsn2);
		CHECK_EQ(s.prev[1], lsn1);
		CHECK(memcmp(s.data[0], c1, n1) == 0);
		CHECK(memcmp(s.data[1], c2, n2) == 0);
	}
	wseen_free(&s);
	ntfs_logfile_close(log);
	free(buf);
}

/*
 * Crash between the tail copy and the in-place page write: the record
 * must still be there, recovered from the tail copy. Refusing the tail
 * copy itself must leave only the earlier record.
 *
 * Write budget counts: format 2, append A page 3 + first-record restart
 * 4-5, append B tail copy 6 + page 7.
 */
static void test_writer_tail_copy_crash(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p1[64], p2[40], c1[512], c2[512];
	struct memio m;
	struct ntfs_log_io io;
	struct faultio f;
	struct ntfs_log_io fio;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct wseen s;
	uint32_t n1, n2;
	uint64_t lsn1, lsn2;
	int i;

	printf("test_writer_tail_copy_crash\n");
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	for (i = 0; i < 40; i++)
		p2[i] = (uint8_t)(0x5A + i);
	n1 = build_noop(c1, p1, sizeof(p1));
	n2 = build_noop(c2, p2, sizeof(p2));

	/* Case 1: the in-place page write is refused, the tail copy landed. */
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	f.m = &m; f.budget = 6; f.writes = 0;
	memset(&fio, 0, sizeof(fio));
	fio.ctx = &f; fio.size = LSIZE; fio.read = fault_read; fio.write = fault_write;
	CHECK_EQ(ntfs_log_writer_format(&fio, &g, LSIZE), 0);
	log = open_with_io(&fio);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(f.writes, 5);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), -EIO);
	CHECK_EQ(f.writes, 7);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, lsn1, wcollect, &s), 0);
	/* The tail scan runs lazily on the first walk; only then does the
	 * info reflect the override. B's lsn is computed, not read back:
	 * the failed append never assigned lsn2. */
	{
		uint64_t exp_b = mk_lsn(FIRST_PAGE + DATA_OFF + LF_ALIGN8(LR_HEADER_SIZE + n1), 1);

		ntfs_logfile_get_info(log, &info);
		CHECK_EQ(info.tail_overrides, 1);
		CHECK_EQ(info.last_lsn, exp_b);
		CHECK_EQ(s.n, 2);
		if (s.n == 2) {
			CHECK_EQ(s.lsn[1], exp_b);
			CHECK(memcmp(s.data[1], c2, n2) == 0);
		}
	}
	wseen_free(&s);
	ntfs_logfile_close(log);

	/* Case 2 (negative control): the tail copy itself is refused. */
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	f.m = &m; f.budget = 5; f.writes = 0;
	CHECK_EQ(ntfs_log_writer_format(&fio, &g, LSIZE), 0);
	log = open_with_io(&fio);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), -EIO);
	CHECK_EQ(f.writes, 6);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, lsn1, wcollect, &s), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.tail_overrides, 0);
	CHECK_EQ(info.last_lsn, lsn1);
	CHECK_EQ(s.n, 1);
	wseen_free(&s);
	ntfs_logfile_close(log);
	free(buf);
}

/*
 * Torn last page: the log ends at the previous record. The restart area
 * lags the torn record (no sync after it), so the scan walks forward
 * from the last synced record and truncates — the walk must return the
 * earlier records, never garbage.
 */

/* Checkpoint: one open attribute with a name, dumped and reloaded. */
struct ckpt_oa {
	uint32_t id, type;
	uint64_t mft_no, open_record_lsn;
	uint16_t mft_seq, name_len;
	uint16_t name[16];
	int n;
};

static int ckpt_oa_cb(const struct ntfs_log_open_attr *a, void *ctx)
{
	struct ckpt_oa *o = ctx;

	o->n++;
	if (o->n == 1) {
		o->id = a->id;
		o->type = a->type;
		o->mft_no = a->mft_no;
		o->mft_seq = a->mft_seq;
		o->open_record_lsn = a->open_record_lsn;
		o->name_len = a->name_len;
		if (a->name && a->name_len <= 16)
			memcpy(o->name, a->name, (size_t)a->name_len * 2);
	}
	return 0;
}

static void test_writer_checkpoint_basic(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p1[64], p2[40], c1[512], c2[512];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct wseen s;
	struct ckpt_oa oa;
	struct ntfs_log_table_cbs cbs;
	static const uint16_t nm[] = { 'D', 'A', 'T', 'A' };
	uint32_t n1, n2, attr_id = 0;
	uint64_t lsn1, lsn2, rlsn, first_dump;
	int i;

	printf("test_writer_checkpoint_basic\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	for (i = 0; i < 40; i++)
		p2[i] = (uint8_t)(0x5A + i);
	n1 = build_noop(c1, p1, sizeof(p1));
	n2 = build_noop(c2, p2, sizeof(p2));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), 0);
	CHECK_EQ(ntfs_log_writer_open_attr(w, ATTR_TYPE_DATA, 6, 1, nm, 4, &attr_id), 0);
	CHECK_EQ(attr_id, 0x18);	/* first open-attribute entry */
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn), 0);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Reopen: the checkpoint is referenced and its tables load. */
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.client_restart_lsn, rlsn);
	CHECK_EQ(ntfs_logfile_load_checkpoint(log), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.table_open_attrs, 1);
	memset(&oa, 0, sizeof(oa));
	memset(&cbs, 0, sizeof(cbs));
	cbs.open_attr = ckpt_oa_cb;
	CHECK_EQ(ntfs_logfile_tables(log, &cbs, &oa), 0);
	CHECK_EQ(oa.n, 1);
	if (oa.n == 1) {
		CHECK_EQ(oa.id, 0x18);
		CHECK_EQ(oa.type, ATTR_TYPE_DATA);
		CHECK_EQ(oa.mft_no, 6);
		CHECK_EQ(oa.mft_seq, 1);
		CHECK_EQ(oa.open_record_lsn, lsn2);
		CHECK_EQ(oa.name_len, 4);
		CHECK(memcmp(oa.name, nm, sizeof(nm)) == 0);
	}
	/* The walk starts at the checkpoint: names dump, open-attr dump,
	 * restart record. Records before the checkpoint are gone from
	 * the walk's range. */
	first_dump = info.checkpoint_lsn;
	CHECK(first_dump != 0 && first_dump != rlsn);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, 0, wcollect, &s), 0);
	CHECK_EQ(s.n, 3);
	if (s.n == 3) {
		CHECK_EQ(s.lsn[0], first_dump);
		CHECK_EQ(s.type[0], LFS_RECORD_TYPE_CLIENT);
		CHECK_EQ(s.tid[0], 0x18);
		CHECK_EQ(s.type[1], LFS_RECORD_TYPE_CLIENT);
		CHECK_EQ(s.tid[1], 0x18);
		CHECK_EQ(s.lsn[2], rlsn);
		CHECK_EQ(s.type[2], LFS_RECORD_TYPE_CLIENT_RESTART);
		CHECK_EQ(lf_get32(s.data[2]), 1);	/* restart major ver */
	}
	wseen_free(&s);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, lsn1, wcollect, &s), -EINVAL);
	wseen_free(&s);
	ntfs_logfile_close(log);
	free(buf);
}

/* A second session's checkpoint must keep the first session's attribute. */
static void test_writer_checkpoint_reopen(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p1[64], c1[512];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct ckpt_oa oa;
	struct ntfs_log_table_cbs cbs;
	static const uint16_t nm[] = { 'D', 'A', 'T', 'A' };
	uint32_t n1, attr_id = 0;
	uint64_t lsn1, rlsn1, rlsn2;
	int i;

	printf("test_writer_checkpoint_reopen\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	n1 = build_noop(c1, p1, sizeof(p1));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_open_attr(w, ATTR_TYPE_DATA, 6, 1, nm, 4, &attr_id), 0);
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn1), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Second session: no new attributes, just another record and a
	 * checkpoint. Without adopting the old checkpoint's tables the
	 * attribute would be dropped here. */
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn2), 0);
	CHECK(rlsn2 > rlsn1);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.client_restart_lsn, rlsn2);
	CHECK_EQ(ntfs_logfile_load_checkpoint(log), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.table_open_attrs, 1);
	memset(&oa, 0, sizeof(oa));
	memset(&cbs, 0, sizeof(cbs));
	cbs.open_attr = ckpt_oa_cb;
	CHECK_EQ(ntfs_logfile_tables(log, &cbs, &oa), 0);
	CHECK_EQ(oa.n, 1);
	if (oa.n == 1) {
		CHECK_EQ(oa.mft_no, 6);
		CHECK_EQ(oa.name_len, 4);
		CHECK(memcmp(oa.name, nm, sizeof(nm)) == 0);
	}
	ntfs_logfile_close(log);
	free(buf);
}

/*
 * Fill the log until appends refuse, checkpoint (which must still fit
 * in the reserve), fill again, checkpoint again. The second checkpoint
 * is the referenced one after reopen.
 */
static void test_writer_checkpoint_reserve(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p[4000], c[4200];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	uint32_t n;
	uint64_t lsn, rlsn1, rlsn2;
	int i, filled1 = 0, filled2 = 0;

	printf("test_writer_checkpoint_reserve\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 4000; i++)
		p[i] = (uint8_t)(0xA5 ^ (i & 0xff));
	n = build_noop(c, p, sizeof(p));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	while (ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn) == 0)
		filled1++;
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn), -ENOSPC);
	CHECK(filled1 > 10);	/* most of the 64-page log filled */
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn1), 0);
	while (ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn) == 0)
		filled2++;
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn), -ENOSPC);
	CHECK(filled2 > 10);	/* the checkpoint freed the log again */
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn2), 0);
	CHECK(rlsn2 > rlsn1);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.client_restart_lsn, rlsn2);
	CHECK_EQ(ntfs_logfile_load_checkpoint(log), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.checkpoint_lsn, rlsn2);	/* no dumps: start is the restart record */
	CHECK_EQ(info.table_open_attrs, 0);
	ntfs_logfile_close(log);
	free(buf);
}


/* wseen holds 16 records; the wrap walk yields ~57, so it needs its own. */
struct wseen64 {
	uint64_t lsn[64];
	uint32_t type[64];
	uint8_t *data[64];
	uint32_t len[64];
	int n;
};

static int wcollect64(const struct ntfs_log_record *r, void *ctx)
{
	struct wseen64 *s = ctx;

	if (s->n < 64) {
		int i = s->n;

		s->lsn[i] = r->lsn;
		s->type[i] = r->record_type;
		s->len[i] = r->data_len;
		s->data[i] = malloc(r->data_len ? r->data_len : 1);
		if (s->data[i] && r->data_len)
			memcpy(s->data[i], r->raw, r->data_len);
	}
	s->n++;
	return 0;
}

static void wseen64_free(struct wseen64 *s)
{
	int i;

	for (i = 0; i < s->n && i < 64; i++)
		free(s->data[i]);
}

/*
 * Wrap read-back: fill, checkpoint, then fill again so the appends
 * wrap around the end of the file onto the first pages. After reopen
 * the walk from the checkpoint must yield the restart record and
 * every fill record in order, byte-exact — including the ones that
 * wrapped (their LSNs sit on lower pages than the checkpoint's).
 * The pre-checkpoint records are outside [oldest, last].
 */
static void test_writer_wrap_readback(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p[4000], c[4200];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct wseen64 s;
	uint32_t n;
	uint64_t lsn, rlsn, fill_lsn[64], first_lsn = 0;
	int i, filled1 = 0, filled2 = 0, wrapped = 0;
	uint32_t rlsn_vbo;

	printf("test_writer_wrap_readback\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 4000; i++)
		p[i] = (uint8_t)(0xA5 ^ (i & 0xff));
	n = build_noop(c, p, sizeof(p));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	/* Fill to the reserve, so the checkpoint lands near the end. */
	while (ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn) == 0) {
		if (!first_lsn)
			first_lsn = lsn;
		filled1++;
	}
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn), -ENOSPC);
	CHECK(filled1 > 10);
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn), 0);
	/* Fill again: the early pages are free now, so these appends
	 * wrap around the end of the file onto the first pages. */
	while (ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn) == 0) {
		if (filled2 < 64)
			fill_lsn[filled2] = lsn;
		filled2++;
	}
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c, n, &lsn), -ENOSPC);
	CHECK(filled2 > 10);
	/* At least one second-fill record wrapped: its page sits below
	 * the checkpoint's page, which forward-only appends cannot reach. */
	rlsn_vbo = (uint32_t)((rlsn & 0xffffu) << 3);
	for (i = 0; i < filled2 && i < 64; i++) {
		uint32_t vbo = (uint32_t)((fill_lsn[i] & 0xffffu) << 3);

		if (vbo < rlsn_vbo)
			wrapped++;
	}
	CHECK(wrapped > 0);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.client_restart_lsn, rlsn);
	CHECK_EQ(info.current_lsn, fill_lsn[filled2 - 1]);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, 0, wcollect64, &s), 0);
	CHECK_EQ(s.n, 1 + filled2);
	if (s.n == 1 + filled2) {
		CHECK_EQ(s.lsn[0], rlsn);
		CHECK_EQ(s.type[0], LFS_RECORD_TYPE_CLIENT_RESTART);
		for (i = 0; i < filled2; i++) {
			CHECK_EQ(s.lsn[1 + i], fill_lsn[i]);
			CHECK_EQ(s.type[1 + i], LFS_RECORD_TYPE_CLIENT);
			CHECK(memcmp(s.data[1 + i], c, n) == 0);
		}
	}
	wseen64_free(&s);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, first_lsn, wcollect64, &s), -EINVAL);
	wseen64_free(&s);
	ntfs_logfile_close(log);
	free(buf);
}


/* Clean close: the log reopens clean, the checkpoint tables load, and
 * a reopened writer starts a new (dirty) session on the same log. */
static void test_writer_clean_close(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t p1[64], p2[40], c1[512], c2[512];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct ckpt_oa oa;
	struct ntfs_log_table_cbs cbs;
	static const uint16_t nm[] = { 'D', 'A', 'T', 'A' };
	uint32_t n1, n2;
	uint64_t lsn1, lsn2, rlsn, rlsn2;
	int i;

	printf("test_writer_clean_close\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	for (i = 0; i < 40; i++)
		p2[i] = (uint8_t)(0x5A + i);
	n1 = build_noop(c1, p1, sizeof(p1));
	n2 = build_noop(c2, p2, sizeof(p2));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_open_attr(w, ATTR_TYPE_DATA, 6, 1, nm, 4, NULL), 0);
	CHECK_EQ(ntfs_log_writer_close(w, &rlsn), 0);
	/* No more records after a clean close; a second close refuses. */
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), -EINVAL);
	CHECK_EQ(ntfs_log_writer_checkpoint(w, &rlsn2), -EINVAL);
	CHECK_EQ(ntfs_log_writer_close(w, &rlsn2), -EINVAL);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Reopen as a reader: clean, checkpoint referenced, tables load. */
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_CLEAN);
	CHECK(info.volume_is_clean_flag);
	CHECK_EQ(info.client_restart_lsn, rlsn);
	CHECK_EQ(ntfs_logfile_load_checkpoint(log), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.table_open_attrs, 1);
	memset(&oa, 0, sizeof(oa));
	memset(&cbs, 0, sizeof(cbs));
	cbs.open_attr = ckpt_oa_cb;
	CHECK_EQ(ntfs_logfile_tables(log, &cbs, &oa), 0);
	CHECK_EQ(oa.n, 1);
	if (oa.n == 1)
		CHECK_EQ(oa.mft_no, 6);
	ntfs_logfile_close(log);

	/* Reopen as a writer: the clean flag clears, the client comes
	 * back in use, and the log is dirty again after an append. */
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), 0);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	log = open_log(&m, &io);
	CHECK(!ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.current_lsn, lsn2);
	ntfs_logfile_close(log);
	free(buf);
}

static void test_writer_torn_tail(void)
{
	uint8_t *buf = malloc(LSIZE);
	uint8_t pbig[3700], p1[64], p2[40], cbig[4096], c1[512], c2[512];
	struct memio m;
	struct ntfs_log_io io;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	struct ntfs_logfile_info info;
	struct wseen s;
	uint32_t nbig, n1, n2;
	uint64_t lsbig, lsn1, lsn2;
	int i;

	printf("test_writer_torn_tail\n");
	memset(buf, 0xff, LSIZE);
	m.buf = buf; m.size = LSIZE; m.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &m; io.size = LSIZE; io.read = mem_read; io.write = mem_write;
	for (i = 0; i < 3700; i++)
		pbig[i] = (uint8_t)(i & 0xff);
	for (i = 0; i < 64; i++)
		p1[i] = (uint8_t)(0x11 + i);
	for (i = 0; i < 40; i++)
		p2[i] = (uint8_t)(0x5A + i);
	nbig = build_noop(cbig, pbig, sizeof(pbig));
	n1 = build_noop(c1, p1, sizeof(p1));
	n2 = build_noop(c2, p2, sizeof(p2));

	CHECK_EQ(ntfs_log_writer_format(&io, &g, LSIZE), 0);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_log_writer_open(log, &w), 0);
	/* Big record ends at 0xF10, A ends on the same page at 0xFA8. */
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, cbig, nbig, &lsbig), 0);
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c1, n1, &lsn1), 0);
	CHECK_EQ(ntfs_log_writer_sync(w), 0);
	/* B starts on page 4 and ends on page 5; no sync after it. */
	CHECK_EQ(ntfs_log_writer_append(w, LFS_RECORD_TYPE_CLIENT, 0x18, 0, c2, n2, &lsn2), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Tear page 5: flip a byte in its first sector's tail. */
	buf[5 * PS + 512 - 2] ^= 0xff;
	(void)lsn2;

	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, lsbig, wcollect, &s), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.last_lsn, lsn1);
	CHECK_EQ(s.n, 2);
	if (s.n == 2) {
		CHECK_EQ(s.lsn[0], lsbig);
		CHECK_EQ(s.lsn[1], lsn1);
		CHECK(memcmp(s.data[0], cbig, nbig) == 0);
		CHECK(memcmp(s.data[1], c1, n1) == 0);
	}
	wseen_free(&s);
	ntfs_logfile_close(log);
	free(buf);
}

/* ---- Journal create-transaction tests (§1.2 Phase 2) -------------------- */

/* $FILE_NAME value: parent ref, 4 timestamps, sizes, flags, ea size,
 * name length, name type, UTF-16LE name. */
static uint32_t build_fn_value(uint8_t *out, uint64_t parent, const char *name)
{
	size_t nlen = strlen(name);
	uint32_t vlen = 66 + (uint32_t)nlen * 2;
	size_t i;

	memset(out, 0, vlen);
	lf_put64(out + 0, parent);
	lf_put32(out + 56, 0x20);		/* flags: ARCHIVE */
	out[64] = (uint8_t)nlen;
	out[65] = 1;				/* WIN32 name type */
	for (i = 0; i < nlen; i++) {
		out[66 + i * 2] = (uint8_t)name[i];
		out[67 + i * 2] = 0;
	}
	return vlen;
}

/* Index entry: 16-byte header + $FILE_NAME key, 8-aligned. */
static uint32_t build_index_entry(uint8_t *out, uint64_t mref, const uint8_t *fn,
				  uint32_t fn_len)
{
	uint32_t total = LF_ALIGN8(16 + fn_len);

	memset(out, 0, total);
	lf_put64(out + 0, mref);
	lf_put16(out + 8, (uint16_t)total);
	lf_put16(out + 10, (uint16_t)fn_len);
	lf_put16(out + 12, 0);			/* flags */
	memcpy(out + 16, fn, fn_len);
	return total;
}

static void fill_init_si(uint8_t *rec, uint32_t *off)
{
	uint8_t si[0x30];
	int i;

	for (i = 0; i < 0x30; i++)
		si[i] = (uint8_t)(0x40 + i);
	put_res_attr(rec, off, ATTR_TYPE_STANDARD_INFORMATION, si, sizeof(si), 0);
}

/* A fresh record header + $STANDARD_INFORMATION, link count 0, next
 * attribute id 1: what InitializeFileRecordSegment redoes. */
static uint32_t build_init_record(uint8_t *rec, uint64_t no)
{
	/* IN_USE from the start: the record is in use the moment it is
	 * initialized (the $MFT:$BITMAP bit mirrors it, and the undo —
	 * Deallocate — clears it). A flags-0 image would fail the replay's
	 * check_file_record on the very next record of the transaction. */
	build_record(rec, no, MFT_RECORD_IN_USE, fill_init_si);
	lf_put16(rec + MR_LINK_COUNT, 0);
	lf_put16(rec + MR_NEXT_ATTR_ID, 1);
	return lf_get32(rec + MR_BYTES_IN_USE);
}

/* Stage the seven records of a regular-file create into @t:
 * Initialize (link 0), Create $SECURITY_DESCRIPTOR, Create $DATA (empty),
 * Create $FILE_NAME (indexed), AddIndexEntryRoot into the parent, update the
 * parent's $STANDARD_INFORMATION mtime fields, SetBits in $MFT:$BITMAP.
 * @init_vcn_override is (uint64_t)-1 for the correct vcn, or a wrong one
 * (negative control: replay must refuse the log). Returns the first error. */
static int stage_create(ntfs_log_txn_t *t, uint32_t mft_attr, uint32_t bmp_attr,
			uint64_t recno, const char *name, uint64_t parent,
			uint64_t init_vcn_override)
{
	uint8_t init_rec[VRS], sd_attr[64], data_attr[32], fn_attr[256];
	uint8_t fn_val[128], entry[256], new_si[24], old_si[24];
	uint32_t init_len, sd_len, data_len, fn_len, fn_vlen, entry_len;
	uint32_t sd_off, data_off, fn_off;
	uint64_t vcn = recno * VRS / VCS;
	uint32_t cluster_off = (uint32_t)((recno * VRS % VCS) / 512);
	uint64_t page_lcn = MFT_LCN + vcn;
	uint64_t init_vcn = init_vcn_override == (uint64_t)-1 ? vcn :
			    init_vcn_override;
	uint64_t pvcn = DIR_REC * VRS / VCS;	/* 8 */
	uint64_t ppage_lcn = MFT_LCN + pvcn;	/* 24 */
	uint16_t dir_ir_off = 0x38 + LF_ALIGN8(AT_RESIDENT_SIZE + 0x30);
	uint16_t dir_si_off = 0x38;
	static const uint8_t sd_val[16] = {
		0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
		0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	};
	static const uint8_t empty_val[1] = { 0 };
	int i, err;

	init_len = build_init_record(init_rec, recno);
	{
		uint32_t off = 0;
		put_res_attr(sd_attr, &off, ATTR_TYPE_SECURITY_DESCRIPTOR,
			     sd_val, sizeof(sd_val), 1);
		sd_len = off;
		off = 0;
		put_res_attr(data_attr, &off, ATTR_TYPE_DATA, empty_val, 0, 2);
		data_len = off;
	}
	fn_vlen = build_fn_value(fn_val, parent, name);
	{
		uint32_t off = 0;
		put_res_attr(fn_attr, &off, ATTR_TYPE_FILE_NAME, fn_val, fn_vlen, 3);
		fn_attr[AT_RES_FLAGS] |= ATTR_RES_FLAG_INDEXED;
		fn_len = off;
	}
	entry_len = build_index_entry(entry, recno | (1ULL << 48), fn_val, fn_vlen);
	for (i = 0; i < 24; i++) {
		new_si[i] = 0x77;
		old_si[i] = (uint8_t)(0x10 + 8 + i);	/* parent $SI value[8..32) */
	}
	/* A CreateAttribute inserts at the END marker, not after it:
	 * the first attribute goes at init_len - 8. */
	sd_off = init_len - 8;
	data_off = sd_off + sd_len;
	fn_off = data_off + data_len;

	err = ntfs_log_txn_init_record(t, mft_attr, init_vcn, cluster_off,
				       page_lcn, init_rec, init_len);
	if (err)
		return err;
	err = ntfs_log_txn_create_attr(t, mft_attr, vcn, cluster_off,
				       page_lcn, sd_off, sd_attr, sd_len);
	if (err)
		return err;
	err = ntfs_log_txn_create_attr(t, mft_attr, vcn, cluster_off,
				       page_lcn, data_off, data_attr, data_len);
	if (err)
		return err;
	err = ntfs_log_txn_create_attr(t, mft_attr, vcn, cluster_off,
				       page_lcn, fn_off, fn_attr, fn_len);
	if (err)
		return err;
	err = ntfs_log_txn_add_index_root(t, mft_attr, pvcn, 0, ppage_lcn,
					  dir_ir_off, 56, entry, entry_len);
	if (err)
		return err;
	err = ntfs_log_txn_update_resident(t, mft_attr, pvcn, 0, ppage_lcn,
					   dir_si_off, AT_RESIDENT_SIZE + 8,
					   new_si, sizeof(new_si), old_si,
					   sizeof(old_si));
	if (err)
		return err;
	return ntfs_log_txn_set_bitmap(t, bmp_attr, 0, 0, MFTBMP_LCN,
				       (uint32_t)recno, 1, true);
}

/* Walk collector: keeps (lsn, redo_op, target_attr, target_vcn, record_off)
 * per client record. */
struct rec_seen {
	uint64_t lsn[64];
	uint16_t redo_op[64];
	uint32_t target_attr[64];
	uint64_t target_vcn[64];
	uint16_t record_off[64];
	int n;
};

static int collect_rec(const struct ntfs_log_record *r, void *ctx)
{
	struct rec_seen *s = ctx;

	if (r->record_type != LFS_RECORD_TYPE_CLIENT || s->n >= 64)
		return 0;
	s->lsn[s->n] = r->lsn;
	s->redo_op[s->n] = r->redo_op;
	s->target_attr[s->n] = r->target_attr;
	s->target_vcn[s->n] = r->target_vcn;
	s->record_off[s->n] = r->record_off;
	s->n++;
	return 0;
}

/* Set up a fresh formatted log with a writer and the two open attributes
 * the create transaction needs ($MFT:$DATA at 0x18, $MFT:$BITMAP at 0x40).
 * The caller owns @log/@w; @io is filled in. */
static int journal_setup(struct memio *m, struct ntfs_log_io *io,
			 ntfs_logfile_t **log, ntfs_log_writer_t **w)
{
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	int err;

	memset(m->buf, 0xff, LSIZE);
	m->size = LSIZE;
	m->writes = 0;
	memset(io, 0, sizeof(*io));
	io->ctx = m;
	io->size = LSIZE;
	io->read = mem_read;
	io->write = mem_write;
	err = ntfs_log_writer_format(io, &g, LSIZE);
	if (err)
		return err;
	*log = open_log(m, io);
	if (!*log)
		return -EIO;
	err = ntfs_log_writer_open(*log, w);
	if (err)
		return err;
	err = ntfs_log_writer_open_attr(*w, ATTR_TYPE_DATA, 0, 0, NULL, 0, NULL);
	if (err)
		return err;
	return ntfs_log_writer_open_attr(*w, ATTR_TYPE_BITMAP, 0, 0, NULL, 0,
					 NULL);
}

/* The full create transaction, committed and replayed: the log carries the
 * seven records plus Commit, checkpoint and Forget; replay redoes exactly
 * those seven, the new record is byte-exact (including the MR_LSN the
 * replay stamps from the last touching record), the parent gains the index
 * entry and the new mtime, bit 40 of $MFT:$BITMAP is set, and a second
 * replay redoes nothing. */
static void test_journal_create_commit(void)
{
	uint8_t *buf = malloc(LSIZE);
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	ntfs_log_txn_t *t;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	struct rec_seen s;
	uint64_t first_lsn, lsn_fn = 0, lsn_upd = 0;
	uint8_t expect40[VRS], expect32[VRS];
	uint8_t got[VRS];
	int i, err;

	printf("test_journal_create_commit\n");
	m.buf = buf;
	CHECK_EQ(journal_setup(&m, &io, &log, &w), 0);
	CHECK_EQ(ntfs_log_txn_begin(w, &t), 0);
	CHECK_EQ(stage_create(t, 0x18, 0x40, NEW_REC, "t.txt", DIR_REC,
			      (uint64_t)-1), 0);
	CHECK_EQ(ntfs_log_txn_commit(t, &first_lsn, NULL), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	/* Walk the log from the first record: 2 open + 7 data + Commit +
	 * 3 table dumps + Forget (the restart record is CLIENT_RESTART,
	 * which the collector skips). */
	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(ntfs_logfile_walk(log, first_lsn, collect_rec, &s), 0);
	CHECK_EQ(s.n, 14);
	if (s.n == 14) {
		static const uint16_t want_op[7] = {
			LOP_InitializeFileRecordSegment, LOP_CreateAttribute,
			LOP_CreateAttribute, LOP_CreateAttribute,
			LOP_AddIndexEntryRoot, LOP_UpdateResidentValue,
			LOP_SetBitsInNonresidentBitMap,
		};
		/* The two open records come first, one per attribute the
		 * transaction touches, in first-use order. */
		CHECK_EQ(s.redo_op[0], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[0], 0x18u);
		CHECK_EQ(s.redo_op[1], LOP_OpenNonresidentAttribute);
		CHECK_EQ(s.target_attr[1], 0x40u);
		for (i = 0; i < 7; i++) {
			CHECK_EQ(s.redo_op[i + 2], want_op[i]);
			CHECK_EQ(s.target_attr[i + 2], i < 6 ? 0x18u : 0x40u);
		}
		CHECK_EQ(s.redo_op[9], LOP_CommitTransaction);
		CHECK_EQ(s.redo_op[10], LOP_TransactionTableDump);
		CHECK_EQ(s.redo_op[11], LOP_DirtyPageTableDump);
		CHECK_EQ(s.redo_op[12], LOP_OpenAttributeTableDump);
		CHECK_EQ(s.redo_op[13], LOP_ForgetTransaction);
		for (i = 0; i < s.n; i++) {
			if (s.redo_op[i] == LOP_CreateAttribute &&
			    s.target_vcn[i] == NEW_REC * VRS / VCS &&
			    s.record_off[i] == 192)
				lsn_fn = s.lsn[i];
			if (s.redo_op[i] == LOP_UpdateResidentValue)
				lsn_upd = s.lsn[i];
		}
		CHECK(lsn_fn != 0);
		CHECK(lsn_upd != 0);
	}
	ntfs_logfile_close(log);

	/* Dry run: exactly the 7 records redo, 1 transaction committed. */
	vol_init(&v);
	vol_apply(&v, &ap);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK_EQ(res.records_redone, 7);
	/* The transaction was committed and then forgotten, so no COMMITTED
	 * entry survives the walk; the count is of entries still in the
	 * table, not of transactions that ever committed. The 7 records
	 * still redo because the dirty-page table references them. */
	CHECK_EQ(res.transactions_committed, 0);
	CHECK_EQ(res.transactions_active, 0);
	CHECK_EQ(res.plan_len, 7);
	CHECK_EQ(res.mft_records_written, 0);
	CHECK_EQ(res.clusters_written, 0);
	CHECK(!res.needs_chkdsk);
	ntfs_logfile_close(log);

	/* Real run: 2 MFT records + 1 bitmap cluster written. */
	CHECK_EQ(ntfs_logfile_replay(log = open_log(&m, &io), &ap, false, &res), 0);
	CHECK_EQ(res.records_redone, 7);
	CHECK_EQ(res.mft_records_written, 2);
	CHECK_EQ(res.clusters_written, 1);
	CHECK(!res.needs_chkdsk);
	ntfs_logfile_close(log);

	/* Record 40 byte-exact: init + $SD + $DATA + $FILE_NAME (indexed),
	 * link 1, next attr id 4, MR_LSN from the $FILE_NAME create. */
	{
		uint8_t fn_val[128], fn_attr[256];
		uint32_t fn_vlen, off = 0;
		static const uint8_t sd_val[16] = {
			0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
			0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
		};
		static const uint8_t empty_val[1] = { 0 };

		build_record(expect40, NEW_REC, MFT_RECORD_IN_USE, fill_init_si);
		lf_put16(expect40 + MR_LINK_COUNT, 1);
		lf_put16(expect40 + MR_NEXT_ATTR_ID, 4);
		off = lf_get32(expect40 + MR_BYTES_IN_USE) - 8;
		put_res_attr(expect40, &off, ATTR_TYPE_SECURITY_DESCRIPTOR,
			     sd_val, sizeof(sd_val), 1);
		put_res_attr(expect40, &off, ATTR_TYPE_DATA, empty_val, 0, 2);
		fn_vlen = build_fn_value(fn_val, DIR_REC, "t.txt");
		put_res_attr(expect40, &off, ATTR_TYPE_FILE_NAME, fn_val, fn_vlen, 3);
		expect40[off - LF_ALIGN8(AT_RESIDENT_SIZE + fn_vlen) + AT_RES_FLAGS] |=
			ATTR_RES_FLAG_INDEXED;
		lf_put32(expect40 + off, ATTR_TYPE_END);
		off += 8;
		lf_put32(expect40 + MR_BYTES_IN_USE, off);
		lf_put64(expect40 + MR_LSN, lsn_fn);
	}
	CHECK_EQ(v_read_mft(&v, NEW_REC, got), 0);
	/* Compare deprotected, with the fixup USN normalised: the USN is
	 * a write counter, not content, and the replayed record went
	 * through a different number of protect cycles than a freshly
	 * built one. */
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	lf_put16(got + MR_FIXUP_OFFSET_3, 0x0001);
	CHECK(memcmp(got, expect40, VRS) == 0);

	/* Record 32 byte-exact: the "t.txt" entry inserted before the END
	 * entry, $SI value[8..32) replaced with 0x77, MR_LSN from the
	 * mtime update. */
	{
		uint8_t fn_val[128], entry[256];
		uint32_t fn_vlen, entry_len, used;
		uint16_t ir_off = 0x38 + LF_ALIGN8(AT_RESIDENT_SIZE + 0x30);

		build_record(expect32, DIR_REC,
			     MFT_RECORD_IN_USE | MFT_RECORD_IS_DIRECTORY, fill_dir);
		used = lf_get32(expect32 + MR_BYTES_IN_USE);
		fn_vlen = build_fn_value(fn_val, DIR_REC, "t.txt");
		entry_len = build_index_entry(entry, NEW_REC | (1ULL << 48),
					      fn_val, fn_vlen);
		/* Insert the entry at attr offset 56 (over the END entry),
		 * move the END entry after it, grow the record. */
		memmove(expect32 + ir_off + 56 + entry_len,
			expect32 + ir_off + 56, used - (ir_off + 56));
		memcpy(expect32 + ir_off + 56, entry, entry_len);
		used += entry_len;
		lf_put32(expect32 + MR_BYTES_IN_USE, used);
		/* The $INDEX_ROOT attr grows by the entry: length, value
		 * length, index length and index allocated size all grow. */
		lf_put32(expect32 + ir_off + AT_LENGTH,
			 lf_get32(expect32 + ir_off + AT_LENGTH) + entry_len);
		lf_put32(expect32 + ir_off + AT_VALUE_LENGTH,
			 lf_get32(expect32 + ir_off + AT_VALUE_LENGTH) + entry_len);
		lf_put32(expect32 + ir_off + AT_RESIDENT_SIZE + 16 + 4,
			 lf_get32(expect32 + ir_off + AT_RESIDENT_SIZE + 16 + 4) +
			 entry_len);
		lf_put32(expect32 + ir_off + AT_RESIDENT_SIZE + 16 + 8,
			 lf_get32(expect32 + ir_off + AT_RESIDENT_SIZE + 16 + 8) +
			 entry_len);
		/* $SI mtime fields: value[8..32) = 0x77. */
		memset(expect32 + 0x38 + AT_RESIDENT_SIZE + 8, 0x77, 24);
		lf_put64(expect32 + MR_LSN, lsn_upd);
	}
	CHECK_EQ(v_read_mft(&v, DIR_REC, got), 0);
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	lf_put16(got + MR_FIXUP_OFFSET_3, 0x0001);
	CHECK(memcmp(got, expect32, VRS) == 0);

	/* Bitmap cluster: bit 40 set, nothing else. */
	CHECK_EQ(v.disk[MFTBMP_LCN * VCS + 5], 0x01);
	for (i = 0; i < (int)VCS; i++) {
		if (i == 5)
			continue;
		CHECK_EQ(v.disk[MFTBMP_LCN * VCS + i], 0);
	}

	/* Second replay: the MFT ops are skipped (the records already carry
	 * a later MR_LSN), but the SetBits op re-applies — clusters carry
	 * no LSN to skip on, and setting an already-set bit is a no-op in
	 * effect. The volume is unchanged by it. */
	err = ntfs_logfile_replay(log = open_log(&m, &io), &ap, false, &res);
	CHECK_EQ(err, 0);
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.mft_records_written, 0);
	CHECK_EQ(res.clusters_written, 1);
	CHECK_EQ(v_read_mft(&v, NEW_REC, got), 0);
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	lf_put16(got + MR_FIXUP_OFFSET_3, 0x0001);
	CHECK(memcmp(got, expect40, VRS) == 0);
	CHECK_EQ(v.disk[MFTBMP_LCN * VCS + 5], 0x01);
	ntfs_logfile_close(log);

	free(v.disk);
	free(buf);
}

/* Abort writes nothing: the volume is byte-identical and replay sees no
 * transaction at all. */
static void test_journal_create_abort(void)
{
	uint8_t *buf = malloc(LSIZE);
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	ntfs_log_txn_t *t;
	struct vol v, v2;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_journal_create_abort\n");
	m.buf = buf;
	CHECK_EQ(journal_setup(&m, &io, &log, &w), 0);
	CHECK_EQ(ntfs_log_txn_begin(w, &t), 0);
	CHECK_EQ(stage_create(t, 0x18, 0x40, NEW_REC, "t.txt", DIR_REC,
			      (uint64_t)-1), 0);
	CHECK_EQ(ntfs_log_txn_abort(t), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	vol_init(&v);
	vol_init(&v2);
	vol_apply(&v, &ap);
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(res.records_analyzed, 0);
	CHECK_EQ(res.records_redone, 0);
	CHECK_EQ(res.transactions_committed, 0);
	CHECK_EQ(res.transactions_active, 0);
	CHECK_EQ(res.mft_records_written, 0);
	CHECK_EQ(res.clusters_written, 0);
	CHECK(!res.needs_chkdsk);
	CHECK(memcmp(v.disk, v2.disk, VOL_CLUSTERS * VCS) == 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(v2.disk);
	free(buf);
}

/* A commit that dies mid-append (the device fails writes) leaves an ACTIVE
 * transaction, and replay undoes it: the second file never appears, bit 41
 * stays clear, the first file survives. The fault budget allows exactly the
 * first record's append (tail copy + page rewrite); the non-vacuity guard
 * is records_undone >= 1 — if no record had landed there would be nothing
 * to undo. */
/* Build the two-transaction log with the second commit faulted after
 * @budget writes. Returns the commit error; *@landed receives the number
 * of second-transaction records durable on the log (everything after the
 * Forget record in the walk from the checkpoint). */
static int build_uncommitted_log(uint8_t *buf, int budget, int *landed)
{
	struct memio m;
	struct fault_memio f;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	ntfs_log_txn_t *t;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	struct rec_seen s;
	int err, i, nforget = -1;

	*landed = 0;
	m.buf = buf;
	memset(buf, 0xff, LSIZE);
	m.size = LSIZE;
	m.writes = 0;
	f.m = &m;
	f.limit = -1;
	f.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &f;
	io.size = LSIZE;
	io.read = fault_mem_read;
	io.write = fault_mem_write;
	err = ntfs_log_writer_format(&io, &g, LSIZE);
	if (err)
		return err;
	log = open_log_keep_io(&io);
	err = ntfs_log_writer_open(log, &w);
	if (err)
		return err;
	err = ntfs_log_writer_open_attr(w, ATTR_TYPE_DATA, 0, 0, NULL, 0, NULL);
	if (err)
		return err;
	err = ntfs_log_writer_open_attr(w, ATTR_TYPE_BITMAP, 0, 0, NULL, 0, NULL);
	if (err)
		return err;
	/* First transaction commits cleanly: the log now has a checkpoint,
	 * so the faulted second transaction is inside replay's window. */
	err = ntfs_log_txn_begin(w, &t);
	if (err)
		return err;
	err = stage_create(t, 0x18, 0x40, NEW_REC, "t.txt", DIR_REC,
			   (uint64_t)-1);
	if (err)
		return err;
	err = ntfs_log_txn_commit(t, NULL, NULL);
	if (err)
		return err;
	err = ntfs_log_txn_begin(w, &t);
	if (err)
		return err;
	err = stage_create(t, 0x18, 0x40, NEW_REC2, "u.txt", DIR_REC,
			   (uint64_t)-1);
	if (err)
		return err;
	f.limit = budget;
	err = ntfs_log_txn_commit(t, NULL, NULL);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);
	if (err != -EIO)
		return err;
	/* Count the second transaction's durable records. */
	memset(&io, 0, sizeof(io));
	io.ctx = &m;
	io.size = LSIZE;
	io.read = mem_read;
	io.write = mem_write;
	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	if (ntfs_logfile_walk(log, 0, collect_rec, &s) != 0) {
		ntfs_logfile_close(log);
		return -EIO;
	}
	ntfs_logfile_close(log);
	for (i = 0; i < s.n; i++) {
		if (s.redo_op[i] == LOP_ForgetTransaction)
			nforget = i;
	}
	*landed = nforget < 0 ? 0 : s.n - nforget - 1;
	return -EIO;
}

/* Like build_uncommitted_log, but the faulted transaction is the FIRST
 * one after the writer opened the attributes: no commit checkpoint has
 * ever dumped them, so replay can only learn the open attributes from
 * the transaction's own OpenNonresidentAttribute records. Without those
 * records replay refuses the log (-EINVAL, "open attribute entry ...
 * not allocated") even though the data records are durable; with them
 * it redoes the landed records and undoes exactly those, and the file
 * never appears. This is the step-3 crash sweep's cuts 22/25 at the
 * unit level. *landed counts data records only (the two open records
 * are skipped by both replay passes). */
static int build_first_txn_uncommitted_log(uint8_t *buf, int budget, int *landed)
{
	struct memio m;
	struct fault_memio f;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	ntfs_log_txn_t *t;
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	struct rec_seen s;
	uint64_t first_lsn = 0;
	int err, i;

	*landed = 0;
	m.buf = buf;
	memset(buf, 0xff, LSIZE);
	m.size = LSIZE;
	m.writes = 0;
	f.m = &m;
	f.limit = -1;
	f.writes = 0;
	memset(&io, 0, sizeof(io));
	io.ctx = &f;
	io.size = LSIZE;
	io.read = fault_mem_read;
	io.write = fault_mem_write;
	err = ntfs_log_writer_format(&io, &g, LSIZE);
	if (err)
		return err;
	log = open_log_keep_io(&io);
	err = ntfs_log_writer_open(log, &w);
	if (err)
		return err;
	/* A checkpoint that predates the attributes, as a mount-open
	 * checkpoint does in real life: the previous session's close
	 * wrote it, and this session's attributes are new slots it
	 * does not contain. Without it replay would bail at "no
	 * checkpoint: nothing was ever logged" and the test would be
	 * vacuous; with it, replay walks the open records and the data
	 * records that follow. */
	err = ntfs_log_writer_checkpoint(w, NULL);
	if (err)
		return err;
	err = ntfs_log_writer_open_attr(w, ATTR_TYPE_DATA, 0, 0, NULL, 0, NULL);
	if (err)
		return err;
	err = ntfs_log_writer_open_attr(w, ATTR_TYPE_BITMAP, 0, 0, NULL, 0, NULL);
	if (err)
		return err;
	err = ntfs_log_txn_begin(w, &t);
	if (err)
		return err;
	err = stage_create(t, 0x18, 0x40, NEW_REC, "t.txt", DIR_REC,
			   (uint64_t)-1);
	if (err)
		return err;
	f.limit = budget;
	err = ntfs_log_txn_commit(t, &first_lsn, NULL);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);
	if (err != -EIO)
		return err;
	/* Count the durable data records (the two open records first).
	 * The walk starts at the transaction's first record: no
	 * checkpoint exists yet, so a walk from 0 would find no start
	 * LSN and return nothing. */
	memset(&io, 0, sizeof(io));
	io.ctx = &m;
	io.size = LSIZE;
	io.read = mem_read;
	io.write = mem_write;
	log = open_log(&m, &io);
	memset(&s, 0, sizeof(s));
	if (ntfs_logfile_walk(log, first_lsn, collect_rec, &s) != 0) {
		ntfs_logfile_close(log);
		return -EIO;
	}
	ntfs_logfile_close(log);
	for (i = 0; i < s.n; i++) {
		if (s.redo_op[i] == LOP_OpenNonresidentAttribute)
			continue;
		(*landed)++;
	}
	return -EIO;
}

/* A commit that dies mid-append leaves an ACTIVE transaction, and replay
 * undoes it: the second file never appears, bit 41 stays clear, the first
 * file survives. The fault budget is found adaptively: how many writes one
 * append costs depends on tail copies and page spanning, so budgets are
 * tried in order until one leaves at least one second-transaction data
 * record durable (a torn spanning record leaves nothing to undo, which
 * is also correct — but it would make this test vacuous, so the loop
 * skips it). The landed count includes the transaction's two open
 * records, which redo/undo skip, so the redo/undo counts subtract
 * them. */
static void test_journal_create_uncommitted(void)
{
	uint8_t *buf = malloc(LSIZE);
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	uint8_t got[VRS];
	int landed = 0, budget, err = 0;

	printf("test_journal_create_uncommitted\n");
	m.buf = buf;
	m.size = LSIZE;
	m.writes = 0;
	for (budget = 1; budget <= 16; budget++) {
		err = build_uncommitted_log(buf, budget, &landed);
		if (err == -EIO && landed >= 3)
			break;
	}
	CHECK_EQ(err, -EIO);
	CHECK(landed >= 3);

	/* Replay redoes the first transaction's 7 records plus the landed
	 * second-transaction data records, then undoes exactly the
	 * landed ones (the 2 open records are skipped on both passes). */
	vol_init(&v);
	vol_apply(&v, &ap);
	memset(&io, 0, sizeof(io));
	io.ctx = &m;
	io.size = LSIZE;
	io.read = mem_read;
	io.write = mem_write;
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(res.transactions_committed, 0);	/* first txn forgotten */
	CHECK_EQ(res.transactions_active, 1);
	CHECK_EQ(res.records_redone, 7 + (uint32_t)landed - 2);
	CHECK_EQ(res.records_undone, (uint32_t)landed - 2);
	CHECK(!res.needs_chkdsk);
	/* First file present, second file rolled back. */
	CHECK_EQ(v_read_mft(&v, NEW_REC, got), 0);
	CHECK(lf_get16(got + MR_FLAGS) & MFT_RECORD_IN_USE);
	CHECK_EQ(v_read_mft(&v, NEW_REC2, got), 0);
	CHECK(!(lf_get16(got + MR_FLAGS) & MFT_RECORD_IN_USE));
	CHECK_EQ(v.disk[MFTBMP_LCN * VCS + 5] & 0x02, 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(buf);
}

/* The first transaction after the attributes were opened, faulted
 * mid-commit before any checkpoint: replay must still redo the landed
 * records and undo exactly those — the open records are what make the
 * attributes known to the analysis. Without them this replay fails
 * -EINVAL (the step-3 crash sweep's cuts 22/25). The budget loop
 * needs at least one landed data record, as in the test above. */
static void test_journal_create_first_txn_uncommitted(void)
{
	uint8_t *buf = malloc(LSIZE);
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	uint8_t got[VRS];
	int landed = 0, budget, err = 0;

	printf("test_journal_create_first_txn_uncommitted\n");
	m.buf = buf;
	m.size = LSIZE;
	m.writes = 0;
	for (budget = 1; budget <= 24; budget++) {
		err = build_first_txn_uncommitted_log(buf, budget, &landed);
		if (err == -EIO && landed >= 1)
			break;
	}
	CHECK_EQ(err, -EIO);
	CHECK(landed >= 1);

	/* Replay redoes exactly the landed records, then undoes them:
	 * the file never appears, bit 40 stays clear. */
	vol_init(&v);
	vol_apply(&v, &ap);
	memset(&io, 0, sizeof(io));
	io.ctx = &m;
	io.size = LSIZE;
	io.read = mem_read;
	io.write = mem_write;
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(res.transactions_committed, 0);
	CHECK_EQ(res.transactions_active, 1);
	CHECK_EQ(res.records_redone, (uint32_t)landed);
	CHECK_EQ(res.records_undone, (uint32_t)landed);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(v_read_mft(&v, NEW_REC, got), 0);
	CHECK(!(lf_get16(got + MR_FLAGS) & MFT_RECORD_IN_USE));
	CHECK_EQ(v.disk[MFTBMP_LCN * VCS + 5] & 0x01, 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(buf);
}

/* Negative control: a create whose Initialize record names the wrong $MFT
 * vcn (11, carrying the page LCN for vcn 10) commits fine — the writer does
 * not validate — but replay refuses the whole log (H1) and writes nothing. */
static void test_journal_create_bad_vcn(void)
{
	uint8_t *buf = malloc(LSIZE);
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	ntfs_log_writer_t *w;
	ntfs_log_txn_t *t;
	struct vol v, v2;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	int err;

	printf("test_journal_create_bad_vcn\n");
	m.buf = buf;
	CHECK_EQ(journal_setup(&m, &io, &log, &w), 0);
	CHECK_EQ(ntfs_log_txn_begin(w, &t), 0);
	CHECK_EQ(stage_create(t, 0x18, 0x40, NEW_REC, "t.txt", DIR_REC, 11), 0);
	CHECK_EQ(ntfs_log_txn_commit(t, NULL, NULL), 0);
	ntfs_log_writer_free(w);
	ntfs_logfile_close(log);

	vol_init(&v);
	vol_init(&v2);
	vol_apply(&v, &ap);
	log = open_log(&m, &io);
	err = ntfs_logfile_replay(log, &ap, false, &res);
	CHECK_EQ(err, -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(res.mft_records_written, 0);
	CHECK_EQ(res.clusters_written, 0);
	CHECK(memcmp(v.disk, v2.disk, VOL_CLUSTERS * VCS) == 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(v2.disk);
	free(buf);
}

int main(void)
{
	test_states();
	test_walk_wrap_multipage();
	test_redo_bitmap();
	test_undo_bitmap();
	test_refuse_incoherent();
	test_mft_record_ops();
	test_tail_copy_and_torn();
	test_tables_api();
	test_v2_log();
	test_flush_failure_is_not_a_partial_write();
	test_replay_cut_points();
	test_first_write_into_fresh_cluster_is_protected();
	test_writer_roundtrip();
	test_writer_reopen_append();
	test_writer_tail_copy_crash();
	test_writer_torn_tail();
	test_writer_checkpoint_basic();
	test_writer_checkpoint_reopen();
	test_writer_checkpoint_reserve();
	test_writer_wrap_readback();
	test_writer_clean_close();
	test_journal_create_commit();
	test_journal_create_abort();
	test_journal_create_uncommitted();
	test_journal_create_first_txn_uncommitted();
	test_journal_create_bad_vcn();
	test_images();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
