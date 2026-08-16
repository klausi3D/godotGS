#!/usr/bin/env python3
"""Guard: every [RequiresGPU] test either RUNS in a named GPU batch or is WAIVED.

Issue #329. The defect this closes is not "some tests fail" — it is "28 tests
executed nowhere and nothing noticed for months". The deferral lived only as
prose in a Python comment, so its count silently drifted from 26 to 28.

The contract enforced here:

  1. Every `[RequiresGPU]` test case is EITHER matched by at least one
     `run_gpu_harness.py` BatchSpec filter (and not excluded), OR declared
     deferred in `renderer_release_gate_manifest.json`. A test that is
     neither is an ORPHAN and fails this guard. That is the #329 failure mode
     itself, and it is now impossible to reintroduce silently for any tag family.

  2. Every `BatchSpec.excludes` pattern resolves to EXACTLY ONE real test case
     inside its own batch. A stale exclude (renamed/removed test) fails loudly
     instead of quietly widening the batch; an over-broad exclude that swallows
     several cases fails too.

  3. Excludes and waivers are in bijection. You cannot exclude a case from a
     batch without declaring it, and you cannot leave a waiver behind after the
     case starts running again.

  4. The manifest's `deferred_count` equals the real number of deferred tests.
     This is the specific check that would have caught 26 -> 28.

Matching uses DOCTEST semantics, not fnmatch: doctest treats only `*` and `?`
as special and `[tags]` as LITERAL, whereas fnmatch reads `[...]` as a character
class. Since every name here is bracket-heavy, using fnmatch gives wrong answers
(it is why earlier lane-coverage audits missed a [World] orphan).

Run standalone:
    python tests/ci/test_gpu_harness_deferred_contract.py
    python tests/ci/test_gpu_harness_deferred_contract.py --print-summary
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent.parent
MANIFEST_PATH = ROOT / "docs" / "reference" / "renderer_release_gate_manifest.json"
HARNESS_PATH = ROOT / "tests" / "ci" / "run_gpu_harness.py"
GATE_PATH = ROOT / "tests" / "ci" / "check_renderer_release_gates.py"

# ---------------------------------------------------------------------------
# PINNED BASELINE (Codex review on #329). Do not "update to make CI pass".
#
# The first version of this guard read the allowed backlog out of the manifest
# and compared the manifest against itself. That is not a ratchet: a PR could
# add a brand-new [RequiresGPU] test, append its name to
# unbatched_requires_gpu_backlog.test_names in the same commit, and go green --
# licensing exactly the "executes in no lane" gap the guard exists to prevent.
#
# The baseline therefore lives HERE, in the guard, not in the data the guard
# checks. Any change to the backlog set -- addition, removal, or rename --
# changes BACKLOG_FINGERPRINT and fails CI first. Growth additionally trips
# BACKLOG_MAX_ENTRIES.
#
# Legitimate SHRINK (the only direction allowed):
#   1. move the test into a real batch in run_gpu_harness.py,
#   2. delete its name from the manifest backlog,
#   3. re-pin both constants below (BACKLOG_MAX_ENTRIES must go DOWN),
#      using: python tests/ci/test_gpu_harness_deferred_contract.py --print-fingerprint
#
# Raising BACKLOG_MAX_ENTRIES is a red flag in review: it means a GPU test was
# added that executes nowhere. A newly written [RequiresGPU] test belongs in a
# batch, full stop.
# Re-pinned by #690 (legitimate SHRINK, 70 -> 69): "World-backed RenderSceneInstance
# drives GPU streaming + sorting" was retagged [SceneTree][RequiresGPU] and moved into
# the new RendererSceneTree batch, so it left the backlog by executing rather than by
# being deleted or excused.
# Re-pinned by #622 (legitimate SHRINK, 69 -> 66): the three sort-ORDER oracles in
# test_gpu_sorting.h ("GPU Bitonic Sorting", "Radix sort factory honors 32-bit key
# layout", "Radix sort 8-bit is correct at all workgroup sizes") were retagged with
# [GpuSort] so they are SELECTED by the required GpuSorting batch, leaving the backlog
# by executing rather than by being deleted or excused. ("GPU Sorting Performance",
# a timing benchmark, is intentionally NOT retagged and remains in the backlog.)
# Re-pinned by T4/#908 (legitimate SHRINK, 66 -> 63): the three test_gpu_streaming.h
# cases ("GPU Memory Streaming", "GPU Memory Streaming Performance", "Stage-B instance
# depth culling toggles") were retagged [GaussianSplatting][Streaming][RequiresGPU] so
# the existing (advisory) Streaming batch's `*Streaming*][RequiresGPU]*` filter selects
# them -- a tag-ORDER defect: the filter needs the token BEFORE `][RequiresGPU]`, and
# the old names carried it only in the descriptive tail (or, for Stage-B, not at all).
# They leave the backlog by executing rather than by being deleted or excused, per
# ADR docs/architecture/adr-phase1-guard-hardening.md section 5.2.
BACKLOG_MAX_ENTRIES = 63
BACKLOG_FINGERPRINT = "f4998c2e91696c4a0ae11ab4f444f04d6f905f967d477b6684224fe66c09fae7"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def doctest_match(pattern: str, name: str) -> bool:
    """doctest wildcard semantics: only `*` and `?` are special."""
    regex = "".join(
        ".*" if ch == "*" else ("." if ch == "?" else re.escape(ch)) for ch in pattern
    )
    return re.fullmatch(regex, name, re.DOTALL) is not None


def backlog_fingerprint(names: list[str]) -> str:
    """Order-independent fingerprint of the backlog name set."""
    payload = "\n".join(sorted(names)).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _corpus() -> list[dict[str, Any]]:
    return _load("gs_gate_contract", GATE_PATH)._extract_requires_gpu_tests(ROOT)


def _batches():
    return _load("gs_harness_contract", HARNESS_PATH).BATCHES


def _manifest() -> dict[str, Any]:
    return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def _batch_membership(tests, batches):
    """-> (runs: {name -> [batch]}, excluded: {name -> [(batch, pattern)]})"""
    runs: dict[str, list[str]] = {}
    excluded: dict[str, list[tuple[str, str]]] = {}
    for batch in batches:
        for test in tests:
            name = test["name"]
            if not any(doctest_match(f, name) for f in batch.filters):
                continue
            hit = next((p for p in batch.excludes if doctest_match(p, name)), None)
            if hit is not None:
                excluded.setdefault(name, []).append((batch.name, hit))
            else:
                runs.setdefault(name, []).append(batch.name)
    return runs, excluded


class GpuHarnessDeferredContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tests = _corpus()
        self.batches = _batches()
        self.manifest = _manifest()
        self.runs, self.excluded = _batch_membership(self.tests, self.batches)
        self.waivers = self.manifest.get("deferred_requires_gpu_waivers", [])
        self.waiver_names = {w["test_name"] for w in self.waivers}

    def _orphans(self) -> list[str]:
        return sorted(
            t["name"]
            for t in self.tests
            if t["name"] not in self.runs and t["name"] not in self.waiver_names
        )

    def test_no_undeclared_orphan_requires_gpu_tests(self):
        """Every [RequiresGPU] test runs, is waived, or is in the recorded backlog.

        This IS #329: a test that executes nowhere and is written down nowhere.
        """
        backlog = set(
            self.manifest.get("unbatched_requires_gpu_backlog", {}).get("test_names", [])
        )
        undeclared = [name for name in self._orphans() if name not in backlog]
        self.assertEqual(
            undeclared,
            [],
            "\n[RequiresGPU] tests that execute in NO named GPU batch, carry NO waiver, and are\n"
            "NOT in the recorded backlog. This is the #329 defect reappearing.\n"
            "Fix by EITHER adding them to a BatchSpec in tests/ci/run_gpu_harness.py (preferred —\n"
            "make them run), OR declaring them in renderer_release_gate_manifest.json.\n"
            "Appending to unbatched_requires_gpu_backlog is allowed ONLY for pre-existing cases;\n"
            "a newly written [RequiresGPU] test must land in a batch.\n"
            "Undeclared orphans:\n  " + "\n  ".join(undeclared),
        )

    def test_backlog_cannot_grow_past_pinned_baseline(self):
        """The manifest backlog may not exceed the baseline pinned in THIS file.

        Codex review: without this, a PR that adds a [RequiresGPU] test and
        appends it to the manifest backlog in the same commit goes green,
        because the guard would be treating the edited manifest as its own
        baseline.
        """
        backlog = self.manifest.get("unbatched_requires_gpu_backlog", {}).get("test_names", [])
        self.assertLessEqual(
            len(backlog),
            BACKLOG_MAX_ENTRIES,
            f"unbatched_requires_gpu_backlog grew to {len(backlog)}, above the pinned "
            f"BACKLOG_MAX_ENTRIES={BACKLOG_MAX_ENTRIES}. A newly written [RequiresGPU] test must "
            f"land in a batch in tests/ci/run_gpu_harness.py, not in the backlog. Do NOT raise "
            f"the constant to make this pass.",
        )

    def test_backlog_fingerprint_matches_pinned_baseline(self):
        """Any edit to the backlog set must be a deliberate, visible re-pin."""
        backlog = self.manifest.get("unbatched_requires_gpu_backlog", {}).get("test_names", [])
        actual = backlog_fingerprint(backlog)
        self.assertEqual(
            actual,
            BACKLOG_FINGERPRINT,
            "unbatched_requires_gpu_backlog changed without re-pinning this guard.\n"
            f"  pinned: {BACKLOG_FINGERPRINT}\n  actual: {actual}\n"
            "If you REMOVED entries (the only allowed direction), re-pin with\n"
            "  python tests/ci/test_gpu_harness_deferred_contract.py --print-fingerprint\n"
            "and lower BACKLOG_MAX_ENTRIES to match. If you ADDED entries, stop: put the test\n"
            "in a batch instead.",
        )

    def test_unbatched_backlog_is_a_ratchet(self):
        """The backlog may only shrink: entries that now run must be removed."""
        backlog = self.manifest.get("unbatched_requires_gpu_backlog", {}).get("test_names", [])
        stale = sorted(name for name in backlog if name in self.runs)
        self.assertEqual(
            stale,
            [],
            "these tests now execute in a named batch — delete them from "
            "unbatched_requires_gpu_backlog.test_names so the count keeps falling:\n  "
            + "\n  ".join(stale),
        )
        known = {t["name"] for t in self.tests}
        gone = sorted(name for name in backlog if name not in known)
        self.assertEqual(
            gone,
            [],
            "backlog names that no longer exist in the corpus (renamed or deleted):\n  "
            + "\n  ".join(gone),
        )

    def test_coverage_snapshot_is_accurate(self):
        """The recorded partition must equal reality: running + waived + backlog == corpus."""
        backlog = self.manifest.get("unbatched_requires_gpu_backlog", {})
        snapshot = backlog.get("coverage_snapshot", {})
        actual = {
            "requires_gpu_total": len(self.tests),
            "running_in_a_named_batch": len(self.runs),
            "deferred_with_waiver": len(self.waiver_names),
            "unbatched_backlog": len(backlog.get("test_names", [])),
        }
        self.assertEqual(
            snapshot,
            actual,
            "unbatched_requires_gpu_backlog.coverage_snapshot has drifted from reality",
        )
        self.assertEqual(
            actual["running_in_a_named_batch"]
            + actual["deferred_with_waiver"]
            + actual["unbatched_backlog"],
            actual["requires_gpu_total"],
            "every [RequiresGPU] test must be exactly one of: running, waived, backlogged",
        )

    def test_every_exclude_resolves_to_exactly_one_case(self):
        for batch in self.batches:
            for pattern in batch.excludes:
                hits = sorted(
                    t["name"]
                    for t in self.tests
                    if doctest_match(pattern, t["name"])
                    and any(doctest_match(f, t["name"]) for f in batch.filters)
                )
                self.assertEqual(
                    len(hits),
                    1,
                    f"batch {batch.name} exclude {pattern!r} matched {len(hits)} cases "
                    f"(expected exactly 1): {hits}. A stale exclude silently widens the "
                    f"batch; an over-broad one silently drops coverage.",
                )

    def test_excludes_and_waivers_are_in_bijection(self):
        excluded_names = set(self.excluded)
        named_waivers = {w["test_name"] for w in self.waivers if w.get("exclude_pattern")}
        self.assertEqual(
            sorted(excluded_names - named_waivers),
            [],
            "excluded from a GPU batch but not declared in deferred_requires_gpu_waivers",
        )
        self.assertEqual(
            sorted(named_waivers - excluded_names),
            [],
            "waived as batch-excluded but no BatchSpec.excludes pattern matches it any more "
            "(the case may have started running again — drop the waiver)",
        )

    def test_waiver_batch_and_pattern_match_reality(self):
        for waiver in self.waivers:
            pattern = waiver.get("exclude_pattern")
            if not pattern:
                continue
            name = waiver["test_name"]
            self.assertIn(name, self.excluded, f"{name} is not excluded by any batch")
            pairs = self.excluded[name]
            self.assertIn(
                (waiver["batch"], pattern),
                pairs,
                f"waiver for {name} claims batch/pattern {(waiver['batch'], pattern)} "
                f"but the harness excludes it via {pairs}",
            )

    def test_deferred_count_matches_reality(self):
        """The check that would have caught 26 -> 28."""
        gate = _load("gs_gate_contract_count", GATE_PATH)
        deferred = gate._deferred_requires_gpu_tests(self.manifest, self.tests)
        declared = self.manifest["requires_gpu_test_snapshot"]["deferred_count"]
        self.assertEqual(
            len(deferred),
            declared,
            f"deferred RequiresGPU count drift: real={len(deferred)} manifest={declared}",
        )
        self.assertEqual(
            len(self.waivers),
            len(deferred),
            "every deferred test needs exactly one waiver",
        )

    def test_leak_listener_samples_after_scenetree_teardown(self):
        """GsGpuRidLeakListener must outrank GodotTestCaseListener (#329, Codex review).

        doctest builds its active-listener list by iterating getListeners() -- a
        map keyed by (priority, name) -- and inserting each at
        reporters_currently_used.begin() (doctest.h:6891-6892), which REVERSES
        map order; callbacks then run front-to-back (doctest.h:4112).

        So the listener that sorts FIRST in the map runs LAST. To sample GPU
        memory after the SceneTree is torn down, GsGpuRidLeakListener must sort
        strictly before GodotTestCaseListener -- i.e. carry a strictly lower
        priority. At equal priority (both were 1) the name breaks the tie,
        "GodotTestCaseListener" < "GsGpuRidLeakListener", and the leak listener
        sampled while the whole SceneTree was still alive: 2.72 GB of phantom
        leaks across the new batches, which run_gpu_harness.py folds into
        gate_failed globally.

        Derive BOTH priorities from source so this still fails if upstream
        Godot changes its listener's priority.
        """
        runner = (
            ROOT / "modules" / "gaussian_splatting" / "tests" / "gs_gpu_test_runner.cpp"
        ).read_text(encoding="utf-8", errors="replace")
        godot = (ROOT / "tests" / "test_main.cpp").read_text(encoding="utf-8", errors="replace")

        def priority_of(text: str, listener: str) -> int:
            match = re.search(
                r'REGISTER_LISTENER\(\s*"' + re.escape(listener) + r'"\s*,\s*(\d+)\s*,', text
            )
            self.assertIsNotNone(match, f"could not find REGISTER_LISTENER for {listener}")
            return int(match.group(1))

        leak_priority = priority_of(runner, "GsGpuRidLeakListener")
        godot_priority = priority_of(godot, "GodotTestCaseListener")
        self.assertLess(
            leak_priority,
            godot_priority,
            f"GsGpuRidLeakListener priority ({leak_priority}) must be strictly lower than "
            f"GodotTestCaseListener's ({godot_priority}) so it runs LAST and samples GPU memory "
            f"AFTER SceneTree teardown. doctest reverses map order when building the listener "
            f"list. Raising this priority silently reintroduces multi-GB phantom leak reports.",
        )

    def test_leak_listener_releases_worlds_before_sampling(self):
        """The SceneDirector's retained renderers must be dropped before sampling.

        They outlive the case's SceneTree, so without this every [SceneTree]
        case that built a renderer reports a false leak regardless of listener
        ordering.
        """
        runner_path = ROOT / "modules" / "gaussian_splatting" / "tests" / "gs_gpu_test_runner.cpp"
        text = runner_path.read_text(encoding="utf-8", errors="replace")
        start = text.find("void test_case_end(")
        self.assertNotEqual(start, -1, "GsGpuRidLeakListener::test_case_end not found")
        end = text.find("void report_query(", start)
        body = text[start:end]

        release_at = body.find("release_all_worlds()")
        self.assertNotEqual(
            release_at,
            -1,
            "test_case_end must call GaussianSplatSceneDirector::release_all_worlds() so "
            "director-retained renderer GPU memory is not attributed to the running case.",
        )
        sample_at = body.find("get_memory_usage(")
        self.assertNotEqual(sample_at, -1, "test_case_end must sample GPU memory usage")
        self.assertLess(
            release_at,
            sample_at,
            "release_all_worlds() must be called BEFORE the post-case memory sample, "
            "otherwise the retained renderers are still counted as a leak.",
        )

    def test_no_duplicate_waivers(self):
        names = [w["test_name"] for w in self.waivers]
        self.assertEqual(sorted(names), sorted(set(names)), "duplicate waiver entries")

    def test_scenetree_is_no_longer_a_deferred_tag(self):
        """#329's headline outcome, pinned so a revert is visible."""
        tags = self.manifest["requires_gpu_test_snapshot"].get("deferred_tags_any", [])
        self.assertNotIn(
            "SceneTree",
            tags,
            "[SceneTree] was re-added as a blanket deferred TAG. That silently re-defers the "
            "whole corpus #329 landed. Defer individual cases by name instead.",
        )


class GpuHarnessTimeoutReportingTests(unittest.TestCase):
    """Guard: a TIMED-OUT batch is reported as a non-result, by name (#677).

    A batch that hits its wall-clock timeout never prints a doctest summary, so
    its `test_cases_*` parse as 0 and its `rid_leak_bytes` holds only whatever the
    listener emitted before the kill. Those partial zeros are then summed into the
    run totals. The gate itself does fail (rc=124 flips `max_rc`), but that alone
    is not enough: before #677 the run-level `DONE` line and the JSON report named
    no batch, so `total_rid_leak_bytes=0` sat next to a half-executed batch reading
    like a clean measurement.

    This test drives `main()` with `_run_batch` stubbed to report a timeout, so it
    is deterministic, needs no GPU, and no real subprocess -- it runs in the
    headless guard lane alongside the rest of this file.
    """

    def _run_main_with_timed_out_batch(self, tmpdir: Path) -> tuple[int, str, dict]:
        import contextlib
        import io

        harness = _load("gs_harness_timeout", HARNESS_PATH)
        batch_name = harness.BATCHES[0].name
        report_path = tmpdir / "report.json"

        def _fake_run_batch(godot, name, filters, excludes, timeout_sec, extra_args):
            r = harness.BatchResult(name=name, filters=filters, excludes=excludes)
            r.rc = harness.SUPERVISOR_EXIT_TIMEOUT
            r.timed_out = True
            r.status = "TIMEOUT"
            r.wall_seconds = float(timeout_sec)
            # Deliberately leave every count at 0 and report NO leak bytes: this is
            # exactly the shape that reads like a clean pass if it is not named.
            return r

        harness._run_batch = _fake_run_batch
        # `_resolve_godot` only checks is_file(), so any real file stands in for
        # the binary -- the stub above means it is never executed.
        argv = [
            "run_gpu_harness.py",
            "--godot", str(HARNESS_PATH),
            "--batch", batch_name,
            "--report", str(report_path),
        ]
        buf = io.StringIO()
        old_argv = sys.argv
        sys.argv = argv
        try:
            with contextlib.redirect_stdout(buf):
                rc = harness.main()
        finally:
            sys.argv = old_argv
        report = json.loads(report_path.read_text(encoding="utf-8"))
        return rc, buf.getvalue(), report

    def test_timed_out_batch_fails_gate_and_is_named(self):
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            rc, stdout, report = self._run_main_with_timed_out_batch(Path(td))

        batch_name = report["batches"][0]["name"]

        # 1. The gate must FAIL. A timed-out batch is never a pass.
        self.assertNotEqual(
            rc, 0,
            "A timed-out batch must fail the gate. It executed an unknown fraction of "
            "its cases and printed no doctest summary.",
        )
        self.assertEqual(
            rc, 124,
            "A timeout must surface as exit 124 so callers can distinguish it from an "
            "assertion failure (1) or a signal kill.",
        )

        # 2. The batch must be NAMED. This is the part that was missing: without a
        #    name, a reader sees only `max_rc=124` beside a 0-byte leak total.
        self.assertIn(
            batch_name, stdout,
            "The timed-out batch must be named in the supervisor's output, not left "
            "implicit behind a bare max_rc=124.",
        )
        self.assertIn(
            "FATAL", stdout,
            "A timed-out batch must be announced as a FATAL non-result.",
        )

        # 3. The report must carry it structurally, so downstream consumers
        #    (baseline_qa.yml's step summary, cross-run leak comparisons) can see
        #    the totals are incomplete without scraping prose.
        self.assertEqual(
            report["timed_out_batches"], [batch_name],
            "report.timed_out_batches must list every batch that timed out.",
        )
        self.assertFalse(
            report["totals_authoritative"],
            "totals_authoritative must be False when any batch timed out -- the "
            "totals and total_rid_leak_bytes exclude whatever that batch never reached.",
        )

        # 4. The trap this issue is really about: a 0-byte leak total sitting next
        #    to an unrun batch must NOT be presentable as a clean result.
        self.assertEqual(report["total_rid_leak_bytes"], 0)
        self.assertEqual(report["totals"]["test_cases_total"], 0)
        self.assertNotEqual(
            report["supervisor_exit"], 0,
            "A report whose totals are all zero because a batch never ran must not "
            "carry a success exit code.",
        )


class GpuHarnessZeroAssertionRequiredBatchTests(unittest.TestCase):
    """Guard: a REQUIRED batch that evaluates zero assertions fails the gate (#692).

    The empty-batch check catches a required batch whose filter stopped matching.
    It does not catch the shape found in #692: the filter matches, the cases run,
    doctest reports every one of them PASSED -- and not a single assertion was
    evaluated. RendererPipeline did exactly that from the day it became required
    (#418), reporting `cases=4/4 asserts=0/0 status=SUCCESS` while all four cases
    early-returned on a null RenderingServer. doctest scores an early return as a
    pass, so the gate treated "verified nothing" as evidence.

    This is scoped to REQUIRED_BATCHES on purpose. "0 tests matched" is documented
    success for advisory batches (catalogued-but-not-yet-running batches are the
    point of that leniency), and these tests pin BOTH halves of that asymmetry so
    a later refactor cannot quietly extend the strict rule to advisory batches or
    quietly drop it from required ones.

    Drives `main()` with `_run_batch` stubbed, so it is deterministic, needs no
    GPU and no subprocess -- it runs in the headless guard lane.
    """

    def _run_main(
        self,
        tmpdir: Path,
        batch_name: str,
        cases: int,
        asserts: int,
        zero_assertion_cases: list[str] | None = None,
        case_assert_audit_ok: bool = True,
        zero_assert_reported: int | None = None,
    ):
        import contextlib
        import io

        harness = _load("gs_harness_zero_assert", HARNESS_PATH)
        report_path = tmpdir / "report.json"

        def _fake_run_batch(godot, name, filters, excludes, timeout_sec, extra_args):
            r = harness.BatchResult(name=name, filters=filters, excludes=excludes)
            r.rc = 0
            r.status = "SUCCESS"
            r.test_cases_total = cases
            r.test_cases_passed = cases
            # #695: doctest's `skipped` column is the whole registered corpus
            # minus this batch's filter reach, so a real batch ALWAYS reports a
            # four-digit value here. Modelling it as 0 is what let the release
            # gate's skip check look correct while rejecting every real report.
            r.test_cases_skipped = 2008 - cases
            r.assertions_total = asserts
            r.assertions_passed = asserts
            # summary_parse_ok=True is the crux: doctest printed a perfectly
            # well-formed summary. Nothing is malformed here -- the run is simply
            # hollow, which is why no pre-existing check caught it.
            r.summary_parse_ok = True
            # #695: the listener's per-case audit. Default is "the audit ran and
            # found nothing hollow", the shape of a healthy batch.
            r.case_assert_audit_ok = case_assert_audit_ok
            r.cases_started = cases
            r.zero_assertion_cases = list(zero_assertion_cases or [])
            # Codex PR #696: the audit marker's own `zero_assert=M` count.
            # Defaults to agreeing with the named cases -- the shape of intact
            # evidence -- so the reconciliation rule stays silent unless a test
            # deliberately drives the two apart.
            r.zero_assert_reported = (
                len(r.zero_assertion_cases)
                if zero_assert_reported is None
                else zero_assert_reported
            )
            return r

        harness._run_batch = _fake_run_batch
        argv = [
            "run_gpu_harness.py",
            "--godot", str(HARNESS_PATH),
            "--batch", batch_name,
            "--report", str(report_path),
        ]
        buf = io.StringIO()
        old_argv = sys.argv
        sys.argv = argv
        try:
            with contextlib.redirect_stdout(buf):
                rc = harness.main()
        finally:
            sys.argv = old_argv
        return rc, buf.getvalue(), json.loads(report_path.read_text(encoding="utf-8"))

    def _a_required_batch(self) -> str:
        harness = _load("gs_harness_zero_assert_names", HARNESS_PATH)
        # Sorted for determinism: REQUIRED_BATCHES is a frozenset.
        return sorted(harness.REQUIRED_BATCHES)[0]

    def _an_advisory_batch(self) -> str:
        harness = _load("gs_harness_zero_assert_names2", HARNESS_PATH)
        advisory = [
            s.name for s in harness.BATCHES if s.name not in harness.REQUIRED_BATCHES
        ]
        self.assertTrue(advisory, "expected at least one advisory batch")
        return sorted(advisory)[0]

    def test_required_batch_with_zero_assertions_fails_gate_and_is_named(self):
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, stdout, report = self._run_main(Path(td), batch_name, cases=4, asserts=0)

        self.assertNotEqual(
            rc, 0,
            "A required batch that ran test cases but evaluated 0 assertions must "
            "fail the gate. Passing here is the #692 defect itself.",
        )
        self.assertEqual(
            report["zero_assertion_required_batches"], [batch_name],
            "The report must name the offending batch so CI triage does not have to "
            "diff per-batch assertion counts by hand.",
        )
        # It matched cases, so it is NOT the empty-batch failure -- the two reasons
        # must stay disjoint or the diagnosis printed to CI is misleading.
        self.assertEqual(report["empty_required_batches"], [])
        self.assertIn(
            batch_name, stdout,
            "The FATAL line must name the batch.",
        )
        self.assertIn(
            "0 assertions", stdout,
            "The FATAL line must state the zero assertion count, which is the "
            "evidence a reader needs to act.",
        )

    def test_required_batch_with_assertions_passes(self):
        """The rule must discriminate: same batch, assertions > 0, gate passes."""
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, _stdout, report = self._run_main(Path(td), batch_name, cases=4, asserts=17)

        self.assertEqual(
            rc, 0,
            "A required batch that evaluated assertions and reported no failures "
            "must still pass. Without this the check above could pass vacuously.",
        )
        self.assertEqual(report["zero_assertion_required_batches"], [])

    def test_advisory_batch_with_zero_assertions_still_passes(self):
        """Scope check: the strict rule applies to REQUIRED batches only.

        Advisory batches are explicitly allowed to match nothing and assert
        nothing (see the BATCHES comment in the harness). If this test ever
        starts failing, the zero-assertion rule has leaked out of its scope.
        """
        import tempfile

        batch_name = self._an_advisory_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, _stdout, report = self._run_main(Path(td), batch_name, cases=0, asserts=0)

        self.assertEqual(
            rc, 0,
            "An advisory batch asserting nothing must not fail the gate.",
        )
        self.assertEqual(report["zero_assertion_required_batches"], [])

    # ---- #695: per-case audit escalation --------------------------------

    def test_required_batch_with_one_hollow_case_fails_gate_and_is_named(self):
        """The per-case refinement of the rule above.

        `zero_assertion_required_batches` only fires when the WHOLE batch
        evaluated zero assertions. A batch of four cases where three early-return
        and one asserts reports `asserts=17/17` and passes it, having verified a
        quarter of what it claims. doctest cannot report this either: its
        `skipped` column counts the ~2004 corpus cases the filter did not select
        and reads the same on a healthy batch (that is the #695 defect). Only the
        listener's per-case audit can see it.
        """
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, stdout, report = self._run_main(
                Path(td),
                batch_name,
                cases=4,
                asserts=17,
                zero_assertion_cases=["Stage results report raster failure"],
            )

        self.assertNotEqual(rc, 0, "A hollow case in a required batch must fail the gate.")
        self.assertEqual(
            report["hollow_required_cases"],
            [f"{batch_name}/Stage results report raster failure"],
        )
        # Disjoint from the batch-level reason: this batch DID assert.
        self.assertEqual(report["zero_assertion_required_batches"], [])
        self.assertEqual(report["empty_required_batches"], [])
        self.assertIn("Stage results report raster failure", stdout)
        # The four-digit filter-miss count is present and is NOT the reason.
        self.assertEqual(report["batches"][0]["test_cases"]["skipped"], 2004)

    def test_required_batch_without_case_audit_marker_fails_gate(self):
        """Fail closed when the producer never affirmed that it looked.

        The hollow-case signal is carried by the ABSENCE of NO-ASSERTS lines, and
        absence is equally what a binary built without the listener emits. A
        perfectly green batch must therefore be refused when the audit marker is
        missing, or removing the producer silently greens the gate.
        """
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, stdout, report = self._run_main(
                Path(td), batch_name, cases=4, asserts=17, case_assert_audit_ok=False
            )

        self.assertNotEqual(rc, 0, "A missing per-case audit must fail the gate.")
        self.assertEqual(report["case_audit_missing_batches"], [batch_name])
        self.assertIn("CASE-ASSERT-AUDIT", stdout)

    def test_advisory_batch_with_hollow_case_still_passes(self):
        """Scope check, mirroring the batch-level asymmetry above.

        Without this the two rejections could pass vacuously via a rule that
        simply always fires.
        """
        import tempfile

        batch_name = self._an_advisory_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, _stdout, report = self._run_main(
                Path(td),
                batch_name,
                cases=2,
                asserts=3,
                zero_assertion_cases=["some advisory case"],
                case_assert_audit_ok=False,
            )

        self.assertEqual(rc, 0, "Advisory batches keep their documented leniency.")
        self.assertEqual(report["hollow_required_cases"], [])
        self.assertEqual(report["case_audit_missing_batches"], [])

    def test_clean_required_batch_with_audit_passes(self):
        """Discrimination: audit ran, nothing hollow, four-digit skip count -> pass."""
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, _stdout, report = self._run_main(Path(td), batch_name, cases=4, asserts=17)

        self.assertEqual(rc, 0)
        self.assertEqual(report["hollow_required_cases"], [])
        self.assertEqual(report["case_audit_missing_batches"], [])
        self.assertEqual(report["batches"][0]["test_cases"]["skipped"], 2004)
        self.assertIs(report["batches"][0]["case_assert_audit_ok"], True)
        # Codex PR #696: intact evidence reconciles, so the mismatch rule is
        # silent here. Asserting the clean case keeps the rejection below from
        # passing via a rule that simply always fires.
        self.assertEqual(report["batches"][0]["zero_assert_reported"], 0)
        self.assertEqual(report["case_audit_mismatch_batches"], [])

    # ------------------------------------------------------------------
    # Codex PR #696: the audit marker is proof the listener LOOKED, not proof
    # that nothing was found. `_run_batch` keeps only the last 64 KiB of stdout,
    # so the per-case NO-ASSERTS lines can be trimmed away while the marker --
    # printed last, at run end -- survives. The gate must reconcile the marker's
    # `zero_assert=M` against the number of named cases and fail closed on any
    # disagreement, rather than read the empty list as "nothing hollow".
    # ------------------------------------------------------------------

    def test_trimmed_audit_count_mismatch_fails_required_batch(self) -> None:
        """End-to-end: the reconciliation must FAIL the gate, not just record it.

        Drives `main()` with the post-trim shape -- marker present, case list
        empty, `zero_assert=2` -- which is precisely a run the pre-fix gate
        accepted: `hollow_required_cases` empty (no names to escalate) and
        `case_audit_missing_batches` empty (the marker was there).
        """
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, stdout, report = self._run_main(
                Path(td),
                batch_name,
                cases=4,
                asserts=8,
                zero_assertion_cases=[],
                case_assert_audit_ok=True,
                zero_assert_reported=2,
            )

        self.assertNotEqual(
            rc, 0, "A batch whose listener counted hollow cases must fail the gate."
        )
        self.assertEqual(
            report["case_audit_mismatch_batches"],
            [f"{batch_name}: zero_assert=2 named=0"],
        )
        # Named, not merely counted: the operator has to be told what is missing.
        self.assertIn("zero_assert=2", stdout)
        self.assertIn("trimmed", stdout)
        # Disjointness: neither pre-existing rule fired, which is exactly why
        # this run went green before the fix.
        self.assertEqual(report["hollow_required_cases"], [])
        self.assertEqual(report["case_audit_missing_batches"], [])
        self.assertEqual(report["zero_assertion_required_batches"], [])

    def test_audit_count_mismatch_in_the_other_direction_also_fails(self) -> None:
        """More named lines than the marker counted is equally disqualifying.

        That direction cannot arise from tail-trimming, so it means the marker
        or the lines are malformed -- the evidence disagrees with itself and is
        refused rather than reconciled in the gate's favour.
        """
        import tempfile

        batch_name = self._a_required_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, _stdout, report = self._run_main(
                Path(td),
                batch_name,
                cases=4,
                asserts=8,
                zero_assertion_cases=["case a", "case b"],
                case_assert_audit_ok=True,
                zero_assert_reported=1,
            )

        self.assertNotEqual(rc, 0)
        self.assertEqual(
            report["case_audit_mismatch_batches"],
            [f"{batch_name}: zero_assert=1 named=2"],
        )

    def test_advisory_batch_with_audit_count_mismatch_still_passes(self) -> None:
        """Scope check: the rule is REQUIRED-only, like its two siblings."""
        import tempfile

        batch_name = self._an_advisory_batch()
        with tempfile.TemporaryDirectory() as td:
            rc, _stdout, report = self._run_main(
                Path(td),
                batch_name,
                cases=2,
                asserts=3,
                zero_assertion_cases=[],
                case_assert_audit_ok=True,
                zero_assert_reported=2,
            )

        self.assertEqual(rc, 0, "Advisory batches keep their documented leniency.")
        self.assertEqual(report["case_audit_mismatch_batches"], [])


class GpuHarnessBatchTimeoutBudgetTests(unittest.TestCase):
    """Guard: NodeSceneTree keeps a timeout budget large enough to finish (#677).

    Measured on ccbef8cc482 / RTX 3090: 18 executing cases at ~2.4-2.9 s each plus
    ~35 s of RenderingDevice bring-up and teardown = ~82 s wall. Under the 60 s
    default the batch was cut off at exactly 9 of 18 cases, so half the corpus that
    #329/#660 landed silently stopped executing while the batch reported 0/0.

    This pins the override so a future edit cannot drop it back to the default and
    re-hide half the batch.

    RE-MEASURED after the #329 waiver-reduction pass un-quarantined the last four
    NodeSceneTree cases: the batch measured 127.7 s, 133.8 s and 153.3 s wall across
    three consecutive runs on an RTX 3090 (run-to-run variance is large here --
    device bring-up dominates). The constant tracks the SLOWEST observed run, not
    the mean, because the failure mode being guarded is truncation on a runner
    slower than this box.

    The batch has since grown to 25 executing cases / 311 assertions (#831 +1,
    #839 round 2 +2 cases; +30 assertions on top of the 281 master reports after
    #708/PR #843 converted four success-path `REQUIRE`/`CHECK` size assertions to
    `if (...) { FAIL(...); return; }` guards, which record no assertion when the
    size is correct -- the conditions are still enforced, see #708 for the mutation
    proof), most recently measured at 113.0 s wall on the same box -- BELOW the
    153 s constant, so the constant is deliberately left where it is rather than
    being lowered to the newest number: lowering it would weaken this guard's own
    headroom requirement.

    The 82 s constant was left stale by the growth from 18 to 22 cases, which is
    the same silent-drift failure #329 was filed about. Against 153 s the previous
    180 s budget was only 1.17x -- below this guard's own 1.5x floor -- so the
    budget was raised with the measurement rather than the floor being lowered.
    """

    def test_node_scenetree_has_headroom_over_measured_wall_time(self):
        MEASURED_WALL_SECONDS = 153
        batches = {b.name: b for b in _batches()}
        spec = batches.get("NodeSceneTree")
        if spec is None:
            self.fail("NodeSceneTree batch is missing from run_gpu_harness.py BATCHES.")

        if spec.timeout_seconds is None:
            self.fail(
                "NodeSceneTree must set an explicit BatchSpec.timeout_seconds. It measured "
                f"~{MEASURED_WALL_SECONDS}s wall and does not fit the 60s default -- without "
                "an override it is cut off at ~9 of 18 cases and reports 0/0 (#677)."
            )
        self.assertGreaterEqual(
            spec.timeout_seconds, int(MEASURED_WALL_SECONDS * 1.5),
            f"NodeSceneTree's timeout must keep real headroom over its measured "
            f"~{MEASURED_WALL_SECONDS}s wall time; a loaded CI runner is slower than a "
            "quiet developer box. Lowering this re-creates the #677 silent truncation.",
        )


def _print_summary() -> int:
    tests, batches, manifest = _corpus(), _batches(), _manifest()
    runs, excluded = _batch_membership(tests, batches)
    waivers = manifest.get("deferred_requires_gpu_waivers", [])
    print(f"[RequiresGPU] corpus: {len(tests)} test cases\n")
    print(f"{'batch':<26} {'runs':>5} {'excluded':>9}")
    print("-" * 42)
    for batch in batches:
        r = sum(1 for n, bs in runs.items() if batch.name in bs)
        x = sum(1 for n, ps in excluded.items() if any(b == batch.name for b, _ in ps))
        print(f"{batch.name:<26} {r:>5} {x:>9}")
    print("-" * 42)
    orphans = [t["name"] for t in tests
               if t["name"] not in runs and t["name"] not in {w["test_name"] for w in waivers}]
    backlog = set(manifest.get("unbatched_requires_gpu_backlog", {}).get("test_names", []))
    undeclared = [n for n in orphans if n not in backlog]
    print(f"{'TOTAL running':<26} {len(runs):>5} {len(excluded):>9}")
    print()
    print(f"running in a named batch : {len(runs):>4}")
    print(f"deferred (waived)        : {len(waivers):>4}")
    print(f"unbatched backlog        : {len(orphans) - len(undeclared):>4}  (declared, ratcheting down)")
    print(f"UNDECLARED orphans       : {len(undeclared):>4}  (must be 0)")
    for name in undeclared:
        print(f"  UNDECLARED: {name}")
    return 1 if undeclared else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--print-summary", action="store_true")
    parser.add_argument("--print-fingerprint", action="store_true")
    args, rest = parser.parse_known_args()
    if args.print_fingerprint:
        _names = _manifest().get("unbatched_requires_gpu_backlog", {}).get("test_names", [])
        print(f"BACKLOG_MAX_ENTRIES = {len(_names)}")
        print(f'BACKLOG_FINGERPRINT = "{backlog_fingerprint(_names)}"')
        raise SystemExit(0)
    if args.print_summary:
        raise SystemExit(_print_summary())
    unittest.main(argv=[sys.argv[0], *rest])
