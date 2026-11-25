#!/bin/bash
# tools/upstream-drift.sh - upstream drift tracking (GAPS.md §5.3).
#
#   tools/upstream-drift.sh port-diff
#       What the port changed against the pinned pristine copy:
#       per-file +/-, upstream-only (Tier 2, not ported) and port-only
#       files. No network. Informational, always exits 0 -- the port
#       diff is expected to grow as the port fixes bugs upstream
#       has not (yet) fixed; the numbers are the discipline, not a
#       failure.
#
#   tools/upstream-drift.sh upstream [--tag TAG] [--out FILE]
#       What upstream did since the pin: fetch the target tag (latest
#       stable v7.x by default), verify the vendored pristine copy
#       still matches the pinned tag byte-for-byte, diff the pinned
#       fs/ntfs against the target's, and list the commits that
#       touched fs/ntfs in between. Exits 0 when there is no drift,
#       1 when there is drift to triage (the weekly CI job fails on
#       this, which is the point -- a drift nobody looks at is not
#       tracked), 2 on any tooling/network error. --out writes the
#       same report to FILE (the CI artifact).
#
# The pin (tag + commit) is read from upstream/UPSTREAM.md, the one
# source of truth; override with UPSTREAM_DRIFT_PIN_TAG /
# UPSTREAM_DRIFT_PIN_COMMIT for testing. UPSTREAM_DRIFT_REPO (default
# torvalds/linux on github.com) and UPSTREAM_DRIFT_PRISTINE (default
# upstream/linux-v7.1/fs/ntfs) are likewise overridable so the
# self-test (tools/test-upstream-drift.sh) can run against a local
# fixture repo with no network.
#
# Bash 3.2 compatible (macOS ships it): no associative arrays, no
# mapfile, no sort -V (version pick is done in python3).
set -uo pipefail
. "$(dirname "$0")/env.sh"

UPSTREAM_REPO="${UPSTREAM_DRIFT_REPO:-https://github.com/torvalds/linux.git}"
PRISTINE_DIR="${UPSTREAM_DRIFT_PRISTINE:-$NTFS_ROOT/upstream/linux-v7.1/fs/ntfs}"
PORT_DIR="$NTFS_ROOT/core/ntfs"
SUBDIR="fs/ntfs"
WORKDIR=""

cleanup() {
	[ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

usage() {
	sed -n '2,/^set -uo pipefail/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
	exit 2
}

pin_tag() {
	echo "${UPSTREAM_DRIFT_PIN_TAG:-$(grep -oE 'tag `v[^`]+`' "$NTFS_ROOT/upstream/UPSTREAM.md" | head -1 | sed 's/tag `//;s/`//')}"
}

pin_commit() {
	echo "${UPSTREAM_DRIFT_PIN_COMMIT:-$(grep -oE 'commit `[0-9a-f]{40}`' "$NTFS_ROOT/upstream/UPSTREAM.md" | head -1 | sed 's/commit `//;s/`//')}"
}

is_local_repo() {
	case "$UPSTREAM_REPO" in
		/*|file://*) return 0 ;;
		*) return 1 ;;
	esac
}

# Latest stable kernel tag (v7.x, no -rc) known to the repo. Version
# pick is python3: BSD sort has no -V, and this must also run on macOS.
latest_stable_tag() {
	git ls-remote --tags "$UPSTREAM_REPO" 'v[0-9]*' 2>/dev/null | awk '{print $2}' |
		sed 's|refs/tags/||' | grep -v '\^{}' | grep -v -- '-rc' |
		python3 -c '
import re, sys
def key(t):
    return tuple(int(x) for x in t.lstrip("v").split("."))
tags = [l.strip() for l in sys.stdin if re.match(r"^v[0-9]+(\.[0-9]+)*$", l.strip())]
print(max(tags, key=key) if tags else "")
'
}

# count_diff <a> <b>: prints "adds dels" for two files (diff -U0,
# counting +/- lines, excluding the ---/+++ headers).
count_diff() {
	local adds dels
	adds=$(diff -U0 "$1" "$2" 2>/dev/null | grep -c '^\+[^+]')
	dels=$(diff -U0 "$1" "$2" 2>/dev/null | grep -c '^-[^-]')
	echo "$adds $dels"
}

# diff_table <dir-a> <dir-b> <a-label> <b-label>: per-file diff
# table between two trees, plus a summary line "SUMMARY <changed>
# <adds> <dels> <a-only> <b-only>". Directories are listed, not
# diffed (their contents are a separate story). Always exits 0.
diff_table() {
	local dir_a="$1" dir_b="$2" a_label="$3" b_label="$4"
	local tmp f adds dels up_lines pct changed=0 t_add=0 t_del=0 a_only=0 b_only=0
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/drift-table.XXXXXX")
	ls "$dir_a" | sort > "$tmp/a.txt"
	ls "$dir_b" | sort > "$tmp/b.txt"
	sort -u "$tmp/a.txt" "$tmp/b.txt" > "$tmp/all.txt"
	printf '%-14s %6s %6s %6s\n' file +add -del '%chg'
	while read -r f; do
		if [ -d "$dir_a/$f" ] || [ -d "$dir_b/$f" ]; then
			if [ -d "$dir_a/$f" ] && [ -d "$dir_b/$f" ]; then
				printf '%-14s   (directory, not diffed)\n' "$f"
			elif [ -d "$dir_a/$f" ]; then
				printf '%-14s   (directory, only in %s)\n' "$f" "$a_label"
				a_only=$((a_only + 1))
			else
				printf '%-14s   (directory, only in %s)\n' "$f" "$b_label"
				b_only=$((b_only + 1))
			fi
		elif [ ! -f "$dir_a/$f" ]; then
			printf '%-14s   (only in %s)\n' "$f" "$b_label"
			b_only=$((b_only + 1))
		elif [ ! -f "$dir_b/$f" ]; then
			printf '%-14s   (only in %s)\n' "$f" "$a_label"
			a_only=$((a_only + 1))
		else
			read -r adds dels <<EOF
$(count_diff "$dir_a/$f" "$dir_b/$f")
EOF
			if [ "$adds" -gt 0 ] || [ "$dels" -gt 0 ]; then
				up_lines=$(wc -l < "$dir_a/$f" | tr -d ' ')
				pct=$(( (adds + dels) * 100 / (up_lines + 1) ))
				if [ "$pct" -gt 50 ]; then
					printf '%-14s %+6s %-6s %5s%%  <-- heavily diverged\n' "$f" "$adds" "$dels" "$pct"
				else
					printf '%-14s %+6s %-6s %5s%%\n' "$f" "$adds" "$dels" "$pct"
				fi
				changed=$((changed + 1))
				t_add=$((t_add + adds))
				t_del=$((t_del + dels))
			fi
		fi
	done < "$tmp/all.txt"
	rm -rf "$tmp"
	echo "SUMMARY $changed $t_add $t_del $a_only $b_only"
}

cmd_port_diff() {
	[ -d "$PRISTINE_DIR" ] || { echo "pristine dir missing: $PRISTINE_DIR" >&2; return 2; }
	[ -d "$PORT_DIR" ] || { echo "port dir missing: $PORT_DIR" >&2; return 2; }
	echo "port diff: $PORT_DIR vs pristine $PRISTINE_DIR"
	echo "pin: $(pin_tag) $(pin_commit)"
	diff_table "$PRISTINE_DIR" "$PORT_DIR" "pristine" "port"
	echo
	echo "Upstream-only files are Tier 2 (rewritten in core/vfs/) or"
	echo "build files, not ported code. A port-only file is new code"
	echo "with no upstream counterpart. %chg over 50% means merging"
	echo "an upstream fix in that file needs care, not that the port"
	echo "is wrong -- the additions are the port's own fixes/features."
	return 0
}

# cmd_upstream [--tag TAG] [--out FILE]
cmd_upstream() {
	local target="" out="" pin="" commit="" repo="" work="" report=""
	local pin_diff="" n_changed=0 t_add=0 t_del=0 n_commits=0
	while [ $# -gt 0 ]; do
		case "$1" in
			--tag) target="$2"; shift 2 ;;
			--out) out="$2"; shift 2 ;;
			*) echo "unknown argument: $1" >&2; return 2 ;;
		esac
	done
	need_tool git || return 2
	need_tool python3 || return 2
	pin=$(pin_tag)
	commit=$(pin_commit)
	[ -n "$pin" ] || { echo "could not parse pin tag from upstream/UPSTREAM.md" >&2; return 2; }
	[ -n "$commit" ] || { echo "could not parse pin commit from upstream/UPSTREAM.md" >&2; return 2; }
	if [ -z "$target" ]; then
		target=$(latest_stable_tag) || return 2
		[ -n "$target" ] || { echo "could not find a stable tag in $UPSTREAM_REPO" >&2; return 2; }
	fi
	echo "==> upstream drift: pin $pin ($commit) -> target $target"
	echo "    repo: $UPSTREAM_REPO"

	WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/upstream-drift.XXXXXX")
	work="$WORKDIR"
	repo="$work/repo"
	if is_local_repo; then
		git clone --quiet "$UPSTREAM_REPO" "$repo" || { echo "clone failed: $UPSTREAM_REPO" >&2; return 2; }
		# Local clone has full history; extract both ends with git
		# archive (all objects are local, no lazy fetch).
		mkdir -p "$work/pin" "$work/target"
		git -C "$repo" archive "$pin" "$SUBDIR" | tar -x -C "$work/pin" || { echo "could not extract $pin:$SUBDIR" >&2; return 2; }
		git -C "$repo" archive "$target" "$SUBDIR" | tar -x -C "$work/target" || { echo "could not extract $target:$SUBDIR" >&2; return 2; }
	else
		case "$UPSTREAM_REPO" in
			*github.com*) ;;
			*) echo "commit list needs github.com (API) or a local repo; got $UPSTREAM_REPO" >&2; return 2 ;;
		esac
		# No full clone: a blob-filtered clone of torvalds/linux still
		# downloads the whole commit/tree history (minutes), and
		# `git archive` in a blob-filtered clone lazy-fetches the
		# entire tree of the tag (tens of MB). Fetch only the two tag
		# tips, depth 1, trees excluded (--filter=tree:0), then
		# sparse-check out fs/ntfs at each tag in turn: the lazy
		# fetch then only pulls the trees/blobs under fs/ntfs.
		git init --quiet "$repo" || return 2
		git -C "$repo" remote add origin "$UPSTREAM_REPO" || return 2
		git -C "$repo" fetch --quiet --depth=1 --filter=tree:0 origin tag "$pin" || { echo "fetch of pin tag $pin failed" >&2; return 2; }
		git -C "$repo" fetch --quiet --depth=1 --filter=tree:0 origin tag "$target" || { echo "fetch of target tag $target failed" >&2; return 2; }
		git -C "$repo" sparse-checkout set "$SUBDIR" || return 2
		mkdir -p "$work/pin" "$work/target"
		git -C "$repo" checkout --quiet "$pin" || { echo "checkout of $pin failed" >&2; return 2; }
		cp -R "$repo/fs" "$work/pin/" || return 2
		git -C "$repo" checkout --quiet "$target" || { echo "checkout of $target failed" >&2; return 2; }
		cp -R "$repo/fs" "$work/target/" || return 2
	fi

	# 1. The vendored pristine copy must still match the pinned tag.
	# If somebody hand-edited upstream/, every diff below is suspect.
	pin_diff=$(diff -r "$PRISTINE_DIR" "$work/pin/$SUBDIR" 2>&1 || true)
	report="$work/report.txt"
	{
		echo "upstream drift report"
		echo "pin:    $pin ($commit)"
		echo "target: $target"
		echo "repo:   $UPSTREAM_REPO"
		echo
		echo "== pin check: vendored pristine copy vs $pin =="
		if [ -z "$pin_diff" ]; then
			echo "OK: pristine copy matches $pin byte-for-byte ($(ls "$PRISTINE_DIR" | wc -l | tr -d ' ') files)"
		else
			echo "MISMATCH: the vendored pristine copy differs from $pin:"
			echo "$pin_diff"
		fi
		echo
	} > "$report"
	if [ -n "$pin_diff" ]; then
		echo "MISMATCH: pristine copy differs from $pin -- re-refresh it per upstream/UPSTREAM.md" >&2
		cat "$report"
		[ -n "$out" ] && cp "$report" "$out"
		return 2
	fi

	# 2. File drift: pinned fs/ntfs vs target fs/ntfs.
	{
		echo "== upstream file drift: $pin:$SUBDIR vs $target:$SUBDIR =="
		diff_table "$work/pin/$SUBDIR" "$work/target/$SUBDIR" "$pin" "$target"
		echo
	} >> "$report"
	n_changed=$(grep '^SUMMARY' "$report" | tail -1 | awk '{print $2}')
	t_add=$(grep '^SUMMARY' "$report" | tail -1 | awk '{print $3}')
	t_del=$(grep '^SUMMARY' "$report" | tail -1 | awk '{print $4}')

	# 3. Commit list: git log for a local repo (full history present),
	# GitHub API (path-filtered, paginated) for github.com.
	{
		echo "== commits touching $SUBDIR, $pin..$target =="
		if is_local_repo; then
			git -C "$repo" log --format='%h %ad %s' --date=short "$pin..$target" -- "$SUBDIR" || echo "(git log failed)"
		else
			repo_path=$(echo "$UPSTREAM_REPO" | sed -e 's|https://github.com/||' -e 's|\.git$||')
			python3 "$NTFS_TOOLS_DIR/upstream_drift.py" commits --repo "$repo_path" \
				--pin-tag "$pin" --target-tag "$target" --path "$SUBDIR" ||
				echo "(commit list unavailable -- see error above)"
		fi
		echo
	} >> "$report"
	n_commits=$(sed -n '/== commits touching/,$p' "$report" | grep -cE '^[0-9a-f]{7,12} [0-9]{4}-[0-9]{2}-[0-9]{2} ' || true)

	# 4. Verdict.
	{
		echo "== verdict =="
		if [ "$n_changed" -eq 0 ] && [ "$n_commits" -eq 0 ]; then
			echo "CLEAN: no upstream drift -- $target has nothing new in $SUBDIR since $pin."
		else
			echo "DRIFT: $n_changed files changed (+$t_add -$t_del), $n_commits commits since $pin."
			echo "Triage: for each changed file, decide whether the upstream"
			echo "fix applies to the port (core/ntfs/) and merge it, or"
			echo "record why not in docs/GAPS.md. To move the pin after a"
			echo "triage, refresh upstream/ per upstream/UPSTREAM.md and"
			echo "update the pin there; the next run then compares against"
			echo "the new pin."
		fi
	} >> "$report"

	cat "$report"
	[ -n "$out" ] && cp "$report" "$out"
	if [ "$n_changed" -eq 0 ] && [ "$n_commits" -eq 0 ]; then
		return 0
	fi
	return 1
}

case "${1:-}" in
	port-diff) cmd_port_diff ;;
	upstream)  shift; cmd_upstream "$@" ;;
	*)         usage ;;
esac
