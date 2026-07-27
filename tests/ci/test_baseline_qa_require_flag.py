#!/usr/bin/env python3
"""Regression test for the QA-baseline enforcement switch (#596 follow-up).

BaselineQARunner._record_qa_baseline_skipped() - the path main() takes when
the "QA Scene Suite" test itself was skipped (e.g. no RenderingDevice in a
headless environment) - returned True unconditionally and never even accepted
a require_baseline argument. So --require-qa-baseline /
GS_CI_REQUIRE_QA_BASELINE had zero effect on that path: turning the switch on
could never fail a run, even with the baseline file completely absent.

HISTORY, kept because the corrections are the point. An early revision said
that skip path is "exactly the code path production CI takes"; that was
overstated and was corrected to "the path NO current CI run takes", because
main() only reaches it when the ``qa`` category is selected and, at the time,
no workflow selected ``qa``.

UPDATED BY #522: a workflow selects ``qa`` now. baseline_qa.yml's GPU job runs
``--categories qa --qa-require-capture --require-qa-baseline`` on the
fork-guarded self-hosted runner, so the QA-skip path IS reachable from CI --
and on that lane a skip is a hard failure rather than a legitimate skip,
because the lane promised a GPU. WorkflowCategorySelectionTest below is
inverted accordingly: it used to assert no lane selects ``qa``; it now asserts
exactly one does, so the gate cannot be silently removed again.

The underlying defect was real regardless of which lane hits it: the switch
was inert wherever that path *is* reached (local/manual ``--category qa``
runs, and the command documented in docs/testing/setup-guide.md).

This test locks in the contract:
  - "legitimately not applicable" (no RenderingDevice, capture not required)
    is still allowed to skip - no environment variable makes a GPU appear.
  - "baseline missing" is what --require-qa-baseline polices, and it fails
    closed on this path too, not just inside compare_qa_baseline().
  - "captured nothing on a lane that promised a GPU" is what
    --qa-require-capture polices, and it fails closed even when a baseline
    exists (QaRequireCaptureTest).
  - The switches' pre-existing off-by-default behavior is unchanged.
  - Requesting --require-qa-baseline on an invocation that never runs ``qa``
    is a hard failure, not a silent no-op (RequireBaselineAppliesTest).
  - Nothing that failed to render may become the golden baseline
    (BaselineCandidateValidationTest), and the blocking lane may not compare
    machine-dependent metrics (NonDeterministicMetricStrippingTest).
"""

from __future__ import annotations

import importlib.util
import json
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "run_baseline_qa.py"
spec = importlib.util.spec_from_file_location("run_baseline_qa", SCRIPT)
assert spec and spec.loader
run_baseline_qa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_baseline_qa)


class RecordQaBaselineSkippedTest(unittest.TestCase):
    """Exercises the exact call main() makes on the QA-scene-skip branch."""

    def _invoke(self, *, require_baseline: bool, baseline_present: bool):
        with tempfile.TemporaryDirectory() as raw_td:
            td = Path(raw_td)
            qa_results_path = td / "qa_results.json"  # never produced: suite didn't run
            baseline_path = td / "baselines" / "qa_results.json"
            if baseline_present:
                baseline_path.parent.mkdir(parents=True, exist_ok=True)
                baseline_path.write_text(json.dumps({"results": []}), encoding="utf-8")

            runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
            qa_ok = runner._record_qa_baseline_skipped(
                qa_results_path=qa_results_path,
                baseline_path=baseline_path,
                report_path=None,
                summary_path=None,
                reason="QA Scene Suite requires local RenderingDevice when run with current headless configuration.",
                require_baseline=require_baseline,
            )
            comparison = runner.test_results["summary"]["qa_baseline"]
            return qa_ok, comparison

    def test_switch_on_missing_baseline_fails_closed(self):
        """The defect: switch ON + baseline absent must FAIL, not silently pass."""
        qa_ok, comparison = self._invoke(require_baseline=True, baseline_present=False)
        self.assertFalse(qa_ok, "require_baseline=True with no baseline must fail the run.")
        self.assertEqual(comparison["status"], "failed")
        self.assertTrue(comparison["coverage_gap"])
        self.assertTrue(comparison["require_baseline"])
        self.assertFalse(comparison["baseline_exists"])

    def test_switch_on_present_baseline_still_allows_legitimate_skip(self):
        """A GPU-unavailable skip is orthogonal to baseline enforcement: if a
        baseline is committed, the switch must not punish an environment that
        simply couldn't produce current results to compare."""
        qa_ok, comparison = self._invoke(require_baseline=True, baseline_present=True)
        self.assertTrue(qa_ok, "A present baseline must not be penalized by an unrelated GPU skip.")
        self.assertEqual(comparison["status"], "skipped")
        self.assertTrue(comparison["coverage_gap"])
        self.assertTrue(comparison["require_baseline"])
        self.assertTrue(comparison["baseline_exists"])

    def test_switch_off_missing_baseline_keeps_prior_default_behavior(self):
        """Default (switch untouched) behavior must be unchanged: warn + skip,
        never a hard failure - the whole reason this switch exists opt-in."""
        qa_ok, comparison = self._invoke(require_baseline=False, baseline_present=False)
        self.assertTrue(qa_ok)
        self.assertEqual(comparison["status"], "skipped")
        self.assertTrue(comparison["coverage_gap"])
        self.assertFalse(comparison["require_baseline"])

    def test_failed_status_renders_as_fail_not_coverage_gap_in_markdown(self):
        """A hard failure must show as [FAIL] in the human-readable summary,
        not be swallowed under the softer "[NO BASELINE - COVERAGE GAP]"
        label just because coverage_gap is also true for that comparison.
        (This precedence bug becomes reachable only once _record_qa_baseline_
        skipped can itself set status="failed", which is exactly what this
        fix introduces.)"""
        _qa_ok, comparison = self._invoke(require_baseline=True, baseline_present=False)
        self.assertEqual(comparison["status"], "failed")
        self.assertTrue(comparison["coverage_gap"])

        runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
        markdown = runner._build_baseline_summary_markdown(comparison)
        self.assertIn("[FAIL]", markdown.splitlines()[2])
        self.assertNotIn("[NO BASELINE - COVERAGE GAP]", markdown)


class RequireBaselineAppliesTest(unittest.TestCase):
    """The second laundering path: enforcement requested where it cannot act.

    Every QA-baseline check in main() is gated on qa_ran. Before this fix,
    setting the switch on a non-``qa`` job meant the request evaporated with
    no warning and the run still exited 0.
    """

    def test_requested_on_non_qa_invocation_is_a_failure(self):
        self.assertFalse(
            run_baseline_qa.require_baseline_applies(True, qa_ran=False),
            "Requesting enforcement where it cannot apply must fail, not no-op.",
        )

    def test_requested_on_qa_invocation_is_fine(self):
        self.assertTrue(run_baseline_qa.require_baseline_applies(True, qa_ran=True))

    def test_not_requested_is_always_fine(self):
        """Off-by-default behavior is untouched on both kinds of invocation."""
        self.assertTrue(run_baseline_qa.require_baseline_applies(False, qa_ran=False))
        self.assertTrue(run_baseline_qa.require_baseline_applies(False, qa_ran=True))

    def test_inert_message_names_the_switch_and_the_remedy(self):
        message = run_baseline_qa.REQUIRE_QA_BASELINE_INERT_MESSAGE
        self.assertIn("--require-qa-baseline", message)
        self.assertIn(run_baseline_qa.REQUIRE_QA_BASELINE_ENV, message)
        self.assertIn("qa", message)


class ResolveQaRanTest(unittest.TestCase):
    """qa_ran is the single fact the whole enforcement surface hangs on."""

    def test_explicit_qa_category_runs_qa(self):
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category="qa", categories=None, quick=False))

    def test_other_single_category_does_not_run_qa(self):
        for category in ("ply", "pipeline", "sorting", "runtime", "module"):
            with self.subTest(category=category):
                self.assertFalse(
                    run_baseline_qa.resolve_qa_ran(category=category, categories=None, quick=False)
                )

    def test_categories_list_runs_qa_only_when_it_contains_qa(self):
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category=None, categories={"qa", "ply"}, quick=False))
        self.assertFalse(
            run_baseline_qa.resolve_qa_ran(
                category=None, categories={"ply", "pipeline", "runtime", "module"}, quick=False
            )
        )

    def test_all_alias_normalizes_to_none_and_runs_qa(self):
        """'all' normalizes to None inside the set, which means every category."""
        self.assertIsNone(run_baseline_qa.normalize_test_category("all"))
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category=None, categories={None}, quick=False))

    def test_quick_skips_qa_but_full_default_run_includes_it(self):
        self.assertFalse(run_baseline_qa.resolve_qa_ran(category=None, categories=None, quick=True))
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category=None, categories=None, quick=False))


def _extract_baseline_qa_invocations(workflows_dir: Path | None = None):
    """Every run_baseline_qa.py invocation in .github/workflows, with the
    category selector each one passes.

    ``workflows_dir`` defaults to the repo's real workflow directory and is
    overridable so the parser's own behaviour can be tested against fixtures
    instead of only against whatever the repo happens to contain today.

    Fail closed: an invocation whose selector cannot be parsed is reported as
    an unparsed entry rather than being quietly dropped, because a dropped
    invocation is exactly how a claim about which lanes run what would rot
    into a false statement.

    Comment lines are excluded. They are not invocations - nothing a comment
    says is ever executed - and counting them made the guard fail on prose
    that merely NAMES the script, which is the one thing a step explaining
    itself is most likely to do. Excluding them narrows what the guard looks
    at without letting any executable invocation through: a selector hidden
    behind a `#` does not run either.
    """
    workflows_dir = workflows_dir or (ROOT / ".github" / "workflows")
    selector_re = re.compile(r"--categor(?:y|ies)\"?,?\s+\"?([A-Za-z][A-Za-z,]*)")
    invocations = []
    for workflow in sorted(workflows_dir.glob("*.yml")):
        lines = workflow.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            if "run_baseline_qa.py" not in line:
                continue
            if line.lstrip().startswith("#"):
                continue
            location = f"{workflow.name}:{index + 1}"
            match = selector_re.search(line)
            if match:
                invocations.append((location, frozenset(match.group(1).split(","))))
                continue
            if "@args" in line:
                # PowerShell splatting: the selector lives in the preceding
                # `$args = @( ... )` block.
                block = []
                for previous in reversed(lines[:index]):
                    block.append(previous)
                    if "$args" in previous and "@(" in previous:
                        break
                else:
                    invocations.append((location, None))
                    continue
                block_match = selector_re.search("\n".join(reversed(block)))
                invocations.append(
                    (location, frozenset(block_match.group(1).split(",")) if block_match else None)
                )
                continue
            invocations.append((location, None))
    return invocations


class WorkflowCategorySelectionTest(unittest.TestCase):
    """Pins the corrected premise to ground truth.

    The PR body and the docstrings above state that no CI workflow selects
    the ``qa`` category, and therefore that the QA-skip path is taken by no
    CI run today. That is a claim about repository state, so it is checked
    here rather than merely asserted in prose. If someone adds a QA lane,
    this test fails and the prose must be corrected with it.
    """

    # Keyed by workflow file + selector, not by line number: a line-number pin
    # would break on any unrelated edit above the invocation and train people
    # to update it mechanically, which is how a pin stops meaning anything.
    EXPECTED = sorted(
        [
            ("baseline_qa.yml", frozenset({"sorting"})),
            ("baseline_qa.yml", frozenset({"qa"})),
            ("baseline_qa.yml", frozenset({"ply", "pipeline", "runtime", "module"})),
            ("gaussian_production_gates.yml", frozenset({"pipeline"})),
        ],
        key=lambda entry: (entry[0], sorted(entry[1])),
    )

    def test_every_invocation_has_a_parsed_selector(self):
        unparsed = [location for location, selector in _extract_baseline_qa_invocations() if selector is None]
        self.assertEqual(
            unparsed,
            [],
            "A run_baseline_qa.py invocation has no parseable category selector. "
            "With no selector the runner defaults to the full suite, which DOES "
            "include 'qa' - so this must be classified, not ignored.",
        )

    def test_invocation_inventory_is_pinned(self):
        """Count/shape pin: adding or changing a lane must be deliberate."""
        actual = sorted(
            [
                (location.split(":")[0], selector)
                for location, selector in _extract_baseline_qa_invocations()
            ],
            key=lambda entry: (entry[0], sorted(entry[1]) if entry[1] else []),
        )
        self.assertEqual(actual, self.EXPECTED)

    def test_the_qa_category_is_selected_by_exactly_one_lane(self):
        """Inverted by #522. This assertion used to read "no workflow selects
        the qa category" and was TRUE: the SSIM scene suite ran nowhere, so the
        renderer had no rendered-output gate at all. Activating the lane is the
        fix, so the guard now polices the opposite fact - that the lane exists
        and has not silently been dropped again.

        Pinned to exactly one lane on purpose. The suite renders and reads back
        viewports, so it only produces signal on a GPU runner; a second
        invocation would almost certainly be a CPU lane that can only skip.
        """
        selecting = [
            location
            for location, selector in _extract_baseline_qa_invocations()
            if selector is not None and "qa" in selector
        ]
        self.assertEqual(
            len(selecting),
            1,
            "Exactly one lane must select the 'qa' category (found: "
            f"{selecting}). If this dropped to zero the visual gate was removed; "
            "if it grew, a second lane is running the suite - confirm it has a "
            "real display, because --headless cannot capture anything.",
        )
        self.assertTrue(
            selecting[0].startswith("baseline_qa.yml"),
            f"The qa lane moved out of baseline_qa.yml (now {selecting[0]}); "
            "update this pin and confirm the new home is fork-guarded and GPU-backed.",
        )

    def test_comment_lines_are_not_counted_as_invocations(self):
        """Prose that names the script is not a lane.

        The extractor scans every line mentioning run_baseline_qa.py, so a
        step whose comment explains what the script does was reported as an
        unparsed invocation and failed the fail-closed check - punishing the
        documentation this repo asks for. Narrowing to non-comment lines must
        not, however, let a real invocation escape.
        """
        with tempfile.TemporaryDirectory() as raw_td:
            fake = Path(raw_td) / "workflows"
            fake.mkdir(parents=True)
            (fake / "sample.yml").write_text(
                "jobs:\n"
                "  a:\n"
                "    steps:\n"
                "    - name: Explain\n"
                "      # run_baseline_qa.py launders a headless run into a skip\n"
                "      run: |\n"
                "        python tests/ci/run_baseline_qa.py --categories qa\n",
                encoding="utf-8",
            )
            found = _extract_baseline_qa_invocations(fake)
        self.assertEqual(len(found), 1, f"Expected exactly one real invocation, got {found}")
        self.assertEqual(found[0][1], frozenset({"qa"}))

    def test_a_commented_out_selector_does_not_satisfy_the_pin(self):
        """The narrowing must not become a hiding place: a lane that is
        commented out is not a lane, and the guard must report zero, not one."""
        with tempfile.TemporaryDirectory() as raw_td:
            fake = Path(raw_td) / "workflows"
            fake.mkdir(parents=True)
            (fake / "sample.yml").write_text(
                "      run: |\n"
                "        # python tests/ci/run_baseline_qa.py --categories qa\n",
                encoding="utf-8",
            )
            self.assertEqual(_extract_baseline_qa_invocations(fake), [])

    def test_the_qa_lane_demands_capture_and_a_baseline(self):
        """The lane selecting 'qa' is worthless without both switches.

        Without --qa-require-capture the suite runs --headless, captures
        nothing, and is laundered into a skip. Without --require-qa-baseline a
        missing baseline downgrades to a warning. Either omission turns a
        blocking gate back into theatre, and neither is visible in the category
        selector this class otherwise pins.
        """
        workflow = ROOT / ".github" / "workflows" / "baseline_qa.yml"
        text = workflow.read_text(encoding="utf-8")
        qa_step = re.search(
            r'"--categories",\s*"qa"(.*?)python tests/ci/run_baseline_qa\.py',
            text,
            re.DOTALL,
        )
        self.assertIsNotNone(qa_step, "Could not locate the qa lane's argv block in baseline_qa.yml.")
        block = qa_step.group(1)
        self.assertIn("--qa-require-capture", block, "The qa lane must require real capture.")
        self.assertIn("--require-qa-baseline", block, "The qa lane must require a committed baseline.")
        self.assertNotIn(
            "--update-qa-baseline",
            block,
            "The qa lane must never rewrite its own baseline: a job that updates "
            "its expectations absorbs regressions instead of detecting them.",
        )


class QaRequireCaptureTest(unittest.TestCase):
    """The third laundering path (#522): a lane that promised a GPU, skipped.

    --require-qa-baseline polices whether a baseline FILE exists.
    --qa-require-capture polices whether the suite actually RENDERED. Before
    #522 only the first existed, and the skip path's own docstring argued a
    skip was always "legitimately not applicable" because no switch can make
    a RenderingDevice appear. That reasoning held only while no lane promised
    one. baseline_qa.yml's qa-visual lane now runs on the self-hosted GPU
    runner, so a skip there means the GPU lane failed to render - and with a
    baseline committed, the old code would have returned True and reported a
    blocking lane green having compared nothing.
    """

    def _invoke(self, *, require_capture: bool, require_baseline: bool, baseline_present: bool):
        with tempfile.TemporaryDirectory() as raw_td:
            td = Path(raw_td)
            baseline_path = td / "baselines" / "qa_results.json"
            if baseline_present:
                baseline_path.parent.mkdir(parents=True, exist_ok=True)
                baseline_path.write_text(json.dumps({"results": []}), encoding="utf-8")

            runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
            qa_ok = runner._record_qa_baseline_skipped(
                qa_results_path=td / "qa_results.json",
                baseline_path=baseline_path,
                report_path=None,
                summary_path=None,
                reason="QA Scene Suite requires local RenderingDevice when run with current headless configuration.",
                require_baseline=require_baseline,
                require_capture=require_capture,
            )
            return qa_ok, runner.test_results["summary"]["qa_baseline"]

    def test_capture_required_skip_fails_even_with_a_baseline_present(self):
        """The precise hole: baseline present + suite skipped used to pass."""
        qa_ok, comparison = self._invoke(require_capture=True, require_baseline=True, baseline_present=True)
        self.assertFalse(qa_ok, "A GPU lane that captured nothing must fail, not skip.")
        self.assertEqual(comparison["status"], "failed")
        self.assertTrue(comparison["require_capture"])

    def test_capture_required_skip_fails_with_no_baseline_too(self):
        qa_ok, comparison = self._invoke(require_capture=True, require_baseline=False, baseline_present=False)
        self.assertFalse(qa_ok)
        self.assertEqual(comparison["status"], "failed")

    def test_capture_not_required_preserves_the_legitimate_skip(self):
        """Off-by-default behavior is untouched: local headless runs still skip."""
        qa_ok, comparison = self._invoke(require_capture=False, require_baseline=True, baseline_present=True)
        self.assertTrue(qa_ok)
        self.assertEqual(comparison["status"], "skipped")
        self.assertFalse(comparison["require_capture"])

    def test_qa_suite_argv_uses_the_real_display_only_when_capture_required(self):
        """--headless has no RenderingDevice, so the suite cannot capture under
        it. The flag must actually change how the suite is launched, not just
        how its result is judged."""
        headless_runner = run_baseline_qa.BaselineQARunner(godot_binary="godot", qa_require_capture=False)
        capture_runner = run_baseline_qa.BaselineQARunner(godot_binary="godot", qa_require_capture=True)

        def qa_command(runner):
            for test in runner._build_test_table():
                if test["name"] == "QA Scene Suite":
                    return test["command"]
            self.fail("QA Scene Suite entry not found in the test table.")

        headless_cmd = qa_command(headless_runner)
        capture_cmd = qa_command(capture_runner)

        self.assertIn("--headless", headless_cmd)
        self.assertNotIn("--headless", capture_cmd)
        for flag in run_baseline_qa.GPU_DISPLAY_ARGS:
            self.assertIn(flag, capture_cmd)
        self.assertNotIn("--display-driver", headless_cmd)

    def test_headless_skip_laundering_cannot_fire_on_a_capture_run(self):
        """_is_expected_headless_qa_skip() converts a non-zero exit into a
        success. It keys off '--headless' being in the argv, so a capture run
        must be structurally out of its reach - otherwise a GPU lane whose
        RenderingDevice died would be laundered into a pass."""
        markers = (
            "Failed to create primary local RenderingDevice\n"
            "Failed to create shared local RenderingDevice\n"
        )
        capture_cmd = [
            "godot",
            *run_baseline_qa.GPU_DISPLAY_ARGS,
            "--script",
            "res://scripts/qa_test_runner.gd",
        ]
        self.assertFalse(
            run_baseline_qa.BaselineQARunner._is_expected_headless_qa_skip(
                "QA Scene Suite", capture_cmd, markers, ""
            )
        )
        headless_cmd = ["godot", "--headless", "--script", "res://scripts/qa_test_runner.gd"]
        self.assertTrue(
            run_baseline_qa.BaselineQARunner._is_expected_headless_qa_skip(
                "QA Scene Suite", headless_cmd, markers, ""
            )
        )


class BaselineCandidateValidationTest(unittest.TestCase):
    """Nothing that did not render may become the golden baseline (#522).

    This is the highest-stakes guard in the lane. The comparator's SSIM rule
    is `current >= baseline - MINIMUM_SSIM_DROP`, so a baseline that recorded
    ssim 0.0 - the value calculate_ssim returned for a NULL capture before
    #522 made it NaN - can never fail again. Freezing one silently disarms a
    blocking gate forever, and nothing downstream would ever report it.
    """

    @staticmethod
    def _scene(name="res://scenes/qa/qa_visual_diff.tscn", **overrides):
        entry = {
            "scene": name,
            "passed": True,
            "skipped": False,
            "message": "SSIM: 0.9900 (threshold: 0.95)",
            "metrics": {"ssim": 0.99, "ssim_threshold": 0.95},
        }
        entry.update(overrides)
        return entry

    def test_a_healthy_run_is_accepted(self):
        self.assertEqual(run_baseline_qa.validate_baseline_candidate([self._scene()]), [])

    def test_empty_results_are_rejected(self):
        self.assertTrue(run_baseline_qa.validate_baseline_candidate([]))

    def test_zero_ssim_is_rejected(self):
        """The exact historical hazard: a null capture scored 0.0."""
        reasons = run_baseline_qa.validate_baseline_candidate(
            [self._scene(metrics={"ssim_min": 0.0, "ssim_threshold": 0.95})]
        )
        self.assertTrue(any("did not render" in reason for reason in reasons), reasons)

    def test_nan_ssim_is_rejected(self):
        reasons = run_baseline_qa.validate_baseline_candidate(
            [self._scene(metrics={"ssim": float("nan"), "ssim_threshold": 0.95})]
        )
        self.assertTrue(any("NaN" in reason for reason in reasons), reasons)

    def test_failing_scene_is_rejected(self):
        reasons = run_baseline_qa.validate_baseline_candidate([self._scene(passed=False)])
        self.assertTrue(any("FAILED" in reason for reason in reasons), reasons)

    def test_skipped_scene_is_rejected(self):
        reasons = run_baseline_qa.validate_baseline_candidate([self._scene(skipped=True)])
        self.assertTrue(any("SKIPPED" in reason for reason in reasons), reasons)

    def test_self_skip_marker_in_message_is_rejected(self):
        """Belt and braces: a scene predating the `skipped` field still says so
        in its message, and the old runner recorded that as passed=true."""
        reasons = run_baseline_qa.validate_baseline_candidate(
            [self._scene(message="[QA_SKIP] Requires non-headless viewport.")]
        )
        self.assertTrue(any("self-skipped" in reason for reason in reasons), reasons)

    def test_metricless_scene_is_rejected(self):
        reasons = run_baseline_qa.validate_baseline_candidate([self._scene(metrics={})])
        self.assertTrue(any("no metrics" in reason for reason in reasons), reasons)

    def test_validator_is_idempotent_over_its_own_sanitized_output(self):
        """A scene whose every metric is machine-dependent (qa_performance_budget
        reports only FPS/frame-time) is left with an empty metrics dict by
        stripping. Without already_sanitized the validator would reject the
        very baseline it had just approved."""
        run = {
            "results": [
                {
                    "scene": "res://scenes/qa/qa_performance_budget.tscn",
                    "passed": True,
                    "skipped": False,
                    "message": "FPS avg=1095.9",
                    "metrics": {"avg_fps": 1095.9, "p99_frame_time_ms": 1.4},
                }
            ]
        }
        self.assertEqual(run_baseline_qa.validate_baseline_candidate(run["results"]), [])
        sanitized, dropped = run_baseline_qa.strip_non_deterministic_metrics(run)
        self.assertEqual(sanitized["results"][0]["metrics"], {})
        self.assertEqual(len(dropped), 2)
        self.assertEqual(
            run_baseline_qa.validate_baseline_candidate(sanitized["results"], already_sanitized=True),
            [],
            "Stripping must not turn an approved run into an invalid baseline.",
        )

    def test_metricless_scene_is_still_rejected_on_a_raw_run(self):
        """already_sanitized relaxes exactly one rule and must not leak into
        raw-run validation, where no metrics means the scene captured nothing."""
        self.assertTrue(
            run_baseline_qa.validate_baseline_candidate([self._scene(metrics={})]),
            "A raw run with a metric-less scene must still be rejected.",
        )

    def test_committed_baseline_on_disk_is_valid(self):
        """The file the blocking lane actually compares against must satisfy
        the guard. A hand-edited or stale baseline is exactly the silent
        disarming this whole mechanism exists to prevent."""
        baseline = ROOT / "tests" / "ci" / "baselines" / "qa_results.json"
        self.assertTrue(baseline.exists(), f"Committed QA baseline missing at {baseline}")
        payload = json.loads(baseline.read_text(encoding="utf-8"))
        reasons = run_baseline_qa.validate_baseline_candidate(
            payload.get("results", []), already_sanitized=True
        )
        self.assertEqual(reasons, [], f"Committed QA baseline is not a valid baseline: {reasons}")

    def test_committed_baseline_holds_no_machine_dependent_metrics(self):
        """Re-stripping the committed baseline must be a no-op; if it drops
        anything, a machine-dependent metric got in and the blocking lane
        would fail on runner contention."""
        baseline = ROOT / "tests" / "ci" / "baselines" / "qa_results.json"
        payload = json.loads(baseline.read_text(encoding="utf-8"))
        _sanitized, dropped = run_baseline_qa.strip_non_deterministic_metrics(payload)
        self.assertEqual(dropped, [], f"Committed baseline still contains machine-dependent metrics: {dropped}")

    def test_ssim_threshold_alone_is_not_treated_as_a_measurement(self):
        """`ssim_threshold` is configuration, not a captured value; a scene
        reporting only the threshold captured nothing, and the 0.95 must not
        be mistaken for a healthy score."""
        reasons = run_baseline_qa.validate_baseline_candidate(
            [self._scene(metrics={"ssim_threshold": 0.95})]
        )
        self.assertEqual(reasons, [], "A threshold is allowed to be present on its own.")


class NonDeterministicMetricStrippingTest(unittest.TestCase):
    """A blocking gate must measure the renderer, not the runner (#522).

    qa_performance_budget reported avg_fps ~1100-1260 across runs on an idle
    RTX 3090. Pinned into a baseline compared at MINIMUM_FPS_RATIO=0.85, a
    contended CI runner would fail the gate for being busy - the same trap
    #630/#624 hit with the frame-time gate. The scenes keep asserting their
    own absolute budgets internally (min_fps=20), which is where a perf
    assertion belongs.
    """

    def test_fps_and_frame_time_metrics_are_classified_non_deterministic(self):
        for name in ("avg_fps", "min_fps", "p1_fps", "p99_frame_time_ms", "max_frame_time_ms",
                     "near_gpu_sorter_last_sort_ms", "far_gpu_sorter_last_sort_ms"):
            self.assertTrue(run_baseline_qa.is_non_deterministic_baseline_metric(name), name)

    def test_ssim_and_pixel_metrics_are_kept(self):
        for name in ("ssim", "ssim_min", "ssim_avg", "ssim_threshold", "red_minus_blue",
                     "visible_splats", "sorted_splats", "red_dominance_margin"):
            self.assertFalse(run_baseline_qa.is_non_deterministic_baseline_metric(name), name)

    def test_serialized_colors_are_classified_by_value_not_name(self):
        """A rendered pixel value is a measurement wearing a string's clothes.

        Found by CI, not by reasoning: the lane's first self-hosted run failed
        on qa_sort_depth_order.center_color, baseline '(1.0, 0.4976, 0.5603,
        1.0)' vs current '(1.0, 0.498, 0.5956, 1.0)'. Both runs were correct —
        the baseline came from an optimized editor build, CI builds -O0, and
        the rasterized blue channel differs ~6%. Name-based classification
        could not have caught this: nothing about 'center_color' says
        'float vector'.
        """
        colour = "(1.0, 0.4976, 0.5603, 1.0)"
        self.assertTrue(run_baseline_qa.is_serialized_numeric_tuple(colour))
        self.assertTrue(run_baseline_qa.is_non_deterministic_baseline_metric("center_color", colour))
        self.assertFalse(
            run_baseline_qa.is_non_deterministic_baseline_metric("center_color"),
            "Without the value there is nothing in the NAME to classify on - "
            "which is exactly why both call sites must pass the value.",
        )

    def test_path_identity_strings_are_not_mistaken_for_measurements(self):
        """The value-shape rule must not swallow the enum-like strings that are
        the whole point of the exact-match comparison."""
        for value in (
            "INSTANCE.RASTER.COMPUTE",
            "ResidentInstanceAtlas",
            "success",
            "compute",
            "COMMON.SKIP.RESIDENT_NOT_FEASIBLE.RESIDENT_NO_INSTANCES",
        ):
            self.assertFalse(run_baseline_qa.is_serialized_numeric_tuple(value), value)
            self.assertFalse(run_baseline_qa.is_non_deterministic_baseline_metric("route_uid", value), value)

    def test_tuple_detection_is_not_fooled_by_near_misses(self):
        for value in ("(a, b)", "(1.0", "1.0, 2.0", "(1.0,)", "", "()", "(v1.0, v2.0)"):
            self.assertFalse(run_baseline_qa.is_serialized_numeric_tuple(value), value)
        for value in ("(1, 2)", "(-1.5, 0.25, 3)", "( 1.0 , 2.0 )"):
            self.assertTrue(run_baseline_qa.is_serialized_numeric_tuple(value), value)

    def test_committed_baseline_pins_no_rendered_pixel_values(self):
        """The regression that produced this rule must not be able to return:
        no metric in the committed baseline may be a serialized numeric tuple."""
        baseline = ROOT / "tests" / "ci" / "baselines" / "qa_results.json"
        payload = json.loads(baseline.read_text(encoding="utf-8"))
        offenders = [
            f"{entry.get('scene')}.{name}"
            for entry in payload.get("results", [])
            for name, value in (entry.get("metrics") or {}).items()
            if run_baseline_qa.is_serialized_numeric_tuple(value)
        ]
        self.assertEqual(offenders, [], f"Baseline pins build-dependent pixel values: {offenders}")

    def test_prefixed_variants_are_caught_by_the_predicate(self):
        """qa_sort_multi_instance emits near_*/far_* copies of every metric, so
        a hardcoded name list would miss them. This is why the classifier is a
        predicate over markers rather than a frozen set."""
        self.assertTrue(run_baseline_qa.is_non_deterministic_baseline_metric("near_gpu_sorter_last_sort_ms"))
        self.assertTrue(run_baseline_qa.is_non_deterministic_baseline_metric("far_avg_fps"))

    def test_stripping_removes_only_machine_dependent_metrics_and_reports_them(self):
        payload = {
            "summary": {"total_tests": 1},
            "results": [
                {
                    "scene": "res://scenes/qa/qa_performance_budget.tscn",
                    "passed": True,
                    "metrics": {"avg_fps": 1095.8, "p99_frame_time_ms": 1.4, "ssim": 0.99},
                }
            ],
        }
        sanitized, dropped = run_baseline_qa.strip_non_deterministic_metrics(payload)
        metrics = sanitized["results"][0]["metrics"]
        self.assertEqual(metrics, {"ssim": 0.99})
        self.assertEqual(
            dropped,
            [
                "res://scenes/qa/qa_performance_budget.tscn.avg_fps",
                "res://scenes/qa/qa_performance_budget.tscn.p99_frame_time_ms",
            ],
        )

    def test_stripping_does_not_mutate_the_caller_payload(self):
        payload = {"results": [{"scene": "s", "metrics": {"avg_fps": 100.0, "ssim": 0.9}}]}
        run_baseline_qa.strip_non_deterministic_metrics(payload)
        self.assertIn("avg_fps", payload["results"][0]["metrics"])


class PathIdentityComparisonTest(unittest.TestCase):
    """Path-identity metrics are compared for exact equality (#522).

    route_uid / data_source / raster_path / stage_*_status name WHICH CODE
    PATH ran. Measured identical across four independent runs, so a tolerance
    is the wrong tool: a route silently degrading to a SKIP is precisely what
    let qa_visual_diff score a perfect 1.0 while rendering nothing (#785), and
    no numeric threshold can see that.
    """

    def _compare(self, baseline_metrics, current_metrics):
        with tempfile.TemporaryDirectory() as raw_td:
            td = Path(raw_td)
            scene = "res://scenes/qa/qa_sort_depth_order.tscn"
            payload = lambda metrics: {  # noqa: E731
                "summary": {"total_tests": 1},
                "results": [{"scene": scene, "passed": True, "skipped": False, "message": "", "metrics": metrics}],
            }
            qa_results = td / "qa_results.json"
            baseline = td / "baselines" / "qa_results.json"
            baseline.parent.mkdir(parents=True, exist_ok=True)
            qa_results.write_text(json.dumps(payload(current_metrics)), encoding="utf-8")
            baseline.write_text(json.dumps(payload(baseline_metrics)), encoding="utf-8")

            runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
            ok = runner.compare_qa_baseline(
                qa_results_path=qa_results,
                baseline_path=baseline,
                update_baseline=False,
                require_baseline=True,
                report_path=None,
                summary_path=None,
            )
            return ok, runner.test_results["summary"]["qa_baseline"]

    def test_identical_path_identity_passes_and_is_counted(self):
        metrics = {"route_uid": "INSTANCE.RASTER.COMPUTE", "stage_sort_status": "success"}
        ok, comparison = self._compare(metrics, dict(metrics))
        self.assertTrue(ok)
        self.assertEqual(comparison["regressions"], [])
        self.assertEqual(comparison["metrics_checked"], 2, "Exact-match metrics must be counted as checked.")

    def test_a_changed_route_is_a_regression(self):
        ok, comparison = self._compare(
            {"route_uid": "INSTANCE.RASTER.COMPUTE"},
            {"route_uid": "COMMON.SKIP.RESIDENT_NOT_FEASIBLE.RESIDENT_NO_INSTANCES"},
        )
        self.assertFalse(ok, "A route degrading to a SKIP must fail the lane.")
        self.assertEqual(len(comparison["regressions"]), 1)
        self.assertEqual(comparison["regressions"][0]["metric"], "route_uid")

    def test_a_missing_metric_is_a_regression_not_a_silent_pass(self):
        """If a scene stops emitting a pinned metric entirely, current is None.
        Skipping the comparison there would let a scene quietly stop reporting
        the very fact the baseline pins."""
        ok, comparison = self._compare({"stage_raster_status": "success"}, {})
        self.assertFalse(ok)
        self.assertEqual(comparison["regressions"][0]["current"], None)

    def test_boolean_flags_compare_exactly(self):
        ok, _ = self._compare({"streaming_data_source_seen": True}, {"streaming_data_source_seen": False})
        self.assertFalse(ok)

    def test_numeric_rules_are_unaffected(self):
        """bool is a subclass of int in Python, so the exact-match branch must
        come first without capturing genuine numeric metrics."""
        ok, comparison = self._compare({"ssim_min": 0.99}, {"ssim_min": 0.985})
        self.assertTrue(ok, "0.985 is within MINIMUM_SSIM_DROP of 0.99.")
        ok, _ = self._compare({"ssim_min": 0.99}, {"ssim_min": 0.50})
        self.assertFalse(ok, "A real SSIM collapse must still be caught by the tolerance rule.")


class DisappearingAndUnruledMetricTest(PathIdentityComparisonTest):
    """Review findings on the comparison's blind spots (#522 review round 1).

    Three separate ways a pinned metric could stop being checked without
    anything saying so. Each is the same underlying shape as the defect this
    whole lane exists to remove: coverage that silently evaporates while the
    gate keeps reporting green.
    """

    def test_a_numeric_metric_that_disappears_is_a_regression(self):
        """Raised in review: a scene that still exits 0 but stops emitting (or
        renames) `ssim_min` used to be skipped, so a refactor could delete the
        SSIM contract and the comparison would report passed having checked
        nothing."""
        ok, comparison = self._compare({"ssim_min": 0.99}, {})
        self.assertFalse(ok, "A vanished numeric metric must fail, not be skipped.")
        self.assertEqual(len(comparison["regressions"]), 1)
        self.assertIn("missing", comparison["regressions"][0]["rule"])

    def test_a_metric_that_changes_type_is_a_regression(self):
        ok, _ = self._compare({"ssim_min": 0.99}, {"ssim_min": "n/a"})
        self.assertFalse(ok)

    def test_acceptance_thresholds_are_pinned_exactly(self):
        """Raised in review: lowering a scene's own acceptance margin makes it
        assert less while still passing. A threshold is a contract value, so no
        tolerance applies to it."""
        ok, _ = self._compare({"red_dominance_margin": 0.15}, {"red_dominance_margin": 0.15})
        self.assertTrue(ok)
        ok, comparison = self._compare({"red_dominance_margin": 0.15}, {"red_dominance_margin": 0.05})
        self.assertFalse(ok, "A lowered acceptance margin must fail the gate.")
        self.assertIn("acceptance contract", comparison["regressions"][0]["rule"])
        ok, _ = self._compare({"ssim_threshold": 0.95}, {"ssim_threshold": 0.5})
        self.assertFalse(ok)

    def test_measured_dominance_has_a_tolerance_but_still_catches_collapse(self):
        """Raised in review: the sort-order signal itself was compared by
        nothing. It is a rendered value, so it moves with the build - hence a
        ratio - but a collapse must still fail."""
        ok, _ = self._compare({"red_minus_blue": 0.440}, {"red_minus_blue": 0.404})
        self.assertTrue(ok, "The measured local-vs-CI build spread must not fail the gate.")
        ok, _ = self._compare({"red_minus_blue": 0.440}, {"red_minus_blue": 0.10})
        self.assertFalse(ok, "A dominance collapse must fail the gate.")

    def test_the_dominance_floor_is_stricter_than_the_scene_s_own_gate(self):
        """The rule only adds value if it fires before the scene's own 0.15
        acceptance gate would. Otherwise it is decoration."""
        baseline = 0.404
        derived_floor = baseline * run_baseline_qa.MINIMUM_DOMINANCE_RATIO
        self.assertGreater(
            derived_floor, 0.15,
            f"Derived floor {derived_floor:.3f} must exceed the scenes' own 0.15 gate.",
        )

    def test_list_metrics_compare_by_equality(self):
        """`warnings` and `sorted_indices_preview` are deterministic
        collections; treating them as uncomparable made identical values fail."""
        ok, _ = self._compare({"warnings": ["Non-uniform scale detected."]},
                              {"warnings": ["Non-uniform scale detected."]})
        self.assertTrue(ok, "Identical lists must compare equal, not fail as uncomparable.")
        ok, _ = self._compare({"warnings": ["Non-uniform scale detected."]}, {"warnings": []})
        self.assertFalse(ok, "A vanished warning must fail.")

    def test_unruled_metrics_are_recorded_not_silently_dropped(self):
        """A numeric metric nobody knows how to compare is a coverage gap. An
        unrecorded gap reads as 'checked and fine' to anyone looking at
        metrics_checked."""
        ok, comparison = self._compare({"sort_cache_hits": 3}, {"sort_cache_hits": 9})
        self.assertTrue(ok, "An unruled metric must not fail the gate...")
        self.assertIn(
            "res://scenes/qa/qa_sort_depth_order.tscn.sort_cache_hits",
            comparison.get("unchecked_metrics", []),
            "...but it must be recorded as unchecked.",
        )

    def test_the_committed_baseline_pins_the_tie_break_winner(self):
        """Review finding: qa_sort_tie_breaker asserts only frame-to-frame
        stability, so a consistently REVERSED tie-break still scores SSIM 1.0.
        The recorded winner is what makes a reversal detectable, so the
        baseline must actually carry it."""
        baseline = ROOT / "tests" / "ci" / "baselines" / "qa_results.json"
        payload = json.loads(baseline.read_text(encoding="utf-8"))
        scene = next(
            (e for e in payload["results"] if "qa_sort_tie_breaker" in e["scene"]), None
        )
        self.assertIsNotNone(scene, "qa_sort_tie_breaker must be in the committed baseline.")
        self.assertIn(
            "tie_break_winner", scene["metrics"],
            "Without a pinned winner a reversed tie-break is invisible to this lane.",
        )
        self.assertIsInstance(
            scene["metrics"]["tie_break_winner"], str,
            "The winner must be a string so it is compared for exact equality.",
        )


class MetricValueFormattingTest(unittest.TestCase):
    """The red path must be able to report red (#522).

    The regression reporter forced every field through float(), so the first
    string regression it ever encountered raised ValueError and killed the run
    instead of printing the failure. Caught by mutation-proving the exact-match
    rule; without the mutation the crash would have shipped in a path that only
    executes when something is already broken.
    """

    def test_strings_and_none_render_without_raising(self):
        self.assertEqual(run_baseline_qa._format_metric_value("INSTANCE.RASTER.COMPUTE"), "'INSTANCE.RASTER.COMPUTE'")
        self.assertEqual(run_baseline_qa._format_metric_value(None), "None")
        self.assertEqual(run_baseline_qa._format_metric_value(True), "True")

    def test_numbers_keep_their_fixed_precision(self):
        self.assertEqual(run_baseline_qa._format_metric_value(0.985), "0.985000")
        self.assertEqual(run_baseline_qa._format_metric_value(3), "3.000000")

    def test_markdown_summary_renders_a_string_regression(self):
        comparison = {
            "status": "failed",
            "mode": "compare",
            "scenes_checked": 1,
            "metrics_checked": 1,
            "missing_scenes": [],
            "new_scenes": [],
            "regressions": [
                {
                    "scene": "res://scenes/qa/qa_sort_depth_order.tscn",
                    "metric": "route_uid",
                    "baseline": "INSTANCE.RASTER.COMPUTE",
                    "current": "COMMON.SKIP.RESIDENT_NOT_FEASIBLE.RESIDENT_NO_INSTANCES",
                    "threshold": "INSTANCE.RASTER.COMPUTE",
                    "rule": "current == baseline (path identity)",
                }
            ],
            "notes": [],
        }
        runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
        markdown = runner._build_baseline_summary_markdown(comparison)
        self.assertIn("route_uid", markdown)
        self.assertIn("COMMON.SKIP", markdown)


if __name__ == "__main__":
    unittest.main()
