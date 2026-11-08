#!/usr/bin/env python3
"""Liveness watchdog of the beyond-VRAM fly-through lane (BV-06b, ADR section 9.1 job 3).

Runs on Mondays (GitHub-hosted, `actions: read` only). It reads the runs of
``.github/workflows/beyond_vram_flythrough.yml`` through the GitHub API and the
``verdict.json`` each fly-through uploads as ``bvw-verdict-<run_id>-<attempt>``,
and fails (exit 1) when the lane is not alive:

* ``pre-m2`` (until BV-14 flips it): within the last 9 days at least one run
  ended in a terminal verdict that measured something -- PASS, FAIL or INVALID.
  VOID (contention) does not count, as the ADR says; NOT_RUN and a run without a
  verdict artifact do not count either, because "the lane did not run" is not
  liveness. Also fails on any run queued for more than 20 h.
* ``m2``: a PASS within 9 days, no two consecutive VOIDs, no run queued for
  more than 20 h, and no run in the window cancelled or replaced.

The watchdog's own runs (``run-name`` starting with :data:`WATCHDOG_TITLE`) are
skipped. Any API failure is a failure of the watchdog, never "nothing found".

Usage::

    python3 tests/runtime/flythrough/lane_watchdog.py --repo OWNER/NAME \\
        --workflow beyond_vram_flythrough.yml --mode pre-m2 --out DIR [--include-dispatch]
"""

from __future__ import annotations

import argparse
import io
import json
import os
import subprocess
import sys
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

WINDOW = timedelta(days=9)
QUEUED_LIMIT = timedelta(hours=20)
WATCHDOG_TITLE = "Beyond-VRAM liveness watchdog"
VERDICT_ARTIFACT_PREFIX = "bvw-verdict-"
ALIVE_PRE_M2 = ("PASS", "FAIL", "INVALID")
QUEUED_STATUSES = ("queued", "pending", "waiting", "requested")
MODES = ("pre-m2", "m2")

Api = Callable[[str], bytes]


class WatchdogError(Exception):
    pass


def _parse_time(value: str) -> datetime:
    return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def evaluate(runs: Sequence[Dict[str, object]], now: datetime, mode: str) -> Tuple[bool, List[str]]:
    """`runs`: newest first, each with id, status, conclusion, created_at and verdict (or None)."""
    if mode not in MODES:
        raise WatchdogError(f"unknown mode {mode!r}")
    problems: List[str] = []
    recent = [r for r in runs if now - _parse_time(str(r["created_at"])) <= WINDOW]
    for run in runs:
        if str(run["status"]) in QUEUED_STATUSES and now - _parse_time(str(run["created_at"])) > QUEUED_LIMIT:
            problems.append(f"run {run['id']} has been {run['status']} since {run['created_at']} (> 20 h)")
    completed = [r for r in recent if r["status"] == "completed"]
    if mode == "pre-m2":
        alive = [r for r in completed if r.get("verdict") in ALIVE_PRE_M2]
        if not alive:
            seen = ", ".join(f"{r['id']}={r.get('verdict') or 'no verdict'}" for r in completed) or "none"
            problems.append(
                f"no run in the last {WINDOW.days} days ended in PASS, FAIL or INVALID (completed: {seen})"
            )
    else:
        if not any(r.get("verdict") == "PASS" for r in completed):
            problems.append(f"no PASS in the last {WINDOW.days} days")
        if len(completed) >= 2 and completed[0].get("verdict") == "VOID" and completed[1].get("verdict") == "VOID":
            problems.append(f"two consecutive VOIDs: runs {completed[0]['id']} and {completed[1]['id']}")
        for run in completed:
            if run.get("conclusion") == "cancelled":
                problems.append(f"run {run['id']} was cancelled or replaced")
    return not problems, problems


def gh_api(path: str) -> bytes:
    proc = subprocess.run(["gh", "api", path], capture_output=True, timeout=120)
    if proc.returncode != 0:
        raise WatchdogError(f"gh api {path} exited {proc.returncode}: {proc.stderr.decode(errors='replace').strip()}")
    return proc.stdout


def fetch_runs(api: Api, repo: str, workflow: str, events: Sequence[str]) -> List[Dict[str, object]]:
    runs: List[Dict[str, object]] = []
    for event in events:
        try:
            listed = json.loads(api(f"repos/{repo}/actions/workflows/{workflow}/runs?event={event}&per_page=50"))
            items = listed["workflow_runs"]
        except (ValueError, KeyError, TypeError) as exc:
            raise WatchdogError(f"the runs list for {event} is unreadable: {exc}") from exc
        for item in items:
            if str(item.get("display_title", "")).startswith(WATCHDOG_TITLE):
                continue
            runs.append({
                "id": item["id"],
                "event": event,
                "status": item["status"],
                "conclusion": item.get("conclusion"),
                "created_at": item["created_at"],
                "run_attempt": item.get("run_attempt", 1),
                "verdict": None,
            })
    runs.sort(key=lambda r: str(r["created_at"]), reverse=True)
    return runs


def fetch_verdict(api: Api, repo: str, run: Dict[str, object]) -> Optional[str]:
    """The verdict of the run's CURRENT attempt, or None when that attempt uploaded none.

    An earlier attempt's verdict never stands in for a rerun that wrote none.
    """
    try:
        listed = json.loads(api(f"repos/{repo}/actions/runs/{run['id']}/artifacts?per_page=100"))
        artifacts = listed["artifacts"]
    except (ValueError, KeyError, TypeError) as exc:
        raise WatchdogError(f"the artifact list of run {run['id']} is unreadable: {exc}") from exc
    wanted = f"{VERDICT_ARTIFACT_PREFIX}{run['id']}-{int(run.get('run_attempt') or 1)}"
    matches = [a["id"] for a in artifacts if a.get("name") == wanted and not a.get("expired")]
    if not matches:
        return None
    artifact_id = matches[0]
    blob = api(f"repos/{repo}/actions/artifacts/{artifact_id}/zip")
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            record = json.loads(archive.read("verdict.json"))
        return str(record["verdict"])
    except (zipfile.BadZipFile, KeyError, ValueError, TypeError) as exc:
        raise WatchdogError(f"the verdict artifact of run {run['id']} is unreadable: {exc}") from exc


def run_watchdog(
    repo: str,
    workflow: str,
    mode: str,
    events: Sequence[str],
    out: Path,
    api: Api = gh_api,
    now: Optional[datetime] = None,
) -> int:
    now = now or datetime.now(timezone.utc)
    report: Dict[str, object] = {"mode": mode, "events": list(events), "now": now.strftime("%Y-%m-%dT%H:%M:%SZ")}
    try:
        runs = fetch_runs(api, repo, workflow, events)
        for run in runs:
            if run["status"] == "completed" and now - _parse_time(str(run["created_at"])) <= WINDOW:
                run["verdict"] = fetch_verdict(api, repo, run)
        report["runs"] = runs
        ok, problems = evaluate(runs, now, mode)
    except WatchdogError as exc:
        ok, problems = False, [f"watchdog could not read the lane: {exc}"]
    report["alive"] = ok
    report["problems"] = problems
    out.mkdir(parents=True, exist_ok=True)
    (out / "watchdog.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [f"## Beyond-VRAM lane liveness watchdog ({mode})", "", f"Alive: **{ok}**", ""]
    lines += [f"- {p}" for p in problems] or ["- no problem found"]
    text = "\n".join(lines) + "\n"
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(text)
    if not ok:
        for problem in problems:
            print(f"::error title=Beyond-VRAM lane not alive::{problem}")
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--workflow", default="beyond_vram_flythrough.yml")
    ap.add_argument("--mode", required=True, choices=MODES)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--include-dispatch", action="store_true",
                    help="also count workflow_dispatch runs (manual evidence runs), not only scheduled ones")
    args = ap.parse_args(argv)
    events = ["schedule"] + (["workflow_dispatch"] if args.include_dispatch else [])
    return run_watchdog(args.repo, args.workflow, args.mode, events, args.out)


if __name__ == "__main__":
    sys.exit(main())
