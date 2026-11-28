/* SPDX-License-Identifier: GPL-2.0 */
/*
 * journal.c - journal glue for file creation (docs/JOURNAL-DESIGN.md Phase 2).
 *
 * The transaction API stages records; this layer snapshots each
 * sub-mutation out of the mapped MFT records as __ntfs_create makes it.
 * Every snapshot is read from the live record at the moment the
 * sub-mutation lands, so the logged bytes are the bytes the driver
 * actually wrote — nothing is reconstructed from a parallel model.
 *
 * Two deliberate divergences from the live bytes, both documented where
 * they happen:
 *
 *   - the $FILE_NAME CreateAttribute copy carries
 *     RESIDENT_ATTR_IS_INDEXED. The live record carries it too
 *     (attrib.c sets it for every AT_FILE_NAME it adds), so the
 *     copy is identical here; the flag is set explicitly anyway
 *     because the replay's link-count bump keys on it — without
 *     it a replayed create would land with link_count 0. The
 *     unlink's DeleteAttribute relies on the same flag: the
 *     replay decrements the link count when it removes an
 *     INDEXED $FILE_NAME, so no separate record carries the
 *     count.
 *   - $EA is logged net: $EA_INFORMATION + $EA with their final values,
 *     one CreateAttribute each. The three ntfs_set_ea() calls grow the
 *     $EA value twice in memory; those intermediate grows never reach
 *     the disk (the flush happens after commit), so the redo lands the
 *     same final state with fewer records.
 *
 * The parent directory's $STANDARD_INFORMATION mtime is NOT logged:
 * __ntfs_create only marks the directory inode dirty (mark_inode_dirty)
 * and the new mtime reaches the mapped record later, in the inode
 * writeback — outside this transaction's window. That update is
 * unjournaled until the setattr class lands in Phase 3; the record
 * still flushes, with the new mtime, after the commit. It is a benign
 * divergence (mtime only), recorded in GAPS.md §1.2, not a hole: the
 * WAL gate holds the directory record until the commit is durable, so
 * the mtime can never reach the disk ahead of the index entry.
 */

#include "ntfs.h"
#include "attrib.h"
#include "dir.h"
#include "mft.h"
#include "reparse.h"
#include "time.h"
#include "journal.h"
#include "../ntfs/runlist.h"
#include "../logfile/ntfs_logfile.h"
#include "../logfile/logfile_layout.h"

struct mft_attr_snapshot {
	u64 alloc, valid, data, total;
	u16 attr_off;		/* attribute offset in record 0 */
	u16 pairs_off;		/* mapping-pairs offset within the attribute */
	u16 pairs_len;		/* mapping-pairs byte length */
	u16 flags;		/* attribute flags (ATTR_IS_SPARSE et al) */
	u16 name_off;		/* name offset within the attribute */
	u8 *pairs;		/* kmalloc'd copy of the pairs */
};

struct ntfs_journal_txn {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	/* Record 0's $DATA and $BITMAP as they stood at begin, before
	 * the allocation. The staging call compares the live record 0
	 * against these and logs what moved. */
	struct mft_attr_snapshot snap_data, snap_bmp;
	bool snap_ok;
	/* $Reparse's MFT record number, when the create staged a $R
	 * index entry there (a symlink): the commit stamps its MR_LSN
	 * too, so the replay skips the $R entry record. 0 when no $R
	 * entry was staged. */
	u64 reparse_mft_no;
};

/* ---- coordinates ------------------------------------------------------- */

/*
 * Byte offset @byte_off in @attr_ni's runs -> (target_vcn, cluster_off,
 * page_lcn) for a log record. cluster_off is in 512-byte units, always:
 * the log's sector size is 512 whatever the volume's is. The page LCN
 * is the run list's answer for that vcn — the same value the replay's
 * H1 check compares against $MFT's own run list, so a record built
 * from these coordinates passes H1 exactly when the run list agrees
 * with the disk, which is the definition of the record being where
 * the log says it is.
 */
static int attr_coords(struct ntfs_volume *vol, struct ntfs_inode *attr_ni,
		       u64 byte_off, u64 *vcn_out, u16 *cluster_off_out,
		       u64 *page_lcn_out)
{
	u64 vcn = byte_off >> vol->cluster_size_bits;
	u16 cluster_off = (u16)((byte_off & vol->cluster_size_mask) >> 9);
	s64 lcn;

	down_read(&attr_ni->runlist.lock);
	lcn = ntfs_attr_vcn_to_lcn_nolock(attr_ni, (s64)vcn, false);
	up_read(&attr_ni->runlist.lock);
	if (lcn <= LCN_HOLE)
		return -EIO;
	*vcn_out = vcn;
	*cluster_off_out = cluster_off;
	*page_lcn_out = (u64)lcn;
	return 0;
}

static int mft_coords(struct ntfs_volume *vol, u64 mft_no,
		      u64 *vcn_out, u16 *cluster_off_out, u64 *page_lcn_out)
{
	return attr_coords(vol, NTFS_I(vol->mft_ino),
			   mft_no * vol->mft_record_size,
			   vcn_out, cluster_off_out, page_lcn_out);
}

static int bitmap_coords(struct ntfs_volume *vol, u64 bit,
			 u64 *vcn_out, u16 *cluster_off_out, u64 *page_lcn_out)
{
	return attr_coords(vol, NTFS_I(vol->mftbmp_ino), bit >> 3,
			   vcn_out, cluster_off_out, page_lcn_out);
}

/*
 * Coordinates of a CLUSTER bit in the volume $Bitmap ($Bitmap:$DATA,
 * record 6) — not bitmap_coords(), which maps an MFT-RECORD bit in
 * $MFT:$BITMAP (record 0, type 0xB0). The two bitmaps live in different
 * attributes with different run lists: a cluster range staged against
 * $MFT:$BITMAP lands on the $MFT:$BITMAP's own cluster on replay and
 * leaves the freed clusters marked allocated in the real $Bitmap — a
 * leak ntfsck flags after a crash. Only the truncate class stages
 * cluster bits; create/unlink stage MFT-record bits via bitmap_coords().
 */
static int cluster_bitmap_coords(struct ntfs_volume *vol, u64 bit,
				 u64 *vcn_out, u16 *cluster_off_out,
				 u64 *page_lcn_out)
{
	return attr_coords(vol, NTFS_I(vol->lcnbmp_ino), bit >> 3,
			   vcn_out, cluster_off_out, page_lcn_out);
}

/*
 * ntfs_journal_notify_flushed - retire dirty pages whose clusters
 * reached the volume (docs/JOURNAL-DESIGN.md §4.3 step 2).
 *
 * Walks the byte range [@byte_off, @byte_off + @len) in @attr_ni's
 * runs, builds the (lcn, len) ranges and calls the writer's
 * DeleteDirtyClusters emitter, which zeroes those slots in the
 * writer's dirty-page table. The caller has just written those
 * bytes synchronously and still holds the folio lock, so the notify
 * can only ever retire changes already on the volume; any later
 * mutation must take the folio lock first and its staging
 * re-creates or re-populates the entry.
 *
 * A vcn with no LCN (a hole — the range runs past the attribute's
 * allocation, e.g. a folio tail beyond initialized_size) is skipped:
 * it was never written, so there is nothing to retire for it. A
 * notification for ranges matching no entry appends nothing, so a
 * stale or duplicate notify is harmless. The emitter's errors are
 * dropped: retirement is best-effort, correctness never depends on
 * it (an unretired entry only costs log space).
 *
 * Takes only the writer lock (inside the emitter), never
 * log_txn_lock — taking the driver's transaction lock from writeback
 * would invert against a commit blocked on a folio lock. The run-list
 * read lock is taken and released per vcn inside attr_coords, never
 * nested with the writer lock.
 */
void ntfs_journal_notify_flushed(struct ntfs_volume *vol,
				 struct ntfs_inode *attr_ni,
				 u64 byte_off, u64 len)
{
	struct ntfs_log_range ranges[8];
	uint32_t nranges = 0;
	u64 vcn, end_vcn;

	if (!vol->log_writer || !attr_ni || !len)
		return;
	if (byte_off + len < byte_off)
		return;
	vcn = byte_off >> vol->cluster_size_bits;
	end_vcn = (byte_off + len + vol->cluster_size - 1) >>
		vol->cluster_size_bits;
	for (; vcn < end_vcn; vcn++) {
		u64 lcn;
		u16 cluster_off;
		u64 v;

		if (attr_coords(vol, attr_ni, vcn << vol->cluster_size_bits,
				&v, &cluster_off, &lcn) != 0)
			continue;
		if (nranges > 0 &&
		    lcn == ranges[nranges - 1].lcn + ranges[nranges - 1].len) {
			ranges[nranges - 1].len++;
			continue;
		}
		if (nranges >= ARRAY_SIZE(ranges)) {
			ntfs_log_writer_delete_dirty_clusters(vol->log_writer,
							      ranges, nranges);
			nranges = 0;
		}
		ranges[nranges].lcn = lcn;
		ranges[nranges].len = 1;
		nranges++;
	}
	if (nranges)
		ntfs_log_writer_delete_dirty_clusters(vol->log_writer,
						      ranges, nranges);
}

/*
 * ntfs_journal_set_checkpoint_every - set the checkpoint policy for
 * this mount's writer (docs/JOURNAL-DESIGN.md §4.3): how many commits
 * run between checkpoints. 0 clamps to 1 (every commit, the Phase 2
 * behaviour). The driver default is NTFS_LOG_CHECKPOINT_EVERY_DEFAULT;
 * the tests that pin per-commit checkpoint behaviour set it to 1
 * explicitly, so they keep observing a checkpoint after each commit
 * whatever the default is. Returns -ENODEV when journaling is
 * inactive on this mount.
 */
int ntfs_journal_set_checkpoint_every(struct ntfs_volume *vol, uint32_t n)
{
	if (!vol || !vol->log_writer)
		return -ENODEV;
	return ntfs_log_writer_set_checkpoint_every(vol->log_writer, n);
}

/* ---- record scanning ---------------------------------------------------- */

/*
 * Find (type, name, name_len) in the base record @m. Attributes are
 * inserted in type order, so the caller must scan after the add, not
 * assume an offset. Returns NULL when the attribute is not in the base
 * record (it went to an extent) or the record is corrupt — the caller
 * then aborts the transaction and proceeds unjournaled, never guessing.
 */
static const struct attr_record *find_attr(const struct mft_record *m,
					   __le32 type, const __le16 *name,
					   u8 name_len, u16 *off_out)
{
	u16 off = le16_to_cpu(m->attrs_offset);
	u32 used = le32_to_cpu(m->bytes_in_use);

	while (off + 8 <= used) {
		const struct attr_record *a =
			(const struct attr_record *)((const u8 *)m + off);
		u32 len;

		if (a->type == AT_END)
			break;
		len = le32_to_cpu(a->length);
		if (len < 8 || off + len > used)
			return NULL;
		if (a->type == type && a->name_length == name_len) {
			const __le16 *an = (const __le16 *)((const u8 *)a +
				le16_to_cpu(a->name_offset));

			if (!name_len ||
			    !memcmp(an, name, name_len * sizeof(__le16))) {
				if (off_out)
					*off_out = off;
				return a;
			}
		}
		off += (u16)len;
	}
	return NULL;
}

/* ---- transaction lifetime ---------------------------------------------- */

/*
 * Snapshot one non-resident attribute of record 0 (its sizes and its
 * mapping pairs) into @snap. The caller maps record 0; this only reads
 * it. The compressed size lives at offset 0x40 and is present only
 * when the attribute is at least 0x48 long (AT_NONRESIDENT_EX_SIZE);
 * shorter attributes carry 0 there. @name/@name_len select a named
 * attribute (the xattr class's ADS); NULL/0 selects the unnamed one
 * (the truncate class's $DATA).
 */
static int snapshot_mft_attr(const struct mft_record *m0, __le32 type,
			     const __le16 *name, u8 name_len,
			     struct mft_attr_snapshot *snap)
{
	const struct attr_record *a;
	u16 off = 0;
	u32 len;
	u16 pairs_off;

	a = find_attr(m0, type, name, name_len, &off);
	if (!a || !a->non_resident)
		return -ENOENT;
	len = le32_to_cpu(a->length);
	pairs_off = le16_to_cpu(a->data.non_resident.mapping_pairs_offset);
	if (pairs_off >= len)
		return -EIO;
	snap->alloc = le64_to_cpu(a->data.non_resident.allocated_size);
	snap->valid = le64_to_cpu(a->data.non_resident.initialized_size);
	snap->data = le64_to_cpu(a->data.non_resident.data_size);
	/* compressed_size exists only when the mapping pairs start
	 * after it (pairs_off >= 0x48). A short-form attribute — the
	 * $MFT $DATA is one, pairs at 0x40, length exactly 0x48 — has
	 * no such field: offset 0x40 IS the pairs. Reading it as a
	 * size would log the pairs bytes as the total, and the replay
	 * would write them back over the pairs if they changed. */
	snap->total = pairs_off >= 0x48 ?
		le64_to_cpu(a->data.non_resident.compressed_size) : 0;
	snap->attr_off = off;
	snap->pairs_off = pairs_off;
	snap->flags = le16_to_cpu(a->flags);
	snap->name_off = le16_to_cpu(a->name_offset);
	snap->pairs_len = (u16)(len - pairs_off);
	snap->pairs = kmalloc(snap->pairs_len, GFP_NOFS);
	if (!snap->pairs)
		return -ENOMEM;
	memcpy(snap->pairs, (const u8 *)a + pairs_off, snap->pairs_len);
	return 0;
}

static void free_snapshot(struct mft_attr_snapshot *snap)
{
	kfree(snap->pairs);
	snap->pairs = NULL;
}

int ntfs_journal_create_begin(struct ntfs_volume *vol, mode_t mode,
			      struct ntfs_journal_txn **out)
{
	struct ntfs_journal_txn *jt;
	int err;

	*out = NULL;
	/* Phase 3 mkdir: directories begin too (their $INDEX_ROOT is
	 * staged in __ntfs_create, next to the $DATA staging for files).
	 * Phase 3 symlink: symlinks begin too (their $REPARSE_POINT,
	 * $R index entry and $SI flags are staged in __ntfs_create,
	 * next to the $DATA/$INDEX_ROOT staging). Phase 3 reparse,
	 * mknod half: device nodes begin too — their $REPARSE_POINT,
	 * $R index entry and $SI flags are staged the same way, in
	 * the device branch of __ntfs_create. Anything else never
	 * begins: a transaction that cannot describe it would abort
	 * anyway. */
	if (!vol->log_writer || (!S_ISREG(mode) && !S_ISDIR(mode) &&
				 !S_ISLNK(mode) && !S_ISBLK(mode) &&
				 !S_ISCHR(mode) && !S_ISSOCK(mode) &&
				 !S_ISFIFO(mode)))
		return 0;
	jt = kmalloc(sizeof(*jt), GFP_NOFS);
	if (!jt)
		return -ENOMEM;
	jt->vol = vol;
	jt->t = NULL;
	jt->snap_data.pairs = NULL;
	jt->snap_bmp.pairs = NULL;
	jt->snap_ok = false;
	jt->reparse_mft_no = 0;
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &jt->t);
	if (err) {
		mutex_unlock(&vol->log_txn_lock);
		kfree(jt);
		return err;
	}
	/*
	 * Snapshot record 0's $DATA and $BITMAP before the allocation:
	 * ntfs_mft_record_alloc extends $MFT (sizes, and the runs when
	 * the allocation itself grows) and the $MFT bitmap (its sizes
	 * every 8 records), and the log must describe the change from
	 * these values to the post-alloc ones. A snapshot failure is a
	 * begin failure: the create proceeds unjournaled rather than
	 * half-journaled. Record 0 is mapped only for the copy.
	 */
	{
		struct mft_record *m0 = map_mft_record(NTFS_I(vol->mft_ino));

		if (IS_ERR(m0))
			err = PTR_ERR(m0);
		else {
			err = snapshot_mft_attr(m0, AT_DATA, NULL, 0, &jt->snap_data);
			if (!err)
				err = snapshot_mft_attr(m0, AT_BITMAP, NULL, 0,
							&jt->snap_bmp);
			unmap_mft_record(NTFS_I(vol->mft_ino));
		}
	}
	if (err) {
		free_snapshot(&jt->snap_data);
		free_snapshot(&jt->snap_bmp);
		(void)ntfs_log_txn_abort(jt->t);
		mutex_unlock(&vol->log_txn_lock);
		kfree(jt);
		return err;
	}
	jt->snap_ok = true;
	atomic_inc(&vol->log_txn_open);
	*out = jt;
	return 0;
}

void ntfs_journal_create_abort(struct ntfs_journal_txn *t)
{
	if (!t)
		return;
	(void)ntfs_log_txn_abort(t->t);
	free_snapshot(&t->snap_data);
	free_snapshot(&t->snap_bmp);
	atomic_dec(&t->vol->log_txn_open);
	mutex_unlock(&t->vol->log_txn_lock);
	kfree(t);
}

/* Stamp MR_LSN into $Reparse's record. A transaction that added or
 * removed a $R index entry there must stamp it, or a post-commit
 * flush + crash replays the entry record onto a record that already
 * carries (or already lacks) it: AddIndexEntryRoot inserts
 * unconditionally and DeleteIndexEntryRoot is offset-based, so either
 * re-application corrupts the index. Best-effort: the commit already
 * succeeded, so a stamp failure here (an iget/map failure on a
 * record that exists — its entry was staged moments ago) cannot fail
 * the operation; it leaves the same re-application risk a failed
 * commit leaves for the other records. */
static void stamp_reparse_mft_lsn(struct ntfs_volume *vol,
				  u64 reparse_mft_no, u64 last_lsn)
{
	struct inode *rp_vi;
	struct mft_record *rp_mrec;

	if (!reparse_mft_no)
		return;
	rp_vi = ntfs_iget(vol->sb, reparse_mft_no);
	if (IS_ERR(rp_vi))
		return;
	mutex_lock_nested(&NTFS_I(rp_vi)->mrec_lock,
			  NTFS_EXTEND_MUTEX_PARENT);
	rp_mrec = map_mft_record(NTFS_I(rp_vi));
	if (rp_mrec) {
		rp_mrec->lsn = cpu_to_le64(last_lsn);
		mark_mft_record_dirty(NTFS_I(rp_vi));
		unmap_mft_record(NTFS_I(rp_vi));
	}
	mutex_unlock(&NTFS_I(rp_vi)->mrec_lock);
	iput(rp_vi);
}

int ntfs_journal_create_commit(struct ntfs_journal_txn *t,
			       struct mft_record *mrec,
			       struct mft_record *parent_mrec)
{
	uint64_t last_lsn = 0;
	int err;

	if (!t)
		return -EINVAL;
	err = ntfs_log_txn_commit(t->t, NULL, &last_lsn);
	if (!err) {
		/*
		 * Stamp MR_LSN into both touched records before the
		 * metadata flush: the replay skips a record whose MR_LSN
		 * is already at or past the log record's LSN, so without
		 * the stamp it would re-apply the CreateAttribute records
		 * onto a record that already has them and duplicate the
		 * attributes. The stamp is the transaction's last LSN —
		 * the SetBits record's — which is >= every record that
		 * touched either MFT record, so it skips exactly the
		 * transaction's own records and nothing else.
		 */
		mrec->lsn = cpu_to_le64(last_lsn);
		if (parent_mrec)
			parent_mrec->lsn = cpu_to_le64(last_lsn);
		/* A symlink also touched $Reparse's record (its $R index
		 * entry): stamp it too, or a post-commit flush + crash
		 * replays the AddIndexEntryRoot onto a record that
		 * already has it and duplicates the entry — the replay's
		 * AddIndexEntryRoot inserts unconditionally, and its
		 * skip check is this record's MR_LSN. */
		stamp_reparse_mft_lsn(t->vol, t->reparse_mft_no, last_lsn);
	}
	free_snapshot(&t->snap_data);
	free_snapshot(&t->snap_bmp);
	atomic_dec(&t->vol->log_txn_open);
	mutex_unlock(&t->vol->log_txn_lock);
	kfree(t);
	return err;
}

/* ---- staging ------------------------------------------------------------ */

int ntfs_journal_create_init(struct ntfs_journal_txn *t,
			     struct ntfs_inode *ni,
			     const struct mft_record *mrec)
{
	u32 len;
	u8 *copy;
	u64 vcn, page_lcn;
	u16 cluster_off;
	const struct attr_record *first, *second;
	u16 off;
	int err;

	if (!t)
		return -EINVAL;
	/*
	 * The image is the record as it stands right after $SI was added:
	 * header + $SI + END, link 0, next attribute id 1. Refuse anything
	 * else — staging a record that already carries more attributes
	 * would make the later CreateAttribute records duplicate them on
	 * replay, and a mis-ordered call must fail loudly, not log a
	 * corrupt transaction.
	 */
	off = le16_to_cpu(mrec->attrs_offset);
	first = (const struct attr_record *)((const u8 *)mrec + off);
	if (first->type != AT_STANDARD_INFORMATION)
		return -EINVAL;
	if (le32_to_cpu(first->length) < 8 ||
	    off + le32_to_cpu(first->length) + 8 >
	    le32_to_cpu(mrec->bytes_in_use))
		return -EINVAL;
	second = (const struct attr_record *)((const u8 *)first +
		le32_to_cpu(first->length));
	if (second->type != AT_END)
		return -EINVAL;
	len = le32_to_cpu(mrec->bytes_in_use);
	if (len > t->vol->mft_record_size)
		return -EINVAL;
	copy = kmalloc(len, GFP_NOFS);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, mrec, len);
	err = mft_coords(t->vol, ni->mft_no, &vcn, &cluster_off, &page_lcn);
	if (!err)
		err = ntfs_log_txn_init_record(t->t, t->vol->log_mft_data_attr,
					       vcn, cluster_off, page_lcn,
					       copy, len);
	kfree(copy);
	return err;
}

/* Stage a CreateAttribute record for the attribute just added to
 * @ni's record, into @t's log transaction. Scans @mrec for (type,
 * name, name_len) and stages it at its true offset — attributes are
 * inserted in type order, so the offset must be read after the add,
 * not assumed. When @index_flag is set and the attribute is a
 * $FILE_NAME, the staged copy carries RESIDENT_ATTR_IS_INDEXED,
 * which the live record does not: the replay's link-count bump keys
 * on it. Returns -ENOENT when the attribute is not in the base
 * record (it went to an extent). */
static int stage_create_attr(ntfs_log_txn_t *t, struct ntfs_volume *vol,
			     struct ntfs_inode *ni,
			     const struct mft_record *mrec,
			     __le32 type, const __le16 *name, u8 name_len,
			     bool index_flag)
{
	const struct attr_record *a;
	u16 off = 0;
	u8 *copy;
	u32 len;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	a = find_attr(mrec, type, name, name_len, &off);
	if (!a)
		return -ENOENT;
	len = le32_to_cpu(a->length);
	copy = kmalloc(len, GFP_NOFS);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, a, len);
	if (index_flag && type == AT_FILE_NAME) {
		/*
		 * The live $FILE_NAME already carries INDEXED
		 * (attrib.c sets RESIDENT_ATTR_IS_INDEXED for every
		 * AT_FILE_NAME it adds); set it on the copy anyway,
		 * because the replay's link-count bump keys on it —
		 * without it a replayed create lands with
		 * link_count 0. Only the copy is touched; the live
		 * record keeps its bytes.
		 */
		struct attr_record *c = (struct attr_record *)copy;

		if (!c->non_resident)
			c->data.resident.flags |= RESIDENT_ATTR_IS_INDEXED;
	}
	err = mft_coords(vol, ni->mft_no, &vcn, &cluster_off, &page_lcn);
	if (!err)
		err = ntfs_log_txn_create_attr(t, vol->log_mft_data_attr,
					       vcn, cluster_off, page_lcn,
					       off, copy, len);
	kfree(copy);
	return err;
}

int ntfs_journal_create_attr(struct ntfs_journal_txn *t,
			     struct ntfs_inode *ni,
			     const struct mft_record *mrec,
			     __le32 type, const __le16 *name, u8 name_len)
{
	if (!t)
		return -EINVAL;
	return stage_create_attr(t->t, t->vol, ni, mrec, type, name,
				 name_len, true);
}

int ntfs_journal_create_ea(struct ntfs_journal_txn *t,
			   struct ntfs_inode *ni,
			   const struct mft_record *mrec)
{
	int err;

	err = ntfs_journal_create_attr(t, ni, mrec,
				       AT_EA_INFORMATION, NULL, 0);
	if (err)
		return err;
	return ntfs_journal_create_attr(t, ni, mrec, AT_EA, NULL, 0);
}

int ntfs_journal_create_index(struct ntfs_journal_txn *t,
			      struct ntfs_inode *dir_ni,
			      const struct mft_record *dir_mrec,
			      u64 child_mref)
{
	const struct attr_record *a;
	const struct index_root *ir;
	const struct index_header *ih;
	const u8 *value, *entries, *end;
	const struct index_entry *e;
	u16 attr_off = 0;
	u8 *copy;
	u32 len;
	u64 vcn, page_lcn;
	u16 cluster_off, entry_off;
	int err;

	if (!t)
		return -EINVAL;
	a = find_attr(dir_mrec, AT_INDEX_ROOT, I30, 4, &attr_off);
	if (!a || a->non_resident)
		return -ENOENT;
	value = (const u8 *)a +
		le16_to_cpu(a->data.resident.value_offset);
	ir = (const struct index_root *)value;
	ih = &ir->index;
	/* entries_offset is relative to the index header, not the value.
	 * index_length is likewise measured from the index header start
	 * (dir.c: index_end = &ir->index + index_length), not from
	 * entries_offset: end is ih + index_length, and entries + the
	 * used entries fit inside it. */
	entries = (const u8 *)ih + le32_to_cpu(ih->entries_offset);
	end = (const u8 *)ih + le32_to_cpu(ih->index_length);
	if (entries < value ||
	    end > value + le32_to_cpu(a->data.resident.value_length))
		return -EINVAL;
	for (e = (const struct index_entry *)entries;
	     (const u8 *)e + 16 <= end;) {
		u16 elen = le16_to_cpu(e->length);

		if (elen < 16 || (const u8 *)e + elen > end)
			return -EINVAL;
		if (e->flags & INDEX_ENTRY_END)
			break;
		if (le64_to_cpu(e->data.dir.indexed_file) == child_mref) {
			len = elen;
			goto found;
		}
		e = (const struct index_entry *)((const u8 *)e + elen);
	}
	/* Not in the root: the entry went to an $INDEX_ALLOCATION block,
	 * which Phase 2 cannot describe. */
	return -ENOENT;

found:
	copy = kmalloc(len, GFP_NOFS);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, e, len);
	/* aoff is the entry's offset from the attribute record's start. */
	entry_off = (u16)((const u8 *)e - (const u8 *)a);
	err = mft_coords(t->vol, dir_ni->mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (!err)
		err = ntfs_log_txn_add_index_root(t->t,
						  t->vol->log_mft_data_attr,
						  vcn, cluster_off, page_lcn,
						  attr_off, entry_off,
						  copy, len);
	kfree(copy);
	return err;
}

/*
 * Find the $R index entry for (tag, file_id) in @rp_mrec's $INDEX_ROOT
 * named "$R". The entry is a struct reparse_index: a 16-byte header,
 * the 12-byte key, a 4-byte filling. On success fills @attr_off_out
 * (the $INDEX_ROOT's offset in the record), @entry_off_out (the
 * entry's offset from the attribute record's start — the aoff the log
 * records carry), @len_out and @entry_out (the entry itself, valid
 * while @rp_mrec stays mapped). Returns -ENOENT when the entry is not
 * in the root (it lives in an $INDEX_ALLOCATION block, which this
 * phase cannot describe) and -EINVAL on a corrupt index. Shared by
 * the create staging (which adds the entry) and the unlink staging
 * (which removes it).
 */
static int find_reparse_index_entry(const struct mft_record *rp_mrec,
				    __le32 tag, u64 file_id,
				    u16 *attr_off_out, u16 *entry_off_out,
				    u32 *len_out,
				    const struct index_entry **entry_out)
{
	const struct attr_record *a;
	const struct index_root *ir;
	const struct index_header *ih;
	const u8 *value, *entries, *end;
	const struct index_entry *e;
	u16 attr_off = 0;

	a = find_attr(rp_mrec, AT_INDEX_ROOT, reparse_index_name, 2,
		      &attr_off);
	if (!a || a->non_resident)
		return -ENOENT;
	value = (const u8 *)a +
		le16_to_cpu(a->data.resident.value_offset);
	ir = (const struct index_root *)value;
	ih = &ir->index;
	entries = (const u8 *)ih + le32_to_cpu(ih->entries_offset);
	end = (const u8 *)ih + le32_to_cpu(ih->index_length);
	if (entries < value ||
	    end > value + le32_to_cpu(a->data.resident.value_length))
		return -EINVAL;
	for (e = (const struct index_entry *)entries;
	     (const u8 *)e + 16 <= end;) {
		u16 elen = le16_to_cpu(e->length);

		if (elen < 16 || (const u8 *)e + elen > end)
			return -EINVAL;
		if (e->flags & INDEX_ENTRY_END)
			break;
		if (elen < sizeof(struct reparse_index))
			return -EINVAL;
		{
			const struct reparse_index *ri =
				(const struct reparse_index *)e;

			if (ri->key.reparse_tag == tag &&
			    le64_to_cpu(ri->key.file_id) == file_id) {
				if (attr_off_out)
					*attr_off_out = attr_off;
				if (entry_off_out)
					*entry_off_out = (u16)((const u8 *)e -
							(const u8 *)a);
				if (len_out)
					*len_out = elen;
				if (entry_out)
					*entry_out = e;
				return 0;
			}
		}
		e = (const struct index_entry *)((const u8 *)e + elen);
	}
	/* Not in the root: the entry went to an $INDEX_ALLOCATION block,
	 * which Phase 2 cannot describe. */
	return -ENOENT;
}

/* Stage the two records a reparse create adds on top of a plain
 * create, in live order: the $REPARSE_POINT attribute (its value was
 * just written into the mapped record) and the $R index entry in
 * $Reparse. The $SI flags are staged separately, by
 * ntfs_journal_create_si_flags, after the $FILE_NAME is built —
 * ni->flags is only final then (devices gain FILE_ATTR_SYSTEM and
 * dir-links gain FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT when
 * __ntfs_create folds fn->file_attributes back into it).
 *
 * Returns -ENOENT when either cannot be described (the attribute
 * went to an extent, the $R entry went to an $INDEX_ALLOCATION
 * block, $Reparse cannot be found) — the caller then aborts and
 * proceeds unjournaled. */
int ntfs_journal_create_reparse(struct ntfs_journal_txn *t,
				struct ntfs_volume *vol,
				struct ntfs_inode *ni,
				const struct mft_record *mrec)
{
	const struct index_entry *e;
	struct inode *rp_vi;
	struct ntfs_inode *rp_ni;
	struct mft_record *rp_mrec;
	u64 reparse_mft_no, vcn, page_lcn;
	u16 cluster_off, attr_off = 0, entry_off;
	u8 *copy;
	u32 len;
	int err;

	if (!t)
		return -EINVAL;
	/* (1) The $REPARSE_POINT attribute, at its true offset, with
	 * the value as it stands after the reparse set. */
	err = ntfs_journal_create_attr(t, ni, mrec, AT_REPARSE_POINT,
				       AT_UNNAMED, 0);
	if (err)
		return err;

	/* (2) The $R index entry in $Reparse: find it by (tag, file_id)
	 * in the $R root, the create_index pattern with a different
	 * key. The entry is a struct reparse_index: a 16-byte header,
	 * the 12-byte key, a 4-byte filling. */
	err = ntfs_reparse_mft_no(vol, &reparse_mft_no);
	if (err)
		return -ENOENT;
	rp_vi = ntfs_iget(vol->sb, reparse_mft_no);
	if (IS_ERR(rp_vi))
		return -ENOENT;
	rp_ni = NTFS_I(rp_vi);
	mutex_lock_nested(&rp_ni->mrec_lock, NTFS_EXTEND_MUTEX_PARENT);
	rp_mrec = map_mft_record(rp_ni);
	if (!rp_mrec) {
		err = -ENOENT;
		goto out_unlock;
	}
	err = find_reparse_index_entry(rp_mrec, ni->reparse_tag,
				       MK_MREF(ni->mft_no, ni->seq_no),
				       &attr_off, &entry_off, &len, &e);
	if (err)
		goto out_unmap;
	copy = kmalloc(len, GFP_NOFS);
	if (!copy) {
		err = -ENOMEM;
		goto out_unmap;
	}
	memcpy(copy, e, len);
	err = mft_coords(vol, reparse_mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (!err)
		err = ntfs_log_txn_add_index_root(t->t,
						  vol->log_mft_data_attr,
						  vcn, cluster_off, page_lcn,
						  attr_off, entry_off,
						  copy, len);
	kfree(copy);
	if (err)
		goto out_unmap;
	/* The commit stamps this record's MR_LSN, so the replay skips
	 * the $R entry record. */
	t->reparse_mft_no = reparse_mft_no;
	unmap_mft_record(rp_ni);
	mutex_unlock(&rp_ni->mrec_lock);
	iput(rp_vi);
	return 0;

out_unmap:
	unmap_mft_record(rp_ni);
out_unlock:
	mutex_unlock(&rp_ni->mrec_lock);
	iput(rp_vi);
	return err;
}

/* Stage the $SI flags update for a reparse create, as a 4-byte
 * UpdateResidentValue on the new file's record. Old is read from
 * the mapped $SI, which still holds the as-added flags — the sync
 * that writes ni->flags into it is deferred by the WAL gate — and
 * new is passed in, because ni->flags is only final once the
 * $FILE_NAME is built: __ntfs_create folds fn->file_attributes
 * back into ni->flags at the end of the create (devices gain
 * FILE_ATTR_SYSTEM there, dir-links gain
 * FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT), so a caller that staged
 * this from ni->flags at the reparse set would replay $SI flags
 * without those bits. The caller passes
 * fn->file_attributes | (ni->flags & FILE_ATTRIBUTE_RECALL_ON_OPEN)
 * — the same formula __ntfs_create applies — after the $FILE_NAME
 * is staged. Stage only on change: the truncate $SI pattern.
 * Returns -ENOENT when the $SI cannot be described. */
int ntfs_journal_create_si_flags(struct ntfs_journal_txn *t,
				 struct ntfs_volume *vol,
				 struct ntfs_inode *ni,
				 const struct mft_record *mrec,
				 __le32 new_flags)
{
	const struct attr_record *si_attr;
	const struct standard_information *si_val;
	u64 vcn, page_lcn;
	u16 cluster_off, si_off = 0;
	__le32 old_flags;
	int err;

	if (!t)
		return -EINVAL;
	si_attr = find_attr(mrec, AT_STANDARD_INFORMATION, NULL, 0,
			    &si_off);
	if (!si_attr || si_attr->non_resident)
		return -ENOENT;
	si_val = (const struct standard_information *)
		 ((const u8 *)si_attr +
		  le16_to_cpu(si_attr->data.resident.value_offset));
	old_flags = si_val->file_attributes;
	if (old_flags != new_flags) {
		err = mft_coords(vol, ni->mft_no, &vcn,
				 &cluster_off, &page_lcn);
		if (err)
			return err;
		err = ntfs_log_txn_update_resident(
			t->t, vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			si_off,
			le16_to_cpu(
				si_attr->data.resident.value_offset) +
				offsetof(struct standard_information,
					 file_attributes),
			&new_flags, sizeof(new_flags),
			&old_flags, sizeof(old_flags));
		if (err)
			return err;
	}
	return 0;
}

int ntfs_journal_create_bitmap(struct ntfs_journal_txn *t,
			       struct ntfs_volume *vol, u64 mft_no)
{
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	if (!t)
		return -EINVAL;
	err = bitmap_coords(vol, mft_no, &vcn, &cluster_off, &page_lcn);
	if (err)
		return err;
	return ntfs_log_txn_set_bitmap(t->t, vol->log_mft_bitmap_attr,
				       vcn, cluster_off, page_lcn,
				       (u32)mft_no, 1, true);
}

/*
 * Stage one record-0 attribute's extension: the mapping pairs first
 * (redo order is extend the runs, then the sizes that name them),
 * then the sizes. The records target the $MFT:$DATA open attribute —
 * the record is addressed through $MFT:$DATA's runs — and select the
 * attribute inside record 0 by its offset (roff): that is how a
 * SetNewAttributeSizes for $MFT:$BITMAP is expressed, since the
 * replay's H1 check requires the target to be $MFT:$DATA. The
 * pairs/sizes the log carries are the live bytes read from record 0,
 * never reconstructed.
 */
static int stage_mft_attr(struct ntfs_journal_txn *t,
			  const struct mft_record *m0, __le32 type,
			  const struct mft_attr_snapshot *snap,
			  u64 vcn, u16 cluster_off, u64 page_lcn)
{
	const struct attr_record *a;
	u16 off = 0, pairs_off;
	u32 len;
	u64 alloc, valid, data, total;
	int err;

	a = find_attr(m0, type, NULL, 0, &off);
	if (!a || !a->non_resident)
		return -ENOENT;
	len = le32_to_cpu(a->length);
	pairs_off = le16_to_cpu(a->data.non_resident.mapping_pairs_offset);
	/* The create path extends $MFT $DATA/$BITMAP by allocation, never
	 * by a hole, so the header never flips here: the old and new
	 * flags, name_off and pairs_off are the same. If they ever
	 * differ, abort to unjournaled rather than log a half-described
	 * attribute. */
	if (off != snap->attr_off || pairs_off != snap->pairs_off ||
	    le16_to_cpu(a->flags) != snap->flags ||
	    le16_to_cpu(a->name_offset) != snap->name_off)
		return -ENOENT;
	if (pairs_off >= len)
		return -EIO;
	if (len - pairs_off != snap->pairs_len ||
	    memcmp((const u8 *)a + pairs_off, snap->pairs,
		   snap->pairs_len) != 0) {
		err = ntfs_log_txn_update_mapping_pairs(
			t->t, t->vol->log_mft_data_attr, vcn, cluster_off,
			page_lcn, off, pairs_off,
			le16_to_cpu(a->flags), le16_to_cpu(a->name_offset),
			(const u8 *)a + pairs_off, (u16)(len - pairs_off),
			snap->flags, snap->name_off, snap->pairs_off,
			snap->pairs, snap->pairs_len);
		if (err)
			return err;
	}
	alloc = le64_to_cpu(a->data.non_resident.allocated_size);
	valid = le64_to_cpu(a->data.non_resident.initialized_size);
	data = le64_to_cpu(a->data.non_resident.data_size);
	/* Same rule as the snapshot: compressed_size exists only
	 * when the pairs start after it (pairs_off >= 0x48). A
	 * short-form attribute's offset 0x40 is the pairs, and
	 * reading it as a size would stage a spurious sizes record
	 * on every create (the snapshot reads 0, this reads the
	 * pairs bytes). */
	total = pairs_off >= 0x48 ?
		le64_to_cpu(a->data.non_resident.compressed_size) : 0;
	if (alloc != snap->alloc || valid != snap->valid ||
	    data != snap->data || total != snap->total) {
		err = ntfs_log_txn_set_attr_sizes(
			t->t, t->vol->log_mft_data_attr, vcn, cluster_off,
			page_lcn, off, alloc, valid, data, total,
			snap->alloc, snap->valid, snap->data, snap->total);
		if (err)
			return err;
	}
	return 0;
}

int ntfs_journal_create_mft_sizes(struct ntfs_journal_txn *t)
{
	struct mft_record *m0;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	if (!t || !t->snap_ok)
		return -ENOENT;
	/*
	 * Record 0's MR_LSN is not stamped by the commit (only the new
	 * record and the parent are): the sizes/pairs records are
	 * idempotent (absolute sizes, whole pairs), and the replay's
	 * apply stamps record 0's MR_LSN itself, so a second replay
	 * skips them. A concurrent unjournaled operation (e.g. mkdir)
	 * extending $MFT inside the begin→staging window would be
	 * folded into these records: redo stays correct, but undo
	 * would restore the pre-mkdir sizes — it needs a simultaneous
	 * crash in a microscopic window, the same class as the
	 * documented $SI mtime gap, and the log_txn_lock serialises
	 * all journaled creates against each other.
	 */
	err = mft_coords(t->vol, 0, &vcn, &cluster_off, &page_lcn);
	if (err)
		return err;
	m0 = map_mft_record(NTFS_I(t->vol->mft_ino));
	if (IS_ERR(m0))
		return -ENOENT;
	err = stage_mft_attr(t, m0, AT_DATA, &t->snap_data,
			     vcn, cluster_off, page_lcn);
	if (!err)
		err = stage_mft_attr(t, m0, AT_BITMAP, &t->snap_bmp,
				     vcn, cluster_off, page_lcn);
	unmap_mft_record(NTFS_I(t->vol->mft_ino));
	return err;
}

/* ---- unlink (Phase 3) ---------------------------------------------------- */

struct ntfs_journal_unlink {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	/* $Reparse's MFT record number, when the unlink staged a $R
	 * index entry removal there (a reparse point whose link count
	 * hit 0): the commit stamps its MR_LSN too, so the replay
	 * skips the $R removal record. 0 when no $R entry was staged.
	 * Initialized at begin: kmalloc does not zero (round 25). */
	u64 reparse_mft_no;
};

int ntfs_journal_unlink_begin(struct ntfs_volume *vol,
			      struct ntfs_journal_unlink **out)
{
	struct ntfs_journal_unlink *u;
	int err;

	*out = NULL;
	if (!vol->log_writer)
		return 0;
	u = kmalloc(sizeof(*u), GFP_NOFS);
	if (!u)
		return -ENOMEM;
	u->vol = vol;
	u->t = NULL;
	u->reparse_mft_no = 0;
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &u->t);
	if (err) {
		mutex_unlock(&vol->log_txn_lock);
		kfree(u);
		return err;
	}
	atomic_inc(&vol->log_txn_open);
	*out = u;
	return 0;
}

void ntfs_journal_unlink_abort(struct ntfs_journal_unlink *u)
{
	if (!u)
		return;
	(void)ntfs_log_txn_abort(u->t);
	atomic_dec(&u->vol->log_txn_open);
	mutex_unlock(&u->vol->log_txn_lock);
	kfree(u);
}

int ntfs_journal_unlink_commit(struct ntfs_journal_unlink *u,
			       struct ntfs_inode *ni,
			       struct ntfs_inode *dir_ni)
{
	uint64_t last_lsn = 0;
	struct mft_record *m;
	int err;

	if (!u)
		return -EINVAL;
	err = ntfs_log_txn_commit(u->t, NULL, &last_lsn);
	if (!err) {
		/*
		 * Stamp MR_LSN into both touched records before the
		 * metadata flush, as in the create commit: without the
		 * stamp a second replay would re-apply the delete
		 * records onto records that no longer carry the
		 * attribute or the entry, and refuse the log. The
		 * records were just touched by the delete and are hot
		 * in cache; a map failure here is a volume error, not
		 * a reason to leave the stamp half done.
		 */
		m = map_mft_record(ni);
		if (IS_ERR(m))
			err = PTR_ERR(m);
		else {
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
		if (!err) {
			m = map_mft_record(dir_ni);
			if (IS_ERR(m))
				err = PTR_ERR(m);
			else {
				m->lsn = cpu_to_le64(last_lsn);
				mark_mft_record_dirty(dir_ni);
				unmap_mft_record(dir_ni);
			}
		}
		/* A reparse point also touched $Reparse's record (its $R
		 * index entry was removed): stamp it too, or a
		 * post-commit flush + crash replays the
		 * DeleteIndexEntryRoot onto a record that already lacks
		 * the entry — the replay's delete is offset-based, so
		 * it would excise whatever bytes now sit at that
		 * offset. */
		stamp_reparse_mft_lsn(u->vol, u->reparse_mft_no, last_lsn);
	}
	atomic_dec(&u->vol->log_txn_open);
	mutex_unlock(&u->vol->log_txn_lock);
	kfree(u);
	return err;
}

int ntfs_journal_unlink_check(struct ntfs_journal_unlink *u,
			      struct ntfs_inode *ni)
{
	struct mft_record *m;
	int err = 0;

	if (!u)
		return -EINVAL;
	/*
	 * An object ID means ntfs_delete also removes an $Extend
	 * index entry, which this class cannot describe — the unlink
	 * must proceed unjournaled. A reparse point is journaled
	 * since the reparse class: its $R entry removal is staged
	 * by ntfs_journal_unlink_stage_reparse in the link-count-0
	 * branch. The record scan covers object IDs, which have no
	 * flag to key on. An unreadable record is the same answer: a
	 * record that cannot be read cannot be journaled.
	 */
	m = map_mft_record(ni);
	if (IS_ERR(m))
		return -ENOENT;
	if (find_attr(m, AT_OBJECT_ID, NULL, 0, NULL))
		err = -ENOENT;
	unmap_mft_record(ni);
	return err;
}

/*
 * Find the index entry whose key is the $FILE_NAME value @fn (@fn_len
 * bytes) in @dir_mrec's $INDEX_ROOT. Match by the NAME (bytes and
 * type), not by the whole $FILE_NAME value and not by mref: a hard
 * link in the same directory has the same parent and the same target
 * record, but a different name, and the name is what the $I30 index
 * is ordered by. The rest of the $FILE_NAME is payload, and the two
 * copies of it legitimately disagree: the file's record carries the
 * packed EA size in the union field at offset 60 (0x2d, the packed
 * size of $LXUID/$LXGID/$LXMOD), while the index copy carries the
 * reparse tag there (0x00 for an ordinary file) — see the PORT note
 * in ntfs_create(). A whole-value match therefore misses the entry
 * for every file with EAs, which is every file this driver creates.
 * On success fills @attr_off_out (the $INDEX_ROOT's offset in the
 * record), @entry_off_out (the entry's offset from the attribute
 * record's start — the aoff the log records carry), @len_out and
 * @entry_out (the entry itself, valid while @dir_mrec stays mapped).
 * Returns -ENOENT when the entry is not in the root (it lives in an
 * $INDEX_ALLOCATION block, which this phase cannot describe) and
 * -EINVAL on a corrupt index.
 */
static int find_index_entry_by_key(const struct mft_record *dir_mrec,
				   const struct file_name_attr *fn, u32 fn_len,
				   u16 *attr_off_out, u16 *entry_off_out,
				   u32 *len_out,
				   const struct index_entry **entry_out)
{
	const struct attr_record *a;
	const struct index_root *ir;
	const struct index_header *ih;
	const u8 *value, *entries, *end;
	const struct index_entry *e;
	u16 attr_off = 0;

	a = find_attr(dir_mrec, AT_INDEX_ROOT, I30, 4, &attr_off);
	if (!a || a->non_resident)
		return -ENOENT;
	value = (const u8 *)a +
		le16_to_cpu(a->data.resident.value_offset);
	ir = (const struct index_root *)value;
	ih = &ir->index;
	/* Same bounds as the create index scan: entries_offset and
	 * index_length are measured from the index header start. */
	entries = (const u8 *)ih + le32_to_cpu(ih->entries_offset);
	end = (const u8 *)ih + le32_to_cpu(ih->index_length);
	if (entries < value ||
	    end > value + le32_to_cpu(a->data.resident.value_length))
		return -EINVAL;
	for (e = (const struct index_entry *)entries;
	     (const u8 *)e + 16 <= end;) {
		u16 elen = le16_to_cpu(e->length);

		if (elen < 16 || (const u8 *)e + elen > end)
			return -EINVAL;
		if (e->flags & INDEX_ENTRY_END)
			break;
		/* The name is the key: same length, same type, same
		 * bytes. The name length bounds the memcmp — the value
		 * length is 66 + 2 * name_length, so key_length == fn_len
		 * plus a matching length already bounds both sides. */
		if (le16_to_cpu(e->key_length) == fn_len &&
		    fn_len >= offsetof(struct file_name_attr, file_name) +
				fn->file_name_length * sizeof(__le16) &&
		    ((const struct file_name_attr *)((const u8 *)e + 16))
			    ->file_name_length == fn->file_name_length &&
		    ((const struct file_name_attr *)((const u8 *)e + 16))
			    ->file_name_type == fn->file_name_type &&
		    !memcmp(((const struct file_name_attr *)
			     ((const u8 *)e + 16))->file_name,
			    fn->file_name,
			    fn->file_name_length * sizeof(__le16))) {
			if (attr_off_out)
				*attr_off_out = attr_off;
			if (entry_off_out)
				*entry_off_out = (u16)((const u8 *)e -
						       (const u8 *)a);
			if (len_out)
				*len_out = elen;
			if (entry_out)
				*entry_out = e;
			return 0;
		}
		e = (const struct index_entry *)((const u8 *)e + elen);
	}
	/* Not in the root: the entry lives in an $INDEX_ALLOCATION
	 * block, which this class cannot describe. */
	return -ENOENT;
}

int ntfs_journal_unlink_stage_index(struct ntfs_journal_unlink *u,
				    struct ntfs_inode *dir_ni,
				    const struct file_name_attr *fn,
				    u32 fn_len)
{
	struct mft_record *dir_mrec;
	const struct index_entry *e = NULL;
	u16 attr_off = 0;
	u8 *copy;
	u32 len = 0;
	u64 vcn, page_lcn;
	u16 cluster_off, entry_off = 0;
	int err;

	if (!u)
		return -EINVAL;
	dir_mrec = map_mft_record(dir_ni);
	if (IS_ERR(dir_mrec))
		return -ENOENT;
	err = find_index_entry_by_key(dir_mrec, fn, fn_len, &attr_off,
				      &entry_off, &len, &e);
	if (err)
		goto out;
	copy = kmalloc(len, GFP_NOFS);
	if (!copy) {
		err = -ENOMEM;
		goto out;
	}
	memcpy(copy, e, len);
	err = mft_coords(u->vol, dir_ni->mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (!err)
		err = ntfs_log_txn_delete_index_root(u->t,
						     u->vol->log_mft_data_attr,
						     vcn, cluster_off, page_lcn,
						     attr_off, entry_off,
						     copy, len);
	kfree(copy);
out:
	unmap_mft_record(dir_ni);
	return err;
}

int ntfs_journal_unlink_stage_attr(struct ntfs_journal_unlink *u,
				   struct ntfs_inode *ni,
				   struct ntfs_attr_search_ctx *actx)
{
	const struct attr_record *a;
	u8 *copy;
	u32 len;
	u64 vcn, page_lcn;
	u16 cluster_off, off;
	int err;

	if (!u)
		return -EINVAL;
	/*
	 * The attribute must live in the base record of the inode
	 * being deleted: the delete records address it by its offset
	 * in that record, and an extent record would need the
	 * extent's own coordinates. $FILE_NAME attributes live in
	 * the base record in practice; anything else proceeds
	 * unjournaled.
	 *
	 * The test is actx->ntfs_ino, not actx->base_mrec: the
	 * search context only fills base_ntfs_ino/base_mrec on the
	 * external (attribute-list) path, so for a plain file
	 * without an $ATTRIBUTE_LIST base_mrec stays NULL and a
	 * base_mrec comparison rejects every attribute. The
	 * context's ntfs_ino is the base inode while the search
	 * stays in the base record and switches to the extent inode
	 * when it walks into one, so ntfs_ino == ni is exactly
	 * "found in the base record".
	 */
	if (actx->ntfs_ino != ni)
		return -ENOENT;
	a = actx->attr;
	off = (u16)((const u8 *)a - (const u8 *)actx->mrec);
	len = le32_to_cpu(a->length);
	if (len < 8 || off + len > le32_to_cpu(actx->mrec->bytes_in_use))
		return -EINVAL;
	copy = kmalloc(len, GFP_NOFS);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, a, len);
	err = mft_coords(u->vol, ni->mft_no,
			 &vcn, &cluster_off, &page_lcn);
	if (!err)
		err = ntfs_log_txn_delete_attr(u->t,
					       u->vol->log_mft_data_attr,
					       vcn, cluster_off, page_lcn,
					       off, copy, len);
	kfree(copy);
	return err;
}

/*
 * Stage one record's free: a Deallocate record (redo clears IN_USE
 * and bumps the sequence number, undo restores the record bytes)
 * and a ClearBits record for its $MFT bitmap bit (redo clears, undo
 * sets). @rec is the record bytes as they stand now; @rec_len is
 * their used length.
 */
static int stage_one_free(struct ntfs_journal_unlink *u,
			  struct ntfs_volume *vol, u64 mft_no,
			  const void *rec, u32 rec_len)
{
	u64 vcn, page_lcn, bvcn, bpage_lcn;
	u16 cluster_off, bcluster_off;
	int err;

	err = mft_coords(vol, mft_no, &vcn, &cluster_off, &page_lcn);
	if (err)
		return err;
	err = ntfs_log_txn_deallocate_record(u->t, vol->log_mft_data_attr,
					     vcn, cluster_off, page_lcn,
					     rec, rec_len);
	if (err)
		return err;
	err = bitmap_coords(vol, mft_no, &bvcn, &bcluster_off, &bpage_lcn);
	if (err)
		return err;
	return ntfs_log_txn_set_bitmap(u->t, vol->log_mft_bitmap_attr,
				       bvcn, bcluster_off, bpage_lcn,
				       (u32)mft_no, 1, false);
}

int ntfs_journal_unlink_stage_free(struct ntfs_journal_unlink *u,
				   struct ntfs_inode *ni,
				   const struct mft_record *mrec)
{
	int err, i, n;
	u32 len;

	if (!u || !mrec)
		return -EINVAL;
	/*
	 * The base record first, in its post-delete state: the
	 * $FILE_NAME is already gone and the link count is 0, so the
	 * Deallocate undo (Initialize with these bytes) restores
	 * exactly this state and the DeleteAttribute undo then
	 * re-adds the $FILE_NAME once — the two undos compose
	 * instead of duplicating it. Then the extent records, whose
	 * bytes are final already (the unlink touches only the base
	 * record and the parent's index).
	 */
	len = le32_to_cpu(mrec->bytes_in_use);
	if (len < (u32)sizeof(struct mft_record) ||
	    len > ni->vol->mft_record_size)
		return -EINVAL;
	err = stage_one_free(u, ni->vol, ni->mft_no, mrec, len);
	if (err)
		return err;
	n = ni->nr_extents;
	if (n < 0)
		n = 0;
	for (i = 0; i < n && !err; i++) {
		struct ntfs_inode *ext = ni->ext.extent_ntfs_inos[i];
		struct mft_record *em;
		u32 elen;

		if (!ext)
			continue;
		em = map_mft_record(ext);
		if (IS_ERR(em)) {
			err = PTR_ERR(em);
			break;
		}
		elen = le32_to_cpu(em->bytes_in_use);
		if (elen < (u32)sizeof(struct mft_record) ||
		    elen > ni->vol->mft_record_size)
			err = -EINVAL;
		else
			err = stage_one_free(u, ni->vol, ext->mft_no,
					     em, elen);
		unmap_mft_record(ext);
	}
	return err;
}

/*
 * Stage the $R index entry removal for a reparse point whose link
 * count just hit 0: a DeleteIndexEntryRoot record for the entry in
 * $Reparse's $INDEX_ROOT, found by (tag, file_id). Call from
 * ntfs_delete in the link-count-0 branch, before
 * ntfs_delete_reparse_index removes the entry — the staging reads
 * the live bytes, and the entry is gone afterwards. The tag comes
 * from the file's own record (its $REPARSE_POINT value): ni->flags
 * is the authority for whether the file IS a reparse point, but
 * ni->reparse_tag is only populated by the readlink path, so it
 * cannot be trusted here. The $REPARSE_POINT attribute itself needs
 * no record: the record free staged after this (stage_free) carries
 * the whole post-delete record, and the attribute dies with the
 * record. The commit stamps MR_LSN into $Reparse's record too
 * (u->reparse_mft_no), so the replay skips this record — its delete
 * is offset-based and would otherwise excise whatever bytes sit at
 * that offset after a re-application. Returns -ENOENT when the entry
 * is not in the root (it lives in an $INDEX_ALLOCATION block) or the
 * file carries no $REPARSE_POINT attribute; the caller then aborts
 * and proceeds unjournaled. A file that is not a reparse point
 * stages nothing and returns 0.
 */
int ntfs_journal_unlink_stage_reparse(struct ntfs_journal_unlink *u,
				      struct ntfs_inode *ni)
{
	struct inode *rp_vi;
	struct ntfs_inode *rp_ni;
	struct mft_record *mrec, *rp_mrec;
	const struct attr_record *a;
	const struct index_entry *e = NULL;
	u64 reparse_mft_no, vcn, page_lcn, file_id;
	u16 cluster_off, attr_off = 0, entry_off = 0;
	u16 rp_off = 0;
	__le32 tag;
	u8 *copy;
	u32 len = 0;
	int err;

	if (!u)
		return -EINVAL;
	/* Not a reparse point: ntfs_delete removes no $R entry. */
	if (!(ni->flags & FILE_ATTR_REPARSE_POINT))
		return 0;
	mrec = map_mft_record(ni);
	if (IS_ERR(mrec))
		return -ENOENT;
	a = find_attr(mrec, AT_REPARSE_POINT, NULL, 0, &rp_off);
	if (!a || a->non_resident) {
		err = -ENOENT;
		goto out_unmap;
	}
	/* The tag is the first 4 bytes of the reparse value. */
	if (le16_to_cpu(a->data.resident.value_offset) + sizeof(tag) >
	    le32_to_cpu(a->data.resident.value_length)) {
		err = -ENOENT;
		goto out_unmap;
	}
	tag = *(const __le32 *)((const u8 *)a +
		le16_to_cpu(a->data.resident.value_offset));
	file_id = MK_MREF(ni->mft_no, ni->seq_no);

	err = ntfs_reparse_mft_no(ni->vol, &reparse_mft_no);
	if (err) {
		err = -ENOENT;
		goto out_unmap;
	}
	rp_vi = ntfs_iget(ni->vol->sb, reparse_mft_no);
	if (IS_ERR(rp_vi)) {
		err = -ENOENT;
		goto out_unmap;
	}
	rp_ni = NTFS_I(rp_vi);
	mutex_lock_nested(&rp_ni->mrec_lock, NTFS_EXTEND_MUTEX_PARENT);
	rp_mrec = map_mft_record(rp_ni);
	if (!rp_mrec) {
		err = -ENOENT;
		goto out_unlock;
	}
	err = find_reparse_index_entry(rp_mrec, tag, file_id,
				       &attr_off, &entry_off, &len, &e);
	if (err)
		goto out_rp_unmap;
	copy = kmalloc(len, GFP_NOFS);
	if (!copy) {
		err = -ENOMEM;
		goto out_rp_unmap;
	}
	memcpy(copy, e, len);
	err = mft_coords(ni->vol, reparse_mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (!err)
		err = ntfs_log_txn_delete_index_root(u->t,
						     ni->vol->log_mft_data_attr,
						     vcn, cluster_off, page_lcn,
						     attr_off, entry_off,
						     copy, len);
	kfree(copy);
	if (err)
		goto out_rp_unmap;
	/* The commit stamps this record's MR_LSN, so the replay skips
	 * the $R removal record. */
	u->reparse_mft_no = reparse_mft_no;
out_rp_unmap:
	unmap_mft_record(rp_ni);
out_unlock:
	mutex_unlock(&rp_ni->mrec_lock);
	iput(rp_vi);
out_unmap:
	unmap_mft_record(ni);
	return err;
}

/* ---- rename (Phase 3) ---------------------------------------------------- */

/*
 * Find the $FILE_NAME attribute in the base record @m whose value's
 * file_name matches (@name, @name_len) and whose parent is
 * @parent_mref. find_attr returns the first $FILE_NAME, but a
 * hard-linked inode carries several, and a cross-directory rename
 * to the same name briefly carries two $FILE_NAMEs with the same
 * name and different parents — the new link is the one with this
 * name AND this parent. Returns NULL when no such attribute is in
 * the base record (it went to an extent) or the record is corrupt.
 */
static const struct attr_record *find_file_name_attr(const struct mft_record *m,
						     const __le16 *name, u8 name_len,
						     u64 parent_mref,
						     u16 *off_out)
{
	u16 off = le16_to_cpu(m->attrs_offset);
	u32 used = le32_to_cpu(m->bytes_in_use);

	while (off + 8 <= used) {
		const struct attr_record *a =
			(const struct attr_record *)((const u8 *)m + off);
		u32 len;

		if (a->type == AT_END)
			break;
		len = le32_to_cpu(a->length);
		if (len < 8 || off + len > used)
			return NULL;
		if (a->type == AT_FILE_NAME && !a->non_resident) {
			u16 voff = le16_to_cpu(a->data.resident.value_offset);
			u32 vlen = le32_to_cpu(a->data.resident.value_length);
			const struct file_name_attr *fn;

			if (voff + sizeof(*fn) <= len && vlen >= sizeof(*fn)) {
				fn = (const struct file_name_attr *)
					((const u8 *)a + voff);
				if (MREF_LE(fn->parent_directory) == parent_mref &&
				    fn->file_name_length == name_len &&
				    sizeof(*fn) + name_len * sizeof(__le16) <= vlen &&
				    !memcmp(fn->file_name, name,
					    name_len * sizeof(__le16))) {
					if (off_out)
						*off_out = off;
					return a;
				}
			}
		}
		off += (u16)len;
	}
	return NULL;
}

int ntfs_journal_rename_stage_link(struct ntfs_journal_unlink *u,
				   struct ntfs_inode *ni,
				   struct ntfs_inode *dir_ni,
				   const __le16 *name, u8 name_len)
{
	struct mft_record *ni_mrec, *dir_mrec;
	const struct attr_record *a;
	const struct index_entry *e = NULL;
	const struct file_name_attr *fn;
	u32 fn_len;
	u16 off = 0, attr_off = 0, entry_off = 0;
	u32 len = 0;
	u8 *copy;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	if (!u)
		return -EINVAL;
	/*
	 * The $FILE_NAME half: find the new name's attribute in the
	 * moved record (by value, not first match) and stage a
	 * CreateAttribute carrying its bytes. The INDEXED flag on the
	 * copy is what keys the replay's link-count bump, as in the
	 * create path.
	 */
	ni_mrec = map_mft_record(ni);
	if (IS_ERR(ni_mrec))
		return -ENOENT;
	a = find_file_name_attr(ni_mrec, name, name_len, dir_ni->mft_no,
				&off);
	if (!a) {
		err = -ENOENT;
		goto out_ni;
	}
	fn_len = le32_to_cpu(a->data.resident.value_length);
	fn = (const struct file_name_attr *)((const u8 *)a +
		le16_to_cpu(a->data.resident.value_offset));
	{
		u32 alen = le32_to_cpu(a->length);
		u8 *acopy = kmalloc(alen, GFP_NOFS);

		if (!acopy) {
			err = -ENOMEM;
			goto out_ni;
		}
		memcpy(acopy, a, alen);
		if (!((struct attr_record *)acopy)->non_resident)
			((struct attr_record *)acopy)->data.resident.flags |=
				RESIDENT_ATTR_IS_INDEXED;
		err = mft_coords(u->vol, ni->mft_no, &vcn, &cluster_off,
				 &page_lcn);
		if (!err)
			err = ntfs_log_txn_create_attr(u->t,
						       u->vol->log_mft_data_attr,
						       vcn, cluster_off, page_lcn,
						       off, acopy, alen);
		kfree(acopy);
		if (err)
			goto out_ni;
	}
	/*
	 * The index half: the entry whose key is that $FILE_NAME value,
	 * in the new parent's $INDEX_ROOT. The moved record stays
	 * mapped while the entry is found, so @fn stays valid.
	 */
	dir_mrec = map_mft_record(dir_ni);
	if (IS_ERR(dir_mrec)) {
		err = -ENOENT;
		goto out_ni;
	}
	err = find_index_entry_by_key(dir_mrec, fn, fn_len, &attr_off,
				      &entry_off, &len, &e);
	if (err)
		goto out_dir;
	copy = kmalloc(len, GFP_NOFS);
	if (!copy) {
		err = -ENOMEM;
		goto out_dir;
	}
	memcpy(copy, e, len);
	err = mft_coords(u->vol, dir_ni->mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (!err)
		err = ntfs_log_txn_add_index_root(u->t,
						  u->vol->log_mft_data_attr,
						  vcn, cluster_off, page_lcn,
						  attr_off, entry_off,
						  copy, len);
	kfree(copy);
out_dir:
	unmap_mft_record(dir_ni);
out_ni:
	unmap_mft_record(ni);
	return err;
}

int ntfs_journal_rename_commit(struct ntfs_journal_unlink *u,
			       struct ntfs_inode *old_ni,
			       struct ntfs_inode *old_dir_ni,
			       struct ntfs_inode *new_dir_ni,
			       struct ntfs_inode *new_ni)
{
	uint64_t last_lsn = 0;
	struct ntfs_inode *targets[4];
	int i, err;

	if (!u)
		return -EINVAL;
	err = ntfs_log_txn_commit(u->t, NULL, &last_lsn);
	if (!err) {
		/*
		 * Stamp MR_LSN into every record the rename touched
		 * before the metadata flush, as in the unlink commit:
		 * without the stamp a second replay would re-apply
		 * the records onto records that already carry their
		 * effects. The stamp is the transaction's last LSN,
		 * >= every record that touched any of them. A record
		 * touched twice (old and new parent are the same
		 * directory) is stamped once.
		 */
		targets[0] = old_ni;
		targets[1] = old_dir_ni;
		targets[2] = new_dir_ni;
		targets[3] = new_ni;
		for (i = 0; i < 4 && !err; i++) {
			struct ntfs_inode *ni = targets[i];
			struct mft_record *m;
			int j;

			if (!ni)
				continue;
			for (j = 0; j < i; j++) {
				if (targets[j] == ni)
					break;
			}
			if (j < i)
				continue;
			m = map_mft_record(ni);
			if (IS_ERR(m)) {
				err = PTR_ERR(m);
				break;
			}
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
		/* As in the unlink commit: a reparse point deleted by
		 * this rename (renaming over an existing link, or the
		 * old name's last link) also touched $Reparse's
		 * record. */
		stamp_reparse_mft_lsn(u->vol, u->reparse_mft_no, last_lsn);
	}
	atomic_dec(&u->vol->log_txn_open);
	mutex_unlock(&u->vol->log_txn_lock);
	kfree(u);
	return err;
}

/* ---- truncate/extend (Phase 3) ------------------------------------------ */

struct ntfs_journal_truncate {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	/* The $DATA sizes + mapping pairs as they stood at begin, before
	 * the resize. */
	struct mft_attr_snapshot snap;
	bool snap_ok;
	/* The $STANDARD_INFORMATION flags as they stood at begin, and
	 * where they live in the record. A resize that flips the file
	 * sparse (or back) changes them (FILE_ATTR_SPARSE_FILE) in
	 * step with the $DATA flag; the stage journals the change so
	 * the replay keeps the two in agreement. */
	u32 si_flags_old;
	u16 si_attr_off;
	u16 si_value_off;
	/* The parent $FILE_NAME's allocated size as it stood at begin.
	 * It is NOT snap.alloc: for a sparse/compressed file the
	 * parent carries ni->itype.compressed.size (the real clusters),
	 * not the $DATA's allocated_size (which counts the hole) —
	 * the same rule ntfs_inode_sync_filename applies. The
	 * parent's record in memory lags behind the log (it is only
	 * refreshed at parent writeback), so the stage cannot read
	 * the old value from the entry; it takes it from here. */
	u64 fn_alloc_old;
	/* The run list as it stood at begin: a copy of ni->runlist.rl
	 * up to and including its length-0 terminator. The bitmap delta
	 * is the set difference between this and the live run list. */
	struct runlist_element *old_rl;
	size_t old_count;
	/* Joined into an ambient setattr transaction (begin_in): the
	 * log transaction, the gate and the lock belong to the outer
	 * transaction, so abort/free must not touch them. */
	bool joined;
};

static void free_truncate_snapshot(struct ntfs_journal_truncate *t)
{
	if (!t)
		return;
	free_snapshot(&t->snap);
	kfree(t->old_rl);
	t->old_rl = NULL;
	t->old_count = 0;
}

/*
 * Snapshot the $DATA sizes + pairs, the $SI flags and the run list
 * before a resize mutates them. The run list must be whole: a
 * partially mapped list would miss the tail runs the shrink frees.
 * The snapshot is taken before log_txn_lock — the same window the
 * create begin has for record 0, and the stage re-verifies the
 * attribute offset before staging anything. (For a joined truncate
 * the outer transaction already holds the lock; the snapshot is
 * then taken under it, which is only safer.) On success the
 * carrier's snap_ok is set and its snapshots are owned by it.
 */
static int truncate_snapshot(struct ntfs_volume *vol,
			     struct ntfs_inode *ni,
			     struct ntfs_journal_truncate *t)
{
	struct mft_record *m;
	int err;

	err = ntfs_attr_map_whole_runlist(ni);
	if (err)
		return err;
	m = map_mft_record(ni);
	if (IS_ERR(m))
		return PTR_ERR(m);
	err = snapshot_mft_attr(m, AT_DATA, NULL, 0, &t->snap);
	if (!err) {
		/* Snapshot the $STANDARD_INFORMATION flags too: a
		 * resize that flips the file sparse changes them, and
		 * the stage needs the old value for the undo payload.
		 * The flags live at file_attributes in the $SI value;
		 * the attribute is always resident and always present
		 * on a base record — anything else aborts the
		 * transaction (the resize then proceeds unjournaled). */
		const struct attr_record *si;
		u16 si_off = 0;

		si = find_attr(m, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
		if (!si || si->non_resident ||
		    le16_to_cpu(si->data.resident.value_offset) +
			    offsetof(struct standard_information,
				     file_attributes) + sizeof(__le32) >
			    le32_to_cpu(si->length))
			err = -ENOENT;
		else {
			t->si_attr_off = si_off;
			t->si_value_off =
				le16_to_cpu(si->data.resident.value_offset);
			t->si_flags_old = le32_to_cpu(
				*(const __le32 *)((const u8 *)si +
					t->si_value_off +
					offsetof(struct standard_information,
						 file_attributes)));
		}
	}
	/* The parent's allocated size as it stands: the compressed
	 * size for a sparse/compressed file, the $DATA's allocated
	 * size otherwise — the rule ntfs_inode_sync_filename
	 * applies when it refreshes the parent's $FILE_NAME. */
	if (!err) {
		if (NInoSparse(ni) || NInoCompressed(ni))
			t->fn_alloc_old = ni->itype.compressed.size;
		else
			t->fn_alloc_old = t->snap.alloc;
	}
	unmap_mft_record(ni);
	if (err)
		return err;
	t->snap_ok = true;
	{
		struct runlist_element *rl;
		size_t n = 0;

		down_read(&ni->runlist.lock);
		rl = ni->runlist.rl;
		if (rl) {
			while (rl[n].length)
				n++;
			n++; /* the length-0 terminator */
			t->old_rl = kmalloc_array(n,
						  sizeof(*t->old_rl),
						  GFP_NOFS);
			if (t->old_rl) {
				memcpy(t->old_rl, rl,
				       n * sizeof(*t->old_rl));
				t->old_count = n;
			}
		}
		up_read(&ni->runlist.lock);
		if (!t->old_rl)
			return -ENOMEM;
	}
	return 0;
}

int ntfs_journal_truncate_begin(struct ntfs_volume *vol,
				struct ntfs_inode *ni,
				struct ntfs_journal_truncate **out)
{
	struct ntfs_journal_truncate *t;
	int err;

	*out = NULL;
	if (!vol->log_writer)
		return 0;
	/* Regular files only, non-resident $DATA only. A resident
	 * $DATA, an attribute list (the $DATA may span extent
	 * records), a compressed or encrypted file: unjournaled. */
	if (!S_ISREG(VFS_I(ni)->i_mode) || !NInoNonResident(ni) ||
	    ni->attr_list || NInoCompressed(ni) || NInoEncrypted(ni))
		return 0;
	t = kmalloc(sizeof(*t), GFP_NOFS);
	if (!t)
		return -ENOMEM;
	t->vol = vol;
	t->t = NULL;
	t->snap.pairs = NULL;
	t->snap_ok = false;
	t->old_rl = NULL;
	t->old_count = 0;
	t->joined = false;
	err = truncate_snapshot(vol, ni, t);
	if (err)
		goto out_free;
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &t->t);
	if (err) {
		mutex_unlock(&vol->log_txn_lock);
		goto out_free;
	}
	atomic_inc(&vol->log_txn_open);
	*out = t;
	return 0;

out_free:
	free_truncate_snapshot(t);
	kfree(t);
	return err;
}

/* The setattr carrier. Defined here, ahead of
 * ntfs_journal_truncate_begin_in, which dereferences it; the rest of
 * the setattr class lives at the end of the file. */
struct ntfs_journal_setattr {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	/* The $STANDARD_INFORMATION's first 36 bytes (four times +
	 * file_attributes) as they stood at begin, built from in-memory
	 * vi/ni state — never from the mapped record, which lags behind
	 * the log. */
	u8 si_old[36];
	u16 si_attr_off;
	u16 si_value_off;
	u32 si_flags_old;	/* CPU order, from si_old's flags */
	/* The $DATA sizes as they stood at begin, for the $FILE_NAME
	 * old dup — the same rule ntfs_inode_sync_filename applies
	 * (0 for a directory, compressed.size for sparse/compressed,
	 * allocated_size otherwise). */
	u64 fn_alloc_old;
	u64 fn_data_old;
	/* The $EA_INFORMATION as it stood at begin: its 8-byte value.
	 * Always resident (8 bytes); anything else is a begin failure. */
	bool ea_info_present;
	u8 ea_info_old[8];
	/* The $EA as it stood at begin: a copy of its value. A
	 * non-resident $EA means the file never begins (this class
	 * cannot describe it); a length change at stage aborts. */
	bool ea_present;
	u16 ea_value_len;
	u8 *ea_old;
	/* The joined truncate carrier, or NULL when there was no size
	 * change or the resize is unjournaled. */
	struct ntfs_journal_truncate *jt;
	/* The joined truncate's begin or stage failed: the resize is
	 * unjournaled, so this transaction must not commit either. */
	bool truncate_failed;
};

int ntfs_journal_truncate_begin_in(struct ntfs_journal_setattr *st,
				   struct ntfs_inode *ni,
				   struct ntfs_journal_truncate **out)
{
	struct ntfs_journal_truncate *t;
	int err;

	*out = NULL;
	if (!st)
		return -EINVAL;
	/* The same journalability gate as the standalone begin: the
	 * setattr transaction is already open, so an unjournalable
	 * resize just means the truncate half is unlogged — the
	 * caller marks the outer transaction unjournaled too. */
	if (!S_ISREG(VFS_I(ni)->i_mode) || !NInoNonResident(ni) ||
	    ni->attr_list || NInoCompressed(ni) || NInoEncrypted(ni))
		return 0;
	t = kmalloc(sizeof(*t), GFP_NOFS);
	if (!t)
		return -ENOMEM;
	t->vol = st->vol;
	t->t = st->t;
	t->snap.pairs = NULL;
	t->snap_ok = false;
	t->old_rl = NULL;
	t->old_count = 0;
	t->joined = true;
	err = truncate_snapshot(st->vol, ni, t);
	if (err) {
		free_truncate_snapshot(t);
		kfree(t);
		return err;
	}
	*out = t;
	return 0;
}

void ntfs_journal_truncate_join_free(struct ntfs_journal_truncate *t)
{
	if (!t)
		return;
	/* The staged records stay in the outer transaction; only the
	 * snapshots are spent. Never touch the log transaction, the
	 * gate or the lock here — they belong to the outer
	 * transaction. */
	free_truncate_snapshot(t);
	kfree(t);
}

void ntfs_journal_truncate_abort(struct ntfs_journal_truncate *t)
{
	if (!t)
		return;
	if (!t->joined) {
		(void)ntfs_log_txn_abort(t->t);
		atomic_dec(&t->vol->log_txn_open);
		mutex_unlock(&t->vol->log_txn_lock);
	}
	/* A joined carrier's log transaction belongs to the outer
	 * setattr transaction, which aborts it; this only frees the
	 * snapshots. The join path uses join_free for the same
	 * reason; abort is joined-aware so a stray call cannot
	 * discard the outer transaction's records. */
	free_truncate_snapshot(t);
	kfree(t);
}

/*
 * Interval set difference: the LCN ranges in @a that are not covered
 * by any range in @b. Both lists are arrays of runlist_elements
 * terminated by length 0; only lcn >= 0 elements count (holes are
 * not clusters). The result is written into @out (at most @out_max
 * entries, each {vcn = start LCN, length = run length}); adjacent
 * results are merged. Returns the number of result entries, or
 * -ENOSPC when @out is too small.
 */
struct lcn_range {
	u64 lcn;
	u64 len;
};

static int lcn_ranges_sub(const struct runlist_element *a,
			  const struct runlist_element *b,
			  struct lcn_range *out, size_t out_max)
{
	size_t n = 0, i, j;

	for (i = 0; a[i].length; i++) {
		u64 start, end, cur;
		size_t k;

		if (a[i].lcn < 0)
			continue;
		start = (u64)a[i].lcn;
		end = start + (u64)a[i].length;
		/* Subtract every b interval from [start, end). */
		for (j = 0; b[j].length; j++) {
			u64 bs, be;

			if (b[j].lcn < 0)
				continue;
			bs = (u64)b[j].lcn;
			be = bs + (u64)b[j].length;
			if (be <= start || bs >= end)
				continue;
			/* [start, end) minus [bs, be): the left part
			 * survives, the right part is re-examined
			 * against the remaining b intervals. */
			if (bs > start) {
				if (n >= out_max)
					return -ENOSPC;
				out[n].lcn = start;
				out[n].len = bs - start;
				n++;
			}
			if (be >= end) {
				start = end;
				break;
			}
			start = be;
		}
		if (start < end) {
			if (n >= out_max)
				return -ENOSPC;
			out[n].lcn = start;
			out[n].len = end - start;
			n++;
		}
		/* Merge with the previous result when adjacent. */
		for (k = 1; k < n; k++) {
			if (out[k - 1].lcn + out[k - 1].len == out[k].lcn) {
				out[k - 1].len += out[k].len;
				memmove(&out[k], &out[k + 1],
					(n - k - 1) * sizeof(*out));
				n--;
				k--;
			}
		}
	}
	return (int)n;
}

/*
 * Stage one $Bitmap range as SetBits (set = true) or ClearBits
 * (set = false) records, split at bitmap-page boundaries: one
 * record addresses one page, and a range crossing into the next
 * page would stage bits the replay applies to the wrong page.
 * A bitmap page holds cluster_size * 8 bits. The txn-taking form
 * is shared by the truncate class and the xattr class (which
 * stages the clusters a named $DATA stream allocates or frees).
 */
static int stage_bitmap_range_txn(struct ntfs_volume *vol,
				  ntfs_log_txn_t *txn,
				  u64 bit, u64 nbits, bool set)
{
	u64 page_bits = (u64)vol->cluster_size * 8;
	int err = 0;

	while (nbits && !err) {
		u64 page_end = (bit / page_bits + 1) * page_bits;
		u64 chunk = page_end - bit;
		u64 vcn, page_lcn;
		u16 cluster_off;

		if (chunk > nbits)
			chunk = nbits;
		err = cluster_bitmap_coords(vol, bit,
					    &vcn, &cluster_off, &page_lcn);
		if (err)
			break;
		err = ntfs_log_txn_set_bitmap(txn,
					      vol->log_lcn_bitmap_attr,
					      vcn, cluster_off, page_lcn,
					      (u32)bit, (u32)chunk, set);
		bit += chunk;
		nbits -= chunk;
	}
	return err;
}

static int stage_bitmap_range(struct ntfs_journal_truncate *t,
			      u64 bit, u64 nbits, bool set)
{
	return stage_bitmap_range_txn(t->vol, t->t, bit, nbits, set);
}

/*
 * Stage one parent's $FILE_NAME dup update. The parent's index entry
 * duplicates the file's allocated/data sizes and flags, and the live
 * driver refreshes them at parent writeback
 * (ntfs_inode_sync_filename) — after the commit, so a crash between
 * the commit and the parent's writeback leaves the entry stale and
 * ntfsck flags the file ("Allocated size is different with
 * IDX/$FN(...), MFT/$DATA(...)"). Staging the update here makes the
 * replay apply it with the transaction (redo) or restore it (undo),
 * so the two agree whatever instant the crash hits.
 *
 * The entry is found by key (the whole $FILE_NAME value) in the
 * parent's $INDEX_ROOT, via ntfs_iget — the same route
 * ntfs_inode_sync_filename takes, and the same lock order (the
 * file's mrec_lock is held by the caller; the parent's is taken
 * nested, NTFS_INODE_MUTEX_PARENT). When the entry lives in an
 * $INDEX_ALLOCATION block, or the parent cannot be reached, this
 * returns -ENOENT and the whole transaction aborts to unjournaled.
 * The record is staged only when the duplicated bytes actually
 * change. Sets *staged_out when a record is staged.
 *
 * The old dup takes its times (and the ea/reparse byte) from the
 * entry — but NOT its old sizes/flags: the parent's record in memory
 * lags behind the log, because it is only refreshed at parent
 * writeback, so the entry still holds the values from before the
 * previous transaction. The old payload therefore takes its
 * sizes/flags from the caller-supplied begin snapshot, and the new
 * payload from the caller-supplied current state — the same rule
 * ntfs_inode_sync_filename applies. Getting the old value from the
 * entry would make an undo restore the pre-previous state: a
 * committed shrink followed by a loser's extend would leave the
 * entry at the pre-shrink size. @flags_old/@flags_new are CPU-order
 * (le32_to_cpu(ni->flags)); only the FILE_ATTR_VALID_FLAGS bits are
 * taken from them, the rest of the entry's flags are preserved.
 */
static int stage_filename_dup(struct ntfs_volume *vol, ntfs_log_txn_t *t,
			      struct ntfs_inode *ni,
			      const struct attr_record *fn_attr,
			      u64 fn_alloc_old, u64 fn_data_old,
			      u32 flags_old,
			      u64 fn_alloc_new, u64 fn_data_new,
			      u32 flags_new,
			      bool *staged_out)
{
	const struct file_name_attr *fn;
	u32 fn_len, vlen;
	u64 parent;
	struct inode *index_vi;
	struct ntfs_inode *index_ni;
	struct mft_record *dir_mrec;
	const struct index_entry *e = NULL;
	u16 attr_off = 0, entry_off = 0;
	u32 len = 0;
	u64 vcn, page_lcn;
	u16 cluster_off;
	u8 old_dup[FN_DUP_SIZE], new_dup[FN_DUP_SIZE];
	u32 old_flags, new_flags;
	__le64 v64;
	__le32 v32;
	int err;

	/* The duplicated fields live at FN_DUP_OFFSET in the $FILE_NAME
	 * value: times, then allocated_size, data_size, flags. The
	 * offsets below are relative to the dup payload start. */
	const u16 dup_alloc = offsetof(struct file_name_attr,
				       allocated_size) - FN_DUP_OFFSET;
	const u16 dup_data = offsetof(struct file_name_attr,
				      data_size) - FN_DUP_OFFSET;
	const u16 dup_flags = offsetof(struct file_name_attr,
				       file_attributes) - FN_DUP_OFFSET;

	fn_len = le32_to_cpu(fn_attr->data.resident.value_length);
	vlen = le16_to_cpu(fn_attr->data.resident.value_offset) + fn_len;
	if (fn_attr->non_resident || fn_len < FN_DUP_OFFSET + FN_DUP_SIZE ||
	    vlen > le32_to_cpu(fn_attr->length))
		return -ENOENT;
	fn = (const struct file_name_attr *)((const u8 *)fn_attr +
		le16_to_cpu(fn_attr->data.resident.value_offset));
	parent = MREF_LE(fn->parent_directory);
	if (parent == ni->mft_no)
		return 0;	/* self-parented ("."), no index entry */
	if (!vol->sb)
		return -ENOENT;
	index_vi = ntfs_iget(vol->sb, parent);
	if (IS_ERR(index_vi))
		return -ENOENT;
	index_ni = NTFS_I(index_vi);
	mutex_lock_nested(&index_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT);
	dir_mrec = map_mft_record(index_ni);
	if (IS_ERR(dir_mrec)) {
		err = -ENOENT;
		goto out_unlock;
	}
	err = find_index_entry_by_key(dir_mrec, fn, fn_len, &attr_off,
				       &entry_off, &len, &e);
	if (err)
		goto out_unmap;
	/* The entry's dup bytes carry the times (unchanged by a
	 * truncate or a setattr — the live driver never updates
	 * $FILE_NAME times after create/link) and the ea/reparse
	 * byte; only the sizes/flags come from the caller. */
	memcpy(old_dup, (const u8 *)e + IE_KEY + FN_DUP_OFFSET,
	       FN_DUP_SIZE);
	memcpy(new_dup, old_dup, FN_DUP_SIZE);
	/* Old: the snapshot's sizes and flags. */
	v64 = cpu_to_le64(fn_alloc_old);
	memcpy(old_dup + dup_alloc, &v64, sizeof(v64));
	v64 = cpu_to_le64(fn_data_old);
	memcpy(old_dup + dup_data, &v64, sizeof(v64));
	memcpy(&v32, old_dup + dup_flags, sizeof(v32));
	old_flags = le32_to_cpu(v32);
	new_flags = (old_flags & ~le32_to_cpu(FILE_ATTR_VALID_FLAGS)) |
		    (flags_old & le32_to_cpu(FILE_ATTR_VALID_FLAGS));
	v32 = cpu_to_le32(new_flags);
	memcpy(old_dup + dup_flags, &v32, sizeof(v32));
	/* New: the caller's current sizes and flags. */
	v64 = cpu_to_le64(fn_alloc_new);
	memcpy(new_dup + dup_alloc, &v64, sizeof(v64));
	v64 = cpu_to_le64(fn_data_new);
	memcpy(new_dup + dup_data, &v64, sizeof(v64));
	memcpy(&v32, new_dup + dup_flags, sizeof(v32));
	old_flags = le32_to_cpu(v32);
	new_flags = (old_flags & ~le32_to_cpu(FILE_ATTR_VALID_FLAGS)) |
		    (flags_new & le32_to_cpu(FILE_ATTR_VALID_FLAGS));
	v32 = cpu_to_le32(new_flags);
	memcpy(new_dup + dup_flags, &v32, sizeof(v32));
	if (!memcmp(new_dup, old_dup, FN_DUP_SIZE)) {
		err = 0;
		goto out_unmap;	/* nothing changed, no record needed */
	}
	err = mft_coords(vol, parent, &vcn, &cluster_off, &page_lcn);
	if (!err)
		err = ntfs_log_txn_update_filename_root(
			t, vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			attr_off, entry_off, new_dup, old_dup);
	if (!err && staged_out)
		*staged_out = true;
out_unmap:
	unmap_mft_record(index_ni);
out_unlock:
	mutex_unlock(&index_ni->mrec_lock);
	iput(index_vi);
	return err;
}

/*
 * Stage one parent's $FILE_NAME update for a truncate: the truncate
 * class's values for the shared helper. The new allocated size is
 * the compressed size for a sparse/compressed file — the $DATA's
 * allocated_size counts the hole — and the new data size comes from
 * the mapped $DATA attribute, as before.
 */
static int stage_truncate_filename_one(struct ntfs_journal_truncate *t,
				       struct ntfs_inode *ni,
				       const struct attr_record *fn_attr,
				       const struct attr_record *data_attr)
{
	u64 fn_alloc_new, fn_data_new;
	u32 flags_new;

	if (NInoSparse(ni) || NInoCompressed(ni))
		fn_alloc_new = ni->itype.compressed.size;
	else
		fn_alloc_new = le64_to_cpu(
			data_attr->data.non_resident.allocated_size);
	fn_data_new = le64_to_cpu(data_attr->data.non_resident.data_size);
	flags_new = le32_to_cpu(ni->flags);
	return stage_filename_dup(t->vol, t->t, ni, fn_attr,
				  t->fn_alloc_old, t->snap.data,
				  t->si_flags_old,
				  fn_alloc_new, fn_data_new, flags_new,
				  NULL);
}

/*
 * Stage the $FILE_NAME updates for a truncate: one UpdateFileNameRoot
 * per $FILE_NAME in the file's record (hard links update every
 * parent). The scan mirrors find_attr's bounds — attributes are
 * contiguous from attrs_offset to bytes_in_use, ending at AT_END.
 */
static int stage_truncate_filename(struct ntfs_journal_truncate *t,
				   struct ntfs_inode *ni,
				   const struct mft_record *mrec,
				   const struct attr_record *data_attr)
{
	u16 off = le16_to_cpu(mrec->attrs_offset);
	u32 used = le32_to_cpu(mrec->bytes_in_use);
	int err = 0;

	while (off + 8 <= used) {
		const struct attr_record *at =
			(const struct attr_record *)((const u8 *)mrec + off);
		u32 len;

		if (at->type == AT_END)
			break;
		len = le32_to_cpu(at->length);
		if (len < 8 || off + len > used)
			return -EINVAL;
		if (at->type == AT_FILE_NAME) {
			err = stage_truncate_filename_one(t, ni, at,
							  data_attr);
			if (err)
				return err;
		}
		off += len;
	}
	return err;
}

int ntfs_journal_truncate_stage(struct ntfs_journal_truncate *t,
				struct ntfs_inode *ni,
				const struct mft_record *mrec)
{
	const struct attr_record *a;
	u16 off = 0, pairs_off;
	u32 len;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	if (!t || !t->snap_ok)
		return -ENOENT;
	a = find_attr(mrec, AT_DATA, NULL, 0, &off);
	if (!a || !a->non_resident)
		return -ENOENT;
	len = le32_to_cpu(a->length);
	pairs_off = le16_to_cpu(a->data.non_resident.mapping_pairs_offset);
	/* The attribute itself must not have moved; the header fields
	 * may legitimately flip. An extend by a hole flips the file
	 * sparse, which inserts the 8-byte compressed_size field and
	 * moves name_offset and mapping_pairs_offset by 8 (and sets
	 * ATTR_IS_SPARSE) — the pairs record carries the old and new
	 * header, so that flip is journaled, not a reason to abort. */
	if (off != t->snap.attr_off)
		return -ENOENT;
	if (pairs_off >= len)
		return -EIO;
	err = mft_coords(t->vol, ni->mft_no,
			 &vcn, &cluster_off, &page_lcn);
	if (err)
		return err;

	/* The $Bitmap delta first: the clusters the resize freed or
	 * allocated, from the set difference of the run lists. */
	{
		struct runlist_element *new_rl = NULL;
		size_t new_count = 0, i;
		struct lcn_range *freed = NULL, *allocd = NULL;
		int n_freed, n_allocd;

		down_read(&ni->runlist.lock);
		if (ni->runlist.rl) {
			size_t n = 0;

			while (ni->runlist.rl[n].length)
				n++;
			n++;
			new_rl = kmalloc_array(n, sizeof(*new_rl),
					       GFP_NOFS);
			if (new_rl) {
				memcpy(new_rl, ni->runlist.rl,
				       n * sizeof(*new_rl));
				new_count = n;
			}
		}
		up_read(&ni->runlist.lock);
		if (!new_rl) {
			err = -ENOMEM;
			goto out;
		}
		/* Upper bound: one result range per input run. */
		freed = kmalloc_array(t->old_count + new_count,
				      sizeof(*freed), GFP_NOFS);
		allocd = kmalloc_array(t->old_count + new_count,
				       sizeof(*allocd), GFP_NOFS);
		if (!freed || !allocd) {
			err = -ENOMEM;
			goto out_rl;
		}
		n_freed = lcn_ranges_sub(t->old_rl, new_rl,
					 freed, t->old_count + new_count);
		n_allocd = lcn_ranges_sub(new_rl, t->old_rl,
					  allocd, t->old_count + new_count);
		if (n_freed < 0 || n_allocd < 0) {
			err = -ENOSPC;
			goto out_rl;
		}
		for (i = 0; i < (size_t)n_freed && !err; i++)
			err = stage_bitmap_range(t, freed[i].lcn,
						 freed[i].len, false);
		for (i = 0; i < (size_t)n_allocd && !err; i++)
			err = stage_bitmap_range(t, allocd[i].lcn,
						 allocd[i].len, true);
out_rl:
		kfree(freed);
		kfree(allocd);
		kfree(new_rl);
		if (err)
			goto out;
	}

	/* The mapping pairs next, when they changed. */
	if (len - pairs_off != t->snap.pairs_len ||
	    memcmp((const u8 *)a + pairs_off, t->snap.pairs,
		   t->snap.pairs_len) != 0) {
		err = ntfs_log_txn_update_mapping_pairs(
			t->t, t->vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			off, pairs_off,
			le16_to_cpu(a->flags), le16_to_cpu(a->name_offset),
			(const u8 *)a + pairs_off, (u16)(len - pairs_off),
			t->snap.flags, t->snap.name_off, t->snap.pairs_off,
			t->snap.pairs, t->snap.pairs_len);
		if (err)
			goto out;
	}

	/* The sizes last, when they changed. */
	{
		u64 alloc = le64_to_cpu(a->data.non_resident.allocated_size);
		u64 valid = le64_to_cpu(a->data.non_resident.initialized_size);
		u64 data = le64_to_cpu(a->data.non_resident.data_size);
		u64 total = pairs_off >= 0x48 ?
			le64_to_cpu(a->data.non_resident.compressed_size) : 0;

		if (alloc != t->snap.alloc || valid != t->snap.valid ||
		    data != t->snap.data || total != t->snap.total) {
			err = ntfs_log_txn_set_attr_sizes(
				t->t, t->vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				off, alloc, valid, data, total,
				t->snap.alloc, t->snap.valid,
				t->snap.data, t->snap.total);
			if (err)
				goto out;
		}
	}

	/* The $STANDARD_INFORMATION flags, when the resize flipped
	 * them. The live driver writes ni->flags into the $SI at
	 * inode sync, after this commit; the $DATA flag flips here,
	 * in the mapped record, during the resize. Without a record
	 * the replay leaves the $DATA sparse and the $SI not, and
	 * libntfs-3g refuses the attribute ("corrupt attribute
	 * flags (0x8000 <> 0x0)"). Stage only on change: a plain
	 * resize leaves the flags alone. The $SI attribute must
	 * still sit where begin found it; anything else aborts. */
	if (le32_to_cpu(ni->flags) != t->si_flags_old) {
		const struct attr_record *si;
		u16 si_off = 0;
		__le32 new_flags = ni->flags;
		__le32 old_flags = cpu_to_le32(t->si_flags_old);

		si = find_attr(mrec, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
		if (!si || si->non_resident || si_off != t->si_attr_off ||
		    le16_to_cpu(si->data.resident.value_offset) !=
			    t->si_value_off) {
			err = -ENOENT;
			goto out;
		}
		err = ntfs_log_txn_update_resident(
			t->t, t->vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			si_off,
			t->si_value_off +
				offsetof(struct standard_information,
					 file_attributes),
			&new_flags, sizeof(new_flags),
			&old_flags, sizeof(old_flags));
		if (err)
			goto out;
	}

	/* The parent's $FILE_NAME, last: its duplicated sizes and flags
	 * must agree with the resized $DATA after the replay, whatever
	 * instant the crash hits. A failure here aborts the whole
	 * transaction (the resize then proceeds unjournaled). */
	err = stage_truncate_filename(t, ni, mrec, a);
	if (err)
		goto out;

out:
	return err;
}

int ntfs_journal_truncate_commit(struct ntfs_journal_truncate *t,
				 struct ntfs_inode *ni)
{
	uint64_t last_lsn = 0;
	struct mft_record *m;
	int err;

	if (!t)
		return -EINVAL;
	err = ntfs_log_txn_commit(t->t, NULL, &last_lsn);
	if (!err) {
		/* Stamp MR_LSN into the file's record before the
		 * metadata flush, as in the other commits: without
		 * the stamp a second replay would re-apply the
		 * sizes/pairs records onto a record that already
		 * carries their effects. */
		m = map_mft_record(ni);
		if (IS_ERR(m))
			err = PTR_ERR(m);
		else {
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
	}
	atomic_dec(&t->vol->log_txn_open);
	mutex_unlock(&t->vol->log_txn_lock);
	free_truncate_snapshot(t);
	kfree(t);
	return err;
}

/* ---- setattr (Phase 3) -------------------------------------------------- */

static void free_setattr_snapshot(struct ntfs_journal_setattr *st)
{
	if (!st)
		return;
	kfree(st->ea_old);
	st->ea_old = NULL;
}

/*
 * Build the $STANDARD_INFORMATION's first 36 bytes (four times +
 * file_attributes) from in-memory state, exactly as
 * ntfs_inode_sync_standard_information writes them at inode sync:
 * creation_time from ni->i_crtime, mtime/ctime/atime from the
 * inode, flags from ni->flags. All little-endian, as stored.
 */
static void si_build(struct inode *vi, struct ntfs_inode *ni, u8 out[36])
{
	__le64 v64;
	__le32 v32;

	v64 = utc2ntfs(ni->i_crtime);
	memcpy(out + 0, &v64, sizeof(v64));
	v64 = utc2ntfs(inode_get_mtime(vi));
	memcpy(out + 8, &v64, sizeof(v64));
	v64 = utc2ntfs(inode_get_ctime(vi));
	memcpy(out + 16, &v64, sizeof(v64));
	v64 = utc2ntfs(inode_get_atime(vi));
	memcpy(out + 24, &v64, sizeof(v64));
	v32 = ni->flags;
	memcpy(out + 32, &v32, sizeof(v32));
}

int ntfs_journal_setattr_begin(struct ntfs_volume *vol,
			       struct ntfs_inode *ni,
			       struct ntfs_journal_setattr **out)
{
	struct ntfs_journal_setattr *st;
	struct mft_record *m;
	struct inode *vi = VFS_I(ni);
	int err;

	*out = NULL;
	if (!vol->log_writer)
		return 0;
	/* Regular files and directories only, the create class's
	 * scope. Symlinks and special files proceed unjournaled —
	 * their reparse staging belongs to their own classes. */
	if (!S_ISREG(vi->i_mode) && !S_ISDIR(vi->i_mode))
		return 0;
	st = kmalloc(sizeof(*st), GFP_NOFS);
	if (!st)
		return -ENOMEM;
	st->vol = vol;
	st->t = NULL;
	st->si_attr_off = 0;
	st->si_value_off = 0;
	st->si_flags_old = 0;
	st->fn_alloc_old = 0;
	st->fn_data_old = 0;
	st->ea_info_present = false;
	st->ea_present = false;
	st->ea_value_len = 0;
	st->ea_old = NULL;
	st->jt = NULL;
	st->truncate_failed = false;
	/* The $SI snapshot is in-memory state: the mapped record
	 * lags behind the log, and a mapped snapshot's undo would
	 * restore the pre-previous state. */
	si_build(vi, ni, st->si_old);
	st->si_flags_old = le32_to_cpu(ni->flags);
	/* The $FILE_NAME old sizes, the sync_filename rule. */
	if (S_ISDIR(vi->i_mode)) {
		st->fn_alloc_old = 0;
		st->fn_data_old = 0;
	} else {
		unsigned long flags;

		read_lock_irqsave(&ni->size_lock, flags);
		if (NInoSparse(ni) || NInoCompressed(ni))
			st->fn_alloc_old = ni->itype.compressed.size;
		else
			st->fn_alloc_old = ni->allocated_size;
		st->fn_data_old = ni->data_size;
		read_unlock_irqrestore(&ni->size_lock, flags);
	}
	/* The $SI offsets and the $EA pair, from the mapped record.
	 * A missing or too-short $SI is a begin failure (the resize
	 * then proceeds unjournaled); a non-resident $EA means the
	 * file never begins (this class cannot describe it). */
	m = map_mft_record(ni);
	if (IS_ERR(m)) {
		err = PTR_ERR(m);
		goto out_free;
	}
	{
		const struct attr_record *si;
		u16 si_off = 0;

		si = find_attr(m, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
		if (!si || si->non_resident ||
		    le16_to_cpu(si->data.resident.value_offset) + 36 >
			    le32_to_cpu(si->length)) {
			err = -ENOENT;
			goto out_unmap;
		}
		st->si_attr_off = si_off;
		st->si_value_off =
			le16_to_cpu(si->data.resident.value_offset);
	}
	{
		const struct attr_record *ei, *ea;
		u16 ei_off = 0, ea_off = 0;

		ei = find_attr(m, AT_EA_INFORMATION, NULL, 0, &ei_off);
		if (ei) {
			u16 vlen;

			if (ei->non_resident ||
			    le16_to_cpu(ei->data.resident.value_length) !=
				    sizeof(struct ea_information)) {
				err = -ENOENT;
				goto out_unmap;
			}
			vlen = le16_to_cpu(ei->data.resident.value_offset);
			memcpy(st->ea_info_old,
			       (const u8 *)ei + vlen, sizeof(st->ea_info_old));
			st->ea_info_present = true;
		}
		ea = find_attr(m, AT_EA, NULL, 0, &ea_off);
		if (ea) {
			u16 vlen;

			if (ea->non_resident) {
				err = 0;
				goto out_nonresident;
			}
			vlen = le16_to_cpu(ea->data.resident.value_length);
			st->ea_old = kmalloc(vlen, GFP_NOFS);
			if (!st->ea_old) {
				err = -ENOMEM;
				goto out_unmap;
			}
			memcpy(st->ea_old,
			       (const u8 *)ea +
				       le16_to_cpu(ea->data.resident.value_offset),
			       vlen);
			st->ea_value_len = vlen;
			st->ea_present = true;
		}
	}
	unmap_mft_record(ni);
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &st->t);
	if (err) {
		mutex_unlock(&vol->log_txn_lock);
		goto out_free;
	}
	atomic_inc(&vol->log_txn_open);
	*out = st;
	return 0;

out_nonresident:
	/* A non-resident $EA: this class cannot describe the file.
	 * Return 0 with *out NULL — proceed unjournaled, by design,
	 * not a failure. */
	unmap_mft_record(ni);
	free_setattr_snapshot(st);
	kfree(st);
	*out = NULL;
	return 0;
out_unmap:
	unmap_mft_record(ni);
out_free:
	free_setattr_snapshot(st);
	kfree(st);
	return err;
}

void ntfs_journal_setattr_abort(struct ntfs_journal_setattr *st)
{
	if (!st)
		return;
	(void)ntfs_log_txn_abort(st->t);
	atomic_dec(&st->vol->log_txn_open);
	mutex_unlock(&st->vol->log_txn_lock);
	free_setattr_snapshot(st);
	kfree(st);
}

void ntfs_journal_setattr_mark_truncate_unjournaled(
	struct ntfs_journal_setattr *st)
{
	if (st)
		st->truncate_failed = true;
}

/*
 * Stage one $STANDARD_INFORMATION record: the 36 bytes built from
 * current in-memory state against a begin snapshot. The $SI must
 * still sit where begin found it; anything else aborts. Stages
 * only on change. Shared by the setattr class (which stages its
 * own snapshot) and the xattr class.
 */
static int stage_si_36(struct ntfs_volume *vol, ntfs_log_txn_t *t,
		       const u8 si_old[36], u16 si_attr_off,
		       u16 si_value_off, struct ntfs_inode *ni,
		       const struct mft_record *mrec,
		       u64 vcn, u16 cluster_off, u64 page_lcn)
{
	u8 si_new[36];
	const struct attr_record *si;
	u16 si_off = 0;

	si_build(VFS_I(ni), ni, si_new);
	if (!memcmp(si_new, si_old, sizeof(si_new)))
		return 0;
	si = find_attr(mrec, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
	if (!si || si->non_resident || si_off != si_attr_off ||
	    le16_to_cpu(si->data.resident.value_offset) !=
		    si_value_off)
		return -ENOENT;
	return ntfs_log_txn_update_resident(
		t, vol->log_mft_data_attr,
		vcn, cluster_off, page_lcn,
		si_off, si_value_off,
		si_new, sizeof(si_new), si_old, 36);
}

static int stage_setattr_si(struct ntfs_journal_setattr *st,
			    struct ntfs_inode *ni,
			    const struct mft_record *mrec,
			    u64 vcn, u16 cluster_off, u64 page_lcn)
{
	return stage_si_36(st->vol, st->t, st->si_old, st->si_attr_off,
			   st->si_value_off, ni, mrec,
			   vcn, cluster_off, page_lcn);
}

/*
 * Stage the $EA_INFORMATION and $EA records, net effect. The values
 * are re-read from the mapped record at stage time — resident $EA
 * writes land straight in the mapped record, so the stage sees the
 * final bytes. Cases:
 *
 *   - absent at begin, present at stage: CreateAttribute for the
 *     pair (the create_ea pattern), $EA_INFORMATION first;
 *   - present at begin, absent at stage: -ENOENT (nothing in a
 *     setattr removes the pair; refusing to guess);
 *   - present both times: UpdateResidentValue for each value that
 *     changed, whole value. A length change aborts (-ENOENT): the
 *     UpdateResidentValue resize path is unproven here, and adding
 *     a $LX* entry to a file that already has other EAs grows the
 *     $EA value. A non-resident $EA at stage aborts the same way.
 *     An attribute that moved is fine — the current offset is
 *     staged, and the replay applies the transaction's records in
 *     order (the $DATA resize records come first and move it to
 *     exactly that offset).
 */
static int stage_setattr_ea(struct ntfs_journal_setattr *st,
			    struct ntfs_inode *ni,
			    const struct mft_record *mrec,
			    u64 vcn, u16 cluster_off, u64 page_lcn)
{
	const struct attr_record *ei, *ea;
	u16 ei_off = 0, ea_off = 0;
	int err;

	ei = find_attr(mrec, AT_EA_INFORMATION, NULL, 0, &ei_off);
	ea = find_attr(mrec, AT_EA, NULL, 0, &ea_off);
	if (!st->ea_info_present && !st->ea_present) {
		if (!ei && !ea)
			return 0;
		if (!ei || !ea)
			return -ENOENT;
		/* The pair was created by this setattr: stage both
		 * as CreateAttributes, $EA_INFORMATION first. */
		err = stage_create_attr(st->t, st->vol, ni, mrec,
					AT_EA_INFORMATION, NULL, 0, false);
		if (err)
			return err;
		return stage_create_attr(st->t, st->vol, ni, mrec,
					 AT_EA, NULL, 0, false);
	}
	if (!ei || !ea)
		return -ENOENT;
	if (ei->non_resident || ea->non_resident)
		return -ENOENT;
	if (le16_to_cpu(ea->data.resident.value_length) !=
	    st->ea_value_len)
		return -ENOENT;
	/* $EA_INFORMATION first, then $EA — the create order. Its
	 * value is always 8 bytes; anything else is a record this
	 * class cannot describe. */
	if (le16_to_cpu(ei->data.resident.value_length) !=
	    sizeof(struct ea_information))
		return -ENOENT;
	{
		const u8 *new_info = (const u8 *)ei +
			le16_to_cpu(ei->data.resident.value_offset);

		if (memcmp(new_info, st->ea_info_old,
			   sizeof(st->ea_info_old)) != 0) {
			err = ntfs_log_txn_update_resident(
				st->t, st->vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				ei_off,
				le16_to_cpu(ei->data.resident.value_offset),
				new_info, sizeof(struct ea_information),
				st->ea_info_old,
				sizeof(struct ea_information));
			if (err)
				return err;
		}
	}
	{
		const u8 *new_ea = (const u8 *)ea +
			le16_to_cpu(ea->data.resident.value_offset);

		if (memcmp(new_ea, st->ea_old, st->ea_value_len) != 0) {
			err = ntfs_log_txn_update_resident(
				st->t, st->vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				ea_off,
				le16_to_cpu(ea->data.resident.value_offset),
				new_ea, st->ea_value_len,
				st->ea_old, st->ea_value_len);
			if (err)
				return err;
		}
	}
	return 0;
}

/*
 * Stage the parent's $FILE_NAME updates for a setattr: one
 * UpdateFileNameRoot per $FILE_NAME in the file's record (hard links
 * update every parent), via the shared helper. The new sizes follow
 * the sync_filename rule (0 for a directory, compressed.size for
 * sparse/compressed, allocated_size otherwise); the new flags are
 * the file's current flags.
 */
static int stage_setattr_filename(struct ntfs_journal_setattr *st,
				  struct ntfs_inode *ni,
				  const struct mft_record *mrec)
{
	u64 fn_alloc_new, fn_data_new;
	u32 flags_new;
	u16 off = le16_to_cpu(mrec->attrs_offset);
	u32 used = le32_to_cpu(mrec->bytes_in_use);
	int err = 0;

	if (S_ISDIR(VFS_I(ni)->i_mode)) {
		fn_alloc_new = 0;
		fn_data_new = 0;
	} else {
		unsigned long flags;

		read_lock_irqsave(&ni->size_lock, flags);
		if (NInoSparse(ni) || NInoCompressed(ni))
			fn_alloc_new = ni->itype.compressed.size;
		else
			fn_alloc_new = ni->allocated_size;
		fn_data_new = ni->data_size;
		read_unlock_irqrestore(&ni->size_lock, flags);
	}
	flags_new = le32_to_cpu(ni->flags);
	while (off + 8 <= used) {
		const struct attr_record *at =
			(const struct attr_record *)((const u8 *)mrec + off);
		u32 len;

		if (at->type == AT_END)
			break;
		len = le32_to_cpu(at->length);
		if (len < 8 || off + len > used)
			return -EINVAL;
		if (at->type == AT_FILE_NAME) {
			err = stage_filename_dup(st->vol, st->t, ni, at,
						 st->fn_alloc_old,
						 st->fn_data_old,
						 st->si_flags_old,
						 fn_alloc_new, fn_data_new,
						 flags_new, NULL);
			if (err)
				return err;
		}
		off += len;
	}
	return err;
}

static int ntfs_journal_setattr_stage(struct ntfs_journal_setattr *st,
				      struct ntfs_inode *ni,
				      const struct mft_record *mrec)
{
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	err = mft_coords(st->vol, ni->mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (err)
		return err;
	/* $SI first (times + flags), then the $EA pair, then the
	 * parent's $FILE_NAME — the order the driver makes the
	 * sub-mutations in (the EA and $FILE_NAME bytes land in the
	 * mapped record during ntfs_vfs_setattr; the $SI bytes are
	 * built from in-memory state, which is final by now). */
	err = stage_setattr_si(st, ni, mrec, vcn, cluster_off, page_lcn);
	if (err)
		return err;
	err = stage_setattr_ea(st, ni, mrec, vcn, cluster_off, page_lcn);
	if (err)
		return err;
	return stage_setattr_filename(st, ni, mrec);
}

static int ntfs_journal_setattr_commit(struct ntfs_journal_setattr *st,
				       struct ntfs_inode *ni)
{
	uint64_t last_lsn = 0;
	struct mft_record *m;
	int err;

	if (!st)
		return -EINVAL;
	err = ntfs_log_txn_commit(st->t, NULL, &last_lsn);
	if (!err) {
		/* Stamp MR_LSN into the file's record before the
		 * metadata flush, as in the other commits. The
		 * parent is not stamped — the $FILE_NAME redo is
		 * idempotent, as in truncate. The finish never
		 * commits an empty transaction, so last_lsn is a
		 * real LSN here, never 0. */
		m = map_mft_record(ni);
		if (IS_ERR(m))
			err = PTR_ERR(m);
		else {
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
	}
	atomic_dec(&st->vol->log_txn_open);
	mutex_unlock(&st->vol->log_txn_lock);
	free_setattr_snapshot(st);
	kfree(st);
	return err;
}

int ntfs_journal_setattr_finish(struct ntfs_journal_setattr *st,
				struct ntfs_inode *ni,
				const struct mft_record *mrec)
{
	int err;

	if (!st)
		return -EINVAL;
	/* The resize is unjournaled: the $DATA part cannot be
	 * described, so the $SI/$EA/$FILE_NAME records must not
	 * commit either — the $FILE_NAME new dup would carry the
	 * new sizes against an old $DATA, the exact mismatch the
	 * truncate class's $FILE_NAME fix closed. */
	if (st->truncate_failed)
		goto out_abort;
	err = ntfs_journal_setattr_stage(st, ni, mrec);
	if (err)
		goto out_abort;
	/* Nothing staged — a no-op setattr, or a resize that moved
	 * nothing and no mode/times change: abort, no log churn.
	 * (Committing an empty transaction writes nothing, but it
	 * would return last_lsn 0, and stamping that would clear
	 * the record's MR_LSN.) */
	if (ntfs_log_txn_is_empty(st->t))
		goto out_abort;
	return ntfs_journal_setattr_commit(st, ni);

out_abort:
	ntfs_journal_setattr_abort(st);
	return 0;
}

/* ---- Xattr (Phase 3, sixth class) --------------------------------------
 *
 * One transaction per ntfs_setxattr / ntfs_removexattr call. The
 * carrier snapshots, at begin, the $SI 36 bytes and the ADS: a copy
 * of the whole attribute record plus its decoded runlist when
 * non-resident. At stage it compares the live record against that
 * snapshot and logs the net effect. The resident value is staged
 * whole, any length — this is the class that proves the replay's
 * UpdateResidentValue length-change path (the setattr class
 * refused length changes on $EA for exactly this reason).
 *
 * The stage runs under the file's mrec_lock (api.c holds it around
 * the finish call), so the record cannot move under it. The commit
 * stamps MR_LSN into the file's record only.
 */

struct ntfs_journal_xattr {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	u8 si_old[36];
	u16 si_attr_off;
	u16 si_value_off;
	__le16 *uname;		/* kmalloc'd copy of the ADS name */
	u8 ulen;
	bool ads_present;
	u16 ads_attr_off;
	u8 *ads_old;		/* kmalloc'd copy of the attribute record */
	u32 ads_old_len;
	struct mft_attr_snapshot ads_snap;
	struct runlist_element *old_rl;
	size_t old_count;
};

static void xattr_free_snapshot(struct ntfs_journal_xattr *xt)
{
	kfree(xt->ads_snap.pairs);
	xt->ads_snap.pairs = NULL;
	kvfree(xt->old_rl);
	xt->old_rl = NULL;
	kfree(xt->ads_old);
	xt->ads_old = NULL;
	kfree(xt->uname);
	xt->uname = NULL;
}

int ntfs_journal_xattr_begin(struct ntfs_volume *vol,
			     struct ntfs_inode *ni,
			     const __le16 *uname, u8 ulen,
			     struct ntfs_journal_xattr **out)
{
	struct ntfs_journal_xattr *xt;
	const struct mft_record *m;
	const struct attr_record *si, *a;
	u16 si_off = 0, ads_off = 0;
	int err;

	*out = NULL;
	if (!vol->log_writer || !(S_ISREG(VFS_I(ni)->i_mode) ||
				  S_ISDIR(VFS_I(ni)->i_mode)))
		return 0;
	/* An attribute list means the ADS may live in an extent, not
	 * in the base record — begin cannot tell "absent" from "in an
	 * extent", so this op proceeds unjournaled. */
	if (ni->attr_list)
		return 0;
	xt = kzalloc(sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;
	xt->vol = vol;
	xt->ulen = ulen;
	xt->uname = kmemdup(uname, sizeof(__le16) * ulen, GFP_KERNEL);
	if (!xt->uname) {
		err = -ENOMEM;
		goto out_free;
	}
	/* Snapshot $SI and the ADS from the mapped base record, before
	 * raising the WAL gate — the setattr class's order. */
	m = map_mft_record(ni);
	if (IS_ERR(m)) {
		err = PTR_ERR(m);
		goto out_free;
	}
	si = find_attr(m, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
	if (!si || si->non_resident ||
	    le16_to_cpu(si->data.resident.value_offset) + 36 >
		    le32_to_cpu(si->length)) {
		err = -ENOENT;
		goto out_unmap;
	}
	xt->si_attr_off = si_off;
	xt->si_value_off =
		le16_to_cpu(si->data.resident.value_offset);
	memcpy(xt->si_old,
	       (const u8 *)si + xt->si_value_off, sizeof(xt->si_old));
	a = find_attr(m, AT_DATA, uname, ulen, &ads_off);
	if (a) {
		u32 alen = le32_to_cpu(a->length);

		xt->ads_present = true;
		xt->ads_attr_off = ads_off;
		xt->ads_old = kmemdup(a, alen, GFP_KERNEL);
		if (!xt->ads_old) {
			err = -ENOMEM;
			goto out_unmap;
		}
		xt->ads_old_len = alen;
		if (a->non_resident) {
			err = snapshot_mft_attr(m, AT_DATA, uname, ulen,
						&xt->ads_snap);
			if (err)
				goto out_unmap;
			xt->old_rl = ntfs_mapping_pairs_decompress(
				vol, a, NULL, &xt->old_count);
			if (IS_ERR(xt->old_rl)) {
				err = PTR_ERR(xt->old_rl);
				xt->old_rl = NULL;
				goto out_unmap;
			}
		}
	}
	unmap_mft_record(ni);
	/* Raise the WAL gate and start the transaction. The lock
	 * serializes whole transactions; the gate holds the file's
	 * record (and $Bitmap) off the disk until the commit. */
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &xt->t);
	if (err) {
		ntfs_error(vol->sb,
			   "journal: xattr transaction begin failed: %d",
			   err);
		mutex_unlock(&vol->log_txn_lock);
		goto out_free;
	}
	atomic_inc(&vol->log_txn_open);
	*out = xt;
	return 0;

out_unmap:
	unmap_mft_record(ni);
out_free:
	xattr_free_snapshot(xt);
	kfree(xt);
	return err;
}

void ntfs_journal_xattr_abort(struct ntfs_journal_xattr *xt)
{
	if (!xt)
		return;
	(void)ntfs_log_txn_abort(xt->t);
	atomic_dec(&xt->vol->log_txn_open);
	mutex_unlock(&xt->vol->log_txn_lock);
	xattr_free_snapshot(xt);
	kfree(xt);
}

static int ntfs_journal_xattr_commit(struct ntfs_journal_xattr *xt,
				     struct ntfs_inode *ni)
{
	uint64_t last_lsn = 0;
	struct mft_record *m;
	int err;

	if (!xt)
		return -EINVAL;
	err = ntfs_log_txn_commit(xt->t, NULL, &last_lsn);
	if (!err) {
		/* Stamp MR_LSN into the file's record before the
		 * metadata flush, as in the other commits. The
		 * finish never commits an empty transaction, so
		 * last_lsn is a real LSN here, never 0. */
		m = map_mft_record(ni);
		if (IS_ERR(m))
			err = PTR_ERR(m);
		else {
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
	}
	atomic_dec(&xt->vol->log_txn_open);
	mutex_unlock(&xt->vol->log_txn_lock);
	xattr_free_snapshot(xt);
	kfree(xt);
	return err;
}

/*
 * Stage the ADS net effect. @a is the live ADS at stage (NULL when
 * absent), @ads_off its offset. The old state comes from the begin
 * snapshot. Returns 0 or -ENOENT (stage refuses to guess) / -EIO.
 * A compressed or encrypted ADS at either end aborts: the $EA
 * class owns those, and a compressed ADS's value bytes are not
 * what the record shows.
 */
static int stage_xattr_ads(struct ntfs_journal_xattr *xt,
			   struct ntfs_inode *ni,
			   const struct mft_record *mrec,
			   const struct attr_record *a, u16 ads_off,
			   u64 vcn, u16 cluster_off, u64 page_lcn)
{
	struct ntfs_volume *vol = xt->vol;
	ntfs_log_txn_t *t = xt->t;
	int err;

	if (!xt->ads_present && !a)
		return -ENOENT;
	if (a && (a->flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)))
		return -ENOENT;
	if (xt->ads_present) {
		const struct attr_record *old =
			(const struct attr_record *)xt->ads_old;

		if (old->flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED))
			return -ENOENT;
	}
	if (!xt->ads_present) {
		/* Create: stage the final attribute bytes, plus the
		 * clusters a non-resident create allocated. */
		err = stage_create_attr(t, vol, ni, mrec, AT_DATA,
					xt->uname, xt->ulen, false);
		if (err)
			return err;
		if (a->non_resident) {
			struct runlist_element *new_rl;
			size_t new_count = 0, i;

			new_rl = ntfs_mapping_pairs_decompress(
				vol, a, NULL, &new_count);
			if (IS_ERR(new_rl))
				return PTR_ERR(new_rl);
			for (i = 0; new_rl[i].length; i++) {
				if (new_rl[i].lcn < 0)
					continue;
				err = stage_bitmap_range_txn(
					vol, t, (u64)new_rl[i].lcn,
					new_rl[i].length, true);
				if (err)
					break;
			}
			kvfree(new_rl);
			if (err)
				return err;
		}
		return 0;
	}
	if (!a) {
		/* Remove: delete the begin-time attribute bytes, plus
		 * the clusters it held. */
		size_t i;

		err = ntfs_log_txn_delete_attr(
			t, vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			xt->ads_attr_off, xt->ads_old, xt->ads_old_len);
		if (err)
			return err;
		if (!xt->old_rl)
			return 0;

		for (i = 0; xt->old_rl[i].length; i++) {
			if (xt->old_rl[i].lcn < 0)
				continue;
			err = stage_bitmap_range_txn(
				vol, t, (u64)xt->old_rl[i].lcn,
				xt->old_rl[i].length, false);
			if (err)
				return err;
		}
		return 0;
	}
	/* Present both times. */
	if (!((const struct attr_record *)xt->ads_old)->non_resident &&
	    !a->non_resident) {
		/* Resident -> resident: one UpdateResidentValue for
		 * the whole value, any length. The attribute must
		 * still sit where begin found it; a move aborts. */
		const struct attr_record *old =
			(const struct attr_record *)xt->ads_old;
		u16 old_voff =
			le16_to_cpu(old->data.resident.value_offset);
		u16 new_voff =
			le16_to_cpu(a->data.resident.value_offset);
		u16 old_vlen =
			le16_to_cpu(old->data.resident.value_length);
		u16 new_vlen =
			le16_to_cpu(a->data.resident.value_length);

		if (ads_off != xt->ads_attr_off || new_voff != old_voff)
			return -ENOENT;
		return ntfs_log_txn_update_resident(
			t, vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			ads_off, new_voff,
			(const u8 *)a + new_voff, new_vlen,
			(const u8 *)old + old_voff, old_vlen);
	}
	if (!((const struct attr_record *)xt->ads_old)->non_resident ||
	    !a->non_resident) {
		/* Flipped resident/non-resident either way: delete
		 * the old attribute and create the new one, plus the
		 * bitmap delta (set for a new non-resident, clear
		 * for a freed one). */
		const struct attr_record *old =
			(const struct attr_record *)xt->ads_old;
		size_t i;

		err = ntfs_log_txn_delete_attr(
			t, vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			xt->ads_attr_off, xt->ads_old, xt->ads_old_len);
		if (err)
			return err;
		err = stage_create_attr(t, vol, ni, mrec, AT_DATA,
					xt->uname, xt->ulen, false);
		if (err)
			return err;
		if (old->non_resident) {
			for (i = 0; xt->old_rl[i].length; i++) {
				if (xt->old_rl[i].lcn < 0)
					continue;
				err = stage_bitmap_range_txn(
					vol, t, (u64)xt->old_rl[i].lcn,
					xt->old_rl[i].length, false);
				if (err)
					return err;
			}
		} else {
			struct runlist_element *new_rl;
			size_t new_count = 0;

			new_rl = ntfs_mapping_pairs_decompress(
				vol, a, NULL, &new_count);
			if (IS_ERR(new_rl))
				return PTR_ERR(new_rl);
			for (i = 0; new_rl[i].length; i++) {
				if (new_rl[i].lcn < 0)
					continue;
				err = stage_bitmap_range_txn(
					vol, t, (u64)new_rl[i].lcn,
					new_rl[i].length, true);
				if (err)
					break;
			}
			kvfree(new_rl);
			if (err)
				return err;
		}
		return 0;
	}
	/* Non-resident -> non-resident: sizes + mapping pairs +
	 * bitmap delta, the truncate machinery, named. */
	{
		struct mft_attr_snapshot new_snap;
		struct runlist_element *new_rl = NULL;
		struct lcn_range *freed = NULL, *allocd = NULL;
		size_t new_count = 0, i;

		if (ads_off != xt->ads_attr_off)
			return -ENOENT;
		err = snapshot_mft_attr(mrec, AT_DATA, xt->uname,
					xt->ulen, &new_snap);
		if (err) {
			return err;
		}
		new_rl = ntfs_mapping_pairs_decompress(vol, a, NULL,
						       &new_count);
		if (IS_ERR(new_rl)) {
			err = PTR_ERR(new_rl);
			new_rl = NULL;
			goto out_free;
		}
		/* The mapping pairs first (redo order is extend the runs,
		 * then the sizes that name them), then the sizes — the
		 * truncate class stages the same way, through the dedicated
		 * mapping-pairs op, not a raw UpdateResidentValue on the
		 * pairs bytes: the replay validates the pairs against the
		 * sizes, and a raw value update leaves them disagreeing.
		 * A header change (flags, name or pairs offset moved) is
		 * not describable here; abort to unjournaled rather than
		 * log a half-described attribute. */
		if (new_snap.flags != xt->ads_snap.flags ||
		    new_snap.name_off != xt->ads_snap.name_off ||
		    new_snap.pairs_off != xt->ads_snap.pairs_off) {
			err = -ENOENT;
			goto out_free;
		}
		if (new_snap.pairs_len != xt->ads_snap.pairs_len ||
		    memcmp(new_snap.pairs, xt->ads_snap.pairs,
			   new_snap.pairs_len)) {
			err = ntfs_log_txn_update_mapping_pairs(
				t, vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				ads_off, new_snap.pairs_off,
				new_snap.flags, new_snap.name_off,
				new_snap.pairs, new_snap.pairs_len,
				xt->ads_snap.flags,
				xt->ads_snap.name_off,
				xt->ads_snap.pairs_off,
				xt->ads_snap.pairs,
				xt->ads_snap.pairs_len);
			if (err)
				goto out_free;
		}
		if (new_snap.alloc != xt->ads_snap.alloc ||
		    new_snap.valid != xt->ads_snap.valid ||
		    new_snap.data != xt->ads_snap.data ||
		    new_snap.total != xt->ads_snap.total) {
			err = ntfs_log_txn_set_attr_sizes(
				t, vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				ads_off, new_snap.alloc, new_snap.valid,
				new_snap.data, new_snap.total,
				xt->ads_snap.alloc, xt->ads_snap.valid,
				xt->ads_snap.data, xt->ads_snap.total);
			if (err)
				goto out_free;
		}
		freed = kmalloc_array(xt->old_count + new_count,
				      sizeof(*freed), GFP_KERNEL);
		allocd = kmalloc_array(xt->old_count + new_count,
				       sizeof(*allocd), GFP_KERNEL);
		if (!freed || !allocd) {
			err = -ENOMEM;
			goto out_free;
		}
		/* lcn_ranges_sub returns the range COUNT (>= 0) or
		 * -ENOSPC: a positive count is success, not an error
		 * (the truncate class stages the same way). */
		{
			int n_freed, n_allocd;

			n_freed = lcn_ranges_sub(xt->old_rl, new_rl, freed,
						 xt->old_count + new_count);
			n_allocd = lcn_ranges_sub(new_rl, xt->old_rl, allocd,
						  xt->old_count + new_count);
			if (n_freed < 0 || n_allocd < 0) {
				err = -ENOSPC;
				goto out_free;
			}
			for (i = 0; i < (size_t)n_freed && !err; i++)
				err = stage_bitmap_range_txn(vol, t,
							       freed[i].lcn,
							       freed[i].len,
						       false);
			for (i = 0; i < (size_t)n_allocd && !err; i++)
				err = stage_bitmap_range_txn(vol, t,
							       allocd[i].lcn,
							       allocd[i].len,
						       true);
			if (err)
				goto out_free;
		}
	out_free:
		kfree(new_snap.pairs);
		kvfree(new_rl);
		kfree(freed);
		kfree(allocd);
		return err;
	}
}

int ntfs_journal_xattr_finish(struct ntfs_journal_xattr *xt,
			      struct ntfs_inode *ni,
			      const struct mft_record *mrec)
{
	struct ntfs_volume *vol = xt->vol;
	ntfs_log_txn_t *t = xt->t;
	u64 vcn, page_lcn;
	u16 cluster_off;
	const struct attr_record *a;
	u16 ads_off = 0;
	int err;

	err = mft_coords(vol, ni->mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (err)
		goto out_abort;

	/* $SI first: the setxattr success path only bumps ctime. */
	err = stage_si_36(vol, t, xt->si_old, xt->si_attr_off,
			  xt->si_value_off, ni, mrec,
			  vcn, cluster_off, page_lcn);
	if (err)
		goto out_abort;
	a = find_attr(mrec, AT_DATA, xt->uname, xt->ulen, &ads_off);
	err = stage_xattr_ads(xt, ni, mrec, a, ads_off,
			      vcn, cluster_off, page_lcn);
	if (err)
		goto out_abort;
	if (ntfs_log_txn_is_empty(t))
		goto out_abort;
	return ntfs_journal_xattr_commit(xt, ni);

out_abort:
	ntfs_journal_xattr_abort(xt);
	return 0;
}

/* ---- EA (Phase 3, eighth class) -----------------------------------------
 *
 * One transaction per VFS xattr-handler set on the $EA path. The
 * carrier snapshots, at begin, the $SI 36 bytes (from in-memory
 * state, the setattr form) and whole-record copies of
 * $EA_INFORMATION and $EA plus their offsets and presence. At
 * stage it compares the live record against that snapshot and
 * logs the pair's net effect. The resident values are staged
 * whole, any length — the replay's length-change path, proven by
 * the xattr class.
 *
 * The stage runs under the file's mrec_lock (ea.c holds it around
 * the finish call), so the record cannot move under it. The commit
 * stamps MR_LSN into the file's record only.
 */

struct ntfs_journal_ea {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	u8 si_old[36];
	u16 si_attr_off;
	u16 si_value_off;
	bool ea_info_present;
	u16 ea_info_attr_off;
	u8 *ea_info_old;	/* kmalloc'd copy of the attribute record */
	u32 ea_info_old_len;
	bool ea_present;
	u16 ea_attr_off;
	u8 *ea_old;		/* kmalloc'd copy of the attribute record */
	u32 ea_old_len;
};

static void ea_free_snapshot(struct ntfs_journal_ea *et)
{
	kfree(et->ea_info_old);
	et->ea_info_old = NULL;
	kfree(et->ea_old);
	et->ea_old = NULL;
}

int ntfs_journal_ea_begin(struct ntfs_volume *vol,
			  struct ntfs_inode *ni,
			  struct ntfs_journal_ea **out)
{
	struct ntfs_journal_ea *et;
	struct mft_record *m;
	struct inode *vi = VFS_I(ni);
	int err;

	*out = NULL;
	if (!vol->log_writer)
		return 0;
	/* Regular files and directories only, the create class's
	 * scope. Symlinks and special files proceed unjournaled —
	 * their $EA, if any, is a foreign-volume artifact. */
	if (!S_ISREG(vi->i_mode) && !S_ISDIR(vi->i_mode))
		return 0;
	/* An attribute list means the $EA pair may live in an
	 * extent, not in the base record — begin cannot tell
	 * "absent" from "in an extent", so this op proceeds
	 * unjournaled. */
	if (ni->attr_list)
		return 0;
	et = kmalloc(sizeof(*et), GFP_NOFS);
	if (!et)
		return -ENOMEM;
	/* Initialize every field at begin: kmalloc does not zero
	 * (round 25: an uninitialized txn field corrupted the
	 * volume). */
	et->vol = vol;
	et->t = NULL;
	et->si_attr_off = 0;
	et->si_value_off = 0;
	et->ea_info_present = false;
	et->ea_info_attr_off = 0;
	et->ea_info_old = NULL;
	et->ea_info_old_len = 0;
	et->ea_present = false;
	et->ea_attr_off = 0;
	et->ea_old = NULL;
	et->ea_old_len = 0;
	/* The $SI snapshot is in-memory state: the mapped record
	 * lags behind the log, and a mapped snapshot's undo would
	 * restore the pre-previous state. */
	si_build(vi, ni, et->si_old);
	/* The $SI offsets and the $EA pair, from the mapped record.
	 * A missing or too-short $SI is a begin failure (the op
	 * then proceeds unjournaled); a non-resident $EA pair, a
	 * wrong-length $EA_INFORMATION, or a half-pair means the
	 * file never begins — this class cannot describe it. */
	m = map_mft_record(ni);
	if (IS_ERR(m)) {
		err = PTR_ERR(m);
		goto out_free;
	}
	{
		const struct attr_record *si;
		u16 si_off = 0;

		si = find_attr(m, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
		if (!si || si->non_resident ||
		    le16_to_cpu(si->data.resident.value_offset) + 36 >
			    le32_to_cpu(si->length)) {
			err = -ENOENT;
			goto out_unmap;
		}
		et->si_attr_off = si_off;
		et->si_value_off =
			le16_to_cpu(si->data.resident.value_offset);
	}
	{
		const struct attr_record *ei, *ea;
		u16 ei_off = 0, ea_off = 0;

		ei = find_attr(m, AT_EA_INFORMATION, NULL, 0, &ei_off);
		ea = find_attr(m, AT_EA, NULL, 0, &ea_off);
		if (!!ei != !!ea)
			goto out_nonresident;
		if (ei) {
			u32 alen;

			if (ei->non_resident ||
			    le16_to_cpu(ei->data.resident.value_length) !=
				    sizeof(struct ea_information))
				goto out_nonresident;
			alen = le32_to_cpu(ei->length);
			et->ea_info_old = kmemdup(ei, alen, GFP_NOFS);
			if (!et->ea_info_old) {
				err = -ENOMEM;
				goto out_unmap;
			}
			et->ea_info_old_len = alen;
			et->ea_info_attr_off = ei_off;
			et->ea_info_present = true;
		}
		if (ea) {
			u32 alen;

			if (ea->non_resident)
				goto out_nonresident;
			alen = le32_to_cpu(ea->length);
			et->ea_old = kmemdup(ea, alen, GFP_NOFS);
			if (!et->ea_old) {
				err = -ENOMEM;
				goto out_unmap;
			}
			et->ea_old_len = alen;
			et->ea_attr_off = ea_off;
			et->ea_present = true;
		}
	}
	unmap_mft_record(ni);
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &et->t);
	if (err) {
		ntfs_error(vol->sb,
			   "journal: EA transaction begin failed: %d",
			   err);
		mutex_unlock(&vol->log_txn_lock);
		goto out_free;
	}
	atomic_inc(&vol->log_txn_open);
	*out = et;
	return 0;

out_nonresident:
	/* A $EA pair this class cannot describe: proceed
	 * unjournaled, by design, not a failure. */
	unmap_mft_record(ni);
	ea_free_snapshot(et);
	kfree(et);
	*out = NULL;
	return 0;
out_unmap:
	unmap_mft_record(ni);
out_free:
	ea_free_snapshot(et);
	kfree(et);
	return err;
}

void ntfs_journal_ea_abort(struct ntfs_journal_ea *et)
{
	if (!et)
		return;
	(void)ntfs_log_txn_abort(et->t);
	atomic_dec(&et->vol->log_txn_open);
	mutex_unlock(&et->vol->log_txn_lock);
	ea_free_snapshot(et);
	kfree(et);
}

static int ntfs_journal_ea_commit(struct ntfs_journal_ea *et,
				  struct ntfs_inode *ni)
{
	uint64_t last_lsn = 0;
	struct mft_record *m;
	int err;

	if (!et)
		return -EINVAL;
	err = ntfs_log_txn_commit(et->t, NULL, &last_lsn);
	if (!err) {
		/* Stamp MR_LSN into the file's record before the
		 * metadata flush, as in the other commits. The
		 * finish never commits an empty transaction, so
		 * last_lsn is a real LSN here, never 0. */
		m = map_mft_record(ni);
		if (IS_ERR(m))
			err = PTR_ERR(m);
		else {
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
	}
	atomic_dec(&et->vol->log_txn_open);
	mutex_unlock(&et->vol->log_txn_lock);
	ea_free_snapshot(et);
	kfree(et);
	return err;
}

/*
 * Stage the $EA_INFORMATION + $EA pair, net effect. The values are
 * re-read from the mapped record at stage time — resident $EA
 * writes land straight in the mapped record, so the stage sees the
 * final bytes. Cases:
 *
 *   - absent at begin, present at stage: CreateAttribute for the
 *     pair (the create_ea pattern), $EA_INFORMATION first;
 *   - present at begin, absent at stage: DeleteAttribute for the
 *     pair, $EA first (the live removal order), from the
 *     begin-time bytes;
 *   - present both times: UpdateResidentValue for each value that
 *     changed, whole value, any length (the xattr-proven
 *     length-change path). The attributes must still sit where
 *     begin found them and keep their value offsets; a move
 *     aborts.
 *   - a half-pair at either end, or a non-resident $EA at stage:
 *     -ENOENT (refusing to guess).
 */
static int stage_ea(struct ntfs_journal_ea *et,
		    struct ntfs_inode *ni,
		    const struct mft_record *mrec,
		    u64 vcn, u16 cluster_off, u64 page_lcn)
{
	const struct attr_record *ei, *ea;
	u16 ei_off = 0, ea_off = 0;
	int err;

	ei = find_attr(mrec, AT_EA_INFORMATION, NULL, 0, &ei_off);
	ea = find_attr(mrec, AT_EA, NULL, 0, &ea_off);
	if (!et->ea_info_present && !et->ea_present) {
		if (!ei && !ea)
			return 0;
		if (!ei || !ea)
			return -ENOENT;
		/* The pair was created by this op: stage both as
		 * CreateAttributes, $EA_INFORMATION first. */
		err = stage_create_attr(et->t, et->vol, ni, mrec,
					AT_EA_INFORMATION, NULL, 0, false);
		if (err)
			return err;
		return stage_create_attr(et->t, et->vol, ni, mrec,
					 AT_EA, NULL, 0, false);
	}
	if (!ei && !ea) {
		/* The pair was removed by this op: stage both as
		 * DeleteAttributes, $EA first (the live removal
		 * order), from the begin-time bytes. */
		err = ntfs_log_txn_delete_attr(
			et->t, et->vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			et->ea_attr_off, et->ea_old, et->ea_old_len);
		if (err)
			return err;
		return ntfs_log_txn_delete_attr(
			et->t, et->vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			et->ea_info_attr_off, et->ea_info_old,
			et->ea_info_old_len);
	}
	if (!ei || !ea)
		return -ENOENT;
	if (ei->non_resident || ea->non_resident)
		return -ENOENT;
	/* $EA_INFORMATION's value is always 8 bytes; anything else
	 * is a record this class cannot describe. */
	if (le16_to_cpu(ei->data.resident.value_length) !=
	    sizeof(struct ea_information))
		return -ENOENT;
	if (ei_off != et->ea_info_attr_off || ea_off != et->ea_attr_off)
		return -ENOENT;
	{
		const struct attr_record *old_ei =
			(const struct attr_record *)et->ea_info_old;
		u16 old_voff =
			le16_to_cpu(old_ei->data.resident.value_offset);
		u16 new_voff =
			le16_to_cpu(ei->data.resident.value_offset);
		const u8 *new_info = (const u8 *)ei + new_voff;
		const u8 *old_info = (const u8 *)old_ei + old_voff;

		if (new_voff != old_voff)
			return -ENOENT;
		if (memcmp(new_info, old_info,
			   sizeof(struct ea_information)) != 0) {
			err = ntfs_log_txn_update_resident(
				et->t, et->vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				ei_off, new_voff,
				new_info, sizeof(struct ea_information),
				old_info, sizeof(struct ea_information));
			if (err)
				return err;
		}
	}
	{
		const struct attr_record *old_ea =
			(const struct attr_record *)et->ea_old;
		u16 old_voff =
			le16_to_cpu(old_ea->data.resident.value_offset);
		u16 new_voff =
			le16_to_cpu(ea->data.resident.value_offset);
		u16 old_vlen =
			le16_to_cpu(old_ea->data.resident.value_length);
		u16 new_vlen =
			le16_to_cpu(ea->data.resident.value_length);
		const u8 *new_ea = (const u8 *)ea + new_voff;
		const u8 *old_ea_val = (const u8 *)old_ea + old_voff;

		if (new_voff != old_voff)
			return -ENOENT;
		if (new_vlen != old_vlen ||
		    memcmp(new_ea, old_ea_val, old_vlen) != 0) {
			err = ntfs_log_txn_update_resident(
				et->t, et->vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				ea_off, new_voff,
				new_ea, new_vlen,
				old_ea_val, old_vlen);
			if (err)
				return err;
		}
	}
	return 0;
}

int ntfs_journal_ea_finish(struct ntfs_journal_ea *et,
			   struct ntfs_inode *ni,
			   const struct mft_record *mrec)
{
	struct ntfs_volume *vol = et->vol;
	ntfs_log_txn_t *t = et->t;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	err = mft_coords(vol, ni->mft_no, &vcn, &cluster_off,
			 &page_lcn);
	if (err)
		goto out_abort;

	/* $SI first: the handler's success path only bumps ctime,
	 * and ntfs_set_ea sets/clears the HasEA flag in ni->flags. */
	err = stage_si_36(vol, t, et->si_old, et->si_attr_off,
			  et->si_value_off, ni, mrec,
			  vcn, cluster_off, page_lcn);
	if (err)
		goto out_abort;
	err = stage_ea(et, ni, mrec, vcn, cluster_off, page_lcn);
	if (err)
		goto out_abort;
	if (ntfs_log_txn_is_empty(t))
		goto out_abort;
	return ntfs_journal_ea_commit(et, ni);

out_abort:
	ntfs_journal_ea_abort(et);
	return 0;
}

/* ---- compressed-write (Phase 3) -------------------------------------- */

struct ntfs_journal_cwrite {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
	/* The $DATA sizes + mapping pairs as they stood at begin,
	 * before the write. */
	struct mft_attr_snapshot snap;
	bool snap_ok;
	/* The $STANDARD_INFORMATION's first 36 bytes (four times +
	 * file_attributes) as they stood at begin, built from
	 * in-memory vi/ni state — never from the mapped record,
	 * which lags behind the log. */
	u8 si_old[36];
	u16 si_attr_off;
	u16 si_value_off;
	/* The $STANDARD_INFORMATION flags as they stood at begin,
	 * from in-memory ni->flags — the parent $FILE_NAME old dup
	 * takes its flags from here. NOT from the mapped record
	 * (the truncate class's source): the flags handler updates
	 * ni->flags but never the $SI record bytes, so the mapped
	 * value lags and stages a wrong undo for handler-flagged
	 * files. See cwrite_snapshot. */
	u32 si_flags_old;
	/* The parent $FILE_NAME's allocated size as it stood at
	 * begin: ni->itype.compressed.size for a compressed file,
	 * the same rule ntfs_inode_sync_filename applies. */
	u64 fn_alloc_old;
	/* The run list as it stood at begin: a copy of
	 * ni->runlist.rl up to and including its length-0
	 * terminator. The bitmap delta is the set difference
	 * between this and the live run list. */
	struct runlist_element *old_rl;
	size_t old_count;
};

static void free_cwrite_snapshot(struct ntfs_journal_cwrite *t)
{
	if (!t)
		return;
	free_snapshot(&t->snap);
	kfree(t->old_rl);
	t->old_rl = NULL;
	t->old_count = 0;
}

/*
 * Snapshot the $DATA sizes + pairs, the $SI flags and 36 bytes, and
 * the run list before a compressed write mutates them. Mirrors
 * truncate_snapshot (same fields, same order, same window: the
 * snapshot is taken before log_txn_lock, and the stage re-verifies
 * the attribute offset before staging anything), plus the $SI 36
 * bytes the write's mtime bump moves. A compressed file's $DATA is
 * always non-resident here — the begin gate refuses resident files
 * — so snapshot_mft_attr's non-resident requirement is met.
 */
static int cwrite_snapshot(struct ntfs_volume *vol,
			   struct ntfs_inode *ni,
			   struct ntfs_journal_cwrite *t)
{
	struct mft_record *m;
	struct inode *vi = VFS_I(ni);
	int err;

	err = ntfs_attr_map_whole_runlist(ni);
	if (err)
		return err;
	m = map_mft_record(ni);
	if (IS_ERR(m))
		return PTR_ERR(m);
	err = snapshot_mft_attr(m, AT_DATA, NULL, 0, &t->snap);
	if (!err) {
		const struct attr_record *si;
		u16 si_off = 0;

		si = find_attr(m, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
		if (!si || si->non_resident ||
		    le16_to_cpu(si->data.resident.value_offset) + 36 >
			    le32_to_cpu(si->length))
			err = -ENOENT;
		else {
			t->si_attr_off = si_off;
			t->si_value_off =
				le16_to_cpu(si->data.resident.value_offset);
		}
	}
	if (!err) {
		/* The parent's allocated size as it stands: the
		 * compressed size for a compressed file, the
		 * $DATA's allocated size otherwise — the rule
		 * ntfs_inode_sync_filename applies when it
		 * refreshes the parent's $FILE_NAME. */
		if (NInoSparse(ni) || NInoCompressed(ni))
			t->fn_alloc_old = ni->itype.compressed.size;
		else
			t->fn_alloc_old = t->snap.alloc;
		/* The $STANDARD_INFORMATION flags from in-memory
		 * ni->flags — NOT from the mapped record, where
		 * the truncate class reads them. The flags
		 * handler (ntfs_new_attr_flags) updates ni->flags
		 * but never the $SI record bytes, so a file
		 * flagged compressed through it carries 0x800 in
		 * ni->flags and 0x0 in its mapped $SI; the
		 * parent's entry is refreshed from ni->flags, so
		 * the in-memory value is the entry's logical old
		 * value and the mapped one stages a wrong undo
		 * (it would clear COMPRESSED from the entry on
		 * replay — the first version of this class did
		 * exactly that, and the overwrite test caught it:
		 * its $FILE_NAME undo carried flags 0x0 against a
		 * live entry of 0x800). The setattr carrier
		 * sources its flags the same way. A compressed
		 * write never changes the flags, so the old and
		 * new values agree; the $SI 36-byte record
		 * carries them either way. */
		t->si_flags_old = le32_to_cpu(ni->flags);
		/* The $SI 36 bytes from in-memory state, the
		 * setattr form: the mapped record lags behind the
		 * log, so a mapped snapshot's undo would restore
		 * the pre-previous state. */
		si_build(vi, ni, t->si_old);
	}
	unmap_mft_record(ni);
	if (err)
		return err;
	t->snap_ok = true;
	{
		struct runlist_element *rl;
		size_t n = 0;

		down_read(&ni->runlist.lock);
		rl = ni->runlist.rl;
		if (rl) {
			while (rl[n].length)
				n++;
			n++; /* the length-0 terminator */
			t->old_rl = kmalloc_array(n,
						  sizeof(*t->old_rl),
						  GFP_NOFS);
			if (t->old_rl) {
				memcpy(t->old_rl, rl,
				       n * sizeof(*t->old_rl));
				t->old_count = n;
			}
		}
		up_read(&ni->runlist.lock);
		if (!t->old_rl)
			return -ENOMEM;
	}
	return 0;
}

int ntfs_journal_cwrite_begin(struct ntfs_volume *vol,
			      struct ntfs_inode *ni,
			      struct ntfs_journal_cwrite **out)
{
	struct ntfs_journal_cwrite *t;
	int err;

	*out = NULL;
	if (!vol->log_writer)
		return 0;
	/* Regular files only, non-resident $DATA only, compressed
	 * only. A resident $DATA (which converts inside the write),
	 * an attribute list (the $DATA may span extent records), an
	 * attribute inode (ADS writes are the xattr class's
	 * domain), or an encrypted file: unjournaled. */
	if (!S_ISREG(VFS_I(ni)->i_mode) || !NInoNonResident(ni) ||
	    ni->attr_list || NInoAttr(ni) || NInoEncrypted(ni) ||
	    !NInoCompressed(ni))
		return 0;
	t = kmalloc(sizeof(*t), GFP_NOFS);
	if (!t)
		return -ENOMEM;
	/* Initialize every field at begin: kmalloc does not zero
	 * (round 25: an uninitialized txn field corrupted the
	 * volume). */
	t->vol = vol;
	t->t = NULL;
	t->snap.pairs = NULL;
	t->snap_ok = false;
	t->si_attr_off = 0;
	t->si_value_off = 0;
	t->si_flags_old = 0;
	t->fn_alloc_old = 0;
	t->old_rl = NULL;
	t->old_count = 0;
	err = cwrite_snapshot(vol, ni, t);
	if (err)
		goto out_free;
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &t->t);
	if (err) {
		mutex_unlock(&vol->log_txn_lock);
		goto out_free;
	}
	atomic_inc(&vol->log_txn_open);
	*out = t;
	return 0;

out_free:
	free_cwrite_snapshot(t);
	kfree(t);
	return err;
}

void ntfs_journal_cwrite_abort(struct ntfs_journal_cwrite *t)
{
	if (!t)
		return;
	(void)ntfs_log_txn_abort(t->t);
	atomic_dec(&t->vol->log_txn_open);
	mutex_unlock(&t->vol->log_txn_lock);
	free_cwrite_snapshot(t);
	kfree(t);
}

static int ntfs_journal_cwrite_commit(struct ntfs_journal_cwrite *t,
				      struct ntfs_inode *ni)
{
	uint64_t last_lsn = 0;
	struct mft_record *m;
	int err;

	if (!t)
		return -EINVAL;
	err = ntfs_log_txn_commit(t->t, NULL, &last_lsn);
	if (!err) {
		/* Stamp MR_LSN into the file's record before the
		 * metadata flush, as in the other commits. The
		 * finish never commits an empty transaction, so
		 * last_lsn is a real LSN here, never 0. */
		m = map_mft_record(ni);
		if (IS_ERR(m))
			err = PTR_ERR(m);
		else {
			m->lsn = cpu_to_le64(last_lsn);
			mark_mft_record_dirty(ni);
			unmap_mft_record(ni);
		}
	}
	atomic_dec(&t->vol->log_txn_open);
	mutex_unlock(&t->vol->log_txn_lock);
	free_cwrite_snapshot(t);
	kfree(t);
	return err;
}

/*
 * Stage one parent's $FILE_NAME dup update for a compressed write.
 * Mirrors stage_truncate_filename_one: the new dup takes its sizes
 * from the live $DATA (compressed.size for the alloc, the same rule
 * ntfs_inode_sync_filename applies) and its flags from ni->flags;
 * the old dup takes its sizes/flags from the begin snapshot, never
 * from the entry (the parent's record in memory lags behind the
 * log). Only the times come from the entry.
 */
static int stage_cwrite_filename_one(struct ntfs_journal_cwrite *t,
				     struct ntfs_inode *ni,
				     const struct attr_record *fn_attr,
				     const struct attr_record *data_attr)
{
	u64 fn_alloc_new, fn_data_new;
	u32 flags_new;

	if (NInoSparse(ni) || NInoCompressed(ni))
		fn_alloc_new = ni->itype.compressed.size;
	else
		fn_alloc_new = le64_to_cpu(
			data_attr->data.non_resident.allocated_size);
	fn_data_new = le64_to_cpu(data_attr->data.non_resident.data_size);
	flags_new = le32_to_cpu(ni->flags);
	return stage_filename_dup(t->vol, t->t, ni, fn_attr,
				  t->fn_alloc_old, t->snap.data,
				  t->si_flags_old,
				  fn_alloc_new, fn_data_new, flags_new,
				  NULL);
}

static int stage_cwrite_filename(struct ntfs_journal_cwrite *t,
				 struct ntfs_inode *ni,
				 const struct mft_record *mrec,
				 const struct attr_record *data_attr)
{
	u16 off = le16_to_cpu(mrec->attrs_offset);
	u32 used = le32_to_cpu(mrec->bytes_in_use);
	int err = 0;

	while (off + 8 <= used) {
		const struct attr_record *at =
			(const struct attr_record *)((const u8 *)mrec + off);
		u32 len;

		if (at->type == AT_END)
			break;
		len = le32_to_cpu(at->length);
		if (len < 8 || off + len > used)
			return -EINVAL;
		if (at->type == AT_FILE_NAME) {
			err = stage_cwrite_filename_one(t, ni, at,
							data_attr);
			if (err)
				return err;
		}
		off += len;
	}
	return err;
}

int ntfs_journal_cwrite_finish(struct ntfs_journal_cwrite *t,
			       struct ntfs_inode *ni,
			       const struct mft_record *mrec)
{
	struct ntfs_volume *vol = t->vol;
	ntfs_log_txn_t *txn = t->t;
	const struct attr_record *a;
	u16 off = 0, pairs_off;
	u32 len;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	if (!t || !t->snap_ok)
		goto out_abort;
	a = find_attr(mrec, AT_DATA, NULL, 0, &off);
	if (!a || !a->non_resident)
		goto out_abort;
	len = le32_to_cpu(a->length);
	pairs_off = le16_to_cpu(a->data.non_resident.mapping_pairs_offset);
	/* The attribute itself must not have moved; the header
	 * fields may legitimately change (a write that flips the
	 * file sparse inserts the 8-byte compressed_size field and
	 * moves name_offset and mapping_pairs_offset — the pairs
	 * record carries the old and new header, so that flip is
	 * journaled, not a reason to abort). */
	if (off != t->snap.attr_off)
		goto out_abort;
	if (pairs_off >= len)
		goto out_abort;
	err = mft_coords(vol, ni->mft_no,
			 &vcn, &cluster_off, &page_lcn);
	if (err)
		goto out_abort;

	/* The $Bitmap delta first: the clusters the write freed or
	 * allocated, from the set difference of the run lists.
	 * Every rewritten block allocates fresh clusters and
	 * punches the old ones, so a rewrite that changes nothing
	 * in the sizes still moves bitmap bits. */
	{
		struct runlist_element *new_rl = NULL;
		size_t new_count = 0, i;
		struct lcn_range *freed = NULL, *allocd = NULL;
		int n_freed, n_allocd;

		down_read(&ni->runlist.lock);
		if (ni->runlist.rl) {
			size_t n = 0;

			while (ni->runlist.rl[n].length)
				n++;
			n++;
			new_rl = kmalloc_array(n, sizeof(*new_rl),
					       GFP_NOFS);
			if (new_rl) {
				memcpy(new_rl, ni->runlist.rl,
				       n * sizeof(*new_rl));
				new_count = n;
			}
		}
		up_read(&ni->runlist.lock);
		if (!new_rl) {
			err = -ENOMEM;
			goto out_abort;
		}
		/* Upper bound: one result range per input run. */
		freed = kmalloc_array(t->old_count + new_count,
				      sizeof(*freed), GFP_NOFS);
		allocd = kmalloc_array(t->old_count + new_count,
				       sizeof(*allocd), GFP_NOFS);
		if (!freed || !allocd) {
			err = -ENOMEM;
			goto out_rl;
		}
		n_freed = lcn_ranges_sub(t->old_rl, new_rl,
					 freed, t->old_count + new_count);
		n_allocd = lcn_ranges_sub(new_rl, t->old_rl,
					  allocd, t->old_count + new_count);
		if (n_freed < 0 || n_allocd < 0) {
			err = -ENOSPC;
			goto out_rl;
		}
		for (i = 0; i < (size_t)n_freed && !err; i++)
			err = stage_bitmap_range_txn(vol, txn,
						     freed[i].lcn,
						     freed[i].len, false);
		for (i = 0; i < (size_t)n_allocd && !err; i++)
			err = stage_bitmap_range_txn(vol, txn,
						     allocd[i].lcn,
						     allocd[i].len, true);
out_rl:
		kfree(freed);
		kfree(allocd);
		kfree(new_rl);
		if (err)
			goto out_abort;
	}

	/* The mapping pairs next, when they changed — through the
	 * dedicated op, before the sizes (redo order: extend the
	 * runs, then the sizes that name them). A raw value update
	 * on the pairs bytes leaves them disagreeing with the
	 * sizes and the replay corrupts the attribute (the xattr
	 * crash sweep caught it: "Corrupt mapping pairs array"). */
	if (len - pairs_off != t->snap.pairs_len ||
	    memcmp((const u8 *)a + pairs_off, t->snap.pairs,
		   t->snap.pairs_len) != 0) {
		err = ntfs_log_txn_update_mapping_pairs(
			txn, vol->log_mft_data_attr,
			vcn, cluster_off, page_lcn,
			off, pairs_off,
			le16_to_cpu(a->flags), le16_to_cpu(a->name_offset),
			(const u8 *)a + pairs_off, (u16)(len - pairs_off),
			t->snap.flags, t->snap.name_off, t->snap.pairs_off,
			t->snap.pairs, t->snap.pairs_len);
		if (err)
			goto out_abort;
	}

	/* The sizes last, when they changed. */
	{
		u64 alloc = le64_to_cpu(a->data.non_resident.allocated_size);
		u64 valid = le64_to_cpu(a->data.non_resident.initialized_size);
		u64 data = le64_to_cpu(a->data.non_resident.data_size);
		u64 total = pairs_off >= 0x48 ?
			le64_to_cpu(a->data.non_resident.compressed_size) : 0;

		if (alloc != t->snap.alloc || valid != t->snap.valid ||
		    data != t->snap.data || total != t->snap.total) {
			err = ntfs_log_txn_set_attr_sizes(
				txn, vol->log_mft_data_attr,
				vcn, cluster_off, page_lcn,
				off, alloc, valid, data, total,
				t->snap.alloc, t->snap.valid,
				t->snap.data, t->snap.total);
			if (err)
				goto out_abort;
		}
	}

	/* The $STANDARD_INFORMATION times, which the write's mtime
	 * bump moved. The flags cannot change on this path (the
	 * file was already compressed), but the 36-byte record
	 * covers them either way; stage_si_36 stages only on
	 * change. The $SI attribute must still sit where begin
	 * found it; anything else aborts. */
	err = stage_si_36(vol, txn, t->si_old, t->si_attr_off,
			  t->si_value_off, ni, mrec,
			  vcn, cluster_off, page_lcn);
	if (err)
		goto out_abort;

	/* The parent's $FILE_NAME, last: its duplicated sizes and
	 * flags must agree with the written $DATA after the replay,
	 * whatever instant the crash hits. A failure here aborts
	 * the whole transaction (the write then proceeds
	 * unjournaled). */
	err = stage_cwrite_filename(t, ni, mrec, a);
	if (err)
		goto out_abort;

	if (ntfs_log_txn_is_empty(txn))
		goto out_abort;
	return ntfs_journal_cwrite_commit(t, ni);

out_abort:
	ntfs_journal_cwrite_abort(t);
	return 0;
}
