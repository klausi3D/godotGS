#!/usr/bin/env python3
"""Guard: no test case is silently stranded outside every CI lane (#520).

## The failure this guards against

A doctest `TEST_CASE` only ever executes if some lane's wildcard filter matches
its name. Lanes are declared in two places:

* `tests/ci/run_module_tests.py` - `MODULE_TEST_FILTERS` + `REQUIRES_RD_TEST_FILTERS`
  (the headless module lanes), and
* `tests/ci/run_gpu_harness.py` - `BATCHES` (the GPU harness batches).

Nothing has ever checked that the union of those filters actually covers the
corpus. When it does not, the affected cases compile, link and register, are
reported by nothing, and fail nothing. They are indistinguishable from tests that
do not exist - except that they look like coverage in the source tree.

`[World]` is the case that motivated this guard: `run_module_tests.py` excludes
`*][World][SceneTree]*` from the `[SceneTree]` lane, which reads as deliberate
de-duplication against a `[World]` lane - but no `[World]` lane was ever defined.
The neighbouring `*][Node][SceneTree]*` / `*][Container][SceneTree]*` exclusions
DO dedupe against real lanes, so the orphan was camouflaged by its own context.

## Why this guard derives its inputs instead of listing them

Issue #520 catalogued three stranded families by hand and missed a fourth that
existed at filing time. A hand-maintained tag list is the same class of artifact
as the bug: it is only as complete as the last person to edit it. So:

* the **corpus** is parsed out of the test sources (every `TEST_CASE` name), and
* the **lanes** are imported from the runner modules themselves.

Neither side is transcribed here. Adding a lane, deleting a lane, adding a test,
or retagging a test all re-derive automatically; nothing needs to be kept in sync
by hand.

## Wildcard semantics: doctest, not fnmatch

This is the detail that lets an audit be confidently wrong. Python's `fnmatch`
treats `[...]` as a **character class**, so `fnmatch("[GaussianSplatting][World] x",
"*][World][SceneTree]*")` answers a question doctest never asked. doctest's own
matcher (`thirdparty/doctest/doctest.h`, `wildcmp`) treats `*` and `?` as the ONLY
special characters - every `[` and `]` is literal.

`_doctest_wildcmp` below is a direct port of that function, including the detail
that doctest's `case_sensitive` option **defaults to false**
(`doctest.h`: `DOCTEST_PARSE_AS_BOOL_OR_FLAG("case-sensitive", "cs", case_sensitive, false)`),
so lane matching is case-INSENSITIVE. Any audit done with `fnmatch`, or with a
case-sensitive matcher, can disagree with what CI actually runs.

## What FAILS

A registered test case that matches **no lane in any runner** and is not declared
in `tests/ci/quarantine_manifest.json` under `unlaned_tests`.

Declaring an exclusion requires an owner, a linked issue and an expiry, so a test
that nobody runs is a tracked decision rather than an accident. Declarations are
verified in both directions: an entry that matches **zero** currently-stranded
cases FAILS as stale, so the list cannot rot into a permanent amnesty.

## What this guard deliberately does NOT check

* **Whether a matched lane is strict.** 409 of 749 registered cases reach no
  *strict* lane - most of them legitimately, because they live in the GPU harness
  or in an advisory safety-net lane. Failing on that today would demand ~400
  quarantine entries, i.e. it would turn the manifest into the very rubber stamp
  this guard exists to prevent. The strict-coverage picture is REPORTED (see the
  summary line) so the number is visible and can be ratcheted deliberately; it is
  not gated here.
* **Whether a matched case actually asserts anything.** A case that matches a lane
  and then early-returns past every assertion is vacuous, not stranded. That is a
  different defect with a different signal (see #520's skip-guard discussion); no
  lane configuration can detect it.
* **Whether a case's object file survives the linker.** That is
  `check_test_linkage.py`'s job. Cases in files it lists in `KNOWN_UNLINKED` are
  skipped here so the two guards do not double-report the same dead cases.
"""

from __future__ import annotations

import importlib.util
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[2]
CI_DIR = ROOT / "tests" / "ci"
MODULE_TESTS_DIR = ROOT / "modules" / "gaussian_splatting" / "tests"
ENGINE_TESTS_DIR = ROOT / "tests"
QUARANTINE_MANIFEST_PATH = CI_DIR / "quarantine_manifest.json"

# Fields every `unlaned_tests` declaration MUST carry. Same posture as the
# quarantine manifest's own entries: a declaration without an owner, an issue and
# an expiry is an untracked skip wearing a manifest entry's clothes.
UNLANED_REQUIRED_FIELDS: tuple[str, ...] = (
    "test_case",
    "reason",
    "issue_url",
    "owner",
    "expires_utc",
)

_CASE_RE = re.compile(r'\bTEST_CASE\s*\(\s*"((?:[^"\\]|\\.)*)"')
_GAUSSIAN_TAG = "[gaussiansplatting]"


def _load_module(alias: str, path: Path):
    """Import a sibling CI script by path without requiring a package layout."""
    spec = importlib.util.spec_from_file_location(alias, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    # Registered before exec so dataclasses in the target module can resolve
    # their own __module__ during class creation.
    sys.modules[alias] = module
    spec.loader.exec_module(module)
    return module


def _doctest_wildcmp(text: str, pattern: str, case_sensitive: bool = False) -> bool:
    """Port of doctest's `wildcmp` (thirdparty/doctest/doctest.h).

    Only `*` and `?` are special; `[` and `]` are literal. Defaults to
    case-insensitive because doctest's `case_sensitive` option defaults to false.
    """
    if not case_sensitive:
        text = text.lower()
        pattern = pattern.lower()

    n_text, n_pat = len(text), len(pattern)
    ti = pi = 0
    star_pat = star_text = 0

    while ti < n_text and (pi >= n_pat or pattern[pi] != "*"):
        if pi >= n_pat:
            return False
        if pattern[pi] != text[ti] and pattern[pi] != "?":
            return False
        pi += 1
        ti += 1

    while ti < n_text:
        if pi < n_pat and pattern[pi] == "*":
            pi += 1
            if pi >= n_pat:
                return True
            star_pat = pi
            star_text = ti + 1
        elif pi < n_pat and (pattern[pi] == text[ti] or pattern[pi] == "?"):
            pi += 1
            ti += 1
        else:
            pi = star_pat
            ti = star_text
            star_text += 1

    while pi < n_pat and pattern[pi] == "*":
        pi += 1
    return pi >= n_pat


def _lane_matches(case_name: str, includes: Iterable[str], excludes: Iterable[str]) -> bool:
    """A lane runs a case when some include matches and no exclude matches."""
    if not any(_doctest_wildcmp(case_name, pattern) for pattern in includes):
        return False
    return not any(_doctest_wildcmp(case_name, pattern) for pattern in excludes)


def _collect_corpus(strip_comments) -> tuple[list[tuple[str, str]], list[str]]:
    """Parse every registered TEST_CASE name out of the test sources.

    Returns (cases, notes) where cases is a list of (case_name, file_name).
    """
    notes: list[str] = []
    cases: list[tuple[str, str]] = []

    if not MODULE_TESTS_DIR.is_dir():
        raise RuntimeError(f"module tests directory missing: {MODULE_TESTS_DIR}")

    linkage = _load_module("_gs_check_test_linkage", CI_DIR / "check_test_linkage.py")
    dropped_files = set(linkage.KNOWN_UNLINKED)

    module_files = sorted(
        list(MODULE_TESTS_DIR.glob("*.h")) + list(MODULE_TESTS_DIR.glob("*.cpp"))
    )
    for path in module_files:
        if path.name in dropped_files:
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in _CASE_RE.finditer(text):
            cases.append((match.group(1), path.name))

    # The corpus is not confined to the module directory: tests/test_projection_math.cpp
    # carries [GaussianSplatting][Projection] cases from the engine tests dir. Discover
    # those by scanning for the module tag rather than by naming the file.
    for path in sorted(ENGINE_TESTS_DIR.glob("*.cpp")):
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        found = [m.group(1) for m in _CASE_RE.finditer(text)]
        tagged = [name for name in found if _GAUSSIAN_TAG in name.lower()]
        if tagged:
            notes.append(
                f"corpus includes {len(tagged)} [GaussianSplatting] case(s) from "
                f"tests/{path.name} (outside the module tests directory)."
            )
            cases.extend((name, path.name) for name in tagged)

    return cases, notes


def _load_unlaned_declarations() -> tuple[list[dict], list[str]]:
    """Read the `unlaned_tests` array from the quarantine manifest.

    A missing file or missing key is treated as "no declarations" (the guard then
    fails on any stranded case), never as a pass.
    """
    if not QUARANTINE_MANIFEST_PATH.is_file():
        return [], [f"quarantine manifest not found at {QUARANTINE_MANIFEST_PATH}"]
    try:
        data = json.loads(QUARANTINE_MANIFEST_PATH.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [], [f"quarantine manifest is not valid JSON: {exc}"]
    if not isinstance(data, dict):
        return [], ["quarantine manifest root must be a JSON object."]

    declarations = data.get("unlaned_tests", [])
    if not isinstance(declarations, list):
        return [], ["quarantine manifest 'unlaned_tests' must be a list."]

    problems: list[str] = []
    valid: list[dict] = []
    now = datetime.now(timezone.utc)
    for index, entry in enumerate(declarations):
        if not isinstance(entry, dict):
            problems.append(f"unlaned_tests[{index}] must be an object.")
            continue
        missing = [f for f in UNLANED_REQUIRED_FIELDS if not str(entry.get(f, "")).strip()]
        if missing:
            problems.append(
                f"unlaned_tests[{index}] ({entry.get('test_case', '?')!r}) is missing "
                f"required field(s): {', '.join(missing)}."
            )
            continue
        expires_raw = str(entry["expires_utc"]).strip()
        try:
            expires = datetime.fromisoformat(expires_raw.replace("Z", "+00:00"))
        except ValueError:
            problems.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) has an unparseable "
                f"'expires_utc': {expires_raw!r} (want ISO-8601, e.g. 2026-10-01T00:00:00Z)."
            )
            continue
        if expires.tzinfo is None:
            expires = expires.replace(tzinfo=timezone.utc)
        if expires <= now:
            problems.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) EXPIRED on {expires_raw}. "
                f"Give the tests a lane, or renew the declaration with fresh justification "
                f"({entry['issue_url']})."
            )
            continue
        valid.append(entry)
    return valid, problems


def main() -> int:
    failures: list[str] = []
    notes: list[str] = []

    runner = _load_module("_gs_run_module_tests", CI_DIR / "run_module_tests.py")
    harness = _load_module("_gs_run_gpu_harness", CI_DIR / "run_gpu_harness.py")
    linkage = _load_module("_gs_check_test_linkage", CI_DIR / "check_test_linkage.py")

    cases, corpus_notes = _collect_corpus(linkage._strip_comments)
    notes.extend(corpus_notes)
    if not cases:
        print("[test-lane-coverage] FAIL parsed 0 TEST_CASEs - the corpus scan is broken.")
        return 1

    module_lanes = [
        (name, inc, exc, strict)
        for name, inc, exc, strict in
        list(runner.MODULE_TEST_FILTERS) + list(runner.REQUIRES_RD_TEST_FILTERS)
    ]
    gpu_batches = [(batch.name, tuple(batch.filters)) for batch in harness.BATCHES]

    stranded: list[tuple[str, str]] = []
    no_strict = 0
    for case_name, file_name in cases:
        module_hits = [
            name for name, inc, exc, _ in module_lanes if _lane_matches(case_name, inc, exc)
        ]
        strict_hits = [
            name for name, inc, exc, strict in module_lanes
            if strict and _lane_matches(case_name, inc, exc)
        ]
        gpu_hits = [
            name for name, filters in gpu_batches
            if any(_doctest_wildcmp(case_name, pattern) for pattern in filters)
        ]
        if not module_hits and not gpu_hits:
            stranded.append((case_name, file_name))
        if not strict_hits and not gpu_hits:
            no_strict += 1

    declarations, manifest_problems = _load_unlaned_declarations()
    failures.extend(manifest_problems)

    undeclared: list[tuple[str, str]] = []
    matched_by: dict[int, int] = {index: 0 for index in range(len(declarations))}
    for case_name, file_name in stranded:
        hit = None
        for index, entry in enumerate(declarations):
            if _doctest_wildcmp(case_name, str(entry["test_case"])):
                hit = index
                break
        if hit is None:
            undeclared.append((case_name, file_name))
        else:
            matched_by[hit] += 1

    for index, entry in enumerate(declarations):
        if matched_by[index] == 0:
            failures.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) matches NO currently "
                f"stranded test case. It is stale - the tests were given a lane, renamed "
                f"or deleted. Remove the declaration."
            )

    for case_name, file_name in undeclared:
        failures.append(
            f"{file_name}: TEST_CASE(\"{case_name}\") matches NO lane in "
            f"run_module_tests.py and NO batch in run_gpu_harness.py. It can never run "
            f"and can never fail CI. Give it a lane, or declare it in "
            f"tests/ci/quarantine_manifest.json under 'unlaned_tests' with an owner, a "
            f"linked issue and an expiry."
        )

    for note in notes:
        print(f"[test-lane-coverage] note: {note}")

    if failures:
        print(
            f"[test-lane-coverage] FAIL {len(undeclared)} undeclared stranded case(s) "
            f"of {len(stranded)} stranded / {len(cases)} registered."
        )
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        f"[test-lane-coverage] PASS {len(cases)} registered case(s); "
        f"{len(stranded)} stranded, all declared in {len(declarations)} manifest "
        f"entr{'y' if len(declarations) == 1 else 'ies'}."
    )
    print(
        f"[test-lane-coverage] report (not gated): {no_strict} case(s) reach no STRICT "
        f"module lane and no GPU batch - see this script's docstring for why that is "
        f"reported rather than failed."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
