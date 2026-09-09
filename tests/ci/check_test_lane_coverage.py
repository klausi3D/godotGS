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

## Strict-coverage contracts: a promotion must not be able to quietly unwind

Reaching *a* lane is not the same as reaching a lane that can fail CI. When a
corpus is deliberately promoted out of the advisory `[untagged]` safety net into
its own strict lane, the promotion is a decision - and nothing used to hold it in
place. `run_module_tests.py` expresses such a promotion with **two** coupled
edits: the tag joins `HEADLESS_GAUSSIAN_SCOPED_TAGS` (so the advisory net stops
claiming those cases) and a `strict=True` lane is added to `MODULE_TEST_FILTERS`.
Delete *both* and every case silently falls back to the advisory net; retag *some*
of the cases and those cases fall back while the strict lane stays green and
non-empty. Neither shape is stranded, so neither trips the check above, and the
un-gated no-strict count moves by an amount nobody watches (measured on PR #850:
381 -> 392 for the both-entries deletion, 381 -> 385 for a four-case retag - both
exit 0).

`STRICT_COVERAGE_CONTRACTS` closes that. A contract names a promoted corpus **by
its source file(s) and by its tag pattern**, and the guard asserts the *property*:
every case in that corpus is executed by at least one lane whose `strict` flag is
true. Both sides are derived - the cases from the test sources, the lanes from
`MODULE_TEST_FILTERS` - so no list of case names or lane names is maintained here.
What is written down is only the policy ("this corpus is load-bearing"), which is
exactly the thing that cannot be derived from a tree that no longer contains it.

The two keys are deliberately redundant, because each covers the other's blind
spot: retagging a case in place keeps it in the *file* set, moving a case to
another file keeps it in the *tag* set. Both must be non-empty on their own,
which is what stops a misspelled tag or a renamed file from turning the contract
into a check of nothing. A contract can still be defeated by editing it, by
deleting the tests, or by moving a case to a new file *and* retagging it in the
same change - all of which are visible, deliberate acts in a diff rather than a
side effect of touching a lane table.

## Corpus scope

The module tests, plus the engine test tree scanned **recursively** for cases
whose name mentions gaussian - `tests/test_projection_math.cpp` carries tagged
`[Projection]` cases, and `tests/servers/rendering/test_renderer_scene_cull.h`
carries an untagged one that a tag-filtered, top-level-only scan misses. Upstream
Godot's other engine tests are deliberately out of scope: they run under Godot's
own unfiltered `--test` suite rather than our gaussian-scoped lanes, so "matches
no lane here" is not a defect for them.

## What this guard deliberately does NOT check

* **Whether a matched lane is strict, for the corpus as a whole.** 381 of 856
  registered cases reach no *strict* lane - most of them legitimately, because
  they live in the GPU harness or in an advisory safety-net lane. Failing on that
  globally would demand ~400 quarantine entries, i.e. it would turn the manifest
  into the very rubber stamp this guard exists to prevent. The global number is
  REPORTED (see the summary line), not gated. Individual corpora ARE gated - see
  the strict-coverage contracts below, which are the deliberate ratchet this
  bullet used to only promise.
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


@dataclass(frozen=True)
class StrictCoverageContract:
    """A corpus whose strict-lane coverage is a promise, not an accident.

    `sources` are test-source **basenames** (the corpus records `path.name`);
    `test_case` is a doctest-style wildcard matched against case names. The
    protected set is the UNION of the two, and each side must be non-empty on its
    own - see the module docstring for why the redundancy is the point.

    This tuple is the one thing here that is written down rather than derived,
    and it has to be: it records a decision ("this corpus must be able to fail
    CI"), which is precisely the fact a tree that has already lost the lane can
    no longer tell you. It is NOT a list of cases and NOT a list of lanes; both
    of those are re-derived on every run.
    """

    name: str
    sources: tuple[str, ...]
    test_case: str
    issue_url: str
    rationale: str


STRICT_COVERAGE_CONTRACTS: tuple[StrictCoverageContract, ...] = (
    StrictCoverageContract(
        name="[DataAuthority]",
        sources=("test_data_authority_hardening.h",),
        test_case="*][DataAuthority]*",
        issue_url="https://github.com/klausi3D/godotGS/issues/846",
        rationale=(
            "#846 promoted this corpus out of the advisory [untagged] safety net into "
            "its own strict lane. Five of its cases are the only executable proof of "
            "the fail-closed persistence defects fixed in #805 (coherent reset, the "
            "oversized-lane SHRINK that was a real OOB read, the refused getter "
            "allocation, transactional materialization, the refusing merge). A "
            "fail-closed proof that cannot fail CI is not a proof."
        ),
    ),
    StrictCoverageContract(
        name="[TestPump]",
        sources=("test_gs_pump.h",),
        test_case="*][TestPump]*",
        issue_url="https://github.com/klausi3D/godotGS/issues/879",
        rationale=(
            "#879 replaced every renderer warm-up's frame budget with the wall-clock "
            "deadline in gs_test_pump.h, so that one helper is now the only thing "
            "standing between those cases and a machine-speed race. Its PR review "
            "found the bound could be evaded from the inside: readiness was accepted "
            "before expiry was checked, so a frame that first observed readiness past "
            "the deadline still passed every caller. These cases are the only "
            "executable proof of the ordering (and of the floor that outranks it), and "
            "they need no GPU, so there is no reason for them to sit in an advisory lane."
        ),
    ),
)


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
class StrictContractResult:
    """What one `StrictCoverageContract` found. Shared by the guard and its test."""

    contract: StrictCoverageContract
    protected: list[tuple[str, str]]
    uncovered: list[tuple[str, str]]
    lanes: list[str]
    problems: list[str]

    @property
    def failures(self) -> list[str]:
        """Every reason this contract is not satisfied, as reportable lines."""
        lines = list(self.problems)
        for case_name, file_name in self.uncovered:
            lines.append(
                f"strict-coverage contract {self.contract.name}: "
                f'{file_name}: TEST_CASE("{case_name}") reaches NO strict lane in '
                f"run_module_tests.py. It can still run in an advisory lane, where a "
                f"failure is printed and tolerated, so it can no longer fail CI. Either "
                f"restore its strict lane (both halves: the HEADLESS_GAUSSIAN_SCOPED_TAGS "
                f"entry AND the strict MODULE_TEST_FILTERS tuple), or retire the contract "
                f"in check_test_lane_coverage.py with justification ({self.contract.issue_url})."
            )
        return lines


def evaluate_strict_coverage_contract(
    contract: StrictCoverageContract,
    cases: list[tuple[str, str]],
    module_lanes: list[tuple[str, tuple[str, ...], tuple[str, ...], bool]],
) -> StrictContractResult:
    """Assert the property: every case in `contract`'s corpus reaches a strict lane.

    Pure, and takes both derived inputs as arguments, so the unit test can drive it
    with a mutated corpus or a mutated lane table without editing the tree - and so
    the guard and the test cannot drift apart the way #520's did.

    Note the two vacuity checks. Without them a contract naming a renamed file and a
    misspelled tag would enumerate zero cases, find zero of them uncovered, and pass:
    a guard whose subject has silently become empty is the failure mode this whole
    file exists to catch, so it is a failure here rather than a green run.
    """
    problems: list[str] = []

    by_source = [(name, file_name) for name, file_name in cases if file_name in contract.sources]
    by_tag = [
        (name, file_name) for name, file_name in cases if _doctest_wildcmp(name, contract.test_case)
    ]

    for source in contract.sources:
        if not any(file_name == source for _, file_name in cases):
            problems.append(
                f"strict-coverage contract {contract.name}: source {source!r} contributes NO "
                f"TEST_CASE to the corpus. The file was renamed, deleted, emptied, or dropped "
                f"from the build (check_test_linkage.KNOWN_UNLINKED). A contract whose subject "
                f"is empty proves nothing, so this fails instead of passing vacuously "
                f"({contract.issue_url})."
            )
    if not by_tag:
        problems.append(
            f"strict-coverage contract {contract.name}: pattern {contract.test_case!r} matches NO "
            f"registered case. The tag was renamed or the pattern is misspelled; either way the "
            f"tag half of this contract is checking nothing ({contract.issue_url})."
        )

    protected = sorted(set(by_source) | set(by_tag))
    if not protected:
        problems.append(
            f"strict-coverage contract {contract.name}: protects ZERO cases. Nothing about it "
            f"can fail, so it is not a guard ({contract.issue_url})."
        )

    strict_lanes = [(name, inc, exc) for name, inc, exc, strict in module_lanes if strict]
    uncovered = [
        (case_name, file_name)
        for case_name, file_name in protected
        if not any(_lane_matches(case_name, inc, exc) for _, inc, exc in strict_lanes)
    ]
    lanes = sorted(
        {
            lane_name
            for case_name, _ in protected
            for lane_name, inc, exc in strict_lanes
            if _lane_matches(case_name, inc, exc)
        }
    )
    return StrictContractResult(contract, protected, uncovered, lanes, problems)


@dataclass
class Analysis:
    """The corpus/lane picture. Single source of truth for the guard AND its test."""

    cases: list[tuple[str, str]]
    stranded: list[tuple[str, str]]
    no_strict: int
    requires_rd_only: int
    notes: list[str]
    strict_contracts: list[StrictContractResult]


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

    strict_contracts = [
        evaluate_strict_coverage_contract(contract, cases, module_lanes)
        for contract in STRICT_COVERAGE_CONTRACTS
    ]

    return Analysis(cases, stranded, no_strict, requires_rd_only, notes, strict_contracts)


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

    # Strict-coverage contracts. A contract with no declared corpus at all would be
    # the same vacuity it forbids, so an empty contract tuple is itself a failure.
    if not analysis.strict_contracts:
        failures.append(
            "STRICT_COVERAGE_CONTRACTS is empty. Removing every contract removes the "
            "only check that a promoted corpus still reaches a lane that can fail CI."
        )
    for result in analysis.strict_contracts:
        failures.extend(result.failures)

    for note in notes:
        print(f"[test-lane-coverage] note: {note}")

    for result in analysis.strict_contracts:
        print(
            f"[test-lane-coverage] strict-coverage contract {result.contract.name}: "
            f"{len(result.protected)} protected case(s), {len(result.uncovered)} without a "
            f"strict lane; covering strict lane(s): "
            f"{', '.join(result.lanes) if result.lanes else '(none)'}."
        )

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
