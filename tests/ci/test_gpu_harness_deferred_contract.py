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
    args, rest = parser.parse_known_args()
    if args.print_summary:
        raise SystemExit(_print_summary())
    unittest.main(argv=[sys.argv[0], *rest])
