# ACL / security-descriptor mapping — design (stream B, §2.2)

Written 2026-11-27, stream B. Status: design first; v1 (read path) to
follow on branch `acl-work`. This doc decides the mapping before any code
lands, because the wrong mapping locks users out of their own files or
silently grants access Windows denies.

## 0. The finding that decides everything: FSKit has no ACL surface

FSKit exposes permissions only as mode/uid/gid (+ BSD flags).
`FSItem.Attributes` has no ACL field and there is no get/set ACL
operation in FSKit; Apple DTS (Aug 2026 forum reply) says requesting
ACL support is reasonable — i.e. it is not present. Consequences:

- A full Windows-SD ↔ macOS-ACL mapping cannot be surfaced to Finder
  or any macOS client through the filesystem interface, now or in any
  SDK this driver targets. Any design that assumes an FSKit ACL op is
  dead on arrival.
- The only permission surface is the one the driver already returns:
  `mode_of()` in `core/vfs/api.c` → `FSItem.Attributes.mode/uid/gid`.
  The volume mounts noowners (`restrictsOwnershipChanges`, uid/gid are
  the mount's, `doesNotSupportSettingFilePermissions`).
- What Paragon/Tuxera actually do, and what §2.2 means in practice,
  is therefore **map the NTFS DACL to Unix mode bits** — not expose
  ACLs. That is implementable inside FSKit, and it is what v1 does.

What v1 is NOT: POSIX ACLs via `$EA` (`CONFIG_NTFS_FS_POSIX_ACL`).
That path stores ACLs as `system.posix_acl_access` / `..._default`
extended attributes — a Linux/WSL interop format, distinct from the
Windows security descriptor. FSKit never calls `get_acl`/`set_acl`,
so enabling it has no user-visible effect; it would only change
create behaviour (extra `$LXACL` EAs) for no macOS benefit. Enabling
it is a separate WSL-interop decision, not part of SD mapping.

## 1. Where the data lives (all already in the tree)

- Every file's `$STANDARD_INFORMATION` (v3) carries `security_id`
  (`core/ntfs/layout.h`, `struct standard_information.ver.v3`).
  It is read at inode load already (`core/vfs/inode.c`).
- NTFS 3.0+ stores each unique SD once in `$Secure:$SDS`, keyed by
  `security_id` via the `$SII` index (hash via `$SDH`).
  `$Secure` is loaded at mount (`vol->secure_ino`, `core/ntfs/super.c`).
- `$SDS` entry, verified against the Linux NTFS `layout.h` doc and
  the NTFS documentation (and to be confirmed empirically against a
  real mkntfs image before the lookup lands):
  20-byte header — `hash` u32, `security_id` u32, `offset` u64,
  `length` u32 (total entry size incl. header) — then the
  self-relative SD, entry padded to a 16-byte boundary. Entries are
  sorted by `security_id`. Each SD is stored twice, second copy at
  +0x40000 (256 KiB, the Windows cache manager's max size).
- The SD itself (`struct security_descriptor_relative`, layout.h):
  revision, control flags (`SE_DACL_PRESENT`, `SE_SELF_RELATIVE`),
  owner/group/sacl/dacl byte offsets. SIDs (`struct ntfs_sid`),
  ACE types (`ACCESS_ALLOWED_ACE_TYPE` 0, `ACCESS_DENIED_ACE_TYPE` 1,
  object ACEs 5–8, audit/alarm 2–3), inheritance flags
  (`INHERIT_ONLY_ACE` 0x08), and the generic/specific access masks
  are all already defined in `core/ntfs/layout.h`.

No new on-disk format is invented. v1 only reads.

## 2. v1 scope: SD → Unix mode, read path only

### 2.1 The one question: who is the macOS user in Windows terms?

The returned uid **is** the mounting user, so the kernel applies the
owner bits to them. A literal DACL→mode mapping (owner SID → owner
bits, group SID → group bits, Everyone → other bits) would therefore
report the access of the *Windows owner*, not of the user sitting at
the Mac — a file whose DACL grants `Users` read and `Administrators`
full control would come out `0700`-ish for the wrong principal, and
the mounting user would be locked out of files any Windows local
user can read. v1 instead computes the **effective access of a normal
interactive Windows user**, because that is the user at the keyboard:

- **Owner bits**: union of allow ACEs for the *interactive-user set* —
  Everyone (`S-1-1-0`), Authenticated Users (`S-1-5-11`), Users
  (`S-1-5-32-545`), Interactive (`S-1-5-4`) — plus allow ACEs naming
  the owner SID itself.
- **Group bits**: allow ACEs naming the group SID; if the group SID
  has none, fall back to the other bits.
- **Other bits**: allow ACEs naming Everyone only.
- **Deny ACEs**: a deny for a principal in the interactive-user set
  removes those bits from owner (and from other, for an Everyone
  deny); a deny for the owner SID removes owner bits; a deny for the
  group SID removes group bits. Denies for SIDs this driver cannot
  resolve (domain SIDs, unknown principals) are **ignored** —
  fail-open, recorded below. Honouring an unresolvable deny by
  clearing bits for everybody would lock users out of files Windows
  grants them; ignoring it preserves today's behaviour for exactly
  those files.
- **Inherit-only ACEs** (`INHERIT_ONLY_ACE`) are skipped: they do not
  apply to the object itself.
- **Object ACEs** (types 5–8) and audit/alarm ACEs (2–3) are skipped:
  they carry object-specific or auditing semantics this mapping does
  not model.
- **Generic rights** map to the file/dir-specific sets:
  `GENERIC_READ` → r (+ list/traverse for dirs), `GENERIC_WRITE` →
  w (+ add file/subdir for dirs), `GENERIC_EXECUTE` → x (+ traverse),
  `GENERIC_ALL` → rwx. Specific bits: `FILE_READ_DATA` /
  `FILE_LIST_DIRECTORY` → r, `FILE_WRITE_DATA` / `FILE_ADD_FILE` → w,
  `FILE_APPEND_DATA` / `FILE_ADD_SUBDIRECTORY` → w, `FILE_EXECUTE` /
  `FILE_TRAVERSE` → x. `DELETE` / `WRITE_DAC` / `WRITE_OWNER` /
  `READ_CONTROL` / `SYNCHRONIZE` do not map to mode bits.
- **NULL DACL** (`SE_DACL_PRESENT` clear, or dacl offset 0) means
  full access in Windows → `0777` (which is today's behaviour anyway).
- **Empty DACL** (present, zero ACEs) means nobody has access in
  Windows. v1 deliberately does **not** report `0000`: it falls back
  to today's behaviour (`0777 & ~mask`). That is fail-open and it is
  a known limitation, not an oversight — v1 must not make any file
  less accessible than it is today, and an empty DACL is rare enough
  (Windows almost never creates one) that locking the user out is the
  worse failure. A later `strict_acl` mount option could opt into
  fail-closed; it is a follow-up, not v1.
- **Anything unparseable** — bad revision, offsets outside the SD,
  truncated ACE/SID, unknown `security_id`, missing `$Secure`, `$SDS`
  read failure — falls back to today's behaviour and never fails the
  inode load. The parser is pure and bounds-checked on every read;
  a crafted SD must produce an error, never an out-of-bounds read
  (ASan + UBSan + the fuzz corpus cover this; see §5).

### 2.2 Precedence (unchanged except for the new step)

1. Symlink → `0777` (as today).
2. Per-file `$LXMOD` EA present → that mode wins (WSL files keep
   their exact mode; upstream 4d730a4dd78c).
3. **NEW: SD-derived mode** (this doc) — only when no `$LXMOD`.
4. Otherwise the mount masks (`0777 & ~fmask` / `~dmask`).
5. `FILE_ATTR_READONLY` still clears write bits on non-directories;
   read-only mounts still clear write bits (as today).

### 2.3 Lookup and caching

- Lookup is by `security_id` from `$SI`, in `$Secure:$SDS`.
  v1 does a linear scan of `$SDS` (typical volumes: a handful of
  distinct SDs; the stream is small) rather than walking the `$SII`
  B+ tree — less new index code, same result. If a volume's `$SDS`
  grows large enough for the scan to matter, the `$SII` walk is a
  follow-up; measure before optimising.
- Result is cached per volume, keyed by `security_id`, bounded
  (64 entries, FIFO eviction), storing the computed perm bits.
  Inode load then costs one cache hit per file after the first of
  each SD. SDs never change through this driver in v1, so no
  invalidation is needed in v1; the write path (§3) must invalidate.
- If the first `$SDS` copy fails validation, try the second copy at
  +0x40000 before giving up — Windows keeps both for exactly this.
- Computed once at inode load (one `ntfs_inode` flag + perm bits,
  same pattern as `HasLXMOD`), consumed by `mode_of()` on every
  getattr. Inode load is once per icache entry; getattr is hot.
  Never look the SD up per getattr.

## 3. Write path (v2, designed, NOT implemented in stream B)

`chmod` today writes the mode to the `$LXMOD` EA (WSL form) and
leaves the SD untouched — Windows still sees the old DACL. A real
write path would update the DACL to match the new mode, preserving
ACEs for principals outside the interactive-user set and rewriting
the interactive-user ACEs. That needs `$Secure` write machinery
that does not exist in this port: allocate a new SD in `$SDS`
(dedup via the `$SDH` hash), update the `$SDH`/`$SII` indexes, and
set the file's `security_id` in `$SI`.

Journal hook (for the journal line, not this stream): ACL/SD sets
call `ntfs_set_ea` directly (EA class note, JOURNAL-DESIGN.md §5).
The `$EA` part follows the EA-class pattern (begin outside
`ntfs_set_ea`, stage the `$EA` pair net effect + `$SI`). But the
`$Secure` mutation and the `security_id` change in the file record
need **new journal builders** (an `UpdateResidentValue` for the
`$SI` `security_id` field, plus builders for the `$SDS` append and
the `$SDH`/`$SII` index updates) — those belong to the journal line
(Phase 4 or a later class). Stream B must not touch the journal
transaction machinery; this doc records the hook point so the
journal line can pick it up.

Also out of v1: enabling `CONFIG_NTFS_FS_POSIX_ACL` (WSL interop,
separate decision), a `strict_acl` mount option, the `$SII` index
walk, and SID→uid/gid mapping (no canonical mapping exists for
domain SIDs; uid/gid stay the mount's under noowners).

## 4. What "done" looks like for v1

1. `docs/ACL-DESIGN.md` (this file) + `docs/GAPS.md` §2.2 updated.
2. `core/ntfs/security.c` (+ header): pure SD parser
   (`ntfs_sd_parse`, `ntfs_sd_dacl_mode`) — no I/O, no globals,
   every offset/length validated before use.
3. `$SDS` lookup + per-volume cache, hooked into inode load and
   consumed by `mode_of()`.
4. `core/tests/test_sd.c`: parser tests on synthetic SDs (typical
   Windows DACL, NULL DACL, empty DACL, deny ACEs, inherit-only,
   generic rights, object/audit ACEs skipped, truncated/malformed
   inputs at every boundary) — each failing without the code — plus
   an integration test on an mkntfs image (lookup by `security_id`,
   parse, derived mode pinned against the observed `$SDS` bytes).
5. Pre-push 9/9 green incl. ASan; branch pushes only (no CI on
   branch pushes — local pre-push is the validation, stated honestly
   in the commit messages).

## 5. Safety notes

- Read-only change: v1 writes nothing to disk. A bug can misreport
  a mode, never corrupt a volume. The fallback (today's behaviour)
  bounds every failure.
- The parser is on-disk input: every length/offset is validated
  against the buffer before dereference; no `memcpy` of
  attacker-controlled lengths. Add SD blobs to the fuzz corpus as a
  follow-up (§5.2).
- The mapping is fail-open by design (§2.1): it can only grant what
  Windows grants to a normal interactive user, never less than today
  except where the DACL clearly denies the interactive-user set.
  The one deliberate over-grant is the empty-DACL fallback, recorded
  above.
