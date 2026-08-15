#!/usr/bin/env python3
"""Unit tests for scripts/agentic/classify_change.py."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "classify_change.py"
spec = importlib.util.spec_from_file_location("classify_change", SCRIPT)
assert spec and spec.loader
classify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(classify)

POLICY = json.loads((ROOT / ".agentic" / "policy.json").read_text(encoding="utf-8"))


class ClassifyChangeTest(unittest.TestCase):
    def _cls(self, paths):
        return classify.classify_paths(paths, POLICY)[0]

    def test_engine_path_is_r3(self):
        self.assertEqual(self._cls(["servers/rendering/foo.cpp"]), "R3")

    def test_persistence_path_is_r3(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/io/ply_loader.cpp"]), "R3")

    def test_renderer_path_is_r2(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/renderer/gpu_sorter.cpp"]), "R2")

    def test_shaders_path_is_r2(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/shaders/raster.glsl"]), "R2")

    def test_core_streaming_is_r2(self):
        # Streaming/VRAM files under core/ are R2, not R1.
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/gaussian_streaming.cpp"]), "R2")
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/streaming_vram_regulator.h"]), "R2")
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/residency_budget_controller.cpp"]), "R2")

    def test_core_nonstreaming_is_r1(self):
        # Ordinary core data files stay R1.
        self.assertEqual(self._cls(["modules/gaussian_splatting/core/gaussian_data.cpp"]), "R1")

    def test_local_module_is_r1(self):
        self.assertEqual(self._cls(["modules/gaussian_splatting/logger/logger.cpp"]), "R1")

    def test_ordinary_test_path_is_r1(self):
        self.assertEqual(self._cls(["tests/ci/test_ply_loader_ci.gd"]), "R1")

    def test_ci_gate_machinery_is_r3(self):
        # The deterministic-check / release-gate runners must not be downgradable at R1.
        self.assertEqual(self._cls(["tests/ci/run_module_tests.py"]), "R3")
        self.assertEqual(self._cls(["tests/runtime/run_runtime_validation.py"]), "R3")
        self.assertEqual(self._cls(["tests/ci/check_renderer_release_gates.py"]), "R3")
        self.assertEqual(self._cls(["tests/ci/run_gpu_harness.py"]), "R3")

    def test_docs_is_r0(self):
        self.assertEqual(self._cls(["docs/governance/review-policy.md"]), "R0")

    def test_root_doc_is_r0(self):
        self.assertEqual(self._cls(["README.md"]), "R0")
        self.assertEqual(self._cls(["CONTRIBUTING.md"]), "R0")

    def test_unknown_sensitive_path_fails_closed_to_r3(self):
        self.assertEqual(self._cls(["some/unmapped/path.bin"]), "R3")

    def test_unmapped_markdown_fails_closed_to_r3(self):
        # A markdown file outside the known doc/root scopes must not slip to R0 via a
        # blanket *.md rule; unrecognized paths fail closed to R3.
        self.assertEqual(self._cls(["weird/unmapped/notes.md"]), "R3")

    def test_overall_is_max_across_paths(self):
        self.assertEqual(
            self._cls(["docs/x.md", "modules/gaussian_splatting/renderer/a.cpp"]),
            "R2",
        )
        self.assertEqual(
            self._cls(["docs/x.md", "servers/y.cpp"]),
            "R3",
        )

    def test_empty_changeset_fails_closed_to_default_unclassified(self):
        """DO NOT "fix" this back to R0 (GS-AUDIT-TEST-002).

        An empty changed-path set is an ABSENCE of information -- a mis-resolved
        base, a filtered path list, a diff form that answered a different question
        -- not a demonstration that nothing risky changed. The predecessor of this
        test asserted R0, i.e. the suite defended the fail-open while
        ``policy.json`` declared ``default_unclassified: R3`` and the module
        docstring claimed fail-closed behaviour.

        Both halves are asserted: the derived class must equal the policy default,
        AND the live policy default must still be R3. Asserting only the first
        would keep passing if someone set the policy default to R0.
        """
        self.assertEqual(self._cls([]), POLICY["classification"]["default_unclassified"])
        self.assertEqual("R3", POLICY["classification"]["default_unclassified"])

    def test_empty_changeset_default_is_read_from_policy_not_hardcoded(self):
        """The fallback must come from the policy, not from a literal "R3".

        A hardcoded default would pass the test above and silently ignore a policy
        that declares something else.
        """
        for expected in ("R2", "R1"):
            policy = copy.deepcopy(POLICY)
            policy["classification"]["default_unclassified"] = expected
            self.assertEqual(classify.classify_paths([], policy)[0], expected)

    def test_a_policy_change_is_forced_to_the_top_class(self):
        """`.agentic/policy.json` is R0 under the shipped rules. It must not be.

        The rules that would grade a change to the risk policy are the rules being
        changed. Forcing the top class is one half of the fix for that; the other
        half is the caller passing the immutable base policy (see below).
        """
        ordering = POLICY["classification"]["ordering"]
        self.assertEqual(self._cls([".agentic/policy.json"]), ordering[-1])

    def test_a_relaxed_rule_cannot_downgrade_the_pr_that_relaxes_it(self):
        """The "compared against itself" shape, as an executable case.

        A PR that flips the renderer rule R2 -> R0 and touches a renderer file used
        to classify **R1**: the renderer path fell through to the
        `modules/gaussian_splatting/**` rule, and `.agentic/policy.json` itself was
        R0. `validate_repo_contract.py --strict-hierarchy` passes throughout,
        because the relaxed policy is still internally consistent. The required
        check then published R1 obligations for a renderer change.
        """
        relaxed = copy.deepcopy(POLICY)
        for rule in relaxed["classification"]["rules"]:
            if rule["class"] == "R2":
                rule["class"] = "R0"
        rank = {cls: i for i, cls in enumerate(POLICY["classification"]["ordering"])}
        paths = ["modules/gaussian_splatting/renderer/gpu_sorter.cpp", ".agentic/policy.json"]

        # Graded by the PR's OWN relaxed policy, the class still cannot drop.
        relaxed_class = classify.classify_paths(paths, relaxed)[0]
        self.assertGreaterEqual(rank[relaxed_class], rank["R2"], relaxed_class)
        self.assertEqual("R3", relaxed_class)

        # And graded by the immutable base policy -- what CI now passes via
        # --policy -- the renderer file is R2 on its own merits regardless.
        self.assertEqual("R2", self._cls([paths[0]]))

    def test_windows_separators_are_normalized(self):
        self.assertEqual(self._cls([r"modules\gaussian_splatting\renderer\a.cpp"]), "R2")


def _git(cwd: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            "git",
            "-c", "user.name=gs-test",
            "-c", "user.email=gs-test@example.invalid",
            "-c", "commit.gpgsign=false",
            "-C", str(cwd),
            *args,
        ],
        capture_output=True,
        text=True,
        check=True,
    )


def _commit(repo: Path, name: str) -> None:
    (repo / name).write_text(name, encoding="utf-8")
    _git(repo, "add", name)
    _git(repo, "commit", "-m", f"add {name}")


class BaseRefResolutionTest(unittest.TestCase):
    """``git_changed_paths`` must fail closed rather than degrade the diff.

    The previous implementation retried an unresolvable ``base...HEAD`` as a
    two-dot ``git diff base``. That is a different question, and it succeeds in
    cases where the three-dot form fails -- so a bad base produced a path set that
    was not the PR's diff, and an under-reported risk class followed
    (GS-AUDIT-TEST-002's stated trigger).
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        _git(self.repo, "init", "-q")
        # Point the module's ROOT at the scratch repository for the duration.
        self._saved_root = classify.ROOT
        classify.ROOT = self.repo
        self.addCleanup(lambda: setattr(classify, "ROOT", self._saved_root))

    def _diverged_repo(self) -> None:
        """base and feature share a root commit, then diverge."""
        _git(self.repo, "checkout", "-q", "-b", "base")
        _commit(self.repo, "shared.txt")
        _git(self.repo, "checkout", "-q", "-b", "feature")
        _commit(self.repo, "only_on_feature.txt")
        _git(self.repo, "checkout", "-q", "base")
        _commit(self.repo, "only_on_base.txt")
        _git(self.repo, "checkout", "-q", "feature")

    def test_diff_is_taken_from_the_merge_base(self):
        """The three-dot meaning is preserved: base-only commits must not appear.

        This is the discriminating half. A two-dot ``git diff base`` from feature
        would additionally report ``only_on_base.txt`` (as a deletion), and a
        classifier fed that list is grading paths the PR never touched.
        """
        self._diverged_repo()
        self.assertEqual(["only_on_feature.txt"], classify.git_changed_paths("base"))

    def test_unresolvable_base_ref_exits_non_zero(self):
        self._diverged_repo()
        with self.assertRaises(SystemExit) as caught:
            classify.git_changed_paths("no-such-ref-42")
        message = str(caught.exception)
        self.assertIn("no-such-ref-42", message, "the error must name the unresolvable ref")
        self.assertNotEqual(0, caught.exception.code)

    def test_base_without_shared_history_fails_closed(self):
        """The exact fail-open the removed fallback produced.

        With two unrelated histories the three-dot form errors while the two-dot
        form succeeds and returns a full path list. The old code therefore turned
        "I cannot tell what this PR changed" into a confident answer. It must now
        be an error.
        """
        _git(self.repo, "checkout", "-q", "-b", "base")
        _commit(self.repo, "on_base.txt")
        _git(self.repo, "checkout", "-q", "--orphan", "feature")
        _git(self.repo, "rm", "-q", "-rf", ".")
        _commit(self.repo, "on_feature.txt")
        # Control: the two-dot form the old code fell back to DOES succeed here.
        two_dot = _git(self.repo, "diff", "--name-only", "base")
        self.assertTrue(two_dot.stdout.strip(), "control broken: two-dot diff produced nothing")
        with self.assertRaises(SystemExit) as caught:
            classify.git_changed_paths("base")
        self.assertIn("no merge base", str(caught.exception))

    def test_cli_exits_non_zero_for_an_unresolvable_base(self):
        """End-to-end through the real entry point, as CI invokes it."""
        self._diverged_repo()
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--base-ref", "no-such-ref-42", "--format", "json"],
            capture_output=True,
            text=True,
            cwd=str(self.repo),
        )
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("no-such-ref-42", result.stdout + result.stderr)


class StepSummaryTest(unittest.TestCase):
    """The required check must publish the class's obligations, from policy."""

    def test_summary_lists_the_class_evidence_and_deterministic_checks(self):
        _, per_path = classify.classify_paths(["servers/rendering/foo.cpp"], POLICY)
        summary = classify.render_markdown_summary("R3", per_path, POLICY, base_ref="abc1234")
        self.assertIn("R3", summary)
        for item in POLICY["risk_classes"]["R3"]["evidence_requirements"]:
            self.assertIn(item, summary)
        for command in POLICY["risk_classes"]["R3"]["deterministic_checks"]:
            self.assertIn(command, summary)
        # The honest-limit note must travel with the summary, not just the class.
        self.assertIn("contract-source ADR", summary)

    def test_summary_items_come_from_the_policy_argument(self):
        """Discriminating: a hardcoded block would ignore an edited policy."""
        policy = copy.deepcopy(POLICY)
        policy["risk_classes"]["R2"]["evidence_requirements"] = ["A UNIQUELY NAMED EVIDENCE ITEM"]
        policy["risk_classes"]["R2"]["deterministic_checks"] = ["python -m no_such_check"]
        summary = classify.render_markdown_summary("R2", [], policy)
        self.assertIn("A UNIQUELY NAMED EVIDENCE ITEM", summary)
        self.assertIn("python -m no_such_check", summary)
        for item in POLICY["risk_classes"]["R2"]["evidence_requirements"]:
            self.assertNotIn(item, summary)

    def test_flag_writes_to_the_github_step_summary_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "summary.md"
            saved = os.environ.get("GITHUB_STEP_SUMMARY")
            os.environ["GITHUB_STEP_SUMMARY"] = str(target)
            try:
                rc = classify.main(
                    [
                        "--paths",
                        "modules/gaussian_splatting/renderer/a.cpp",
                        "--format",
                        "json",
                        "--github-step-summary",
                    ]
                )
            finally:
                if saved is None:
                    os.environ.pop("GITHUB_STEP_SUMMARY", None)
                else:
                    os.environ["GITHUB_STEP_SUMMARY"] = saved
            self.assertEqual(0, rc)
            written = target.read_text(encoding="utf-8")
        self.assertIn("R2", written)
        self.assertIn(POLICY["risk_classes"]["R2"]["evidence_requirements"][0], written)


if __name__ == "__main__":
    unittest.main()
