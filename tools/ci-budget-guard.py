#!/usr/bin/env python3
"""Enforce the 20% GitHub Actions minutes budget for this repo.

Runs on ubuntu-latest as the first job of .github/workflows/ci.yml. It
decides whether the expensive macOS jobs may run, and prints
allow=true|false to $GITHUB_OUTPUT.

Rules (user, 2026-11-25): at most 20% of the account's monthly included
Actions minutes may be spent on this repo's macOS runs.

- Public repo: GitHub does not bill standard runners in public repos, so
  there is nothing to enforce; allow unconditionally.
- Private repo: sum this calendar month's completed macOS job durations
  for this workflow (via the Actions API), multiply by 10 (the macOS
  billing multiplier), and allow only while the total is under 20% of the
  plan's included minutes (Free 2000, Pro/Team 3000).
- Fail-safe: any API error denies — the job exits 1 without writing
  allow=true, so a broken guard never silently spends minutes. The guard
  itself runs on ubuntu-latest for a few seconds (~1 billed minute), not
  on macOS.
- The window is the billing cycle, not the calendar month: set BILLING_DAY
  to the account's billing day-of-month (github.com/settings/billing shows
  the cycle; default 1 = calendar month). Other repos' usage is not counted
  here; this guard tracks this repo only. A run already in flight is not
  counted until it completes.

Usage: GH_TOKEN=<token> REPO=<owner/repo> [BILLING_DAY=<1-31>]
  python3 tools/ci-budget-guard.py
  --self-test runs the duration/cycle math on canned data, no network.
"""
import calendar
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

MACOS_MULTIPLIER = 10
BUDGET_FRACTION = 0.20
INCLUDED_MINUTES = {"free": 2000, "pro": 3000, "team": 3000}


def gh_json(*args):
    r = subprocess.run(["gh", "api", *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"gh api {' '.join(args)} failed: {r.stderr.strip()[:300]}")
    return json.loads(r.stdout)


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def job_macos_minutes(jobs):
    """Wall-clock minutes of macOS jobs in a jobs-list payload."""
    total = 0.0
    for j in jobs:
        labels = [str(x).lower() for x in j.get("labels", [])]
        if not any("macos" in lb or "mac-os" in lb for lb in labels):
            continue
        started, completed = j.get("started_at"), j.get("completed_at")
        if not started or not completed:
            continue
        total += (parse_ts(completed) - parse_ts(started)).total_seconds() / 60.0
    return total


def cycle_start(now, billing_day):
    """Start of the current billing cycle (UTC), given the billing
    day-of-month (1-31). The cycle started on the most recent occurrence
    of that day; short months clamp to their last day (billing day 31 in
    February starts on the 28th/29th). billing_day=1 is the calendar month."""
    def day_in(y, m):
        return min(billing_day, calendar.monthrange(y, m)[1])
    y, m = now.year, now.month
    if now.day >= day_in(y, m):
        return datetime(y, m, day_in(y, m), tzinfo=timezone.utc)
    if m == 1:
        y, m = y - 1, 12
    else:
        m -= 1
    return datetime(y, m, day_in(y, m), tzinfo=timezone.utc)


def main():
    repo = os.environ.get("REPO") or os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        print("REPO env var is required", file=sys.stderr)
        return 1
    out_path = os.environ.get("GITHUB_OUTPUT")
    if not out_path:
        print("GITHUB_OUTPUT env var is required", file=sys.stderr)
        return 1

    def emit(allow, used, budget):
        with open(out_path, "a") as f:
            f.write(f"allow={'true' if allow else 'false'}\n")
            f.write(f"used_billed_minutes={used:.1f}\n")
            f.write(f"budget_billed_minutes={budget:.1f}\n")

    info = gh_json("repos/" + repo, "--jq", "{private: .private, owner: .owner.login}")
    if not info["private"]:
        print("Repo is public: GitHub does not bill standard runners in "
              "public repos, so the minutes budget does not apply. allow=true")
        emit(True, 0, 0)
        return 0

    owner = info["owner"]
    try:
        plan = gh_json("users/" + owner, "--jq", ".plan.name")
    except RuntimeError:
        plan = "free"
    included = INCLUDED_MINUTES.get(str(plan).lower(), 2000)
    budget = included * BUDGET_FRACTION

    try:
        billing_day = int(os.environ.get("BILLING_DAY", "1"))
    except ValueError:
        billing_day = 1
    billing_day = min(max(billing_day, 1), 31)

    now = datetime.now(timezone.utc)
    since = cycle_start(now, billing_day).strftime("%Y-%m-%dT%H:%M:%SZ")
    runs = gh_json(
        f"repos/{repo}/actions/workflows/ci.yml/runs"
        f"?per_page=100&status=completed&created=>={since}"
    ).get("workflow_runs", [])

    wall = 0.0
    for run in runs:
        jobs = gh_json(
            f"repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100"
        ).get("jobs", [])
        wall += job_macos_minutes(jobs)
    used = wall * MACOS_MULTIPLIER

    allow = used < budget
    print(f"Private repo, plan {plan} ({included} included min/month). "
          f"Budget: 20% = {budget:.0f} billed min. "
          f"Used since billing cycle start {since[:10]}: {used:.1f} billed min "
          f"({wall:.1f} wall-clock macOS min x10) across {len(runs)} runs. "
          f"allow={'true' if allow else 'false'}")
    emit(allow, used, budget)
    return 0


def self_test():
    jobs = [
        {"labels": ["macOS", "macos-26"],
         "started_at": "2026-11-01T10:00:00Z", "completed_at": "2026-11-01T10:08:00Z"},
        {"labels": ["ubuntu-latest"],
         "started_at": "2026-11-01T10:00:00Z", "completed_at": "2026-11-01T10:30:00Z"},
        {"labels": ["macos-26-xlarge"],
         "started_at": "2026-11-02T09:00:00Z", "completed_at": "2026-11-02T09:05:30Z"},
        {"labels": ["macOS"], "started_at": None, "completed_at": None},
    ]
    got = job_macos_minutes(jobs)
    assert abs(got - 13.5) < 1e-9, got
    print("self-test ok: macOS minutes =", got)

    def cs(y, m, d, bd):
        return cycle_start(datetime(y, m, d, tzinfo=timezone.utc), bd)
    assert cs(2026, 11, 25, 15) == datetime(2026, 11, 15, tzinfo=timezone.utc)
    assert cs(2026, 11, 10, 15) == datetime(2026, 10, 15, tzinfo=timezone.utc)
    assert cs(2026, 3, 30, 31) == datetime(2026, 2, 28, tzinfo=timezone.utc)
    assert cs(2026, 11, 25, 1) == datetime(2026, 11, 1, tzinfo=timezone.utc)
    assert cs(2026, 1, 5, 20) == datetime(2025, 12, 20, tzinfo=timezone.utc)
    print("self-test ok: billing cycle starts")
    return 0


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv else main())
