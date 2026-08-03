#!/usr/bin/env python3
"""Unit tests for the test-quarantine mechanism in run_module_tests.py.

Production-readiness C3 / exit criterion G5 (ledger #458). These tests exercise
the schema guard, the loader, and the doctest-lane wiring against temporary
manifest fixtures. They never write a non-empty manifest into the repo: every
fixture lives in a TemporaryDirectory and the module's QUARANTINE_MANIFEST_PATH
global is patched to point at it.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import re
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "run_module_tests.py"
spec = importlib.util.spec_from_file_location("run_module_tests", SCRIPT)
assert spec and spec.loader
harness = importlib.util.module_from_spec(spec)
# Register in sys.modules before exec: run_module_tests uses
# `from __future__ import annotations` + @dataclass, and dataclasses resolves the
# stringized annotations via sys.modules[cls.__module__] at class-creation time.
sys.modules[spec.name] = harness
spec.loader.exec_module(harness)


# A real lane name from MODULE_TEST_FILTERS so entries pass the unknown-lane check.
VALID_LANE = harness.MODULE_TEST_FILTERS[0][0]


COMMITTED_MANIFEST_PATH = ROOT / "tests" / "ci" / "quarantine_manifest.json"

# ---------------------------------------------------------------------------
# PINNED BASELINE / SHRINK-ONLY RATCHET (#650). Do not "update to make CI pass".
#
# Before this block the manifest had no size or content ratchet at all. A PR
# could append a quarantine entry, or an 11th `unlaned_tests` declaration, in a
# single hunk and go green -- the guard read the manifest and compared it
# against itself, which is not a ratchet. That is the same hole
# tests/ci/test_gpu_harness_deferred_contract.py:55-89 closed for
# unbatched_requires_gpu_backlog, and its comment records why the first version
# there did not work either.
#
# The baseline therefore lives HERE, in the guard, not in the data the guard
# checks. Both arrays are pinned three ways, because each catches something the
# others do not:
#   * a MAX      -- growth in size fails,
#   * a BASELINE -- the actual pinned set, so an addition is rejected by set
#                   INCLUSION (a fix-one/add-one swap nets zero and would pass a
#                   count-only check),
#   * a FINGERPRINT -- one hash over the COMPLETE declaration objects, in order,
#                   so any edit at all is a deliberate, review-visible re-pin
#                   rather than a one-liner. Both arrays get identical treatment
#                   (array_fingerprint): every field, including fields invented
#                   after this guard was written, and including the ORDER, which
#                   is semantic because lane-coverage attribution is
#                   first-match-wins (#664). Round 2 (Codex on #821) found the
#                   unlaned hash covering only (test_case, count), which left a
#                   rewritten owner/reason/risk/expiry - and an issue_url swapped
#                   between two allowlisted issues - completely invisible. That
#                   is the orphaning failure this branch exists to close, running
#                   in reverse.
#
# The ratchet turns ONE WAY: counts may go DOWN, never UP; entries may leave the
# manifest, never join it without a matching guard edit in the same PR.
# RAISING any constant below is a review RED FLAG. It means a test was newly
# stranded, or a new failure was quarantined, instead of being given a lane.
#
# Legitimate SHRINK (the only allowed direction):
#   1. give the case a lane in tests/ci/run_module_tests.py (or a batch in
#      run_gpu_harness.py) so it actually runs,
#   2. delete or lower its declaration in tests/ci/quarantine_manifest.json,
#   3. re-pin the constants below with
#        python tests/ci/test_quarantine_manifest.py --print-fingerprint
#      which REFUSES to print anything for a growth or an addition. There is no
#      writer: the constants are pasted by a human, and nothing in this repo can
#      regenerate this block from the current tree.
# ---------------------------------------------------------------------------

# The manifest's only legitimate homes. Pinned so a third array cannot be added
# as a fresh, unratcheted place to park declarations.
MANIFEST_TOP_LEVEL_KEYS = frozenset({"schema_version", "entries", "unlaned_tests"})

# 'entries' ships EMPTY and the measured headless baseline at the base SHA is
# zero laned failures, so the pinned maximum is 0. The one known surviving
# failure ([Thumbnail] cache, #814) structurally cannot be enrolled here: an
# entry names a MODULE_TEST_FILTERS lane, and no lane selects [Thumbnail].
QUARANTINE_ENTRIES_MAX = 0
QUARANTINE_ENTRIES_BASELINE: tuple[tuple[str, str], ...] = ()
QUARANTINE_ENTRIES_FINGERPRINT = (
    "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"
)

# 'unlaned_tests': 10 declarations covering 86 stranded cases, matching
# check_test_lane_coverage.py's "86 stranded, all declared in 10 manifest
# entries". Measured, not transcribed -- see --print-fingerprint.
UNLANED_MAX_DECLARATIONS = 10
UNLANED_MAX_TOTAL_COUNT = 86
UNLANED_BASELINE: tuple[tuple[str, int], ...] = (
    ("*][RequiresGPU]*", 59),
    ("[GPU Memory Stream]*", 6),
    ("[GaussianSplatting][GeneratePLY]*", 1),
    ("[GaussianSplatting][NodeSurface][World]*", 1),
    ("[GaussianSplatting][Thumbnail]*", 2),
    ("[GaussianSplatting][World]*", 3),
    ("[Integration]*", 9),
    ("[RendererSceneCull] Hidden indexing policy gates Gaussian exemption", 1),
    ("[Streaming VRAM]*", 1),
    ("[VisualCompare]*", 3),
)
# Re-pinned in round 2 (Codex on #821) because the HASH INPUT widened, not
# because the data moved: the fingerprint now covers every field of every
# declaration, in order, exactly like QUARANTINE_ENTRIES_FINGERPRINT, instead of
# only the (test_case, count) projection. The declaration set is byte-identical
# either side of the re-pin -- 10 declarations, 86 total, LOST 0 / GAINED 0 --
# and UNLANED_MAX_DECLARATIONS / UNLANED_MAX_TOTAL_COUNT / UNLANED_BASELINE are
# untouched above. Previous value, for audit: 599451ce55bba30f68959d61a61526989
# fe2046ed0e7ac31cedfa77c9f525b9e.
UNLANED_FINGERPRINT = (
    "16d05a33e1ffa19ceca12e86896f77f66d02e07f24a90358dcce810fa87300f7"
)

# ---------------------------------------------------------------------------
# Per-declaration content rules (both arrays).
#
# Field PRESENCE was already checked by run_module_tests.py's schema guard, but
# presence is not hygiene: 'reason' could be "TODO", 'issue_url' could be any
# string, and 'expires_utc' could be a decade out -- a permanent silencer
# wearing a manifest entry's clothes.
# ---------------------------------------------------------------------------
MIN_REASON_CHARS = 40
PLACEHOLDER_REASON_TOKENS = frozenset(
    {"todo", "tbd", "fixme", "n/a", "na", "none", "unknown", "wip", "xxx", "?", "-"}
)
ISSUE_URL_RE = re.compile(r"^https://github\.com/klausi3D/godotGS/issues/([0-9]+)$")
VALID_RISK_CLASSES = frozenset({"R0", "R1", "R2", "R3"})
BASE_SHA_RE = re.compile(r"^[0-9a-f]{40}$")

# A quarantine is bounded by definition. An expiry a decade out is not a
# quarantine, it is a deletion with extra steps.
EXPIRY_HORIZON_DAYS = 180
# Absolute ceiling, pinned like the counts. The relative horizon alone does not
# stop SERIAL renewal: a PR could push every expiry out by 179 days, forever,
# and never trip it. Moving the ceiling is a guard edit, so a renewal is
# review-visible. Lowering it is always fine; raising it is the red flag.
MAX_EXPIRY_UTC = "2026-10-15T00:00:00Z"

# ---------------------------------------------------------------------------
# Tracking-issue liveness, checked OFFLINE.
#
# #520 was CLOSED while 8 declarations still pointed at it, and #329 was CLOSED
# while the 59-case [RequiresGPU] catch-all still pointed at it. Nothing noticed,
# because "the issue that owns this declaration is still open" was never
# checked. That is a silent expiry: the declaration outlives its tracking issue
# and the work becomes untracked while still looking blessed.
#
# This is enforced against a pinned ALLOWLIST rather than the GitHub API on
# purpose. A guard that needs the network is a guard that fails when the network
# does, and CI would then either block on rate limits or fail open. An allowlist
# is fail-closed in the useful direction: an issue nobody has verified as OPEN is
# rejected, so referencing a new tracking issue is a deliberate two-file diff.
#
# Verified OPEN on the date below with:
#   gh issue view <n> --repo klausi3D/godotGS --json number,state
# Verified CLOSED at the same time, and therefore deliberately NOT listed:
#   #329, #520.
ISSUES_VERIFIED_OPEN = frozenset({641, 814, 819, 820})
ISSUES_VERIFIED_OPEN_UTC = "2026-08-03T00:00:00Z"
# An allowlist can only answer "was this open when a human last looked". Bound
# how stale that answer may get, so the verification cannot silently become
# folklore. This horizon is deliberately LATER than MAX_EXPIRY_UTC: every
# declaration must already be renewed by then, and re-checking the issue state
# is part of that renewal.
ISSUE_VERIFICATION_MAX_AGE_DAYS = 180


def _future_iso(days: int = 365) -> str:
    return (datetime.now(timezone.utc) + timedelta(days=days)).isoformat()


def _past_iso(days: int = 1) -> str:
    return (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()


# The doctest case that _fail_output() reports as failing, and a test_case glob
# that matches it (and not other cases in the lane).
FAILING_CASE = "[GaussianSplatting][Animation] plays a clip"
MATCHING_TEST_CASE = "*plays a clip*"
# A second, distinct approved failing case in the SAME lane (multiple entries).
SECOND_FAILING_CASE = "[GaussianSplatting][Animation] loops the clip"
SECOND_TEST_CASE = "*loops the clip*"


def _valid_entry(**overrides) -> dict:
    entry = {
        "lane": VALID_LANE,
        "test_case": MATCHING_TEST_CASE,
        "reason": "known failure reproduced on base SHA",
        "issue_url": "https://github.com/example/repo/issues/999",
        "base_sha_proven_failing": "7c5b79aacbe2188da22f837cb37824a829d8e074",
        "owner": "alexander-klaus",
        "risk": "R3",
        "expires_utc": _future_iso(),
        "mitigation": "optional mitigation note",
    }
    entry.update(overrides)
    return entry


def _pass_output(tests: int = 3, asserts: int = 10) -> str:
    return (
        f"[doctest] test cases: {tests} | {tests} passed | 0 failed\n"
        f"[doctest] assertions: {asserts} | {asserts} passed | 0 failed\n"
    )


def _case_failure_block(case: str) -> str:
    # Mirrors doctest's ConsoleReporter failure block: a lazily-printed
    # "TEST CASE:  <name>" header followed by a "<file>(<line>): ERROR: ..." line.
    return (
        "===============================================================================\n"
        "modules/gaussian_splatting/tests/test_animation.h(42):\n"
        f"TEST CASE:  {case}\n"
        "\n"
        "modules/gaussian_splatting/tests/test_animation.h(50): ERROR: CHECK( a == b ) is NOT correct!\n"
        "  values: CHECK( 1 == 2 )\n"
        "\n"
    )


def _scenario_failure_block(scenario_name: str) -> str:
    # doctest prints a BDD SCENARIO's name WITHOUT the "TEST CASE:  " prefix
    # (thirdparty/doctest/doctest.h:6069-6071), because SCENARIO(x) expands to
    # TEST_CASE("  Scenario: " x) and logTestStart() suppresses the prefix for
    # names starting with "  Scenario:". Everything else in the block is
    # identical to a normal test case.
    return (
        "===============================================================================\n"
        "modules/gaussian_splatting/tests/test_animation.h(80):\n"
        f"  Scenario: {scenario_name}\n"
        "\n"
        "modules/gaussian_splatting/tests/test_animation.h(88): ERROR: CHECK( c == d ) is NOT correct!\n"
        "  values: CHECK( 3 == 4 )\n"
        "\n"
    )


def _fail_output(case: str = FAILING_CASE, total_cases: int = 3) -> str:
    # A realistic doctest failing run: one failing case block + the summary.
    return (
        _case_failure_block(case)
        + f"[doctest] test cases: {total_cases} | {total_cases - 1} passed | 1 failed\n"
        + "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _multi_fail_output(case_names: list[str]) -> str:
    blocks = "".join(_case_failure_block(case) for case in case_names)
    n = len(case_names)
    return (
        blocks
        + f"[doctest] test cases: {n + 1} | 1 passed | {n} failed\n"
        + f"[doctest] assertions: 10 | {10 - n} passed | {n} failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _failing_summary_no_case_output() -> str:
    # Summary reports failures but no TEST CASE header -> case name unparseable.
    return (
        "[doctest] test cases: 3 | 2 passed | 1 failed\n"
        "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        "[doctest] Status: FAILURE!\n"
    )


def _fail_output_with_skip(case: str = FAILING_CASE) -> str:
    # The matching failing case PLUS one environment-skip marker.
    #
    # This fixture used to be the skip prose alone, starting at column 0, under a
    # comment asserting that doctest emits it that way. doctest does not, and
    # never has. ConsoleReporter::log_message (thirdparty/doctest/doctest.h:6423)
    # calls file_line_to_stream() (:6051) FIRST, so every MESSAGE is printed as
    #     <file>(<line>): MESSAGE: <text>
    # A column-0 marker is unreachable, and the line-anchored
    # DOCTEST_SKIP_MARKER_RE that matched this fixture therefore matched nothing
    # a real run has ever produced. The fixture certified a fiction, and the
    # policy it "covered" had never fired once (#595).
    #
    # Both real shapes are exercised: the canonical `GS_ENV_SKIP:` token emitted
    # by modules/gaussian_splatting/tests/test_macros.h, and one of the ~354
    # not-yet-converted legacy prose sites, which must keep being counted until
    # slice GS-595-B converts them.
    return (
        _case_failure_block(case)
        + "modules/gaussian_splatting/tests/test_animation.h(61): MESSAGE: "
        "GS_ENV_SKIP: RenderingDevice unavailable\n"
        + "[doctest] test cases: 3 | 1 passed | 1 failed | 1 skipped\n"
        + "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _fail_output_with_legacy_skip(case: str = FAILING_CASE) -> str:
    # The legacy, unconverted prose shape in its REAL console framing. Counted
    # exactly like the canonical token, so the ~354 sites #595 deliberately does
    # not rewrite stay visible instead of dropping out of the number.
    return (
        _case_failure_block(case)
        + "modules/gaussian_splatting/tests/test_animation.h(61): MESSAGE: "
        "Skipping test - GPU device unavailable\n"
        + "[doctest] test cases: 3 | 1 passed | 1 failed | 1 skipped\n"
        + "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _no_coverage_output() -> str:
    return (
        "[doctest] test cases: 0 | 0 passed | 0 failed\n"
        "[doctest] assertions: 0 | 0 passed | 0 failed\n"
    )


def _no_summary_output() -> str:
    return "engine started\nfilter matched nothing meaningful\nengine exited\n"


@contextlib.contextmanager
def _manifest(entries, schema_version=1):
    """Write a temporary manifest and patch the module global to point at it."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "quarantine_manifest.json"
        payload: dict = {"schema_version": schema_version, "entries": entries}
        path.write_text(json.dumps(payload), encoding="utf-8")
        with mock.patch.object(harness, "QUARANTINE_MANIFEST_PATH", path):
            yield path


@contextlib.contextmanager
def _raw_manifest(text: str):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "quarantine_manifest.json"
        path.write_text(text, encoding="utf-8")
        with mock.patch.object(harness, "QUARANTINE_MANIFEST_PATH", path):
            yield path


def _run_lane(lane_name: str, strict: bool, godot_result):
    """Run the doctest-lane loop for a single lane with _run_godot stubbed."""
    buffer = io.StringIO()
    with mock.patch.object(harness, "_run_godot", return_value=godot_result):
        with contextlib.redirect_stdout(buffer):
            rc = harness._run_doctest_lanes(
                "godot",
                [(lane_name, ["--headless", "--test"], strict)],
                "warn-only",
                False,
            )
    return rc, buffer.getvalue()


class QuarantineGuardTests(unittest.TestCase):
    def test_empty_manifest_passes_guard_and_loads_empty(self) -> None:
        with _manifest([]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            self.assertEqual(harness._load_quarantine(), {})

    def test_committed_manifest_is_schema_valid(self) -> None:
        # The guard runs this unit test in CI, so this must assert only that the
        # committed manifest is SCHEMA-VALID - never that it is specifically
        # EMPTY. Asserting empty here would block the Slice 2 population path the
        # moment a valid human-approved entry is committed. An empty manifest and
        # a valid populated manifest both satisfy this.
        committed = ROOT / "tests" / "ci" / "quarantine_manifest.json"
        self.assertTrue(committed.is_file(), "committed manifest must exist")
        data = json.loads(committed.read_text(encoding="utf-8"))
        self.assertEqual(data.get("schema_version"), 1)
        self.assertIsInstance(data.get("entries"), list)
        ok, messages = harness._validate_quarantine_manifest_schema()
        self.assertTrue(ok, messages)

    def test_missing_file_loads_empty_and_guard_passes(self) -> None:
        missing = ROOT / "tests" / "ci" / "does_not_exist_quarantine.json"
        self.assertFalse(missing.is_file())
        with mock.patch.object(harness, "QUARANTINE_MANIFEST_PATH", missing):
            self.assertEqual(harness._load_quarantine(), {})
            ok, _ = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok)

    def test_expired_entry_fails_guard(self) -> None:
        with _manifest([_valid_entry(expires_utc=_past_iso())]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(any("past its expires_utc" in m for m in messages), messages)

    def test_unknown_lane_fails_guard(self) -> None:
        with _manifest([_valid_entry(lane="No Such Lane")]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("not present in MODULE_TEST_FILTERS" in m for m in messages), messages
            )

    def test_missing_required_field_fails_guard(self) -> None:
        entry = _valid_entry()
        del entry["issue_url"]
        with _manifest([entry]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("missing required field 'issue_url'" in m for m in messages), messages
            )

    def test_missing_test_case_fails_guard(self) -> None:
        # test_case is REQUIRED (round-3): a lane-only quarantine would tolerate
        # any failure in the lane, so the schema guard rejects an entry without it.
        entry = _valid_entry()
        del entry["test_case"]
        with _manifest([entry]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("missing required field 'test_case'" in m for m in messages), messages
            )

    def test_two_entries_same_lane_different_test_case_accepted(self) -> None:
        # Round-6: multiple entries per lane are allowed, one per approved case.
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            loaded = harness._load_quarantine()
            self.assertEqual(len(loaded[VALID_LANE]), 2)

    def test_exact_duplicate_lane_test_case_rejected(self) -> None:
        # Same (lane, test_case) twice is a real duplicate and is rejected.
        entries = [_valid_entry(), _valid_entry()]
        with _manifest(entries):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("(lane, test_case)" in m for m in messages), messages
            )

    def test_malformed_json_fails_guard(self) -> None:
        with _raw_manifest("{not valid json"):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(any("not valid JSON" in m for m in messages), messages)

    def test_valid_populated_manifest_is_accepted_and_honored(self) -> None:
        # Forward-compat (Slice 2): a populated manifest carrying a valid,
        # human-approved entry must be ACCEPTED by the guard and honored by the
        # loader - never blocked. Proves the documented population path works.
        entry = _valid_entry()
        with _manifest([entry]):
            # Schema validation accepts the populated manifest.
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            # The full guard also accepts it (recursion env set so the nested
            # unit-test step short-circuits instead of re-spawning the suite).
            with mock.patch.dict(
                os.environ, {harness.QUARANTINE_UNITTEST_ACTIVE_ENV: "1"}
            ):
                guard_ok, _ = harness._run_quarantine_manifest_guard()
            self.assertTrue(guard_ok)
            # The loader returns a LIST of entries keyed by lane.
            loaded = harness._load_quarantine()
            self.assertIn(VALID_LANE, loaded)
            self.assertEqual(len(loaded[VALID_LANE]), 1)
            self.assertEqual(loaded[VALID_LANE][0]["issue_url"], entry["issue_url"])

    def test_guard_messages_are_ascii(self) -> None:
        # A non-ASCII byte has crashed CI's cp1252 stdout before; keep it clean.
        with _manifest([_valid_entry(lane="No Such Lane", expires_utc=_past_iso())]):
            _, messages = harness._validate_quarantine_manifest_schema()
            for message in messages:
                message.encode("ascii")  # raises UnicodeEncodeError on failure

    def test_full_guard_runs_unittest_and_recursion_guard_short_circuits(self) -> None:
        # The full guard runs schema validation AND the mechanism's unit test.
        # With the recursion-guard env var set (as it is in the spawned child),
        # the unit-test step short-circuits instead of re-spawning the suite, so
        # this test never forks. Schema passes on the committed manifest (empty
        # or validly populated).
        with mock.patch.dict(
            os.environ, {harness.QUARANTINE_UNITTEST_ACTIVE_ENV: "1"}
        ):
            ok, messages = harness._run_quarantine_manifest_guard()
        self.assertTrue(ok, messages)
        self.assertTrue(
            any("nested guard invocation" in m for m in messages), messages
        )


class QuarantineLaneWiringTests(unittest.TestCase):
    def test_quarantined_matching_case_failure_is_tolerated(self) -> None:
        # The only failing case matches the entry's test_case -> tolerated.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _fail_output()))
        self.assertEqual(rc, 0, out)
        self.assertIn("[module-tests][QUARANTINE]", out)
        self.assertIn("failed as expected in matched case(s)", out)
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_different_case_in_same_lane_fails_the_run(self) -> None:
        # Round-3 core: a NEW/unrelated case failing in the same quarantined lane
        # must NOT be tolerated - it is a regression and fails the run.
        other = "[GaussianSplatting][Animation] a different unrelated test"
        with _manifest([_valid_entry(test_case=MATCHING_TEST_CASE)]):
            rc, out = _run_lane(
                VALID_LANE, strict=True, godot_result=(False, False, _fail_output(case=other))
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn(other, out)

    def test_quarantined_two_entries_same_lane_both_cases_tolerated(self) -> None:
        # Round-6: two entries for the same lane (distinct approved cases). A run
        # where BOTH approved cases fail is tolerated (union of patterns).
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(False, False, _multi_fail_output([FAILING_CASE, SECOND_FAILING_CASE])),
            )
        self.assertEqual(rc, 0, out)
        self.assertIn("failed as expected in matched case(s)", out)
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_two_entries_third_unapproved_case_fails_the_run(self) -> None:
        # Round-6 core: with two approved entries, a THIRD non-approved failing
        # case in the same lane still fails the run.
        third = "[GaussianSplatting][Animation] a third unapproved test"
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(
                    False,
                    False,
                    _multi_fail_output([FAILING_CASE, SECOND_FAILING_CASE, third]),
                ),
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn(third, out)

    def test_quarantined_stale_entry_warns_but_still_tolerates(self) -> None:
        # Round-7: a 2-entry lane where only ONE approved case actually fails.
        # The lane is still tolerated (rc 0), and the entry that matched no
        # current failing case is surfaced as a QUARANTINE-STALE-ENTRY WARN (not
        # a failure - it may be fixed OR simply did not run this pass).
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(False, False, _fail_output()),  # only FAILING_CASE fails
            )
        self.assertEqual(rc, 0, out)
        self.assertIn("failed as expected in matched case(s)", out)
        # Exactly one stale-entry WARN, naming the un-matched (second) entry's
        # pattern + its issue, and NOT the still-live first pattern.
        stale_lines = [ln for ln in out.splitlines() if "QUARANTINE-STALE-ENTRY" in ln]
        self.assertEqual(len(stale_lines), 1, out)
        self.assertIn(SECOND_TEST_CASE, stale_lines[0])
        self.assertIn("https://x/issues/2", stale_lines[0])
        self.assertNotIn(MATCHING_TEST_CASE, stale_lines[0])
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_mixed_matching_and_unexpected_fails_the_run(self) -> None:
        # A lane where the quarantined case AND another case both fail: the
        # unexpected one still forces the run to fail.
        other = "[GaussianSplatting][Animation] a different unrelated test"
        with _manifest([_valid_entry(test_case=MATCHING_TEST_CASE)]):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(False, False, _multi_fail_output([FAILING_CASE, other])),
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn(other, out)

    def test_quarantined_crash_is_whole_lane_tolerated(self) -> None:
        # A crash (nonzero exit, no per-case summary) cannot be narrowed; the
        # whole lane is tolerated (documented limitation).
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _no_summary_output()))
        self.assertEqual(rc, 0, out)
        self.assertIn("crashed as expected", out)
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_runnable_failure_without_case_name_fails(self) -> None:
        # A runnable failure whose failing case name cannot be parsed fails closed
        # (we cannot confirm it is the quarantined case).
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(
                VALID_LANE, strict=True, godot_result=(False, False, _failing_summary_no_case_output())
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNVERIFIED]", out)

    def test_quarantined_matching_case_with_new_skip_marker_fails_in_strict_ci(self) -> None:
        # Round-4: the approved failing case is present, but the lane ALSO printed
        # a new "Skipping test" marker. In strict CI a quarantine must apply the
        # same skipped-marker policy as a normal lane and FAIL - it tolerates only
        # its exact known failure, never newly skipped coverage.
        with mock.patch.dict(os.environ, {"CI": "1"}):
            with _manifest([_valid_entry()]):
                rc, out = _run_lane(
                    VALID_LANE, strict=True, godot_result=(False, False, _fail_output_with_skip())
                )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn("newly skipped coverage", out)

    def test_quarantined_skip_marker_tolerated_updates_totals_when_not_ci(self) -> None:
        # Outside strict CI the skipped-marker policy does not fail the lane, but
        # the skip counts must still be folded into the totals so they are not
        # silently reported as 0 behind the quarantine.
        with mock.patch.dict(os.environ, {"CI": ""}):
            with _manifest([_valid_entry()]):
                rc, out = _run_lane(
                    VALID_LANE, strict=True, godot_result=(False, False, _fail_output_with_skip())
                )
        self.assertEqual(rc, 0, out)
        self.assertIn("failed as expected in matched case(s)", out)
        self.assertIn("quarantined_failing=1", out)
        self.assertIn("lanes_with_skips=1", out)
        self.assertIn("skipped_markers=1", out)

    def test_quarantined_legacy_prose_skip_marker_fails_in_strict_ci(self) -> None:
        # #595: the ~354 not-yet-converted `MESSAGE("Skip…")` sites must keep
        # being detected in their REAL console framing. If the detector only
        # recognised the new canonical token, repairing it would shrink the
        # reported number while the hidden surface grew.
        with mock.patch.dict(os.environ, {"CI": "1"}):
            with _manifest([_valid_entry()]):
                rc, out = _run_lane(
                    VALID_LANE,
                    strict=True,
                    godot_result=(False, False, _fail_output_with_legacy_skip()),
                )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn("newly skipped coverage", out)

    def test_baseline_skip_regex_is_inert_on_real_console_output(self) -> None:
        # #595 regression pin. The pre-fix pattern was line-anchored, and doctest
        # ALWAYS prefixes a message with `<file>(<line>): MESSAGE: `
        # (thirdparty/doctest/doctest.h:6051, :6423), so it could not match any
        # real output. Both fixtures above are real-shaped; the old pattern must
        # find nothing in either, and the current detector must find exactly one
        # marker in each. Without this, re-anchoring the regex would silently
        # restore the inert gate and every fixture here would still be green.
        inert = re.compile(r"(?m)^\s*(?:Skipping(?: test)?\s*-\s+.+)$")
        for label, sample in (
            ("canonical", _fail_output_with_skip()),
            ("legacy prose", _fail_output_with_legacy_skip()),
        ):
            self.assertEqual([], inert.findall(sample), label)
            self.assertEqual(
                1, len(harness.DOCTEST_SKIP_MARKER_RE.findall(sample)), label
            )

    def test_quarantined_zero_coverage_is_coverage_lost_and_nonzero(self) -> None:
        # Codex P2 (comment 3601513465): a quarantined lane that exits 0 with a
        # summary but zero executed coverage (its filter stopped matching any
        # test) must NOT be tolerated - it means the entry is stale/misconfigured
        # and lost its coverage. It fails the run instead of being counted as an
        # expected failure.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _no_coverage_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("exercised no failing test", out)
        self.assertNotIn("failed as expected", out)

    def test_quarantined_pass_is_stale_and_nonzero(self) -> None:
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _pass_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-STALE]", out)
        self.assertIn("PASSED", out)

    def test_quarantined_clean_summary_with_nonzero_exit_fails(self) -> None:
        # Round-5: a CLEAN all-pass summary that then exits nonzero (a teardown /
        # harness crash after all tests passed) must NOT be tolerated as a crash.
        # The summary is authoritative: the tracked failure is gone (stale) AND
        # there is a new teardown crash - both reasons to fail the run.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _pass_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-STALE]", out)
        self.assertIn("passed all tests", out)
        self.assertNotIn("crashed as expected", out)
        self.assertNotIn("failed as expected", out)

    def test_quarantined_harness_error_is_nonzero(self) -> None:
        # exit 0 with no doctest summary must NOT be read as an expected failure.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _no_summary_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("no doctest summary", out)

    def test_empty_manifest_is_a_noop_for_lanes(self) -> None:
        # With an empty manifest a passing lane behaves exactly as today: rc 0,
        # no quarantine output at all.
        with _manifest([]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _pass_output()))
        self.assertEqual(rc, 0, out)
        self.assertNotIn("[QUARANTINE", out)
        self.assertIn("quarantined_failing=0", out)


class QuarantineClassificationTests(unittest.TestCase):
    def test_classify_clean_pass(self) -> None:
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(True, _pass_output()),
            "clean_pass",
        )

    def test_classify_clean_pass_even_with_nonzero_exit(self) -> None:
        # Summary-first: an all-pass summary is clean_pass regardless of exit code.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(False, _pass_output()),
            "clean_pass",
        )

    def test_classify_expected_fail_on_crash(self) -> None:
        # Nonzero exit with no summary is a genuine crash-failure.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(False, _no_summary_output()),
            "expected_fail",
        )

    def test_classify_expected_fail_on_failing_summary(self) -> None:
        # Nonzero exit with a failing summary is a genuine failure signal.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(False, _fail_output()),
            "expected_fail",
        )

    def test_classify_coverage_lost_on_exit0_zero_coverage(self) -> None:
        # Exit 0 with a summary but zero executed coverage -> coverage_lost.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(True, _no_coverage_output()),
            "coverage_lost",
        )

    def test_classify_harness_error_on_clean_exit_no_summary(self) -> None:
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(True, _no_summary_output()),
            "harness_error",
        )


class DoctestCaseParsingTests(unittest.TestCase):
    def test_parses_single_failing_case(self) -> None:
        self.assertEqual(
            harness._parse_failing_doctest_cases(_fail_output()),
            [FAILING_CASE],
        )

    def test_parses_two_failing_cases_in_order(self) -> None:
        other = "[GaussianSplatting][Animation] second failing case"
        self.assertEqual(
            harness._parse_failing_doctest_cases(_multi_fail_output([FAILING_CASE, other])),
            [FAILING_CASE, other],
        )

    def test_ignores_warning_and_message_only_cases(self) -> None:
        # A case that only emits WARNING/MESSAGE (no ERROR) is not a failure.
        output = (
            "===============================================================================\n"
            "test.h(10):\n"
            "TEST CASE:  [GaussianSplatting][Animation] warns but passes\n"
            "\n"
            "test.h(12): WARNING: CHECK( x ) is NOT correct!\n"
            "test.h(13): MESSAGE: some note\n"
            "\n"
            "[doctest] test cases: 2 | 2 passed | 0 failed\n"
            "[doctest] assertions: 5 | 5 passed | 0 failed\n"
        )
        self.assertEqual(harness._parse_failing_doctest_cases(output), [])

    def test_no_case_header_yields_no_names(self) -> None:
        self.assertEqual(
            harness._parse_failing_doctest_cases(_failing_summary_no_case_output()), []
        )

    def test_bdd_scenario_failure_is_named_not_inherited(self) -> None:
        # REGRESSION: a BDD SCENARIO prints its name without the "TEST CASE:  "
        # prefix, so before the separator-anchored parse the scenario's ERROR
        # line inherited the PRECEDING case's name. In a quarantined lane that
        # made a brand-new scenario regression look like the approved failure.
        output = (
            _case_failure_block(FAILING_CASE)
            + _scenario_failure_block("a brand new unrelated regression")
            + "[doctest] test cases: 4 | 2 passed | 2 failed\n"
            "[doctest] assertions: 10 | 8 passed | 2 failed\n"
            "[doctest] Status: FAILURE!\n"
        )
        self.assertEqual(
            harness._parse_failing_doctest_cases(output),
            [FAILING_CASE, "  Scenario: a brand new unrelated regression"],
        )

    def test_scenario_only_failure_is_named(self) -> None:
        output = (
            _scenario_failure_block("standalone scenario")
            + "[doctest] test cases: 2 | 1 passed | 1 failed\n"
            "[doctest] Status: FAILURE!\n"
        )
        self.assertEqual(
            harness._parse_failing_doctest_cases(output),
            ["  Scenario: standalone scenario"],
        )

    def test_separator_clears_attribution_for_unnamed_failures(self) -> None:
        # A failure block whose name line never arrives must not be attributed
        # to the previous case; it stays unparseable so callers fail closed.
        output = (
            _case_failure_block(FAILING_CASE)
            + "===============================================================================\n"
            "modules/gaussian_splatting/tests/test_animation.h(99): ERROR: unnamed failure\n"
            "[doctest] Status: FAILURE!\n"
        )
        self.assertEqual(harness._parse_failing_doctest_cases(output), [FAILING_CASE])


class DoctestCaseMatchingTests(unittest.TestCase):
    def test_substring_glob_matches(self) -> None:
        self.assertTrue(harness._test_case_matches("*plays a clip*", FAILING_CASE))

    def test_tag_prefixed_glob_matches_literally(self) -> None:
        # '[' ']' are literal (not fnmatch char classes).
        self.assertTrue(
            harness._test_case_matches("[GaussianSplatting][Animation]*", FAILING_CASE)
        )

    def test_non_matching_pattern_does_not_match(self) -> None:
        self.assertFalse(harness._test_case_matches("*unrelated*", FAILING_CASE))

    def test_question_mark_wildcard(self) -> None:
        self.assertTrue(harness._test_case_matches("*play? a clip*", FAILING_CASE))


class ManifestUnreadable(RuntimeError):
    """The committed manifest is missing, unreadable, or not the expected shape."""


def load_committed_manifest() -> dict:
    """Read the COMMITTED manifest, failing closed on anything unexpected.

    A missing or unparseable manifest is NOT a clean manifest: every
    `unlaned_tests` declaration the lane-coverage guard relies on vanishes with
    it, and the ratchet below would have nothing to compare. So this raises
    instead of returning a permissive default.
    """
    if not COMMITTED_MANIFEST_PATH.is_file():
        raise ManifestUnreadable(
            f"committed quarantine manifest is missing ({COMMITTED_MANIFEST_PATH}). "
            "Deleting it does not make the manifest clean - it deletes the ratchet."
        )
    try:
        raw = COMMITTED_MANIFEST_PATH.read_text(encoding="utf-8")
    except OSError as exc:
        raise ManifestUnreadable(
            f"committed quarantine manifest is unreadable ({COMMITTED_MANIFEST_PATH}): {exc}"
        ) from exc
    try:
        data = json.loads(raw)
    except ValueError as exc:
        raise ManifestUnreadable(
            f"committed quarantine manifest is not valid JSON ({COMMITTED_MANIFEST_PATH}): {exc}"
        ) from exc
    if not isinstance(data, dict):
        raise ManifestUnreadable("committed quarantine manifest root must be a JSON object.")
    for key in ("entries", "unlaned_tests"):
        if not isinstance(data.get(key), list):
            raise ManifestUnreadable(
                f"committed quarantine manifest '{key}' must be a list "
                f"(got {type(data.get(key)).__name__})."
            )
    return data


def array_fingerprint(items: list) -> str:
    """Hash a WHOLE manifest array: every field of every element, in order.

    Both arrays get exactly this treatment. Round 2 (Codex on #821) found that
    `unlaned_tests` had been given a NARROWER rule than its sibling `entries`,
    with no stated reason: the hash covered only (test_case, count), so a
    declaration's `reason`, `owner`, `risk` and `expires_utc` - and an
    `issue_url` swapped between two already-allowlisted issues - could all be
    rewritten with the fingerprint unchanged and every test still green. That
    defeats the "any edit at all is visible" guarantee exactly where it matters,
    waiver OWNERSHIP and TRACKING, which is the failure mode this branch exists
    to close: 9 of 10 declarations had been quietly orphaned onto CLOSED issues.
    A hash over a hand-listed subset of fields is the same class of defect as an
    invariant guarded by a hand-written list.

    Two properties follow from hashing the array as committed, and both are
    deliberate:
      * TOTALITY - a field added by a future schema change is hashed the day it
        appears, because nothing here enumerates field names.
      * ORDER - check_test_lane_coverage.py attributes stranded cases
        FIRST-MATCH-WINS, so declaration order is semantic (#664): a reorder can
        silently re-attribute cases between declarations. Order is therefore part
        of the pinned content, not cosmetic.
    """
    payload = json.dumps(items, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def entries_fingerprint(entries: list) -> str:
    """Hash over the WHOLE entries array, so any field edit is visible."""
    return array_fingerprint(entries)


def entry_keys(entries: list) -> list:
    """Identity of each entry: (lane, test_case), sorted."""
    return sorted(
        (str(entry.get("lane", "")), str(entry.get("test_case", "")))
        for entry in entries
        if isinstance(entry, dict)
    )


def unlaned_pairs(declarations: list) -> list:
    """Sorted (test_case, count) pairs; a non-int count sorts as -1 and fails."""
    pairs = []
    for declaration in declarations:
        if not isinstance(declaration, dict):
            continue
        count = declaration.get("count")
        pairs.append(
            (
                str(declaration.get("test_case", "")),
                count if isinstance(count, int) and not isinstance(count, bool) else -1,
            )
        )
    return sorted(pairs)


def unlaned_fingerprint(declarations: list) -> str:
    """Hash over the WHOLE declaration objects - see array_fingerprint.

    Takes the declarations themselves, NOT the (test_case, count) projection.
    `unlaned_pairs` still exists, but only for the SIZE and SET-INCLUSION rules,
    which answer a different question ("did the declared set grow?") than the
    fingerprint ("did anything at all change?").
    """
    return array_fingerprint(declarations)


def _perturb(value: object) -> object:
    """Return a value that is JSON-different from `value`, whatever its type.

    Type-driven, so the totality test needs no per-field knowledge.
    """
    if isinstance(value, bool):
        return not value
    if isinstance(value, int):
        return value + 1
    if isinstance(value, float):
        return value + 1.0
    if isinstance(value, str):
        return value + " (perturbed)"
    if isinstance(value, list):
        return list(value) + ["perturbed"]
    if isinstance(value, dict):
        return {**value, "perturbed": True}
    return "perturbed"


def _parse_utc(value: object):
    if not isinstance(value, str) or not value.strip():
        return None
    try:
        parsed = datetime.fromisoformat(value.strip().replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def declaration_problems(kind: str, index: int, declaration: object, now: datetime) -> list:
    """Content rules for one declaration in either array. ASCII messages only.

    The existing schema guard checks field PRESENCE. Presence is not hygiene: a
    present 'reason' can be "TODO", a present 'issue_url' can point at a closed
    issue, and a present 'expires_utc' can be a decade out. Each rule below names
    the constant it enforces so the failure says what to do, not just what broke.
    """
    label = f"{kind}[{index}]"
    if not isinstance(declaration, dict):
        return [f"{label} must be a JSON object."]
    label = f"{kind}[{index}] ({str(declaration.get('test_case', '?'))!r})"
    problems: list = []

    reason = declaration.get("reason")
    if not isinstance(reason, str) or not reason.strip():
        problems.append(f"{label} has no 'reason'.")
    else:
        text = reason.strip()
        token = text.strip(".!:;-? ").lower()
        # An all-punctuation reason ("-", "???") strips to nothing; it is a
        # placeholder too, not merely a short reason.
        if not token or token in PLACEHOLDER_REASON_TOKENS:
            problems.append(
                f"{label} 'reason' is the placeholder {text!r} (PLACEHOLDER_REASON_TOKENS). "
                f"A placeholder reason is an untracked skip wearing a manifest entry's clothes: "
                f"say what is broken and what it would take to give the case a lane."
            )
        elif len(text) < MIN_REASON_CHARS:
            problems.append(
                f"{label} 'reason' is {len(text)} characters, under MIN_REASON_CHARS="
                f"{MIN_REASON_CHARS}. A reason too short to state the defect and the exit "
                f"condition cannot be reviewed."
            )

    issue_url = declaration.get("issue_url")
    match = ISSUE_URL_RE.match(issue_url.strip()) if isinstance(issue_url, str) else None
    if match is None:
        problems.append(
            f"{label} 'issue_url' {issue_url!r} does not match ISSUE_URL_RE "
            f"(https://github.com/klausi3D/godotGS/issues/<number>). A declaration must point at "
            f"a tracking issue in THIS repo, not at prose or another project."
        )
    else:
        number = int(match.group(1))
        if number not in ISSUES_VERIFIED_OPEN:
            problems.append(
                f"{label} 'issue_url' points at issue #{number}, which is not in the guard's "
                f"ISSUES_VERIFIED_OPEN allowlist. A declaration whose tracking issue is CLOSED is "
                f"a SILENT EXPIRY: the work stops being tracked while the declaration still looks "
                f"blessed (#520 and #329 were both closed under live declarations). Check with "
                f"'gh issue view {number} --repo klausi3D/godotGS --json number,state'. If OPEN, "
                f"add it to ISSUES_VERIFIED_OPEN and refresh ISSUES_VERIFIED_OPEN_UTC in the same "
                f"PR. If CLOSED, re-point the declaration at live work - do not widen the "
                f"allowlist to cover a closed issue."
            )

    expires_raw = declaration.get("expires_utc")
    expires = _parse_utc(expires_raw)
    if expires is None:
        problems.append(
            f"{label} has a missing or unparseable 'expires_utc' {expires_raw!r} "
            f"(want ISO-8601 UTC, e.g. 2026-10-15T00:00:00Z)."
        )
    else:
        if expires <= now:
            problems.append(
                f"{label} EXPIRED on {expires_raw}. Give the case(s) a lane, or renew the "
                f"declaration with fresh justification."
            )
        horizon = now + timedelta(days=EXPIRY_HORIZON_DAYS)
        if expires > horizon:
            problems.append(
                f"{label} 'expires_utc' {expires_raw} is more than EXPIRY_HORIZON_DAYS="
                f"{EXPIRY_HORIZON_DAYS} days beyond this run (past {horizon.date().isoformat()}). "
                f"A quarantine is bounded by definition; an expiry that far out is a permanent "
                f"silencer, not a quarantine."
            )
        ceiling = _parse_utc(MAX_EXPIRY_UTC)
        if ceiling is not None and expires > ceiling:
            problems.append(
                f"{label} 'expires_utc' {expires_raw} is beyond the pinned MAX_EXPIRY_UTC="
                f"{MAX_EXPIRY_UTC}. The relative horizon alone never stops SERIAL renewal, so the "
                f"ceiling is pinned in the guard and renewing is a deliberate two-file diff. Do "
                f"NOT raise MAX_EXPIRY_UTC to make this pass."
            )

    risk = declaration.get("risk")
    if risk is not None and (
        not isinstance(risk, str) or risk.strip() not in VALID_RISK_CLASSES
    ):
        problems.append(
            f"{label} 'risk' {risk!r} is not one of "
            f"{sorted(VALID_RISK_CLASSES)} (VALID_RISK_CLASSES)."
        )

    owner = declaration.get("owner")
    if not isinstance(owner, str) or not owner.strip():
        problems.append(
            f"{label} has no 'owner'. A declaration without an owner is nobody's job."
        )

    if kind == "entries":
        base_sha = declaration.get("base_sha_proven_failing")
        if not isinstance(base_sha, str) or not BASE_SHA_RE.match(base_sha.strip()):
            problems.append(
                f"{label} 'base_sha_proven_failing' {base_sha!r} is not 40 lowercase hex "
                f"characters (BASE_SHA_RE). An entry must name the exact commit the failure was "
                f"reproduced on; an abbreviated, uppercase or absent SHA cannot be re-verified."
            )
        for field in ("lane", "test_case"):
            value = declaration.get(field)
            if not isinstance(value, str) or not value.strip():
                problems.append(f"{label} has no '{field}'.")
    else:
        count = declaration.get("count")
        if not isinstance(count, int) or isinstance(count, bool) or count < 1:
            problems.append(
                f"{label} 'count' must be a positive integer (the number of stranded cases this "
                f"declaration covers), got {count!r}."
            )

    return problems


class QuarantineManifestRatchetTests(unittest.TestCase):
    """The shrink-only ratchet over the COMMITTED manifest (#650).

    Every test here reads tests/ci/quarantine_manifest.json directly rather than
    through the harness global, which other tests in this file patch to point at
    temporary fixtures.
    """

    def setUp(self) -> None:
        try:
            self.data = load_committed_manifest()
        except ManifestUnreadable as exc:
            self.fail(str(exc))
        self.entries = self.data["entries"]
        self.declarations = self.data["unlaned_tests"]

    # -- structure ----------------------------------------------------------
    def test_manifest_has_no_unratcheted_third_home(self) -> None:
        keys = set(self.data)
        self.assertEqual(
            keys,
            set(MANIFEST_TOP_LEVEL_KEYS),
            "the manifest's top-level keys changed. Both 'entries' and 'unlaned_tests' are "
            "ratcheted here; a NEW top-level array would be an unratcheted third place to park "
            "declarations, which is exactly the bypass this pin exists to stop "
            "(MANIFEST_TOP_LEVEL_KEYS).",
        )

    # -- entries ratchet ----------------------------------------------------
    def test_entries_count_is_within_pinned_max(self) -> None:
        self.assertLessEqual(
            len(self.entries),
            QUARANTINE_ENTRIES_MAX,
            f"'entries' grew to {len(self.entries)}, above the pinned "
            f"QUARANTINE_ENTRIES_MAX={QUARANTINE_ENTRIES_MAX}. Do NOT raise the constant to make "
            f"this pass: an entry tolerates a real failure in a real lane and is human-gated "
            f"(ADR docs/architecture/adr-test-quarantine-manifest.md, charter section 6). Fix the "
            f"test, or - if the failure is genuinely approved - re-pin this guard in the SAME PR "
            f"so the addition is a two-file diff.",
        )

    def test_entries_are_a_subset_of_the_pinned_baseline(self) -> None:
        baseline = set(QUARANTINE_ENTRIES_BASELINE)
        added = [key for key in entry_keys(self.entries) if key not in baseline]
        self.assertEqual(
            added,
            [],
            "these (lane, test_case) entries are NOT in QUARANTINE_ENTRIES_BASELINE. Set "
            "inclusion, not a net count: removing one entry and adding another leaves the count "
            "unchanged and must still fail.\n  " + "\n  ".join(map(repr, added)),
        )

    def test_entries_fingerprint_matches_the_pinned_baseline(self) -> None:
        actual = entries_fingerprint(self.entries)
        self.assertEqual(
            actual,
            QUARANTINE_ENTRIES_FINGERPRINT,
            "'entries' changed without re-pinning this guard.\n"
            f"  pinned: {QUARANTINE_ENTRIES_FINGERPRINT}\n  actual: {actual}\n"
            "The fingerprint covers every field of every entry, so an edit to a reason, an "
            "expiry or an issue link is as visible as an addition. Re-pin with\n"
            "  python tests/ci/test_quarantine_manifest.py --print-fingerprint",
        )

    # -- unlaned_tests ratchet ---------------------------------------------
    def test_unlaned_declaration_count_is_within_pinned_max(self) -> None:
        self.assertLessEqual(
            len(self.declarations),
            UNLANED_MAX_DECLARATIONS,
            f"'unlaned_tests' grew to {len(self.declarations)} declarations, above the pinned "
            f"UNLANED_MAX_DECLARATIONS={UNLANED_MAX_DECLARATIONS}. The path of least resistance "
            f"when check_test_lane_coverage.py fails on a newly stranded case is to add one more "
            f"declaration; that is the growth this constant refuses. Give the case a lane in "
            f"tests/ci/run_module_tests.py (or a batch in run_gpu_harness.py) instead. Do NOT "
            f"raise the constant.",
        )

    def test_unlaned_total_count_is_within_pinned_max(self) -> None:
        total = sum(count for _, count in unlaned_pairs(self.declarations))
        self.assertLessEqual(
            total,
            UNLANED_MAX_TOTAL_COUNT,
            f"'unlaned_tests' now declares {total} stranded cases, above the pinned "
            f"UNLANED_MAX_TOTAL_COUNT={UNLANED_MAX_TOTAL_COUNT}. Raising an existing 'count' by 1 "
            f"is the same amnesty as adding a declaration, so it is capped the same way.",
        )

    def test_unlaned_declarations_are_a_subset_of_the_pinned_baseline(self) -> None:
        baseline = dict(UNLANED_BASELINE)
        problems = []
        for test_case, count in unlaned_pairs(self.declarations):
            if test_case not in baseline:
                problems.append(
                    f"{test_case!r} is a NEW declaration (count {count}); it is not in "
                    f"UNLANED_BASELINE."
                )
            elif count > baseline[test_case]:
                problems.append(
                    f"{test_case!r} count rose {baseline[test_case]} -> {count}."
                )
        self.assertEqual(
            problems,
            [],
            "unlaned_tests grew by set inclusion even if the totals did not. A same-size swap "
            "(drop one family, add another) and a fix-one/add-one trade both net zero and must "
            "still fail here.\n  " + "\n  ".join(problems),
        )

    def test_unlaned_fingerprint_matches_the_pinned_baseline(self) -> None:
        actual = unlaned_fingerprint(self.declarations)
        self.assertEqual(
            actual,
            UNLANED_FINGERPRINT,
            "'unlaned_tests' changed without re-pinning this guard.\n"
            f"  pinned: {UNLANED_FINGERPRINT}\n  actual: {actual}\n"
            "The fingerprint covers every field of every declaration, in order - a rewritten "
            "owner, reason, risk or expiry, and an issue_url swapped between two allowlisted "
            "issues, are all as visible as an addition. A count-only ratchet would pass a "
            "same-size swap; this will not. If the change is a legitimate SHRINK, re-pin with\n"
            "  python tests/ci/test_quarantine_manifest.py --print-fingerprint",
        )

    def test_fingerprint_covers_every_field_of_every_declaration(self) -> None:
        """TOTALITY: no field is unhashed, including fields not invented yet.

        Written as the property, not against today's field list. Round 2 (#821):
        the previous fingerprint hashed 2 of the 7 fields, so `owner`,
        `reason`, `risk`, `expires_utc` and an `issue_url` swapped between two
        allowlisted issues were all invisible. A guard whose coverage is a
        hand-written list of field names is the defect this repo keeps paying
        for; this test derives the list from the data instead.
        """
        baseline = unlaned_fingerprint(self.declarations)
        checked = 0
        for index, declaration in enumerate(self.declarations):
            self.assertIsInstance(declaration, dict)
            for key, value in declaration.items():
                checked += 1
                edited = copy.deepcopy(self.declarations)
                edited[index][key] = _perturb(value)
                self.assertNotEqual(
                    unlaned_fingerprint(edited),
                    baseline,
                    f"rewriting unlaned_tests[{index}][{key!r}] does not change the "
                    f"fingerprint - that field is UNHASHED and can be edited silently.",
                )
                dropped = copy.deepcopy(self.declarations)
                del dropped[index][key]
                self.assertNotEqual(
                    unlaned_fingerprint(dropped),
                    baseline,
                    f"DELETING unlaned_tests[{index}][{key!r}] does not change the fingerprint.",
                )
        self.assertGreater(checked, 0, "no declaration fields were checked - vacuous test")
        # A field added by a future schema change must be hashed the day it
        # appears, with no edit to this guard.
        extended = copy.deepcopy(self.declarations)
        extended[0]["a_field_invented_after_this_guard_was_written"] = "value"
        self.assertNotEqual(
            unlaned_fingerprint(extended),
            baseline,
            "a NEW field added to a declaration is not hashed; the fingerprint is enumerating "
            "field names somewhere instead of hashing the whole object.",
        )

    def test_fingerprint_pins_declaration_order(self) -> None:
        """Order is semantic, so it is pinned.

        check_test_lane_coverage.py attributes stranded cases FIRST-MATCH-WINS
        (#664), so moving a catch-all above a narrow family silently
        re-attributes cases between declarations while every count stays put.
        """
        self.assertGreaterEqual(len(self.declarations), 2)
        reordered = list(self.declarations)
        reordered[0], reordered[1] = reordered[1], reordered[0]
        self.assertNotEqual(
            unlaned_fingerprint(reordered),
            unlaned_fingerprint(self.declarations),
            "reordering two declarations leaves the fingerprint unchanged, so a first-match-wins "
            "re-attribution (#664) would be invisible.",
        )

    def test_fingerprint_is_not_the_old_narrow_projection(self) -> None:
        """Round-2 regression pin (#821): re-narrowing the hash must fail here.

        If a future change reverts the fingerprint to the (test_case, count)
        projection, every other test in this file still passes - the pinned
        constant would simply be re-derived from the narrower input. This is the
        test that notices.
        """
        narrow_payload = "\n".join(
            f"{test_case}\t{count}" for test_case, count in unlaned_pairs(self.declarations)
        )
        narrow = hashlib.sha256(narrow_payload.encode("utf-8")).hexdigest()
        self.assertNotEqual(
            UNLANED_FINGERPRINT,
            narrow,
            "UNLANED_FINGERPRINT equals the hash of the (test_case, count) projection. The "
            "fingerprint must cover the WHOLE declaration objects; the narrow form cannot see a "
            "rewritten owner, reason or issue_url.",
        )
        self.assertEqual(
            unlaned_fingerprint(self.declarations),
            array_fingerprint(self.declarations),
            "unlaned_tests must get the SAME treatment as entries, not a narrower one.",
        )

    # -- the pins must agree with each other -------------------------------
    def test_pinned_constants_are_internally_consistent(self) -> None:
        """A half-finished re-pin is caught here, not six months later.

        Someone editing UNLANED_BASELINE but forgetting the MAX constants (or
        the reverse) would otherwise leave one of the pins silently describing a
        set nobody committed.

        UNLANED_FINGERPRINT is deliberately NOT cross-checked against
        UNLANED_BASELINE: since round 2 the fingerprint hashes the complete
        declaration objects, which the (test_case, count) baseline does not
        carry, so it cannot be derived from it. That is the point - the
        fingerprint sees strictly more than the baseline does. It is instead
        checked against the committed manifest, and against re-narrowing, by
        test_unlaned_fingerprint_matches_the_pinned_baseline and
        test_fingerprint_is_not_the_old_narrow_projection.
        """
        self.assertEqual(
            len(UNLANED_BASELINE),
            UNLANED_MAX_DECLARATIONS,
            "UNLANED_MAX_DECLARATIONS must equal len(UNLANED_BASELINE).",
        )
        self.assertEqual(
            sum(count for _, count in UNLANED_BASELINE),
            UNLANED_MAX_TOTAL_COUNT,
            "UNLANED_MAX_TOTAL_COUNT must equal the sum of UNLANED_BASELINE counts.",
        )
        self.assertEqual(
            len(QUARANTINE_ENTRIES_BASELINE),
            QUARANTINE_ENTRIES_MAX,
            "QUARANTINE_ENTRIES_MAX must equal len(QUARANTINE_ENTRIES_BASELINE).",
        )
        if not QUARANTINE_ENTRIES_BASELINE:
            # While the baseline is empty the fingerprint is fully determined, so
            # a stale hash left behind by a partial re-pin is caught here.
            self.assertEqual(
                QUARANTINE_ENTRIES_FINGERPRINT,
                entries_fingerprint([]),
                "QUARANTINE_ENTRIES_BASELINE is empty, so QUARANTINE_ENTRIES_FINGERPRINT must "
                "hash the empty entries array - the re-pin is half done.",
            )

    # -- per-declaration content -------------------------------------------
    def test_committed_declarations_pass_the_content_rules(self) -> None:
        now = datetime.now(timezone.utc)
        problems: list = []
        for index, entry in enumerate(self.entries):
            problems.extend(declaration_problems("entries", index, entry, now))
        for index, declaration in enumerate(self.declarations):
            problems.extend(
                declaration_problems("unlaned_tests", index, declaration, now)
            )
        self.assertEqual(problems, [], "\n  " + "\n  ".join(problems))

    def test_issue_open_verification_is_not_stale(self) -> None:
        verified = _parse_utc(ISSUES_VERIFIED_OPEN_UTC)
        self.assertIsNotNone(
            verified, "ISSUES_VERIFIED_OPEN_UTC must be an ISO-8601 UTC timestamp."
        )
        age = datetime.now(timezone.utc) - verified
        self.assertLessEqual(
            age.days,
            ISSUE_VERIFICATION_MAX_AGE_DAYS,
            f"ISSUES_VERIFIED_OPEN was last checked against GitHub on {ISSUES_VERIFIED_OPEN_UTC} "
            f"({age.days} days ago), over ISSUE_VERIFICATION_MAX_AGE_DAYS="
            f"{ISSUE_VERIFICATION_MAX_AGE_DAYS}. An offline allowlist can only say 'these were "
            f"open when a human last looked'; re-check every referenced issue with "
            f"'gh issue view <n> --repo klausi3D/godotGS --json number,state', drop any that "
            f"closed, and refresh the date. Every declaration's own expires_utc falls due before "
            f"this, so this is a backstop, not the primary cadence.",
        )

    def test_ratchet_messages_are_ascii(self) -> None:
        """CI's cp1252 stdout has been broken by a non-ASCII byte before."""
        now = datetime.now(timezone.utc)
        broken = {
            "reason": "TODO",
            "issue_url": "https://example.com/issues/1",
            "expires_utc": "2099-01-01T00:00:00Z",
            "risk": "R9",
            "owner": "",
            "count": 0,
            "test_case": "*x*",
        }
        for kind in ("entries", "unlaned_tests"):
            for message in declaration_problems(kind, 0, broken, now):
                message.encode("ascii")


class QuarantineDeclarationContentRuleTests(unittest.TestCase):
    """Each content rule is proven to DISCRIMINATE, against synthetic input.

    The committed manifest satisfies every rule, so if these rules were only
    exercised against it they would be indistinguishable from rules that never
    fire. Each case below mutates exactly one field of an otherwise-valid
    declaration and asserts the specific rule reports it.
    """

    def setUp(self) -> None:
        self.now = datetime.now(timezone.utc)
        self.open_issue = sorted(ISSUES_VERIFIED_OPEN)[0]

    def _unlaned(self, **overrides) -> dict:
        declaration = {
            "test_case": "[GaussianSplatting][Example]*",
            "count": 1,
            "reason": (
                "Placeholder-free reason long enough to describe the defect, why no lane "
                "selects the case, and what would retire the declaration."
            ),
            "issue_url": f"https://github.com/klausi3D/godotGS/issues/{self.open_issue}",
            "owner": "klausi3D",
            "risk": "R1",
            "expires_utc": MAX_EXPIRY_UTC,
        }
        declaration.update(overrides)
        return declaration

    def _entry(self, **overrides) -> dict:
        entry = self._unlaned()
        entry.pop("count")
        entry["lane"] = VALID_LANE
        entry["base_sha_proven_failing"] = "8fae40f00de2e4efe07017b7660fb4d90043fd45"
        entry.update(overrides)
        return entry

    def _problems(self, kind: str, declaration: dict) -> str:
        return "\n".join(declaration_problems(kind, 0, declaration, self.now))

    def test_valid_declarations_report_nothing(self) -> None:
        self.assertEqual(declaration_problems("unlaned_tests", 0, self._unlaned(), self.now), [])
        self.assertEqual(declaration_problems("entries", 0, self._entry(), self.now), [])

    def test_placeholder_reason_is_rejected(self) -> None:
        for token in sorted(PLACEHOLDER_REASON_TOKENS):
            with self.subTest(token=token):
                out = self._problems("unlaned_tests", self._unlaned(reason=token.upper()))
                self.assertIn("PLACEHOLDER_REASON_TOKENS", out)

    def test_short_reason_is_rejected(self) -> None:
        out = self._problems("unlaned_tests", self._unlaned(reason="no lane yet, sorry"))
        self.assertIn("MIN_REASON_CHARS", out)

    def test_reason_at_the_boundary_is_accepted(self) -> None:
        # The rule must discriminate at its boundary, not merely somewhere.
        text = "x" * MIN_REASON_CHARS
        self.assertNotIn("MIN_REASON_CHARS", self._problems("unlaned_tests", self._unlaned(reason=text)))
        self.assertIn(
            "MIN_REASON_CHARS",
            self._problems("unlaned_tests", self._unlaned(reason="x" * (MIN_REASON_CHARS - 1))),
        )

    def test_foreign_or_malformed_issue_url_is_rejected(self) -> None:
        for url in (
            "https://github.com/example/repo/issues/999",
            "https://github.com/klausi3D/godotGS/pull/650",
            "see the tracking issue",
            "https://github.com/klausi3D/godotGS/issues/",
        ):
            with self.subTest(url=url):
                self.assertIn("ISSUE_URL_RE", self._problems("unlaned_tests", self._unlaned(issue_url=url)))

    def test_closed_issue_is_rejected(self) -> None:
        # #520 and #329 are the real closures that motivated the rule.
        for number in (520, 329):
            with self.subTest(issue=number):
                out = self._problems(
                    "unlaned_tests",
                    self._unlaned(
                        issue_url=f"https://github.com/klausi3D/godotGS/issues/{number}"
                    ),
                )
                self.assertIn("ISSUES_VERIFIED_OPEN", out)
                self.assertIn("SILENT EXPIRY", out)

    def test_past_expiry_is_rejected(self) -> None:
        out = self._problems("unlaned_tests", self._unlaned(expires_utc=_past_iso()))
        self.assertIn("EXPIRED", out)

    def test_far_future_expiry_is_rejected_by_both_the_horizon_and_the_ceiling(self) -> None:
        out = self._problems("unlaned_tests", self._unlaned(expires_utc="2099-01-01T00:00:00Z"))
        self.assertIn("EXPIRY_HORIZON_DAYS", out)
        self.assertIn("MAX_EXPIRY_UTC", out)

    def test_serial_renewal_inside_the_horizon_still_trips_the_ceiling(self) -> None:
        # This is the case a relative horizon alone cannot catch: a renewal that
        # is comfortably within EXPIRY_HORIZON_DAYS but past the pinned ceiling.
        renewed = (_parse_utc(MAX_EXPIRY_UTC) + timedelta(days=1)).isoformat()
        out = self._problems("unlaned_tests", self._unlaned(expires_utc=renewed))
        self.assertIn("MAX_EXPIRY_UTC", out)
        self.assertNotIn("EXPIRY_HORIZON_DAYS", out)

    def test_unparseable_expiry_is_rejected(self) -> None:
        out = self._problems("unlaned_tests", self._unlaned(expires_utc="soon"))
        self.assertIn("unparseable", out)

    def test_unknown_risk_class_is_rejected(self) -> None:
        out = self._problems("unlaned_tests", self._unlaned(risk="R9"))
        self.assertIn("VALID_RISK_CLASSES", out)

    def test_absent_risk_is_allowed_on_unlaned_declarations(self) -> None:
        declaration = self._unlaned()
        declaration.pop("risk")
        self.assertEqual(declaration_problems("unlaned_tests", 0, declaration, self.now), [])

    def test_missing_owner_is_rejected(self) -> None:
        self.assertIn("owner", self._problems("unlaned_tests", self._unlaned(owner="  ")))

    def test_entry_requires_a_full_lowercase_hex_base_sha(self) -> None:
        for value in ("8fae40f", "8FAE40F00DE2E4EFE07017B7660FB4D90043FD45", "", "not a sha"):
            with self.subTest(value=value):
                self.assertIn(
                    "BASE_SHA_RE",
                    self._problems("entries", self._entry(base_sha_proven_failing=value)),
                )

    def test_unlaned_count_must_be_a_positive_integer(self) -> None:
        for value in (0, -1, "2", 1.5, True, None):
            with self.subTest(value=value):
                self.assertIn("'count'", self._problems("unlaned_tests", self._unlaned(count=value)))

    def test_non_object_declaration_is_rejected(self) -> None:
        self.assertEqual(
            declaration_problems("unlaned_tests", 0, "a string", self.now),
            ["unlaned_tests[0] must be a JSON object."],
        )


class QuarantineRepinToolTests(unittest.TestCase):
    """--print-fingerprint is a re-pin AID, and must never launder growth.

    Any regenerate-style helper is a laundering primitive unless it refuses
    additions by set inclusion: a fix-one/add-one trade nets zero, so a net-count
    check would wave it through. There is deliberately no writer at all - the
    tool prints, a human pastes, and no path in this repo can synthesise the
    pinned block from the current tree.
    """

    def test_tool_refuses_a_new_declaration(self) -> None:
        pairs = sorted(list(UNLANED_BASELINE) + [("[Brand New Family]*", 1)])
        refusals = repin_refusals([], pairs)
        self.assertTrue(any("NEW" in r for r in refusals), refusals)

    def test_tool_refuses_a_same_size_swap(self) -> None:
        pairs = sorted(list(UNLANED_BASELINE)[1:] + [("[Brand New Family]*", UNLANED_BASELINE[0][1])])
        refusals = repin_refusals([], pairs)
        self.assertTrue(any("NEW" in r for r in refusals), refusals)

    def test_tool_refuses_a_raised_count(self) -> None:
        pairs = sorted(
            [(UNLANED_BASELINE[0][0], UNLANED_BASELINE[0][1] + 1)] + list(UNLANED_BASELINE)[1:]
        )
        refusals = repin_refusals([], pairs)
        self.assertTrue(any("count rose" in r for r in refusals), refusals)

    def test_tool_refuses_a_new_entry(self) -> None:
        refusals = repin_refusals(
            [{"lane": VALID_LANE, "test_case": "*anything*"}], sorted(UNLANED_BASELINE)
        )
        self.assertTrue(any("entries" in r for r in refusals), refusals)

    def test_tool_allows_a_genuine_shrink(self) -> None:
        self.assertEqual(repin_refusals([], sorted(UNLANED_BASELINE)[1:]), [])
        lowered = sorted(
            [(UNLANED_BASELINE[0][0], UNLANED_BASELINE[0][1] - 1)] + list(UNLANED_BASELINE)[1:]
        )
        self.assertEqual(repin_refusals([], lowered), [])


def repin_refusals(entries: list, pairs: list) -> list:
    """Reasons --print-fingerprint must refuse to emit new constants."""
    baseline = dict(UNLANED_BASELINE)
    entry_baseline = set(QUARANTINE_ENTRIES_BASELINE)
    refusals: list = []
    for test_case, count in pairs:
        if test_case not in baseline:
            refusals.append(
                f"unlaned_tests declaration {test_case!r} is NEW (count {count}). A newly "
                f"stranded case belongs in a lane, not in the manifest."
            )
        elif count > baseline[test_case]:
            refusals.append(
                f"unlaned_tests {test_case!r} count rose {baseline[test_case]} -> {count}."
            )
    for key in entry_keys(entries):
        if key not in entry_baseline:
            refusals.append(f"entries {key!r} is NEW; a quarantine entry is human-gated.")
    return refusals


def _print_fingerprint() -> int:
    try:
        data = load_committed_manifest()
    except ManifestUnreadable as exc:
        print(f"REFUSED: {exc}")
        return 1
    entries = data["entries"]
    pairs = unlaned_pairs(data["unlaned_tests"])
    refusals = repin_refusals(entries, pairs)
    if refusals:
        print(
            "REFUSED: --print-fingerprint re-pins a SHRINK only, decided by set inclusion "
            "(a fix-one/add-one trade nets zero and is still growth)."
        )
        for refusal in refusals:
            print(f"  - {refusal}")
        return 1
    print(f"QUARANTINE_ENTRIES_MAX = {len(entries)}")
    print(f'QUARANTINE_ENTRIES_FINGERPRINT = "{entries_fingerprint(entries)}"')
    print("QUARANTINE_ENTRIES_BASELINE = (")
    for lane, test_case in entry_keys(entries):
        print(f"    ({lane!r}, {test_case!r}),")
    print(")")
    print(f"UNLANED_MAX_DECLARATIONS = {len(pairs)}")
    print(f"UNLANED_MAX_TOTAL_COUNT = {sum(count for _, count in pairs)}")
    print("UNLANED_BASELINE = (")
    for test_case, count in pairs:
        print(f"    ({test_case!r}, {count}),")
    print(")")
    # Hashes the declarations themselves, not the pair projection printed above.
    print(f'UNLANED_FINGERPRINT = "{unlaned_fingerprint(data["unlaned_tests"])}"')
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--print-fingerprint", action="store_true")
    args, rest = parser.parse_known_args()
    if args.print_fingerprint:
        raise SystemExit(_print_fingerprint())
    unittest.main(argv=[sys.argv[0], *rest])
