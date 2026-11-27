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
#include "time.h"
#include "journal.h"
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
 * shorter attributes carry 0 there.
 */
static int snapshot_mft_attr(const struct mft_record *m0, __le32 type,
			     struct mft_attr_snapshot *snap)
{
	const struct attr_record *a;
	u16 off = 0;
	u32 len;
	u16 pairs_off;

	a = find_attr(m0, type, NULL, 0, &off);
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
	 * Symlinks, devices and other non-regular, non-directory files
	 * never begin — their reparse staging belongs to their own
	 * classes, and a transaction that cannot describe them would
	 * abort anyway. */
	if (!vol->log_writer || (!S_ISREG(mode) && !S_ISDIR(mode)))
		return 0;
	jt = kmalloc(sizeof(*jt), GFP_NOFS);
	if (!jt)
		return -ENOMEM;
	jt->vol = vol;
	jt->t = NULL;
	jt->snap_data.pairs = NULL;
	jt->snap_bmp.pairs = NULL;
	jt->snap_ok = false;
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
			err = snapshot_mft_attr(m0, AT_DATA, &jt->snap_data);
			if (!err)
				err = snapshot_mft_attr(m0, AT_BITMAP,
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
	 * A reparse point or an object ID means ntfs_delete also
	 * removes an $Extend index entry, which this class cannot
	 * describe — the unlink must proceed unjournaled. The flags
	 * are the authority for reparse points (ntfs_delete_reparse_index
	 * keys on them); the record scan covers object IDs, which have
	 * no such flag. An unreadable record is the same answer: a
	 * record that cannot be read cannot be journaled.
	 */
	if (ni->flags & FILE_ATTR_REPARSE_POINT)
		return -ENOENT;
	m = map_mft_record(ni);
	if (IS_ERR(m))
		return -ENOENT;
	if (find_attr(m, AT_REPARSE_POINT, NULL, 0, NULL) ||
	    find_attr(m, AT_OBJECT_ID, NULL, 0, NULL))
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
	err = snapshot_mft_attr(m, AT_DATA, &t->snap);
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
 * A bitmap page holds cluster_size * 8 bits.
 */
static int stage_bitmap_range(struct ntfs_journal_truncate *t,
			      u64 bit, u64 nbits, bool set)
{
	u64 page_bits = (u64)t->vol->cluster_size * 8;
	int err = 0;

	while (nbits && !err) {
		u64 page_end = (bit / page_bits + 1) * page_bits;
		u64 chunk = page_end - bit;
		u64 vcn, page_lcn;
		u16 cluster_off;

		if (chunk > nbits)
			chunk = nbits;
		err = cluster_bitmap_coords(t->vol, bit,
					 &vcn, &cluster_off, &page_lcn);
		if (err)
			break;
		err = ntfs_log_txn_set_bitmap(t->t,
					      t->vol->log_lcn_bitmap_attr,
					      vcn, cluster_off, page_lcn,
					      (u32)bit, (u32)chunk, set);
		bit += chunk;
		nbits -= chunk;
	}
	return err;
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
 * Stage the $STANDARD_INFORMATION record: the 36 bytes built from
 * current in-memory state against the begin snapshot. The $SI must
 * still sit where begin found it; anything else aborts. Stages
 * only on change.
 */
static int stage_setattr_si(struct ntfs_journal_setattr *st,
			    struct ntfs_inode *ni,
			    const struct mft_record *mrec,
			    u64 vcn, u16 cluster_off, u64 page_lcn)
{
	u8 si_new[36];
	const struct attr_record *si;
	u16 si_off = 0;

	si_build(VFS_I(ni), ni, si_new);
	if (!memcmp(si_new, st->si_old, sizeof(si_new)))
		return 0;
	si = find_attr(mrec, AT_STANDARD_INFORMATION, NULL, 0, &si_off);
	if (!si || si->non_resident || si_off != st->si_attr_off ||
	    le16_to_cpu(si->data.resident.value_offset) !=
		    st->si_value_off)
		return -ENOENT;
	return ntfs_log_txn_update_resident(
		st->t, st->vol->log_mft_data_attr,
		vcn, cluster_off, page_lcn,
		si_off, st->si_value_off,
		si_new, sizeof(si_new), st->si_old, sizeof(st->si_old));
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
