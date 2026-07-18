#!/usr/bin/env python3
"""Guard: no test case is silently stranded outside every CI lane (#520).

## The failure this guards against

A doctest `TEST_CASE` only ever executes if some lane's wildcard filter matches
its name. The lanes a CI run can actually execute are:

* `tests/ci/run_module_tests.py` - `MODULE_TEST_FILTERS` (the headless module
  lanes), and
* `tests/ci/run_gpu_harness.py` - `BATCHES` (the GPU harness batches).

`REQUIRES_RD_TEST_FILTERS` is deliberately **not** counted. `_build_module_test_runs()`
only appends that lane under `--gpu` / `GS_RUN_GPU_TESTS=1`, and the blocking
workflow (`.github/workflows/gaussian_production_gates.yml`) invokes the runner
without it; even when it does run it is `strict=False`. Its own comment calls it
"a catalogue of renderer-dependent tests". Crediting a catalogue as coverage
would make cases look covered when no CI lane can fail on them - which is the
defect this guard exists to find, not one it should commit (Codex, PR #658). The
count of stranded cases that appear in that catalogue is printed as a separate,
non-gating number.

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

A registered test case that matches **no lane a CI run can execute** and is not
declared in `tests/ci/quarantine_manifest.json` under `unlaned_tests`.

Declaring an exclusion requires an owner, a linked issue, an expiry and a
**count**, so a test that nobody runs is a tracked decision rather than an
accident.

The count is what stops a declaration becoming an open-ended amnesty. Patterns
like `[TileRenderer]*` are family wildcards, so without it a brand-new stranded
case joining an already-declared family would pass silently - the declaration
would cover tests written long after anyone agreed to it. Declarations are
verified in **both** directions: an entry matching zero stranded cases FAILS as
stale, an entry matching more than it declares FAILS as an undeclared newcomer,
and an entry matching fewer FAILS with an instruction to lower the count so the
slack cannot be reoccupied.

## Corpus scope

The module tests, plus the engine test tree scanned **recursively** for cases
whose name mentions gaussian - `tests/test_projection_math.cpp` carries tagged
`[Projection]` cases, and `tests/servers/rendering/test_renderer_scene_cull.h`
carries an untagged one that a tag-filtered, top-level-only scan misses. Upstream
Godot's other engine tests are deliberately out of scope: they run under Godot's
own unfiltered `--test` suite rather than our gaussian-scoped lanes, so "matches
no lane here" is not a defect for them.

## What this guard deliberately does NOT check

* **Whether a matched lane is strict.** 416 of 756 registered cases reach no
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
from dataclasses import dataclass
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
    "count",
    "reason",
    "issue_url",
    "owner",
    "expires_utc",
)

_CASE_RE = re.compile(r'\bTEST_CASE\s*\(\s*"((?:[^"\\]|\\.)*)"')
_GAUSSIAN_NAME = "gaussian"


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

    # The corpus is not confined to the module directory. Scan the whole engine
    # test tree RECURSIVELY and keep any case whose NAME mentions gaussian, rather
    # than requiring the [GaussianSplatting] tag: tests/test_projection_math.cpp
    # carries tagged [Projection] cases, while
    # tests/servers/rendering/test_renderer_scene_cull.h carries an untagged
    # "[RendererSceneCull] Hidden indexing policy gates Gaussian exemption" case
    # that a tag-filtered, non-recursive scan misses entirely.
    #
    # Matching on the name (not the tag) is the derived rule: our lanes are
    # gaussian-scoped, so a gaussian-relevant case is exactly what they are
    # supposed to cover. Upstream Godot's other engine tests are deliberately out
    # of scope - they run under Godot's own unfiltered --test suite, not our lanes,
    # so "matches no gaussian lane" is not a defect for them.
    for path in sorted(
        list(ENGINE_TESTS_DIR.rglob("*.cpp")) + list(ENGINE_TESTS_DIR.rglob("*.h"))
    ):
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        relevant = [
            m.group(1) for m in _CASE_RE.finditer(text) if _GAUSSIAN_NAME in m.group(1).lower()
        ]
        if relevant:
            rel = path.relative_to(ROOT).as_posix()
            notes.append(
                f"corpus includes {len(relevant)} gaussian-relevant case(s) from "
                f"{rel} (outside the module tests directory)."
            )
            cases.extend((name, path.name) for name in relevant)

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
        if not isinstance(entry.get("count"), int) or entry["count"] < 1:
            problems.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) 'count' must be a positive "
                f"integer (the number of stranded cases this declaration covers)."
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


@dataclass
class Analysis:
    """The corpus/lane picture. Single source of truth for the guard AND its test."""

    cases: list[tuple[str, str]]
    stranded: list[tuple[str, str]]
    no_strict: int
    requires_rd_only: int
    notes: list[str]


def analyze() -> Analysis:
    """Compute which registered cases no CI lane can run.

    Exposed (rather than inlined in main) so the unit test asserts against the
    SAME logic the guard uses. An earlier version re-derived the lane set inside
    the test, and the two silently disagreed the moment the guard's rules changed
    - the exact drift this guard exists to catch.
    """
    runner = _load_module("_gs_run_module_tests", CI_DIR / "run_module_tests.py")
    harness = _load_module("_gs_run_gpu_harness", CI_DIR / "run_gpu_harness.py")
    linkage = _load_module("_gs_check_test_linkage", CI_DIR / "check_test_linkage.py")

    cases, notes = _collect_corpus(linkage._strip_comments)

    # Lanes that a CI run can actually execute. REQUIRES_RD_TEST_FILTERS is NOT
    # among them: `_build_module_test_runs()` only appends that lane under
    # --gpu / GS_RUN_GPU_TESTS=1, and the blocking workflow
    # (.github/workflows/gaussian_production_gates.yml) invokes the runner
    # without it. Its own comment says it "serves as a catalogue of
    # renderer-dependent tests" - crediting a catalogue as coverage would report
    # coverage that does not exist, which is the defect this guard exists to find.
    module_lanes = list(runner.MODULE_TEST_FILTERS)
    requires_rd_lanes = list(runner.REQUIRES_RD_TEST_FILTERS)
    gpu_batches = [(batch.name, tuple(batch.filters)) for batch in harness.BATCHES]

    stranded: list[tuple[str, str]] = []
    no_strict = 0
    requires_rd_only = 0
    for case_name, file_name in cases:
        module_hit = any(_lane_matches(case_name, inc, exc) for _, inc, exc, _ in module_lanes)
        strict_hit = any(
            _lane_matches(case_name, inc, exc)
            for _, inc, exc, strict in module_lanes
            if strict
        )
        gpu_hit = any(
            _doctest_wildcmp(case_name, pattern)
            for _, filters in gpu_batches
            for pattern in filters
        )
        if not module_hit and not gpu_hit:
            stranded.append((case_name, file_name))
            if any(_lane_matches(case_name, inc, exc) for _, inc, exc, _ in requires_rd_lanes):
                requires_rd_only += 1
        if not strict_hit and not gpu_hit:
            no_strict += 1

    return Analysis(cases, stranded, no_strict, requires_rd_only, notes)


def attribute(
    stranded: list[tuple[str, str]], declarations: list[dict]
) -> tuple[dict[int, int], list[tuple[str, str]]]:
    """First-match attribution of stranded cases to declarations.

    First match wins, so declaration ORDER matters: a narrow family listed before
    a catch-all keeps its own cases. Shared with the unit test for the same reason
    as analyze().
    """
    matched_by: dict[int, int] = {index: 0 for index in range(len(declarations))}
    undeclared: list[tuple[str, str]] = []
    for case_name, file_name in stranded:
        for index, entry in enumerate(declarations):
            if _doctest_wildcmp(case_name, str(entry["test_case"])):
                matched_by[index] += 1
                break
        else:
            undeclared.append((case_name, file_name))
    return matched_by, undeclared


def main() -> int:
    failures: list[str] = []

    analysis = analyze()
    cases, notes = analysis.cases, analysis.notes
    if not cases:
        print("[test-lane-coverage] FAIL parsed 0 TEST_CASEs - the corpus scan is broken.")
        return 1
    stranded = analysis.stranded
    no_strict = analysis.no_strict
    requires_rd_only = analysis.requires_rd_only

    declarations, manifest_problems = _load_unlaned_declarations()
    failures.extend(manifest_problems)

    matched_by, undeclared = attribute(stranded, declarations)

    for index, entry in enumerate(declarations):
        actual = matched_by[index]
        declared = entry["count"]
        if actual == 0:
            failures.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) matches NO currently "
                f"stranded test case. It is stale - the tests were given a lane, renamed "
                f"or deleted. Remove the declaration."
            )
        elif actual > declared:
            failures.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) now matches {actual} "
                f"stranded case(s) but declares {declared}. {actual - declared} NEW stranded "
                f"case(s) joined an already-declared family - a wildcard declaration must not "
                f"silently amnesty cases written after it. Give the new case(s) a lane, or "
                f"raise 'count' deliberately with justification ({entry['issue_url']})."
            )
        elif actual < declared:
            failures.append(
                f"unlaned_tests[{index}] ({entry['test_case']!r}) matches {actual} stranded "
                f"case(s) but declares {declared}. Case(s) were laned, renamed or deleted - "
                f"lower 'count' to {actual} so the slack cannot be reoccupied."
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
            f"[test-lane-coverage] FAIL {len(failures)} problem(s); "
            f"{len(undeclared)} undeclared of {len(stranded)} stranded / "
            f"{len(cases)} registered."
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
    print(
        f"[test-lane-coverage] report (not gated): {requires_rd_only} of the stranded "
        f"case(s) are listed in the opt-in [requires-RD] catalogue, which blocking CI "
        f"never runs. The catalogue is not counted as coverage."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
