# TT NTFS Native — open work, 2026-09-15

## Needs the Windows machine
- [x] Phase 4 scenarios A, C, D and E3 -- all reproduce Windows. E3 is the
      strongest: chkdsk found no problems on a volume only our replay recovered.
- [x] E1 done: a clean Windows safe-removal leaves v1.1 CLEAN, confirming the
      documented downgrade, and every structural field of its restart area matches
      what mark_clean writes. Only current_lsn differs, which is a position.
- [x] B2 (Fast Startup) done 2026-09-17, and it is the most user-facing result of
      the lot: a hybrid shutdown does NOT dismount a removable volume, so the log
      is left open and reads dirty with nothing at all to replay. The app now
      distinguishes that from real pending work (Close Journal vs Replay Journal).
- [x] B1 attempted twice 2026-09-17 and not pursued further. Hibernation leaves the
      log open like Fast Startup, with one transaction to roll back. Both attempts
      failed to strand a write: Windows seems to finish a pending copy before
      hibernating. Expected finding is already known from ntfsrecover's refusal.
- [x] Done 2026-09-17 after four attempts: 26,543 records, 6,634 UpdateMappingPairs,
      8 clusters per dirty page. Our replay matches Windows -- D.bin identical, its
      MFT record differing in 5 per-write bytes. The lcns_to_follow > 1 path works.
- [x] Answered by B2 and E1: a *safely removed* stick comes back v1.1 CLEAN (E1),
      and an open v2.0 log is what a shutdown that does not dismount leaves (B2).
      The removal policy was never the cause. Ordinary users DO hit the read-only
      path, via Fast Startup, which is why the Close Journal wording exists.
- [x] Confirmed 2026-09-17: Windows opens what this driver wrote, hard links report
      all four paths, and our symlinks are structurally valid. But Windows cannot
      follow them (WSL tag) and plain `dir` hides them (we set FILE_ATTR_SYSTEM).
      Recorded as finding 20.
- [x] FILE_ATTR_SYSTEM dropped for symlinks (50a558c). Confirmed on Windows: native
      links now appear in a plain `dir`.
- [x] Symlink tag: done and verified on Windows 2026-09-17. Native tag by default,
      per link, WSL fallback for targets Windows cannot name; wsl_symlinks mount
      flag forces the old behaviour. Windows follows them, chkdsk is clean, and the
      EA coexistence turned out not to matter. Finding 20.
- [x] Fixed 2026-09-17 (finding 22): a symlink to an existing directory is written
      as a directory record. Windows shows <SYMLINKD> and cd works.
- [x] Fixed 2026-11-25 (finding 23, docs/GAPS.md §2.1): our driver can read
      ntfs-3g's default Interix symlinks (IntxLNK $DATA files, commit
      bf3ff225).
- [x] Fixed 2026-09-17 (finding 21): Windows directory symlinks and junctions read
      as links and resolve, deleting one leaves the target alone.
- [x] Confirmed 2026-09-18: links from e6b3418 survive chkdsk + Mac write + Windows.
      The real cause was chkdsk rewriting the index entry of any file carrying both
      $EA and a reparse point (finding 23). Symlinks no longer carry $EA.
- [x] Migration done 2026-11-25 (docs/GAPS.md §2.5): symlinks from builds before
      e6b3418 are stripped of $EA on first load on a read-write mount
      (ntfs_migrate_symlink_ea), and their index entries are re-synced to the
      reparse tag, so chkdsk no longer rewrites them. Read-only mounts leave
      them alone. Verified by test_symlink_ea_migration (fixture built with
      tools/mkfixtures/patch_ea.py, chkdsk damage with index_union.py).
- [x] Xattrs confirmed 2026-09-17: `dir /r` shows them as ordinary alternate data
      streams on Windows. Not separately checked: the 8 KB `user.big` xattr in
      F-xattr, which exceeds the inline limit and takes a different storage path.
- [x] Compressed round trip done 2026-09-18: chkdsk clean, Windows reads our
      modifications to its own compressed file byte for byte. Phase 2 closed. Found
      and fixed two volume-goes-read-only defects on the way (finding 24).
- [x] Compressed-file creation (finding 25) fixed 2026-11-25: the writer
      routes fresh compressed files through ntfs_compress_write() and the
      expand rounds the runlist terminator to a compression-block boundary;
      NV_Compression is set and regular files inherit COMPRESSED.
      test_compress 412 checks 0 failures; ntfs-3g reads a 200 KB file
      written into a compressed folder in full. A Windows round trip to
      verify is still outstanding.

## Known, with the evidence

- Disk Utility shows our volumes as "Unknown" (or "MS-DOS (FAT)" in `diskutil`),
  because `FSShortName` must be `ttntfs` and `diskutil` resolves VolumeKind
  against `/System/Library/Filesystems`. `ntfs` is taken: it fixes the label and
  breaks the mount with ECONNREFUSED from `fskitd`. Measured 2026-09-23, see
  docs/TESTING.md. Not fixable without Apple changing FSKit, or shipping a
  `/Library/Filesystems/ttntfs.fs` bundle to teach `diskutil` our name -- which
  is untested and would put a file outside the app bundle.

## Blocked on a decision
- [x] Git remote, 2026-09-19: github.com/dr-kbadawi/ttntfs, public. GPL §3 is met
      from that moment. CI runs on every push (macos-26 runner) and is green.
- [x] Author email, 2026-09-19: all commits rewritten to karim.badawi@techtag.ch
      before the first push. Same day, all AI attribution trailers were stripped
      from every commit and force-pushed; attribution is to the user only, always.
      Old history kept locally in backup-before-*-2026-09-19 branches, never pushed.
- [x] Website, 2026-09-19: https://ntfs.techtag.ch (GitHub Pages from site/, custom
      domain, Let's Encrypt, HTTPS enforced). Release v0.4.1 carries the DMG under
      a stable name (releases/latest/download/TT-NTFS-Native.dmg) and a versioned
      one for the cask. The version line on the page is hand-written per release.

## Unblocked, not started
- [x] Done 2026-09-17: the logfile fixture now carries a real $INDEX_ALLOCATION,
      and both gaps have load-bearing unit tests --
      test_first_write_into_fresh_cluster_is_protected (finding 19) and
      test_rw_mount_retires_the_journal_in_two_writes (327 writes vs 2).
- [ ] Port ntfsprogs-plus's v2.0 acceptance into core/ntfs/logfile.c so the log
      stops saying "LogFile version 2.0 is not supported" on every mount.

- [ ] x86_64 build. arm64 only; nothing has ever compiled for a second arch.
- [x] mkfs in the app: **decided against, 2026-09-19.** NTFS on a disk means the disk
      comes from or goes to Windows, and Windows formats it. A Mac user starting from
      a blank disk should use exFAT, which everything reads and writes natively. This
      driver is for disks that are already NTFS. Not a gap; a stance, stated on the site.
- [ ] fsck in the app: not planned either; chkdsk is the tool and we will not
      pretend otherwise. Journal recovery is the safe subset and it is done.
- [x] Homebrew cask, 2026-09-19: tap at github.com/dr-kbadawi/homebrew-ttntfs.
      `brew tap dr-kbadawi/ttntfs && brew install --cask ttntfs`. Install, run,
      uninstall --zap all verified end to end. Submit to homebrew-cask proper once
      the repo clears their notability bar (~75 stars). Per release: upload a
      versioned DMG (TT-NTFS-Native-X.Y.Z.dmg) alongside the stable one, bump
      `version` and `sha256` in Casks/ttntfs.rb.
- [x] Rebuild the DMG. Done 2026-09-15 from a47cd6e: notarized, stapled,
      `source=Notarized Developer ID`, installed from the DMG itself and verified
      serving a real volume. Release notes in `RELEASE-NOTES.md`.
