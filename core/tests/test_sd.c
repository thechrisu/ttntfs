// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_sd.c - the SD -> Unix mode mapping (docs/ACL-DESIGN.md §2).
 *
 * The parser (core/ntfs/security.c) is pure, so these tests build
 * synthetic self-relative SDs in memory and check the exact mode (or
 * the exact error) for each. Positive cases fail if the parser is a
 * stub returning -EINVAL; malformed cases fail if the parser reads
 * past a boundary instead of refusing (run under ASan + UBSan via
 * pre-push stage 9 / the asan ctest stage).
 *
 * Well-known SIDs used below (docs/ACL-DESIGN.md §2.1):
 *   Everyone S-1-1-0, Authenticated Users S-1-5-11, Users S-1-5-32-545,
 *   Interactive S-1-5-4, SYSTEM S-1-5-18, Administrators S-1-5-32-544.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "layout.h"
#include "security.h"

static int failures, checks;
#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

/* ---- SD builder ---------------------------------------------------------- */

struct sid_spec {
	u32 auth;
	u32 subs[4];
	int nsub;
};

static const struct sid_spec SID_EVERYONE = { 1, { 0 }, 1 };
static const struct sid_spec SID_AUTH_USERS = { 5, { 11 }, 1 };
static const struct sid_spec SID_USERS = { 5, { 32, 545 }, 2 };
static const struct sid_spec SID_INTERACTIVE = { 5, { 4 }, 1 };
static const struct sid_spec SID_SYSTEM = { 5, { 18 }, 1 };
static const struct sid_spec SID_ADMINS = { 5, { 32, 544 }, 2 };
static const struct sid_spec SID_OWNER = { 5, { 21, 100, 200, 1001 }, 4 };
static const struct sid_spec SID_GROUP = { 5, { 21, 100, 200, 513 }, 4 };
static const struct sid_spec SID_DOMAIN_UNKNOWN = { 5, { 21, 100, 200, 9999 }, 4 };
static const struct sid_spec SID_CREATOR_OWNER = { 3, { 0 }, 1 };

struct sd_builder {
	u8 buf[2048];
	size_t len;		/* bytes used */
	size_t acl_off;		/* offset of the ACL header, or 0 */
	size_t ace_count_off;	/* offset of ace_count in the ACL header */
	int ace_count;
};

static void put16(u8 *p, u16 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void put32(u8 *p, u32 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static size_t sid_len(const struct sid_spec *s)
{
	return 8 + 4 * s->nsub;
}

static size_t sid_write(u8 *buf, size_t off, const struct sid_spec *s)
{
	struct ntfs_sid *sid = (struct ntfs_sid *)(buf + off);
	int i;

	sid->revision = 1;
	sid->sub_authority_count = s->nsub;
	memset(sid->identifier_authority.value, 0, 6);
	sid->identifier_authority.value[5] = s->auth & 0xff;
	for (i = 0; i < s->nsub; i++)
		sid->sub_authority[i] = cpu_to_le32(s->subs[i]);
	return sid_len(s);
}

/* Start an SD: header with no owner/group/DACL yet. */
static void sb_init(struct sd_builder *b)
{
	memset(b, 0, sizeof(*b));
	b->len = sizeof(struct security_descriptor_relative);
	b->buf[0] = 1; /* revision */
	put16(b->buf + 2, le16_to_cpu(SE_SELF_RELATIVE | SE_DACL_PRESENT));
}

static size_t sb_add_sid(struct sd_builder *b, const struct sid_spec *s)
{
	size_t off = b->len;

	b->len += sid_write(b->buf, off, s);
	return off;
}

static void sb_set_owner(struct sd_builder *b, size_t off)
{
	put32(b->buf + 4, off); /* owner field: bytes 4-7 of the header */
}

static void sb_set_group(struct sd_builder *b, size_t off)
{
	put32(b->buf + 8, off); /* group field: bytes 8-11 of the header */
}

static void sb_begin_acl(struct sd_builder *b)
{
	b->acl_off = b->len;
	b->ace_count = 0;
	b->len += sizeof(struct ntfs_acl);
	b->buf[b->acl_off] = 2; /* ACL revision */
	b->ace_count_off = b->acl_off + 4;
}

static void sb_add_ace(struct sd_builder *b, u8 type, u8 flags, u32 mask,
		       const struct sid_spec *sid)
{
	size_t off = b->len;
	size_t size = 8 + sid_len(sid);
	struct ntfs_ace *ace = (struct ntfs_ace *)(b->buf + off);

	ace->type = type;
	ace->flags = flags;
	put16(b->buf + off + 2, size);
	put32(b->buf + off + 4, mask);
	sid_write(b->buf, off + 8, sid);
	b->len += size;
	b->ace_count++;
}

static void sb_finish(struct sd_builder *b)
{
	put32(b->buf + 16, b->acl_off); /* dacl offset */
	put16(b->buf + b->acl_off + 2, b->len - b->acl_off); /* ACL size */
	put16(b->buf + b->ace_count_off, b->ace_count);
}

/* A typical Windows file DACL: SYSTEM + Administrators full, Users read. */
static void build_typical(struct sd_builder *b, u32 users_mask)
{
	size_t owner_off, group_off;

	sb_init(b);
	owner_off = sb_add_sid(b, &SID_OWNER);
	group_off = sb_add_sid(b, &SID_GROUP);
	sb_set_owner(b, owner_off);
	sb_set_group(b, group_off);
	sb_begin_acl(b);
	sb_add_ace(b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_SYSTEM);
	sb_add_ace(b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_ADMINS);
	sb_add_ace(b, ACCESS_ALLOWED_ACE_TYPE, 0, users_mask, &SID_USERS);
	sb_finish(b);
}

static int mode_of(struct sd_builder *b, bool is_dir, unsigned int *mode)
{
	return ntfs_sd_dacl_mode(b->buf, b->len, is_dir, mode);
}

/* ---- cases ---------------------------------------------------------------- */

static void test_typical_file(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* Users read+execute: the mounting user reads and executes. */
	build_typical(&b, GENERIC_READ | GENERIC_EXECUTE);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0500);

	/* Users read+write+execute: full for the mounting user. */
	build_typical(&b, GENERIC_ALL);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0700);

	/* Users read only. */
	build_typical(&b, GENERIC_READ);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0400);
}

static void test_everyone(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_EVERYONE);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0777);

	/* Everyone read only: owner, group and other all read. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, &SID_EVERYONE);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0444);
}

static void test_null_and_empty_dacl(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* No DACL at all (SE_DACL_PRESENT clear): full access. */
	sb_init(&b);
	put16(b.buf + 2, le16_to_cpu(SE_SELF_RELATIVE));
	b.len = sizeof(struct security_descriptor_relative);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0777);

	/* SE_DACL_PRESENT set but dacl offset 0: explicitly NULL DACL. */
	sb_init(&b);
	b.len = sizeof(struct security_descriptor_relative);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0777);

	/* Present but zero ACEs: -ENODATA (caller falls back). */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == -ENODATA);
}

static void test_deny_aces(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* Everyone full, then Everyone denied write: 0555. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_EVERYONE);
	sb_add_ace(&b, ACCESS_DENIED_ACE_TYPE, 0, FILE_WRITE_DATA, &SID_EVERYONE);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0555);

	/* Users full, then Users denied write: owner loses write only. */
	build_typical(&b, GENERIC_ALL);
	sb_add_ace(&b, ACCESS_DENIED_ACE_TYPE, 0, FILE_WRITE_DATA, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0500);

	/* Owner full, then owner denied write. */
	sb_init(&b);
	{
		size_t owner_off = sb_add_sid(&b, &SID_OWNER);
		sb_set_owner(&b, owner_off);
	}
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_OWNER);
	sb_add_ace(&b, ACCESS_DENIED_ACE_TYPE, 0, FILE_WRITE_DATA, &SID_OWNER);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0500);

	/* A deny for an unresolvable domain SID is ignored (fail-open). */
	build_typical(&b, GENERIC_READ);
	sb_add_ace(&b, ACCESS_DENIED_ACE_TYPE, 0, GENERIC_ALL, &SID_DOMAIN_UNKNOWN);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0400);
}

static void test_skipped_aces(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* Inherit-only Users full + normal Users read: only the read counts. */
	build_typical(&b, GENERIC_READ);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, INHERIT_ONLY_ACE, GENERIC_ALL,
		   &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0400);

	/* Object-allow and audit ACEs are skipped. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_OBJECT_ACE_TYPE, 0, GENERIC_ALL, &SID_USERS);
	sb_add_ace(&b, SYSTEM_AUDIT_ACE_TYPE, 0, GENERIC_ALL, &SID_USERS);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0400);
}

static void test_generic_mapping(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* Each generic right maps to exactly its rwx bit. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0 && mode == 0400);

	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_WRITE, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0 && mode == 0200);

	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_EXECUTE, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0 && mode == 0100);

	/* Bare attribute / standard rights grant no data access. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0,
		   FILE_READ_ATTRIBUTES | DELETE | WRITE_DAC, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0 && mode == 0000);

	/* Specific bits: read+write+execute. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0,
		   FILE_READ_DATA | FILE_WRITE_DATA | FILE_EXECUTE, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0 && mode == 0700);
}

static void test_group_and_creator(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;
	size_t owner_off, group_off;

	/* Group SID read+execute, Users read: owner 4, group 5, other 0. */
	sb_init(&b);
	owner_off = sb_add_sid(&b, &SID_OWNER);
	group_off = sb_add_sid(&b, &SID_GROUP);
	sb_set_owner(&b, owner_off);
	sb_set_group(&b, group_off);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0,
		   FILE_READ_DATA | FILE_EXECUTE, &SID_GROUP);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0450);

	/* Group full, then group denied write: group 5. */
	sb_init(&b);
	owner_off = sb_add_sid(&b, &SID_OWNER);
	group_off = sb_add_sid(&b, &SID_GROUP);
	sb_set_owner(&b, owner_off);
	sb_set_group(&b, group_off);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_GROUP);
	sb_add_ace(&b, ACCESS_DENIED_ACE_TYPE, 0, FILE_WRITE_DATA, &SID_GROUP);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0450);

	/* Creator Owner full: the owner class gets it. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, &SID_CREATOR_OWNER);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0700);

	/* Authenticated Users and Interactive are interactive principals:
	 * their grants land in the owner bits, not the other bits. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_READ, &SID_AUTH_USERS);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_EXECUTE, &SID_INTERACTIVE);
	sb_finish(&b);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0);
	CHECK(mode == 0500);
}

static void test_dir_delete_child(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* FILE_DELETE_CHILD grants write on a directory, nothing on a file. */
	sb_init(&b);
	sb_begin_acl(&b);
	sb_add_ace(&b, ACCESS_ALLOWED_ACE_TYPE, 0, FILE_DELETE_CHILD, &SID_USERS);
	sb_finish(&b);
	err = mode_of(&b, true, &mode);
	CHECK(err == 0 && mode == 0200);
	err = mode_of(&b, false, &mode);
	CHECK(err == 0 && mode == 0000);
}

static void test_malformed(void)
{
	struct sd_builder b;
	unsigned int mode;
	int err;

	/* Shorter than the SD header. */
	build_typical(&b, GENERIC_READ);
	err = ntfs_sd_dacl_mode(b.buf, 10, false, &mode);
	CHECK(err == -EINVAL);

	/* Bad revision. */
	build_typical(&b, GENERIC_READ);
	b.buf[0] = 2;
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* Not self-relative. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + 2, le16_to_cpu(SE_DACL_PRESENT));
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* Owner offset past the end. */
	build_typical(&b, GENERIC_READ);
	put32(b.buf + 8, 0xffff);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* Owner SID claims 4 sub-authorities but the buffer ends after 1.
	 * The owner SID starts at offset 20 (after the 20-byte header). */
	build_typical(&b, GENERIC_READ);
	b.buf[20 + 1] = 4; /* sub_authority_count in the owner SID */
	err = ntfs_sd_dacl_mode(b.buf, 20 + 12, false, &mode);
	CHECK(err == -EINVAL);

	/* Owner SID sub-authority count above the 15 maximum. */
	build_typical(&b, GENERIC_READ);
	b.buf[20 + 1] = 16;
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* DACL offset past the end. */
	build_typical(&b, GENERIC_READ);
	put32(b.buf + 16, 0xffff);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* ACL size smaller than the header. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + b.acl_off + 2, 4);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* ACL size runs past the SD end. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + b.acl_off + 2, 0x7fff);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* ACL revision 1 is not accepted (2 and 4 only). */
	build_typical(&b, GENERIC_READ);
	b.buf[b.acl_off] = 1;
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* First ACE size zero: the walk cannot advance. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + b.acl_off + 8 + 2, 0);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* First ACE size not a multiple of 4. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + b.acl_off + 8 + 2, 9);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* First ACE size runs past the ACL end. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + b.acl_off + 8 + 2, 0x7fff);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* ace_count claims more ACEs than fit in the ACL. */
	build_typical(&b, GENERIC_READ);
	put16(b.buf + b.ace_count_off, 40);
	err = mode_of(&b, false, &mode);
	CHECK(err == -EINVAL);

	/* NULL inputs. */
	err = ntfs_sd_dacl_mode(NULL, 100, false, &mode);
	CHECK(err == -EINVAL);
	build_typical(&b, GENERIC_READ);
	err = ntfs_sd_dacl_mode(b.buf, b.len, false, NULL);
	CHECK(err == -EINVAL);
}

int main(void)
{
	test_typical_file();
	test_everyone();
	test_null_and_empty_dacl();
	test_deny_aces();
	test_skipped_aces();
	test_generic_mapping();
	test_group_and_creator();
	test_dir_delete_child();
	test_malformed();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
