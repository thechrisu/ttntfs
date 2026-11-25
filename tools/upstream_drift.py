#!/usr/bin/env python3
"""GitHub API helper for tools/upstream-drift.sh (GAPS.md §5.3).

Lists the commits touching one path between two tags, following
pagination. Stdlib only; no third-party packages.

Usage:
  python3 tools/upstream_drift.py commits --repo torvalds/linux \
      --pin-tag v7.1 --target-tag v7.3 --path fs/ntfs

Prints one line per commit, newest first:
  "<sha12> <YYYY-MM-DD> <subject>"

The date window comes from the tags themselves (tagger/committer date
of each tag), so the caller never has to know a date. Auth: uses
GH_TOKEN or GITHUB_TOKEN when set (CI provides GITHUB_TOKEN); without
one the unauthenticated 60-requests/hour limit applies, which is
plenty for a weekly check. Exit 2 on any API or network error.
"""

import json
import os
import sys
import urllib.error
import urllib.request

API = os.environ.get("UPSTREAM_DRIFT_API", "https://api.github.com")


def _headers():
    h = {"User-Agent": "ttntfs-upstream-drift", "Accept": "application/vnd.github+json"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        h["Authorization"] = "Bearer " + token
    return h


def _get(url):
    req = urllib.request.Request(url, headers=_headers())
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.headers, json.load(r)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")[:500]
        raise RuntimeError(f"GitHub API {e.code} for {url}: {body}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"GitHub API request failed for {url}: {e}")


def _get_all(url):
    """GET a paginated list endpoint, following rel=next links."""
    out = []
    while url:
        headers, page = _get(url)
        out.extend(page)
        url = None
        link = headers.get("Link", "")
        for part in link.split(","):
            if 'rel="next"' in part:
                url = part.split(";")[0].strip().strip("<>")
    return out


def tag_date(repo, tag):
    """Committer/tagger date (ISO 8601) of a tag, annotated or not."""
    _, ref = _get(f"{API}/repos/{repo}/git/ref/tags/{tag}")
    obj = ref["object"]
    if obj["type"] == "tag":
        _, tagobj = _get(f"{API}/repos/{repo}/git/tags/{obj['sha']}")
        return tagobj["tagger"]["date"]
    _, commit = _get(f"{API}/repos/{repo}/git/commits/{obj['sha']}")
    return commit["committer"]["date"]


def cmd_commits(args):
    repo = args.repo
    since = tag_date(repo, args.pin_tag)
    until = tag_date(repo, args.target_tag)
    commits = _get_all(
        f"{API}/repos/{repo}/commits?path={args.path}"
        f"&since={since}&until={until}&per_page=100"
    )
    for c in commits:
        subject = c["commit"]["message"].splitlines()[0]
        print(f"{c['sha'][:12]} {c['commit']['committer']['date'][:10]} {subject}")


def main(argv):
    if len(argv) < 2 or argv[1] != "commits":
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    import argparse

    p = argparse.ArgumentParser(prog="upstream_drift.py commits")
    p.add_argument("--repo", required=True)
    p.add_argument("--pin-tag", required=True)
    p.add_argument("--target-tag", required=True)
    p.add_argument("--path", required=True)
    args = p.parse_args(argv[2:])
    try:
        cmd_commits(args)
    except RuntimeError as e:
        print(f"upstream_drift.py: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
