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
#include "journal.h"
#include "../logfile/ntfs_logfile.h"

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

int ntfs_journal_create_attr(struct ntfs_journal_txn *t,
			     struct ntfs_inode *ni,
			     const struct mft_record *mrec,
			     __le32 type, const __le16 *name, u8 name_len)
{
	const struct attr_record *a;
	u16 off = 0;
	u8 *copy;
	u32 len;
	u64 vcn, page_lcn;
	u16 cluster_off;
	int err;

	if (!t)
		return -EINVAL;
	a = find_attr(mrec, type, name, name_len, &off);
	if (!a)
		return -ENOENT;
	len = le32_to_cpu(a->length);
	copy = kmalloc(len, GFP_NOFS);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, a, len);
	if (type == AT_FILE_NAME) {
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
	err = mft_coords(t->vol, ni->mft_no, &vcn, &cluster_off, &page_lcn);
	if (!err)
		err = ntfs_log_txn_create_attr(t->t, t->vol->log_mft_data_attr,
					       vcn, cluster_off, page_lcn,
					       off, copy, len);
	kfree(copy);
	return err;
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
 * bytes) in @dir_mrec's $INDEX_ROOT. Match by the whole $FILE_NAME
 * value, not by mref: a hard link in the same directory has the same
 * parent and the same target record, but a different name. The entry
 * key is the $FILE_NAME value; key length first, then bytes.
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
		if (le16_to_cpu(e->key_length) == fn_len &&
		    !memcmp((const u8 *)e + 16, fn, fn_len)) {
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
	/* The run list as it stood at begin: a copy of ni->runlist.rl
	 * up to and including its length-0 terminator. The bitmap delta
	 * is the set difference between this and the live run list. */
	struct runlist_element *old_rl;
	size_t old_count;
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

int ntfs_journal_truncate_begin(struct ntfs_volume *vol,
				struct ntfs_inode *ni,
				struct ntfs_journal_truncate **out)
{
	struct ntfs_journal_truncate *t;
	struct mft_record *m;
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
	/*
	 * Snapshot the $DATA sizes + pairs and the run list before
	 * anything mutates. The run list must be whole: a partially
	 * mapped list would miss the tail runs the shrink frees.
	 * The snapshot is taken before log_txn_lock — the same
	 * window the create begin has for record 0, and the stage
	 * re-verifies the attribute offset before staging anything.
	 */
	err = ntfs_attr_map_whole_runlist(ni);
	if (err)
		goto out_free;
	m = map_mft_record(ni);
	if (IS_ERR(m)) {
		err = PTR_ERR(m);
		goto out_free;
	}
	err = snapshot_mft_attr(m, AT_DATA, &t->snap);
	unmap_mft_record(ni);
	if (err)
		goto out_free;
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
		if (!t->old_rl) {
			err = -ENOMEM;
			goto out_free;
		}
	}
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

void ntfs_journal_truncate_abort(struct ntfs_journal_truncate *t)
{
	if (!t)
		return;
	(void)ntfs_log_txn_abort(t->t);
	atomic_dec(&t->vol->log_txn_open);
	mutex_unlock(&t->vol->log_txn_lock);
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
		err = bitmap_coords(t->vol, bit,
				    &vcn, &cluster_off, &page_lcn);
		if (err)
			break;
		err = ntfs_log_txn_set_bitmap(t->t,
					      t->vol->log_mft_bitmap_attr,
					      vcn, cluster_off, page_lcn,
					      (u32)bit, (u32)chunk, set);
		bit += chunk;
		nbits -= chunk;
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
		}
	}

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
