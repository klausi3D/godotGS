#!/usr/bin/env python3
"""No-loosening guard for the beyond-VRAM thresholds file (BV-09, ADR section 8.4).

`tests/runtime/flythrough/flythrough_thresholds.json` freezes the fly-through
gates, the validity conditions and their globals. This guard compares it with
the same file at an immutable review base and fails on any of (ADR 8.4):

* a loosened value (an `<=` value that rises, a `>=` value that falls, any
  change of an `==` value);
* a removed entry, tier or key;
* a changed `op`, `arm`, window, exclusion, percentile method, `source_field`
  or any other frozen field of an entry (everything except `tiers`, `class`,
  `mode`, `note`);
* a hard -> report_only change, and any class change except
  statistical -> correctness;
* `min_vram_ratio` lowered; `gpu_bytes_per_splat`, a device, a tier's world
  set or the binary contract changed;
* a world's frozen digests (`path_sha256`, `generator_params_sha`, file
  digest, counts) changed without a new `reprime_records` entry that names
  the new values, a run and a calibration record;
* a removed `reprime_records` or `waivers` entry;
* non-finite numbers anywhere;
* an unresolvable base, or a thresholds file missing at HEAD;
* a `settings` edit outside the allowlist. The allowlist is a direction
  table: drawn budget up, tau down, SH bands farther, codec bits up. Every
  other setting is frozen. An edit in the work-reducing direction needs a new
  waiver line (setting, maintainer, date, reason, evidence URL) **and** G11q
  and Q-codec hard in the same file.

The base is `--base-ref`, else `GS_PR_BASE_SHA`, else `origin/$GITHUB_BASE_REF`,
else `origin/master`; never HEAD. `run_module_tests.py --guard-only` passes the
review base of the required `agentic-pr-gate`. On schedule and dispatch the
base resolves to HEAD's own commit and the comparison is vacuous: the guard
says so ("informational") and still validates the file on its own. A base that
does not have the file yet (the PR that introduces it) is reported as such and
the file is validated on its own.

Exit codes: 0 clean, 1 a rule fired, 2 the guard could not run (no base, no
file, unparsable JSON). A guard that cannot run never reports clean.

Limits (stated, not hidden): the guard reads values, not their meaning. It
cannot tell whether a new gate's value is sensible, whether a calibration
record is truthful, or whether the waiver's evidence shows G11q and Q-codec
passing at the new value; those are review questions.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
THRESHOLDS_REL = "tests/runtime/flythrough/flythrough_thresholds.json"
SCHEMA = "godotgs-flythrough-thresholds/1"
SECTIONS = ("validity", "gates")
OPS = ("<=", ">=", "==")
ARMS = ("flat", "hlod", "both")
MODES = ("hard", "report_only")
CLASSES = ("validity", "correctness", "statistical")
#: Entry keys a PR may change, each under its own rule below.
MUTABLE_ENTRY_KEYS = frozenset({"tiers", "class", "mode", "note"})
REQUIRED_ENTRY_KEYS = ("class", "mode", "arm", "op", "tiers", "source_field", "window", "exclusions",
                       "percentile_method")
#: Settings allowlist (ADR 8.4): the direction that increases work or fidelity.
#: Every setting not matched here is frozen.
DIRECTION_RULES: Tuple[Tuple[re.Pattern, str], ...] = (
    (re.compile(r"(^|\.)drawn_budget_splats$"), "up"),
    (re.compile(r"(^|\.)tau$"), "down"),
    (re.compile(r"(^|\.)sh_bands?_[a-z_]*far[a-z_]*$"), "up"),
    (re.compile(r"(^|\.)codec\.[a-z_]*bits(\[\d+\])?$"), "up"),
)
#: A work-reducing settings edit also needs these gates hard (ADR 8.4).
WAIVER_GATES = ("G11q", "Q-codec")
WAIVER_KEYS = ("setting", "maintainer", "date", "reason", "evidence_url")
REPRIME_KEYS = ("world", "run_id", "date", "calibration_record")
HEX64 = re.compile(r"^[0-9a-f]{64}$")

EXIT_CLEAN, EXIT_FIRED, EXIT_CANNOT_RUN = 0, 1, 2


class GuardError(Exception):
    """The guard cannot run. Exit 2, never clean."""


# --------------------------------------------------------------------------
# parsing and standalone validation
# --------------------------------------------------------------------------


def _reject_constant(name: str) -> float:
    raise ValueError(f"non-finite number {name} is not allowed")


def parse(text: str, where: str) -> dict:
    try:
        doc = json.loads(text, parse_constant=_reject_constant)
    except ValueError as exc:
        raise GuardError(f"{where}: not valid JSON: {exc}") from exc
    if not isinstance(doc, dict):
        raise GuardError(f"{where}: top level is not an object")
    return doc


def _walk(obj: Any, path: str = "") -> Iterable[Tuple[str, Any]]:
    if isinstance(obj, dict):
        for key, value in obj.items():
            yield from _walk(value, f"{path}.{key}" if path else str(key))
    elif isinstance(obj, list):
        for index, value in enumerate(obj):
            yield from _walk(value, f"{path}[{index}]")
    else:
        yield path, obj


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def validate(doc: dict) -> List[str]:
    """Rules the file must satisfy on its own, at HEAD."""
    problems: List[str] = []
    for path, value in _walk(doc):
        if isinstance(value, float) and not math.isfinite(value):
            problems.append(f"{path}: non-finite value {value!r}")
    if doc.get("schema") != SCHEMA:
        problems.append(f"schema is {doc.get('schema')!r}, expected {SCHEMA!r}")
    prov = doc.get("provenance")
    run = prov.get("source_run") if isinstance(prov, dict) else None
    if not isinstance(run, dict) or not str(run.get("run_id", "")).isdigit() \
            or not re.fullmatch(r"[0-9a-f]{40}", str(run.get("commit", ""))):
        problems.append("provenance.source_run needs a numeric run_id and a full commit sha")
    if not isinstance(prov, dict) or not prov.get("method"):
        problems.append("provenance.method is empty: thresholds need a documented derivation")
    glob = doc.get("globals")
    if not isinstance(glob, dict):
        problems.append("globals absent")
        glob = {}
    ratio = glob.get("min_vram_ratio")
    if not _is_number(ratio) or ratio <= 0:
        problems.append(f"globals.min_vram_ratio {ratio!r} is not a positive number")
    for name, world in (glob.get("worlds") or {}).items():
        for key in ("path_sha256", "generator_params_sha", "file_sha256"):
            if not HEX64.match(str((world or {}).get(key, ""))):
                problems.append(f"globals.worlds.{name}.{key} is not a sha256")
    if not isinstance(glob.get("settings"), dict):
        problems.append("globals.settings absent")
    for section in SECTIONS:
        entries = doc.get(section)
        if not isinstance(entries, dict) or not entries:
            problems.append(f"{section} absent or empty")
            continue
        for gid, entry in entries.items():
            problems += [f"{section}.{gid}: {p}" for p in _validate_entry(section, entry)]
    for key in ("reprime_records", "waivers"):
        if not isinstance(doc.get(key), list):
            problems.append(f"{key} must be a list")
    for index, waiver in enumerate(doc.get("waivers") or []):
        missing = [k for k in WAIVER_KEYS if not str((waiver or {}).get(k, "")).strip()]
        if missing:
            problems.append(f"waivers[{index}] lacks {missing}")
    for index, record in enumerate(doc.get("reprime_records") or []):
        missing = [k for k in REPRIME_KEYS if not str((record or {}).get(k, "")).strip()]
        if missing:
            problems.append(f"reprime_records[{index}] lacks {missing}")
    return problems


def _validate_entry(section: str, entry: Any) -> List[str]:
    if not isinstance(entry, dict):
        return ["not an object"]
    problems = [f"lacks {k}" for k in REQUIRED_ENTRY_KEYS if k not in entry]
    if entry.get("op") not in OPS:
        problems.append(f"op {entry.get('op')!r} not in {OPS}")
    if entry.get("arm") not in ARMS:
        problems.append(f"arm {entry.get('arm')!r} not in {ARMS}")
    if entry.get("mode") not in MODES:
        problems.append(f"mode {entry.get('mode')!r} not in {MODES}")
    if entry.get("class") not in CLASSES or (section == "validity") != (entry.get("class") == "validity"):
        problems.append(f"class {entry.get('class')!r} does not fit section {section}")
    tiers = entry.get("tiers")
    if not isinstance(tiers, dict) or not tiers:
        problems.append("tiers absent or empty")
    elif entry.get("op") in ("<=", ">=") and not all(
            _is_number(v) or (isinstance(v, str) and v.startswith("globals.")) for v in tiers.values()):
        problems.append(f"an ordered op needs numeric values, got {tiers}")
    return problems


# --------------------------------------------------------------------------
# comparison against the base
# --------------------------------------------------------------------------


def _loosened(op: str, old: Any, new: Any) -> bool:
    if op in ("<=", ">=") and _is_number(old) and _is_number(new):
        return new > old if op == "<=" else new < old
    return old != new


def _compare_entries(section: str, base: dict, head: dict) -> List[str]:
    problems: List[str] = []
    for gid, old in base.items():
        where = f"{section}.{gid}"
        new = head.get(gid)
        if not isinstance(new, dict):
            problems.append(f"{where}: removed")
            continue
        if not isinstance(old, dict):
            continue
        for key in sorted(set(old) - MUTABLE_ENTRY_KEYS):
            if key not in new:
                problems.append(f"{where}.{key}: removed")
            elif new[key] != old[key]:
                problems.append(f"{where}.{key}: changed {old[key]!r} -> {new[key]!r} (frozen)")
        if old.get("mode") == "hard" and new.get("mode") != "hard":
            problems.append(f"{where}: hard -> {new.get('mode')!r} (promotion is one-way)")
        if new.get("class") != old.get("class") and (old.get("class"), new.get("class")) != ("statistical", "correctness"):
            problems.append(f"{where}: class {old.get('class')!r} -> {new.get('class')!r}")
        old_tiers, new_tiers = old.get("tiers") or {}, new.get("tiers") or {}
        op = old.get("op")
        for tier, value in old_tiers.items():
            if tier not in new_tiers:
                problems.append(f"{where}.tiers.{tier}: removed")
            elif _loosened(op, value, new_tiers[tier]):
                problems.append(f"{where}.tiers.{tier}: {value!r} -> {new_tiers[tier]!r} loosens op {op}")
    return problems


def _flatten(obj: Any, prefix: str = "") -> Dict[str, Any]:
    return {path: value for path, value in _walk(obj, prefix)}


def _direction(key: str) -> Optional[str]:
    for pattern, direction in DIRECTION_RULES:
        if pattern.search(key):
            return direction
    return None


def _compare_settings(base: dict, head: dict) -> List[str]:
    problems: List[str] = []
    old_flat = _flatten((base.get("globals") or {}).get("settings") or {}, "settings")
    new_flat = _flatten((head.get("globals") or {}).get("settings") or {}, "settings")
    old_waivers = {json.dumps(w, sort_keys=True) for w in base.get("waivers") or []}
    new_waivers = [w for w in head.get("waivers") or [] if json.dumps(w, sort_keys=True) not in old_waivers]
    gates = head.get("gates") or {}
    hard_waiver_gates = all((gates.get(g) or {}).get("mode") == "hard" for g in WAIVER_GATES)
    for key, old in old_flat.items():
        if key not in new_flat:
            problems.append(f"globals.{key}: removed")
            continue
        new = new_flat[key]
        if new == old:
            continue
        direction = _direction(key)
        if direction is None:
            problems.append(f"globals.{key}: {old!r} -> {new!r}; frozen (not in the settings allowlist)")
            continue
        if not (_is_number(old) and _is_number(new)):
            problems.append(f"globals.{key}: {old!r} -> {new!r} is not a numeric edit")
            continue
        if (new > old) == (direction == "up"):
            continue  # more work or fidelity: allowed
        waived = [w for w in new_waivers if str((w or {}).get("setting", "")).strip() in (key, f"globals.{key}")]
        if not waived or not hard_waiver_gates:
            problems.append(
                f"globals.{key}: {old!r} -> {new!r} reduces work; needs a new waiver line naming the setting "
                f"and {' and '.join(WAIVER_GATES)} hard (waiver {'present' if waived else 'absent'}, "
                f"gates {'hard' if hard_waiver_gates else 'not hard'})"
            )
    return problems


def _compare_globals(base: dict, head: dict) -> List[str]:
    problems: List[str] = []
    old_g, new_g = base.get("globals") or {}, head.get("globals") or {}
    old_ratio, new_ratio = old_g.get("min_vram_ratio"), new_g.get("min_vram_ratio")
    if _is_number(old_ratio) and (not _is_number(new_ratio) or new_ratio < old_ratio):
        problems.append(f"globals.min_vram_ratio: {old_ratio!r} -> {new_ratio!r} (may only rise)")
    if old_g.get("gpu_bytes_per_splat") != new_g.get("gpu_bytes_per_splat"):
        problems.append("globals.gpu_bytes_per_splat: changed (frozen)")
    for section in ("devices", "tiers", "binary"):
        old_s, new_s = old_g.get(section) or {}, new_g.get(section) or {}
        for key, value in old_s.items():
            if key not in new_s:
                problems.append(f"globals.{section}.{key}: removed")
            elif new_s[key] != value:
                problems.append(f"globals.{section}.{key}: changed (frozen)")
    head_records = {json.dumps(r, sort_keys=True) for r in head.get("reprime_records") or []}
    base_records = [json.dumps(r, sort_keys=True) for r in base.get("reprime_records") or []]
    for record in base_records:
        if record not in head_records:
            problems.append("reprime_records: a base record was removed or edited")
    base_set = set(base_records)
    new_records = [r for r in head.get("reprime_records") or [] if json.dumps(r, sort_keys=True) not in base_set]
    old_worlds, new_worlds = old_g.get("worlds") or {}, new_g.get("worlds") or {}
    for name, old in old_worlds.items():
        new = new_worlds.get(name)
        if not isinstance(new, dict):
            problems.append(f"globals.worlds.{name}: removed")
            continue
        changed = sorted(k for k in set(old) | set(new) if old.get(k) != new.get(k))
        if not changed:
            continue
        covering = [
            r for r in new_records
            if isinstance(r, dict) and r.get("world") == name and all(
                str(r.get(k, "")).strip() for k in REPRIME_KEYS)
            and all(r.get(k) == new.get(k) for k in changed if k in new)
        ]
        if not covering:
            problems.append(
                f"globals.worlds.{name}: {changed} changed without a new reprime_records entry for {name} "
                "that names the new values, the run and the gate re-calibration record"
            )
    old_waivers = [json.dumps(w, sort_keys=True) for w in base.get("waivers") or []]
    new_waivers = {json.dumps(w, sort_keys=True) for w in head.get("waivers") or []}
    if any(w not in new_waivers for w in old_waivers):
        problems.append("waivers: a base waiver was removed or edited")
    return problems


def compare(base: dict, head: dict) -> List[str]:
    problems: List[str] = []
    for section in SECTIONS:
        problems += _compare_entries(section, base.get(section) or {}, head.get(section) or {})
    problems += _compare_globals(base, head)
    problems += _compare_settings(base, head)
    return problems


# --------------------------------------------------------------------------
# base resolution
# --------------------------------------------------------------------------


def _git(repo: Path, *args: str) -> Tuple[int, str, str]:
    proc = subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", check=False)
    return proc.returncode, proc.stdout, proc.stderr


def resolve_base(repo: Path, base_ref: Optional[str]) -> Tuple[str, str]:
    """(commit sha, how it was chosen). Never HEAD by default; an unresolvable base is an error."""
    if base_ref:
        candidates = [(base_ref, "--base-ref")]
    elif os.environ.get("GS_PR_BASE_SHA", "").strip():
        candidates = [(os.environ["GS_PR_BASE_SHA"].strip(), "GS_PR_BASE_SHA")]
    elif os.environ.get("GITHUB_BASE_REF", "").strip():
        candidates = [(f"origin/{os.environ['GITHUB_BASE_REF'].strip()}", "GITHUB_BASE_REF")]
    else:
        candidates = [("origin/master", "default origin/master")]
    ref, how = candidates[0]
    code, out, err = _git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
    if code != 0 or not out.strip():
        raise GuardError(f"cannot resolve the review base {ref!r} ({how}): {err.strip() or 'unknown revision'}")
    return out.strip(), f"{ref} ({how})"


def read_base(repo: Path, sha: str) -> Optional[dict]:
    """The file at the base, or None when the base predates it."""
    code, out, _ = _git(repo, "cat-file", "-e", f"{sha}:{THRESHOLDS_REL}")
    if code != 0:
        code, out, err = _git(repo, "ls-tree", "--name-only", sha, "--", THRESHOLDS_REL)
        if code != 0:
            raise GuardError(f"cannot list {THRESHOLDS_REL} at {sha}: {err.strip()}")
        if out.strip():
            raise GuardError(f"{THRESHOLDS_REL} exists at {sha} but cannot be read")
        return None
    code, out, err = _git(repo, "show", f"{sha}:{THRESHOLDS_REL}")
    if code != 0:
        raise GuardError(f"cannot read {THRESHOLDS_REL} at {sha}: {err.strip()}")
    return parse(out, f"{THRESHOLDS_REL}@{sha[:12]}")


def run(repo: Path, base_ref: Optional[str]) -> Tuple[int, List[str]]:
    lines: List[str] = []
    head_path = repo / THRESHOLDS_REL
    if not head_path.is_file():
        raise GuardError(f"{THRESHOLDS_REL} is missing at HEAD")
    head = parse(head_path.read_text(encoding="utf-8"), THRESHOLDS_REL)
    sha, how = resolve_base(repo, base_ref)
    problems = [f"HEAD: {p}" for p in validate(head)]
    code, head_sha, _ = _git(repo, "rev-parse", "HEAD")
    base = read_base(repo, sha)
    if base is None:
        lines.append(f"base {how} = {sha[:12]} has no {THRESHOLDS_REL}: the file is introduced here; "
                     "validated on its own, nothing to compare")
    else:
        if code == 0 and head_sha.strip() == sha:
            lines.append(f"informational: base {how} is HEAD itself ({sha[:12]}); the comparison is vacuous "
                         "(schedule / dispatch). The file was validated on its own.")
        problems += compare(base, head)
        lines.append(f"compared against base {how} = {sha[:12]}")
    if problems:
        return EXIT_FIRED, lines + [f"FAIL {p}" for p in problems]
    return EXIT_CLEAN, lines + ["flythrough thresholds: no loosening, file valid"]


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--base-ref", default=None, help="the review base (commit or ref); never HEAD")
    parser.add_argument("--repo", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        code, lines = run(args.repo, args.base_ref)
    except GuardError as exc:
        print(f"check_flythrough_thresholds: CANNOT RUN: {exc}")
        return EXIT_CANNOT_RUN
    for line in lines:
        print(f"check_flythrough_thresholds: {line}")
    return code


if __name__ == "__main__":
    sys.exit(main())
