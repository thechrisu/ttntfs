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
 *     RESIDENT_ATTR_IS_INDEXED, which the live record does not (the
 *     driver never sets it). The replay's link-count bump keys on it,
 *     so without it a replayed create would land with link_count 0.
 *     The replayed record is one flag bit more correct than the live
 *     one; the live record gains the flag in a later commit, not here.
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
#include "journal.h"
#include "../logfile/ntfs_logfile.h"

struct ntfs_journal_txn {
	struct ntfs_volume *vol;
	ntfs_log_txn_t *t;
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

int ntfs_journal_create_begin(struct ntfs_volume *vol, mode_t mode,
			      struct ntfs_journal_txn **out)
{
	struct ntfs_journal_txn *jt;
	int err;

	*out = NULL;
	if (!vol->log_writer || !S_ISREG(mode))
		return 0;
	jt = kmalloc(sizeof(*jt), GFP_NOFS);
	if (!jt)
		return -ENOMEM;
	jt->vol = vol;
	jt->t = NULL;
	mutex_lock(&vol->log_txn_lock);
	err = ntfs_log_txn_begin(vol->log_writer, &jt->t);
	if (err) {
		mutex_unlock(&vol->log_txn_lock);
		kfree(jt);
		return err;
	}
	atomic_inc(&vol->log_txn_open);
	*out = jt;
	return 0;
}

void ntfs_journal_create_abort(struct ntfs_journal_txn *t)
{
	if (!t)
		return;
	(void)ntfs_log_txn_abort(t->t);
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
		 * The live $FILE_NAME carries no INDEXED flag (the driver
		 * never sets RESIDENT_ATTR_IS_INDEXED); the log copy must
		 * carry it, because the replay's link-count bump keys on
		 * it — without it a replayed create lands with
		 * link_count 0. Only the copy is touched; the live record
		 * keeps its bytes.
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
