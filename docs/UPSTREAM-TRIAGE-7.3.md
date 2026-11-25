# Upstream triage: Linux v7.1 -> v7.3 (ntfs)

Triage of every commit between the pinned v7.1 and v7.3 in
`fs/ntfs3` (renamed to `fs/ntfs` mid-range), as recorded in GAPS.md
§5.3. 124 commits in the drift diff, plus one more recovered from a
merge aggregate (see the quota note below) — **125 items**.

## Verdicts

- **port** — the port's vendored code contains the same bug and the fix
  applies. 61 items. These are the next rounds' work, in the priority
  order of §"Porting order" below.
- **already-covered** — the port already contains the fix's substance,
  either via its own implementation or via an earlier gap fix. 13 items.
- **not-applicable** — Linux-only (VFS op signatures, `mnt_idmap`,
  folio/pagecache/bio/kmap APIs, merges, mechanical renames with no
  behaviour change, FITRIM with no FSKit path). 35 items.
- **needs-work** — a feature or a fix that needs a design first. 16 items.

## Method

Each commit's patch was read from the GitHub API (fetched 2026-11-25
into `~/workspace/ttntfs-work/upstream-scratch/commits/`, one JSON per
commit) and checked against the port tree (`core/ntfs/` for the
vendored core, `core/vfs/` for the FSKit glue). A first pass matched
removed/added lines against the port files; every commit was then
classified by hand from the patch and the port code at the cited
lines. Line numbers are the port tree at `30dc2fda` (§1.2 design).

Coverage check: all 124 drift-diff commits appear exactly once below.
One extra item, `cbe287fd65f1`, is not in the 124: the quota removal
only exists in the `dbad5db3a0bb` merge aggregate — no individual
commit touching `fs/ntfs/quota.c` is in the drift list — so it was
fetched separately from torvalds/linux and triaged here.

## Porting order (the 61 "port" items, highest first)

1. **Crash/corruption, one-liners first.** `dc09bf79b76f` (index_root
   heap OOB write in `ntfs_ir_to_ib`), `0ab6dc1ac460` + `7ed0028126b5`
   + `9969d58a9b04` + `8d139e3635c8` + `acb1095fd2db` (five bugs in the
   one function `ntfs_new_attr_flags` — port as a single commit),
   `eb67671cd814` (compressed write punches old clusters before the
   new data lands), `9692b1b4fc00` (reparse-index failure swallowed,
   symlink creation falsely succeeds), `cbe287fd65f1` (quota removal;
   the port's rw-remount path currently always fails with -EROFS).
2. **Bounds/validation.** `19cac7902a8a` + `fea9e4488f38` (mapping-pairs
   decoder: start-LCN bound, then run-end bound — port together),
   `766062a82e1c` (empty mapping pairs), `323751a604e7` +
   `cc9d09fef784` (boot-sector validation: sectors_per_cluster,
   MFT LCNs), `81684340963d` (usa_ofs), `d97a36bae86a` (MFT refs),
   `a83e82b0ec3a` + `ffc755828de1` (non-resident attribute validation),
   `8bed376124ab` (runlist realloc), `c8504fc1245f` ($AttrDef walk),
   `b8d6c528e9d5` (resident attr in runlist map), `650a2751c9ae` (EA
   size), `5b6eedd7cc29` (index_block fixup).
3. **Error propagation.** `56cc42a7b387` (ntfs_attr_readall),
   `8efe00b098b5` + `ba1b61ddaa76` (error codes collapsed to -EIO /
   literal -1), `ada728801999` (-ERANGE), `1923eeffa63e` (folio
   errors), `3d3de2aee17d` (DT_UNKNOWN), `ea6a67ef6445` ($LX* xattrs),
   `0ebe8f625ab0` (attrlist on inactive superblock),
   `41e1cf4ec10d` (EA stream rewrite), `772e4c8f2555` (empty EA pair),
   `ac727d86fb84` (HasEA on failure), `60038154b40e` (ctime on
   failure), `4dc8f4ee2d46` (fallocate EINTR), `8c5dc7587fdd` (EINTR
   as corruption), `607a9478833d` (dio zero-range).
4. **Locking/races (glue + core).** `88496c4ac5a6` + `81fe702ff176`
   (stale runlist dereference), `9cc5761b8f28` + `67aded1da114` +
   `5c32f0b8d791` (invalidate_lock / size extension),
   `4e646ecd4475` (stale page cache on shrink), `5f2a22b36fe3` +
   `be9e89ccb8e5` (free-cluster accounting), `631946431ddc` (MFT
   record accounting), `0e4c83990541` (sync_fs clears dirty on
   errors — journal-adjacent, see JOURNAL-DESIGN.md),
   `4d730a4dd78c` (chmod mode over mount masks), `71feaa7686fb`
   (windows_names ordering), `f72df3a4c33b` (system files
   immutable), `06769b8f23b4` (hole runlist leak),
   `cf06dcd57284` (kmap leak in MFT writeback),
   `98716c9fce21` (decompress off-by-one), `b54559689a00`
   (initialized size / page state), `3e497af63725` (compress ctx
   ENOMEM), `523307b8516f` (RECALL_ON_OPEN dropped).
5. **Performance-only ports (lowest).** `33200693eb22` (reuse
   compression context), `8cd2bf1d7af9` (reuse output workspace),
   `5b676a77f3e4` (skip reads for full overwrites). `f2b2aeeaadce`
   (large pages) is listed here, not with the correctness items:
   the port hard-codes 4K pages while macOS on Apple Silicon uses
   16K pages, so it needs a page-size design decision first, not a
   straight port.

## Needs-work (16)

Features and design-first items, in rough size order:

- **WOF (Windows Overlay Filter) — 9 commits, one feature.**
  `2bef615ebf28` (non-resident decompression), `a7b842f182d1`
  (resident), `760c9d271d40` (parse REPARSE_TAG_WOF),
  `41a52ba4a5fe` (chunk reads outside the lock), `f2240a2ff4db`
  (codec interface), `7ddb3fef3532` (LZX/XPRESS codec ops),
  `c34e33a60fc7` (LZNT1 codec ops), `f39cd3f7fcaf` (LZX/XPRESS
  decompressors from ntfs-3g-system-compression), `d38ce59d16d5`
  (config option). The port has no `wof.c`, no codec interface and
  no lib/ decompressors; WOF-compressed files are currently
  unreadable. This is the biggest remaining feature after §1.2.
- **Runlist locking — `91709ba5d6d7`.** No `_locked` plumbing exists
  in the port at all. Design question: how `locked_ni` /
  `defer_attrlist` threads through the FSKit-vfs write paths, since
  the upstream `ntfs_write_cb` / `ntfs_trim_prealloc` callers live
  in the port's `core/vfs/` glue, not core.
- **$MFT/$ATTRIBUTE LIST repack — `b1d732e62a5b`.** Needs a new
  function, contiguous-reserve allocation, and a buffered $MFT-list
  rewrite with rollback; design question is whether the repack's
  buffered pread/pwrite can run while holding the runlist lock and
  how the contiguous-reserve interacts with `ntfs_cluster_alloc`'s
  existing `is_contig` parameter.
- **Dynamic MFT tail reservation — `6d8c197c9992`.** 334-line mft.c
  refactor (RESERVED 64 / record-400 cap replaced by an in-memory
  tail reserve). Land `631946431ddc` (MFT accounting) before or
  with it — they touch the same allocator.
- **Index root relocation — `9badcfb91bc3`.** Design question is
  whether the port wants index roots in extent records at all.
- **Native-symlink mount option — `115446b06fb7`.** The port
  already decodes and creates native symlinks via its own
  implementation (see already-covered below); only the
  `native_symlink=` option is missing — the port always translates
  absolute targets volume-relative (upstream `rel` behaviour) with
  no `raw` mode.
- **mrec_lock ABBA in rename — `eb94f5a41a19`.** Both sides of the
  ABBA exist in the port; needs the upstream reordering designed
  in (rename: old inode -> old dir -> target inode -> target dir;
  fsync: child -> parent).
- **Resident iomap reads under mrec_lock — `d9e00c457d4a`.** The
  port has the equivalent unprotected resident-read path; needs
  design for how long to hold the lock in the folio-based read
  (write side uses trylock).

## Already-covered (13) — worth knowing

- **Native Windows symlinks are already implemented independently.**
  `2b58f93a1313` (absolute paths), `517cd625ad79` (relative paths)
  and `d8f1df2e133f` (creation) are all covered by the port's own
  `ntfs_make_symlink()` / `ntfs_reparse_set_win_symlink()` — the
  port diverged from upstream here before 7.2, and its decoder
  handles absolute and relative targets plus mount-point reparse
  points. `121dedb3b666` and `05a5ff86a7f1` are covered the same
  way. Only the mount option (`115446b06fb7`, above) is missing.
- **`229e8188307b`** ($MFTMirr write offset) — already fixed in the
  port with a PORT comment (UPSTREAM-BUGS finding 15).
- **`a79899ca38af`** (record-size UB) — the §5.2 bound caps the
  shift at 16, so the sign-bit UB is unreachable.
- **`6faa235a649e`** (bi_sector) — the port already fixed this at
  the source with a PORT comment noting upstream was wrong.
- **`0c32a42fd96a` / `d7aa04984f12` / `fc440366c470`** (inode-init
  failures) — the port's `ntfs_iget()` already unhashes, returns
  `ERR_PTR(err)` and drops failed inodes.

## Not-applicable (35) — why

- 11× `mnt_idmap` VFS churn (`03a56ee3418b`, `16d90b57d843`,
  `566707035a6e`, `59ffddf0fbeb`, `6bd532e33875`, `737362c7012a`,
  `7d0db72041b7`, `8ee61322bb53`, `c3150ded7bc0`, `e2f0d6d4569d`,
  `eb50cbe14ff9`): `const mnt_idmap*` signature changes. FSKit has
  no idmapped mounts; the port has no `mnt_idmap`.
- 6× merge commits (`1c3e8cef79ea`, `9ea8d6197d9f`, `ab5ed08f2d83`,
  `61c03dfde854`, `bfda5a01aa99`, `dbad5db3a0bb`): containers.
- VFS/iomap/bio/mm-only: `b2f1e6301efa`, `a380b9693c7a`,
  `044472d5ee7d`, `7a7bf7551624`, `0fecc393f206`, `77dc384207d5`.
- Mechanical, no behaviour change: `3a2c4d55e32a` (kmalloc_obj),
  `34d22551add7` / `eec908b5c0d3` (kmemdup), `5e150dc65d87`
  (NULL checks), `7266767f67e0` (WSL cleanup), `f98d80d51777`
  (inline), `a3976da121ae` / `f6ce4cbcc53c` (folio index),
  `8a81803f8750` (zero_user_segment), `6e03fbd5f772` (kmap — the
  port shims it), `a8a4681f2bfc` (bio per write unit).
- `03c6ecc4b4b1` (FITRIM): no FSKit path for TRIM (GAPS.md §2.3).

## Master table

| SHA | Subject | Verdict | Reason | Port evidence |
|---|---|---|---|---|
| `03a56ee3418b` | fs: port ->get_acl() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* signature change; FSKit has no idmapped mounts, port has no mnt_idmap | core/ntfs/ea.c, ea.h (1-line signature change) |
| `03c6ecc4b4b1` | ntfs: fix FITRIM range alignment | **not-applicable** | Pre-fix code exists but FITRIM has no FSKit path - ntfs_trim_fs has no caller in the port | core/ntfs/bitmap.c:67,76-80; no caller in core/ |
| `044472d5ee7d` | iomap: consolidate bio submission | **not-applicable** | iomap bio-submission consolidation; aops.c/inode.c only, no port equivalent | Linux iomap/bio only |
| `05a5ff86a7f1` | ntfs: fix incorrect size of symbolic link | **already-covered** | Symlink size already set from target: dir-record branch sets i_size = strlen(ni->target), and getattr sizes via link_target() | core/vfs/inode.c:1041; core/vfs/api.c:229 |
| `06769b8f23b4` | ntfs: fix hole runlist memory leak in insert range error path | **port** | map_whole_runlist error path returns without freeing hole_rl; later paths in same function do free it | core/ntfs/attrib.c:5271-5274 vs :5276,5284 |
| `0ab6dc1ac460` | ntfs: move attribute payload before shrinking its record | **ported** | Ported 2026-11-25 (with 7ed0028126b5, 9969d58a9b04, 8d139e3635c8, acb1095fd2db, one commit): payload moved before the shrink; regression test test_attr_flags (core/tests/test_compress.c) - the shrink/re-enable case fails without the fix (lookup -95 after remount, ntfsck exit 4). | core/ntfs/ea.c:719; no old_arec_size/memmove in ntfs_new_attr_flags |
| `0c32a42fd96a` | ntfs: unhash failed inode reads | **already-covered** | Port's ntfs_iget() already calls remove_inode_hash(vi) on the init-failure path before dropping the inode | core/vfs/inode.c:181 |
| `0e4c83990541` | ntfs: do not mark the volume clean in sync_fs when errors were recorded | **port** | ntfs_sync_fs clears VOLUME_IS_DIRTY unconditionally, unlike put_super/remount which guard on NVolErrors | core/ntfs/super.c:2026 |
| `0ebe8f625ab0` | ntfs: fail attrlist updates when the superblock is inactive | **port** | No SB_ACTIVE guard in ntfs_attrlist_update; the SB_ACTIVE idiom already exists in the port, so the guard ports directly | core/ntfs/attrlist.c:57; cf. core/vfs/inode.c:2767, core/vfs/super_glue.c:486 |
| `0fecc393f206` | ntfs: take invalidate_lock in ntfs_filemap_page_mkwrite() | **not-applicable** | Port has no page_mkwrite handler at all (no Linux mmap fault path in the FSKit glue) | n/a - no page_mkwrite in core/vfs/file.c |
| `115446b06fb7` | ntfs: add native_symlink mount option | **needs-work** | No native_symlink= option exists; port always translates absolute targets volume-relative (upstream rel behaviour) with no raw mode | n/a - no native_symlink in core/ntfs/super.c or core/ntfs/volume.h |
| `121dedb3b666` | ntfs: simplify ntfs_reparse_set_native_symlink() | **already-covered** | Port's equivalent writer never had the strlen loop; / to \ is done inline in the copy loop | core/ntfs/reparse.c:827 |
| `16d90b57d843` | fs: port xattr to const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* signature change on xattr handlers | core/ntfs/ea.c |
| `1923eeffa63e` | ntfs: propagate folio errors | **port** | Pre-fix line verbatim in the FGP_CREAT branch: __filemap_get_folio() failure is replaced with -ENOMEM instead of PTR_ERR(folio) (the adjacent partial-page branch already propagates correctly) | core/vfs/inode.c:3631 |
| `19cac7902a8a` | ntfs: bound the free-cluster bitmap scan to the volume | **port** | i_size unclamped to $Bitmap size, and mapping-pairs decoder has no lcn >= nr_clusters check at all | core/ntfs/lcnalloc.c:301; core/ntfs/runlist.c:879-894 (no nr_clusters in file) |
| `1c3e8cef79ea` | Merge tag 'vfs-7.3-rc1.misc' of git://git.kernel.org/pub/scm/linux/kernel/git/vfs/vfs | **not-applicable** | Merge commit (vfs-7.3-rc1.misc); contents are the individual commits in this table | container |
| `229e8188307b` | ntfs: fix $MFTMirr write offset when it spans multiple folios | **already-covered** | Port already adds (folio->index << PAGE_SHIFT) to the mirror base; no cluster_size_mask truncation | core/ntfs/mft.c:512-513 (+ PORT comment, UPSTREAM-BUGS finding 15) |
| `2b58f93a1313` | ntfs: support following Windows native symlink with absolute paths | **already-covered** | Port's own decoder handles IO_REPARSE_TAG_SYMLINK/MOUNT_POINT absolute targets (prefix/drive-letter stripping) and link_target() rewrites them volume-relative | core/ntfs/reparse.c:194; core/vfs/api.c:624 |
| `2bd734ee2cc8` | ntfs: punch all-zero compressed blocks | **already-covered** | Port already punches the block when allocated, via its own cb_is_allocated guard (upstream punches unconditionally) | core/ntfs/compress.c:1418-1432 |
| `2bef615ebf28` | ntfs: add non-resident WOF decompression | **needs-work** | Whole WOF feature absent: no wof.c, no NInoWofCompressed, no WOF routing in read path | n/a (no core/ntfs/wof.c) |
| `323751a604e7` | ntfs: reject invalid sectors_per_cluster in the boot sector | **port** | Old range test still accepts non-power-of-two values (e.g. 3, 5, 6); only 0 is rejected - the is_power_of_2 check is absent | core/ntfs/super.c:528-529; :639 |
| `33200693eb22` | ntfs: reuse the compression context during writes | **port** | Perf-only: context still kvzalloc'd per sub-block inside ntfs_compress_block, no caller-owned ctx parameter | core/ntfs/compress.c:1097,1115 |
| `34d22551add7` | ntfs: dir: use kmemdup() instead of kmalloc() and memcpy() | **not-applicable** | Mechanical kmalloc+memcpy to kmemdup, no behaviour change; port's readdir has no such pair | n/a |
| `3a2c4d55e32a` | treewide: refresh kmalloc_obj() conversions | **not-applicable** | Mechanical treewide rename, no behaviour change; port has no kmalloc_obj | n/a |
| `3d3de2aee17d` | ntfs: return DT_UNKNOWN on inode lookup failure in readdir | **port** | Pre-fix code verbatim: ntfs_reparse_tag_dt_types() still returns PTR_ERR(vi) (a negative errno) as an unsigned int d_type | core/ntfs/reparse.c:300 |
| `3e497af63725` | ntfs: propagate compression context allocation errors | **port** | ntfs_compress_block still returns unsigned int, so return -ENOMEM truncates to a huge positive size; caller only tests !sz | core/ntfs/compress.c:1097,1115-1116 |
| `41a52ba4a5fe` | ntfs: read WOF chunks outside the decompression lock | **needs-work** | WOF-only locking rework; wof.c does not exist in the port | n/a (no core/ntfs/wof.c) |
| `41e1cf4ec10d` | ntfs: rewrite EA stream before updating metadata | **port** | Old two-step path: writes compacted $EA + $EA_INFORMATION, then appends new entry at ea_info_qsize; no whole-stream rewrite, no restore on failure | core/ntfs/ea.c:270-275, :330 |
| `4d730a4dd78c` | ntfs: respect per-file chmod mode over mount masks | **port** | ntfs_ea_get_wsl_inode has old signature (no has_lxmod), returns -EIO if $LXUID/$LXGID absent; masks applied unconditionally in FSKit glue | core/ntfs/ea.c:400; core/vfs/file.c:328-330, core/vfs/inode.c:878 |
| `4dc8f4ee2d46` | ntfs: handle signal interruption in fallocate | **port** | Port has a fallocate path and signal_pending checks, but both jump to out with err possibly 0, so interruption returns success | core/ntfs/attrib.c:5593,5614; no signal_out/-EINTR in ntfs_attr_fallocate (:5473) |
| `4e646ecd4475` | ntfs: drop stale page-cache when shrinking a non-resident attr | **port** | ntfs_non_resident_attr_shrink updates sizes with no cache drop; truncate_inode_pages exists in the port, so the call is portable | core/ntfs/attrib.c:4189; cf. core/ntfs/logfile.c:680, core/vfs/api.c:1576 |
| `517cd625ad79` | ntfs: support following Windows native symlink with relative paths | **already-covered** | Port's decoder handles relative native symlinks (SYMLINK_FLAG_RELATIVE check, UTF-16 to NLS conversion, separator normalisation) for file and directory records | core/ntfs/reparse.c:255; core/vfs/inode.c:872 |
| `523307b8516f` | ntfs: preserve RECALL_ON_OPEN on WSL special-file reparse points | **port** | __ntfs_create reloads ni->flags = fn->file_attributes, dropping the RECALL_ON_OPEN set just before, so valid_reparse_data later rejects the reparse point | core/vfs/namei.c:663 (set at :546-547; required at core/ntfs/reparse.c:127) |
| `566707035a6e` | fs: port ->mkdir() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->mkdir() | VFS op only; port has no namei.c |
| `56cc42a7b387` | ntfs: return errors from ntfs_attr_readall | **port** | ntfs_attr_readall returns NULL on every failure; all callers test !ptr and lose the errno | core/ntfs/attrib.c:5197-5240; core/ntfs/ea.c:123,133; core/ntfs/index.c:1011-1013 |
| `59ffddf0fbeb` | fs: port acl to const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ACL paths | core/ntfs/ea.c, ea.h |
| `5b676a77f3e4` | ntfs: skip reads for full compression unit overwrites | **port** | Perf-only: ntfs_compress_write always read_mapping_folios every page of the unit, no full-overwrite shortcut | core/ntfs/compress.c:1560 |
| `5b6eedd7cc29` | ntfs: avoid calling post_write_mst_fixup() for invalid index_block | **port** | ntfs_ib_write returns early on pre_write_mst_fixup failure with no restore, and the caller still calls post_write_mst_fixup on any error | core/ntfs/index.c:131-150; :181 |
| `5c32f0b8d791` | ntfs: use pagecache_isize_extended() on size extension | **port** | Port still uses the pre-fix pattern (truncate_setsize + ntfs_vfs_zero_range only when old_size % PAGE_SIZE != 0); pagecache_isize_extended() exists nowhere in the port | core/vfs/file.c:243 |
| `5e150dc65d87` | ntfs: reparse: remove redundant NULL checks before kvfree() | **not-applicable** | Mechanical NULL-check removal, no behaviour change; port still has the checks, harmlessly | core/ntfs/reparse.c:287 |
| `5f2a22b36fe3` | ntfs: skip free cluster decrement when rollback fails | **port** | ntfs_dec_free_clusters(vol, delta) runs unconditionally after the failed-rollback branch, subtracting a negative delta | core/ntfs/lcnalloc.c:1048 |
| `60038154b40e` | ntfs: do not update ctime when setxattr fails | **port** | ntfs_setxattr unconditionally sets ctime and marks dirty on the out: path regardless of err | core/ntfs/ea.c:837-838 |
| `607a9478833d` | ntfs: treat any nonzero dio zero-range return as an error | **port** | Check is if (err > 0), but the port's ntfs_dio_zero_range returns only 0 or negative errno, so a failed zero is silently ignored | core/ntfs/attrib.c:5590; core/vfs/aops.c:486-513 |
| `61c03dfde854` | Merge tag 'ntfs-for-7.2-rc3' of git://git.kernel.org/pub/scm/linux/kernel/git/linkinjeon/ntfs | **not-applicable** | Merge commit (ntfs-for-7.2-rc3); contents are the individual commits in this table | container |
| `631946431ddc` | ntfs: account for MFT records added during allocation | **port** | Old accounting throughout: bitmap-extend credits a fixed 64, record-free credits unconditionally, inc/dec still gated on FreeClusterKnown, statfs exposes raw count | core/ntfs/mft.c:1533,2687; core/ntfs/volume.h:255-268; core/ntfs/super.c:2247-2250,2317 |
| `650a2751c9ae` | ntfs: validate final EA attribute size | **port** | Only new_ea_size is bounds-checked, after the old entry is already removed; no pre-removal check of ea_info_qsize - ea_size + new_ea_size | core/ntfs/ea.c:316 |
| `67aded1da114` | ntfs: fix race between fallocate and mmap reads | **port** | Pre-fix code verbatim: filemap_invalidate_lock is taken only for FALLOC_FL_PUNCH_HOLE; plain allocation leaves the unzeroed-cluster window open to mmap faults | core/vfs/file.c:509 |
| `6bd532e33875` | fs: port ->symlink() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->symlink() | VFS op only; port has no namei.c |
| `6d8c197c9992` | ntfs: use dynamic MFT tail reservation | **needs-work** | 334-line mft.c refactor: replaces RESERVED 64 / record-400 cap with in-memory tail reserve, new mft_data_vcn allocator parameter and bootstrap-record reuse | core/ntfs/mft.c:960,1064 |
| `6e03fbd5f772` | ntfs: fix kmap_local_page() usage in compress | **not-applicable** | Linux highmem/kmap bug; port shims kmap_local_page to return folio data and kunmap_local to a no-op | platform/include/linux/highmem.h:11-14 |
| `6faa235a649e` | ntfs: compute bi_sector in 512-byte units | **already-covered** | Port already fixed this at the source: NTFS_B_TO_SECTOR and ntfs_bytes_to_sector() both shift by SECTOR_SHIFT (512-byte units), with a PORT comment noting upstream was wrong; only the dead nr_blocks block in ntfs_read_inode_mount remains | core/ntfs/ntfs.h:91; core/ntfs/ntfs.h:176 |
| `71feaa7686fb` | ntfs: apply Windows name checks only with windows_names | **port** | Pre-fix order verbatim: ntfs_check_bad_char() runs before the NVolCheckWindowsNames check, so : etc. are rejected even without windows_names | core/vfs/namei.c:65 |
| `7266767f67e0` | ntfs: clean up target name conversion for WSL symlinks | **not-applicable** | Behaviour-preserving cleanup only; port still uses the old __le16 signature with the ntfs_ucstonls round-trip, which works identically | core/ntfs/reparse.c:882 |
| `737362c7012a` | fs: port ->set_acl() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->set_acl() | core/ntfs/ea.c, ea.h |
| `760c9d271d40` | ntfs: parse REPARSE_TAG_WOF | **needs-work** | Tag constant exists but no WOF case in reparse validation/parsing and no NInoWofCompressed flag; prerequisite of the WOF feature | core/ntfs/layout.h:2255; core/ntfs/reparse.c:91-131 |
| `766062a82e1c` | ntfs: reject invalid empty mapping pairs | **port** | No empty-mapping-pairs validation before the decode loop; an empty array is silently accepted regardless of highest VCN/size | core/ntfs/runlist.c:772-775 (no !*buf check anywhere) |
| `772e4c8f2555` | ntfs: remove empty EA attribute pair | **port** | Removing the last xattr just goto out, leaving an empty $EA + $EA_INFORMATION pair that ntfsck reports corrupt | core/ntfs/ea.c:279-282 |
| `77dc384207d5` | ntfs: avoid self-deadlock during inode eviction | **not-applicable** | Linux ntfs_writepages/iomap-writeback construct; port has no ntfs_writepages (per-folio writeback only) and ntfs_drop_big_inode() is a stub noting the kernel writeback race does not exist in the port | core/vfs/inode.c:380; core/ntfs/mft.c:2956 |
| `7a7bf7551624` | ntfs: convert iomap ops to ->iomap_next() | **not-applicable** | iomap ops converted to ->iomap_next(); iomap.c only | Linux iomap only |
| `7d0db72041b7` | fs: port ->mknod() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->mknod() | VFS op only; port has no namei.c |
| `7ddb3fef3532` | ntfs: implement codec ops for LZX and XPRESS | **needs-work** | LZX/XPRESS codec-ops feature; no lib decompressors and no codec interface in the port | n/a (no core/ntfs/lib/, no ntfs_codec.h) |
| `7ed0028126b5` | ntfs: fix resident conversion in ntfs_new_attr_flags | **ported** | Ported 2026-11-25 (same commit): resident $DATA converted via ntfs_attr_make_non_resident before the flag is set; regression test test_attr_flags - the resident-conversion case fails without the fix (no compressed flag, lookup -95 after remount, ntfsck exit 4). | core/ntfs/ea.c:692 |
| `81684340963d` | ntfs: validate usa_ofs before preserving the update sequence number | **port** | usn read straight from unvalidated on-disk m->usa_ofs with no bounds/evenness check | core/ntfs/mft.c:2454 |
| `81fe702ff176` | ntfs: avoid stale runlist element dereference in MFT writeback | **port** | rl is looked up under runlist.lock, lock dropped, then rl->length/rl->vcn dereferenced later for the folio-size decision | core/ntfs/mft.c:2759,2845-2848,2875 |
| `88496c4ac5a6` | ntfs: avoid stale runlist element dereference in fallocate | **port** | Runlist element is borrowed under the read lock, lock dropped, then rl->lcn/length/vcn read - exact pre-fix pattern, UAF if a writer replaces the array | core/ntfs/attrib.c:5558-5572 |
| `8a81803f8750` | ntfs: Use zero_user_segment() in handle_bounds_compressed_page() | **not-applicable** | Linux highmem zeroing API; port's memset on shimmed page_address is correct with no highmem | core/ntfs/compress.c:105-118 |
| `8bed376124ab` | ntfs: harden runlist realloc size calculations | **port** | ntfs_rl_realloc still does unchecked old_size * sizeof(*rl) / new_size * sizeof(*rl); no check_mul_overflow, no shared gfp helper - the §5.2 hardening did not touch this function | core/ntfs/runlist.c:74-94 |
| `8c5dc7587fdd` | ntfs: ignore interrupted inode reads as corruption | **port** | All four sites log/set volume errors unconditionally on EINTR/ERESTARTSYS; the inode.c hunks live in the port's vfs/inode.c, not in a missing file | core/ntfs/mft.c:216; core/vfs/inode.c:1236,1459-1466,1718-1724 |
| `8cd2bf1d7af9` | ntfs: reuse compression output workspace across write units | **port** | Perf-only: ntfs_write_cb still kcalloc + alloc_pages + vmaps a fresh output workspace per compression unit | core/ntfs/compress.c:1350-1367 |
| `8d139e3635c8` | ntfs: fix incorrect MFT record pointer passed to ntfs_attr_record_resize | **ported** | Ported 2026-11-25 (same commit): resize uses ctx->mrec, not the base record m. No behavioural test - only reachable for an attribute in an extent record, which the flag toggle cannot produce through the test API; verified by the function being byte-identical to upstream v7.3. | core/ntfs/ea.c:719 |
| `8ee61322bb53` | fs: port ->rename() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->rename() | VFS op only; port has no namei.c |
| `8efe00b098b5` | ntfs: preserve error code in ntfs_resident_attr_record_add() | **port** | put_err_out collapses every failure to -EIO, defeating the -ENOSPC resident-to-non-resident fallback in ntfs_attr_add | core/ntfs/attrib.c:2372 (function at :2264) |
| `91709ba5d6d7` | ntfs: protect runlist updates with the runlist lock | **needs-work** | No _locked plumbing exists anywhere in the port (locked_ni, ntfs_attr_update_mapping_pairs_locked, ntfs_attrlist_update_locked all absent); shrink/expand call lock-requiring helpers without the runlist lock | core/ntfs/attrib.c:4061 (ntfs_non_resident_attr_shrink, no runlist lock); zero _locked/locked_ni hits |
| `9692b1b4fc00` | ntfs: propagate reparse index insertion failure | **port** | Pre-fix code verbatim: set_reparse_index() return value is discarded (if (... && oldsize > 0)), so a failed index insert still returns 0 and symlink creation falsely reports success | core/ntfs/reparse.c:618 |
| `98716c9fce21` | ntfs: fix off-by-one page overflow in ntfs_decompress() | **port** | Pre-fix > still present; upstream changes it to >=, allowing a one-page overrun at the sub-block end | core/ntfs/compress.c:356 |
| `9969d58a9b04` | ntfs: fix resource leak in ntfs_new_attr_flags | **ported** | Ported 2026-11-25 (same commit): no-change early return now goes through err_out (unmap_mft_record + ntfs_attr_put_search_ctx). No behavioural test - the map pin count is not observable from the test API; verified by the byte-identical diff to upstream v7.3. | core/ntfs/ea.c:682-683 |
| `9badcfb91bc3` | ntfs: allow index root relocation | **needs-work** | Old ENOSPC-only relocation path; no ntfs_ir_move_to_base, no ntfs_inode_free_empty_extents, INDEX_ROOT still pinned to base record - design question is whether the port wants roots in extent records at all | core/ntfs/index.c:1243; core/vfs/inode.c:3408 |
| `9cc5761b8f28` | ntfs: take invalidate_lock in ntfs_setattr_size() | **port** | Port's ntfs_setattr_size() resizes (truncate_setsize + ntfs_truncate_vfs) with no filemap_invalidate_lock, so a concurrent fault can resolve a VCN against the transient runlist | core/vfs/file.c:224 |
| `9ea8d6197d9f` | Merge tag 'vfs-7.3-rc1.iomap' of git://git.kernel.org/pub/scm/linux/kernel/git/vfs/vfs | **not-applicable** | Merge commit (vfs-7.3-rc1.iomap); contents are the individual commits in this table | container |
| `a380b9693c7a` | ntfs: drop redundant S_IFDIR from mkdir | **not-applicable** | VFS mkdir cleanup (redundant S_IFDIR); namei.c only | VFS op only |
| `a3976da121ae` | ntfs: Remove use of __folio_index in handle_bounds_compressed_page() | **not-applicable** | Linux folio-API rename with no behaviour change; port still uses __folio_index there | core/ntfs/compress.c:129 |
| `a79899ca38af` | ntfs: fix undefined behavior in mft/index record size calculation | **already-covered** | Literal 1 << remains, but the §5.2 bound caps the shift at 16 (1<<9-1<<16), so the sign-bit UB the fix targets is unreachable | core/ntfs/super.c:699,705,755,761 |
| `a7b842f182d1` | ntfs: support resident WOF decompression | **needs-work** | Resident-WOF extension of the absent WOF feature; wof.c does not exist in the port | n/a (no core/ntfs/wof.c) |
| `a83e82b0ec3a` | ntfs: validate non-resident attribute offsets | **port** | Same inline validator checks only mapping_pairs_offset bounds; no name_offset/name_end checks, no compressed_size room check | core/ntfs/attrib.c:771-780 |
| `a8a4681f2bfc` | ntfs: submit one bio per compressed write unit | **not-applicable** | Linux bio-submission rework; port has no bio and already does a single direct write per unit | core/ntfs/compress.c:1483 |
| `ab5ed08f2d83` | Merge tag 'vfs-7.3-rc1.lookup' of git://git.kernel.org/pub/scm/linux/kernel/git/vfs/vfs | **not-applicable** | Merge commit (vfs-7.3-rc1.lookup); contents are the individual commits in this table | container |
| `ac727d86fb84` | ntfs: leave HasEA flag untouched on setxattr failure | **port** | ntfs_set_ea out: path updates HasEA unconditionally, so a failed setxattr hides existing on-disk EAs | core/ntfs/ea.c:345-349 |
| `acb1095fd2db` | ntfs: fix memmove overlap in ntfs_new_attr_flags | **ported** | Ported 2026-11-25 (same commit): name moved after mapping pairs when the record shrinks while payload offsets increase. No behavioural test - the trigger is mathematically unreachable via the flag toggle (record changes by exactly +8/-8 for an empty file); verified by the byte-identical diff to upstream v7.3. | core/ntfs/ea.c:644-775 (no memmove in ntfs_new_attr_flags) |
| `ada728801999` | ntfs: return -ERANGE for undersized xattr buffer | **port** | Undersized system.dos_attrib / system.ntfs_attrib[_be] buffers return -ENODATA instead of -ERANGE | core/ntfs/ea.c:613,626 |
| `b1d732e62a5b` | ntfs: repack $MFT/$ATTRIBUTE LIST | **needs-work** | Port still has the old truncate-to-0 fallback; repack needs a new function, contiguous-reserve allocation, and buffered $MFT-list rewrite with rollback | core/ntfs/attrlist.c:74-81; no ntfs_attrlist_repack / NTFS_MAX_ATTR_LIST_SIZE anywhere |
| `b2f1e6301efa` | Remove excl arg to ->create inode_operation | **not-applicable** | VFS ->create op signature change (excl arg removed); port has no namei.c | VFS op only |
| `b54559689a00` | ntfs: fix initialized size and page state after compressed writes | **port** | Initialized-size part covered (1633), but the !copied -> -EFAULT check is missing and pages are marked uptodate unconditionally even on error | core/ntfs/compress.c:1602-1611 |
| `b8d6c528e9d5` | ntfs: fix WARN_ON for resident attribute in ntfs_map_runlist_nolock() | **port** | Crafted image with a resident attr where non-resident is expected still hits WARN_ON and then dereferences non-resident fields | core/ntfs/attrib.c:177 |
| `ba1b61ddaa76` | ntfs: return real error from ntfs_non_resident_attr_record_add() | **port** | put_err_out returns literal -1, propagated as -EPERM to userspace | core/ntfs/attrib.c:2511 (function at :2385) |
| `be9e89ccb8e5` | ntfs: only count successfully cleared runs when freeing clusters | **port** | Pre-fix if/else: a failed ntfs_bitmap_clear_run still adds the run length to nr_freed whenever the error-bookkeeping condition is false | core/ntfs/lcnalloc.c:55-58 |
| `bfda5a01aa99` | Merge tag 'ntfs-for-7.3-rc4' of git://git.kernel.org/pub/scm/linux/kernel/git/linkinjeon/ntfs | **not-applicable** | Merge commit (ntfs-for-7.3-rc4); contents are the individual commits in this table | container |
| `c3150ded7bc0` | fs: port ->getattr() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->getattr() | core/ntfs/inode.h |
| `c34e33a60fc7` | ntfs: define LZNT1 codec ops under transparent codec interface | **needs-work** | Codec-interface refactor; port still calls ntfs_decompress/ntfs_compress_block directly with no ops table | core/ntfs/compress.c:791,1381 |
| `c8504fc1245f` | ntfs: bound $AttrDef table walk to the loaded table size | **port** | Walk bounds only the entry start, not the whole 160-byte entry; loader also accepts any i_size > 0 instead of requiring >= one entry | core/ntfs/attrib.c:1599-1600; core/ntfs/super.c:1340 |
| `cc9d09fef784` | ntfs: reject invalid MFT LCNs from boot sector | **port** | MFT/MFTMirr LCNs still decoded into s64 ll and compared signed, so a high-bit LCN wraps negative and passes the bounds check | core/ntfs/super.c:616,789-799 |
| `cf06dcd57284` | ntfs: fix kmap_local leak in write_mft_record_nolock() error paths | **port** | Port has the same kmap pattern: pre_write_mst_fixup failure and bio_add_folio failure both jump out without kunmap_local; unmap only happens on the success path | core/ntfs/mft.c:561,599,614 |
| `d38ce59d16d5` | ntfs: add WOF compression config option | **needs-work** | Kconfig gate for the absent WOF feature; port has no such option or WOF code to gate | n/a |
| `d7aa04984f12` | ntfs: return errors from inode initialization | **already-covered** | Port's ntfs_iget() already returns ERR_PTR(err) for any init error, not just -ENOMEM | core/vfs/inode.c:175 |
| `d8f1df2e133f` | ntfs: support creating Windows native symlinks | **already-covered** | Native symlink creation already exists via the port's own ntfs_reparse_set_win_symlink() (per-link Windows-safety rule instead of a mount option); only the symlink= option itself is missing | core/ntfs/reparse.c:827; core/vfs/namei.c:537 |
| `d97a36bae86a` | ntfs: sanitize MFT references returned from ntfs_lookup_inode_by_name() | **port** | All four return paths return the raw on-disk mref with no IS_ERR_MREF sanitization to ERR_MREF(-EIO) | core/ntfs/dir.c:185,283,459,582 |
| `d9e00c457d4a` | ntfs: serialize resident iomap reads with mrec_lock | **needs-work** | Port has the equivalent unprotected resident-read path (ntfs_read_folio_resident walks the MFT record via ntfs_attr_lookup with no mrec_lock while writers memmove under it); needs design for how long to hold the lock in the folio-based read (write side uses trylock) | core/vfs/aops.c:183; core/vfs/aops.c:388 |
| `dbad5db3a0bb` | Merge tag 'ntfs-for-7.2-rc1' of git://git.kernel.org/pub/scm/linux/kernel/git/linkinjeon/ntfs | **not-applicable** | Merge commit (ntfs-for-7.2-rc1); contents are the individual commits in this table - except the quota removal, which has no individual commit in the 124 (see cbe287fd65f1 below) | container |
| `dc09bf79b76f` | ntfs: Fix index_root heap OOB write in ntfs_ir_to_ib() | **ported** | ntfs_ir_to_ib memcpys all root entries into the fresh index block with no capacity check against allocated_size - entries_offset. Ported 2026-11-25: verbatim check in ntfs_ir_to_ib(); regression test test_index_root_oversized_reparent (core/tests/test_bigdir.c) + fixture patcher tools/mkfixtures/patch_ib_size.py. | core/ntfs/index.c:1039-1070 |
| `e2f0d6d4569d` | fs: port ->setattr() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->setattr() | core/ntfs/inode.h |
| `ea6a67ef6445` | ntfs: reject unprivileged writes to reserved $LX* xattrs | **port** | No reserved-name check in ntfs_setxattr; any caller can write $LXUID/$LXGID/$LXMOD/$LXDEV (only the privilege predicate needs an FSKit equivalent for capable(CAP_SYS_ADMIN)) | core/ntfs/ea.c:773 |
| `eb50cbe14ff9` | fs: port ->create() to pass const mnt_idmap | **not-applicable** | Linux VFS idmapped-mount churn: const mnt_idmap* on ->create() | VFS op only; port has no namei.c |
| `eb67671cd814` | ntfs: write compressed data before replacing old clusters | **ported** | Old clusters are punched (1444) and the runlist merged before the new data is written (1483), so a failed write loses the old block. Ported 2026-11-25: allocate, write the new block, then punch + merge, with a shared free_rlc exit on failure; regression test test_compressed_block_survives_failed_rewrite (core/tests/test_compress.c) | core/ntfs/compress.c:1444,1483 |
| `eb94f5a41a19` | ntfs: fix mrec_lock ABBA deadlock in rename | **needs-work** | Both sides of the ABBA exist in the port and need the upstream reordering designed in: rename takes old inode -> old dir -> target inode -> target dir, while fsync takes child -> parent | core/vfs/namei.c:1389; core/vfs/file.c:147 |
| `eec908b5c0d3` | ntfs: mft: use kmemdup() instead of kmalloc() and memcpy() | **not-applicable** | Mechanical kmalloc+memcpy to kmemdup, no behaviour change (pair still present in port, harmless) | core/ntfs/mft.c:2541 |
| `f2240a2ff4db` | ntfs: introduce transparent compression codec interface | **needs-work** | New ntfs_codec.h interface feature; file does not exist in the port | n/a (no ntfs_codec.h) |
| `f2b2aeeaadce` | ntfs: support large pages in compressed writes | **port** | Port hard-codes 4K pages (NTFS_PAGE_SHIFT 12) and ntfs_write_cb has no page_offset; pages_per_cb = block_size >> PAGE_SHIFT is 0 for a 4K block under 16K pages | core/ntfs/compress.c:1320,1508; platform/include/ntfsport/config.h:17 |
| `f39cd3f7fcaf` | ntfs: port lzx/xpress decompressors from ntfs-3g-system-compression | **needs-work** | LZX/XPRESS decompressor feature; port has no lib/ decompressors at all | n/a (no core/ntfs/lib/) |
| `f6ce4cbcc53c` | ntfs: Remove references to page->__folio_index | **not-applicable** | Linux folio-API rename with no behaviour change; port still uses __folio_index on its shimmed page/folio types | core/ntfs/compress.c:476 |
| `f72df3a4c33b` | ntfs: make system files immutable to prevent corruption | **port** | Port only marks system files immutable when the sys_immutable mount option is set; the unconditional mft_no < FILE_first_user rule for regular files is missing, so $Bitmap etc. stay writable by default | core/vfs/inode.c:1204 |
| `f98d80d51777` | ntfs: Inline zero_partial_compressed_page() | **not-applicable** | Pure code motion, no behaviour change; port still has the separate function | core/ntfs/compress.c:105 |
| `fbd74e665e18` | ntfs: file extension before write submission | **already-covered** | Port expands on data_size (stricter than upstream's allocated_size test) inside ntfs_compress_write; upstream's file.c/iomap.c move has no port equivalent | core/ntfs/compress.c:1529 |
| `fc440366c470` | ntfs: discard inodes that fail initialization | **already-covered** | Port already drops failed inodes (unhash + iput + ERR_PTR) instead of keeping a bad inode in cache, per its PORT comment | core/vfs/inode.c:175 |
| `fea9e4488f38` | ntfs: verify run length exceeding volume boundary | **port** | Mapping-pairs decoder checks only lcn < -1 and zero-length runs; no lcn + length <= nr_clusters overflow/boundary check | core/ntfs/runlist.c:879-894 |
| `ffc755828de1` | ntfs: Fix min_len for compressed/sparse attributes in ntfs_non_resident_attr_value_is_valid() | **port** | Port has no ntfs_non_resident_attr_value_is_valid at all; the inline validator uses min_len 64 for all non-resident attrs, so a 64-71 byte sparse/compressed record passes | core/ntfs/attrib.c:771-772 |
| `cbe287fd65f1` | ntfs: remove unsupported quota handling | **port** | Remove unsupported quota handling (not in the 124; recovered from the 7.2-rc1 merge aggregate). The port still has quota.c/quota.h, called from super.c:286 on rw remount - and the port's own super_glue.c:302 note says ntfs_mark_quotas_out_of_date() always fails (looks up $Quota under the wrong name "$I30"), so a rw remount currently returns -EROFS. Upstream removed the marker, the $Quota/$Q loading state and the volume flag; kept layout.h definitions. Port: delete quota.c/quota.h, the super.c:286 call, load_and_init_quota + vol->quota_ino/quota_q_ino, update the super_glue note. | core/ntfs/super.c:20,252,286,1247-1310,1767-1768,1876-1878; core/vfs/super_glue.c:302 |
