#!/usr/bin/env python3
"""Every GPU job waits for a free runner, and says so afterwards (#875).

`tests/ci/runner_gpu_contention.py` is the mechanism. This is the guard that
makes it reach the jobs that need it, and that keeps its verdict logic honest.

Why a guard
-----------
The fix has three halves and each is useless alone:

* the **preflight**, which waits for the GPU to be free and fails loudly if it
  never is. Without it a job starts on a contended machine and produces timing
  numbers nobody can attribute;
* the **postflight**, which reports what happened *between* the two. Without it
  a run that was clean at start and contended by the end -- #881, exactly --
  reads as clean;
* the postflight's ``always()`` condition. A postflight that only runs on
  success cannot speak about the failure whose attribution it exists to settle,
  which is the only case anyone reads it in.

A job missing any one of the three is worse than a job with none, because it
looks covered. So all three are checked, in a job set this **derives** from the
workflows by label routing -- reusing `test_preflight_runner_gpu_environment`'s
derivation wholesale rather than restating it, so the two guards cannot come to
disagree about which jobs are the GPU pool.

An empty derived set is a failure, not a pass.

Run directly (``python tests/ci/test_runner_gpu_contention.py``) or via
``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import sys
import time
import unittest
from pathlib import Path
from typing import Dict, List
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
README = WORKFLOW_DIR / "README.md"

sys.path.insert(0, str(Path(__file__).resolve().parent))

import runner_gpu_contention as contention  # noqa: E402
import test_preflight_runner_gpu_environment as gpu_guard  # noqa: E402

#: Relative path of the script, as it appears in a workflow `run:`.
SCRIPT = "tests/ci/runner_gpu_contention.py"
PREFLIGHT_INVOCATION = f"{SCRIPT} preflight"
POSTFLIGHT_INVOCATION = f"{SCRIPT} postflight"

README_SECTION_HEADING = "### GPU contention"


# --------------------------------------------------------------------------
# The workflow contract
# --------------------------------------------------------------------------


class GpuJobContentionContract(unittest.TestCase):
    def setUp(self) -> None:
        self.jobs = gpu_guard.gpu_jobs()

    def test_gpu_pool_is_not_empty(self) -> None:
        """A derivation that found nothing must fail, not pass over an empty set."""
        self.assertTrue(
            self.jobs,
            f"No self-hosted job in {WORKFLOW_DIR} carries the "
            f"{gpu_guard.GPU_LABEL!r} label, so every assertion below would pass "
            "vacuously. Do not read this as 'nothing to check'.",
        )

    def test_every_gpu_job_waits_for_a_free_runner(self) -> None:
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                self.assertIsNotNone(
                    gpu_guard.first_index(lines, PREFLIGHT_INVOCATION),
                    f"{workflow}: job {job!r} runs on the shared self-hosted GPU pool but "
                    f"never runs `{PREFLIGHT_INVOCATION}`. Without it the job starts "
                    "whenever the queue reaches it, contended or not, and its wall-clock "
                    "budgets measure whatever else was on the machine (#867, #881).",
                )

    def test_the_wait_precedes_the_build(self) -> None:
        """Waiting after the build has already run measures nothing worth having."""
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                wait_at = gpu_guard.first_index(lines, PREFLIGHT_INVOCATION)
                build_at = gpu_guard.first_index(lines, gpu_guard.BUILD_MARKER)
                self.assertIsNotNone(
                    build_at,
                    f"{workflow}: job {job!r} has no {gpu_guard.BUILD_MARKER!r} invocation, "
                    "so this ordering assertion has nothing to order against and would pass "
                    "for the wrong reason.",
                )
                self.assertIsNotNone(wait_at)
                self.assertLess(
                    wait_at,
                    build_at,
                    f"{workflow}: job {job!r} waits for a free GPU after it has already "
                    "started building. The build is the most contended part of the job; a "
                    "wait placed after it protects nothing.",
                )

    def test_every_gpu_job_reports_a_postflight_verdict(self) -> None:
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                self.assertIsNotNone(
                    gpu_guard.first_index(lines, POSTFLIGHT_INVOCATION),
                    f"{workflow}: job {job!r} waits for a free GPU at the start and then "
                    "never checks again. Contention that begins mid-run is exactly what "
                    "happened on PR #881, and a start-only check calls that run clean.",
                )

    def test_the_postflight_runs_even_when_the_job_failed(self) -> None:
        """`always()`, or the verdict is missing from every run that needs it."""
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                index = gpu_guard.first_index(lines, POSTFLIGHT_INVOCATION)
                self.assertIsNotNone(index)
                window = "\n".join(lines[max(0, index - 6) : index + 1])
                self.assertIn(
                    "always()",
                    window,
                    f"{workflow}: job {job!r} runs the contention postflight without an "
                    "`if: always()` condition, so it is skipped precisely when a step above "
                    "failed -- the only situation in which anyone reads it. A timing failure "
                    "would once again arrive with no attribution.",
                )

    def test_the_postflight_runs_after_the_build(self) -> None:
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                post_at = gpu_guard.first_index(lines, POSTFLIGHT_INVOCATION)
                build_at = gpu_guard.first_index(lines, gpu_guard.BUILD_MARKER)
                self.assertIsNotNone(build_at)
                self.assertGreater(
                    post_at,
                    build_at,
                    f"{workflow}: job {job!r} runs the contention postflight before its "
                    "build, so the window it reports on excludes the job's own GPU work.",
                )

    def test_readme_documents_the_contention_handling(self) -> None:
        text = README.read_text(encoding="utf-8")
        self.assertIn(
            README_SECTION_HEADING,
            text,
            f"{README} must carry a {README_SECTION_HEADING!r} section: how a job behaves "
            "when the shared runner is busy is a behaviour property of the persistent "
            "runner, and `.github/workflows/AGENTS.md` requires those to be readable there.",
        )


class ContentionPolicyIsCoherent(unittest.TestCase):
    """The constants have to be consistent with each other, or the guard misfires."""

    def test_busy_exit_code_is_not_a_test_failure_code(self) -> None:
        """The whole point is that a void run does not look like a failing test."""
        self.assertNotIn(contention.EXIT_RUNNER_BUSY, (0, 1, 2))

    def test_every_non_clean_verdict_is_void(self) -> None:
        """No verdict may be neither clean nor void -- that is the silent pass."""
        all_verdicts = {
            contention.VERDICT_CLEAN,
            contention.VERDICT_BUSY,
            contention.VERDICT_CONTENDED_MID_RUN,
            contention.VERDICT_UNMEASURED,
        }
        self.assertEqual(
            all_verdicts - {contention.VERDICT_CLEAN}, set(contention.VOID_VERDICTS)
        )

    def test_the_busy_threshold_clears_the_measured_desktop_floor(self) -> None:
        """Measured idle on this runner is 0.5-5.5 % per process; the gate is 15 %.

        A threshold at or below the desktop floor would void every run and be
        switched off within a week, which is how a gate stops existing.
        """
        self.assertGreaterEqual(contention.FOREIGN_GPU_BUSY_PERCENT, 10.0)
        self.assertLess(contention.FOREIGN_GPU_BUSY_PERCENT, 50.0)

    def test_the_wait_is_bounded_and_fits_inside_the_job_timeout(self) -> None:
        """A wait longer than the job's own timeout would fail as a timeout instead.

        The shortest GPU job timeout in the workflows is 120 minutes. A bound
        that approached it would turn "the runner was busy" back into an
        uninterpretable red, which is the defect being removed.
        """
        self.assertGreater(contention.DEFAULT_WAIT_TIMEOUT_SEC, 0)
        self.assertLess(contention.DEFAULT_WAIT_TIMEOUT_SEC, 0.25 * 120 * 60)

    def test_the_sample_gap_tolerance_exceeds_the_sampling_interval(self) -> None:
        """Otherwise every healthy run is UNMEASURED and the guard is pure noise."""
        self.assertGreater(
            contention.MAX_SAMPLE_GAP_SEC, 2 * contention.SAMPLER_INTERVAL_SEC
        )

    def test_the_clean_ratio_baseline_matches_the_measurement_it_cites(self) -> None:
        """The discriminator printed with every verdict is #630/#624's number."""
        self.assertAlmostEqual(contention.CLEAN_FRAME_P95_TO_AVG_RATIO, 1.15)
        self.assertIn("#630", contention.FRAME_RATIO_REFERENCE)
        text = "\n".join(contention.discriminator_note())
        self.assertIn("1.15", text)
        self.assertIn("#630", text)
        self.assertIn("void", text)


# --------------------------------------------------------------------------
# Attribution
# --------------------------------------------------------------------------


def _table(entries: Dict[int, Dict[str, object]]) -> Dict[int, Dict[str, object]]:
    return entries


class Attribution(unittest.TestCase):
    """Our own GPU work must not be reported as contention, and vice versa."""

    TABLE = _table(
        {
            4: {"ppid": 0, "name": "System", "path": ""},
            100: {"ppid": 4, "name": "services.exe", "path": "C:\\W\\services.exe"},
            200: {"ppid": 100, "name": "Runner.Worker.exe", "path": "C:\\r\\Runner.Worker.exe"},
            300: {"ppid": 200, "name": "python.exe", "path": "C:\\P\\python.exe"},
            400: {"ppid": 300, "name": "godot.exe", "path": "C:\\work\\repo\\bin\\godot.exe"},
            500: {"ppid": 100, "name": "Godot_v4.7-stable_win64.exe", "path": "C:\\g\\G.exe"},
        }
    )

    def test_ancestry_walk_stops_at_the_runner_worker(self) -> None:
        """It must not climb to `services.exe` and declare the machine ours."""
        self.assertEqual(contention.resolve_job_root_pid(self.TABLE, 400), 200)

    def test_without_a_runner_the_root_is_our_own_process(self) -> None:
        """A local run degrades to our own subtree, never to the whole machine."""
        table = {pid: dict(entry) for pid, entry in self.TABLE.items()}
        table[200]["name"] = "pwsh.exe"
        self.assertEqual(contention.resolve_job_root_pid(table, 400), 400)

    def test_descendants_of_the_runner_include_the_job_and_exclude_others(self) -> None:
        ours = contention.descendants_of(self.TABLE, 200)
        self.assertEqual(ours, {200, 300, 400})
        self.assertNotIn(500, ours)

    def test_a_foreign_godot_is_foreign_and_our_godot_is_not(self) -> None:
        loads = contention.classify(
            {400: 60.0, 500: 40.0},
            self.TABLE,
            ours_pids={200, 300, 400},
            roots=("c:\\work\\repo",),
        )
        by_pid = {load.pid: load for load in loads}
        self.assertTrue(by_pid[400].ours, "the job's own godot must not count as contention")
        self.assertFalse(by_pid[500].ours, "an unrelated Godot install is contention")

    def test_an_image_under_the_workspace_is_ours_even_without_ancestry(self) -> None:
        """The detached sampler has no live ancestry; the path rule covers it."""
        loads = contention.classify(
            {400: 60.0}, self.TABLE, ours_pids=set(), roots=("c:\\work\\repo",)
        )
        self.assertTrue(loads[0].ours)


# --------------------------------------------------------------------------
# The wait
# --------------------------------------------------------------------------


def _sample(busy: bool = False, error: str = None, at: float = 0.0) -> contention.Sample:
    contenders = ()
    if busy:
        contenders = (
            contention.ProcessLoad(500, 42.0, "Godot_v4.7-stable_win64.exe", "C:\\g\\G.exe", False),
        )
    return contention.Sample(at, contenders, contenders, ("0 %,",), error)


class WaitLoop(unittest.TestCase):
    def _wait(self, samples: List[contention.Sample], timeout: float = 100.0):
        clock = {"t": 0.0}

        def now() -> float:
            return clock["t"]

        def sleep(seconds: float) -> None:
            clock["t"] += max(seconds, 1.0)

        feed = iter(samples)
        last = samples[-1]
        with mock.patch.object(
            contention, "take_sample", side_effect=lambda *a, **k: next(feed, last)
        ):
            return contention.wait_for_free_gpu(
                timeout, 10.0, 15.0, 200, sleep=sleep, now=now
            )

    def test_a_free_gpu_releases_the_wait_without_sleeping_forever(self) -> None:
        free, samples, _lines = self._wait([_sample(), _sample()])
        self.assertTrue(free)
        self.assertEqual(len(samples), contention.IDLE_SAMPLES_REQUIRED)

    def test_the_wait_polls_until_the_gpu_frees_rather_than_failing_at_once(self) -> None:
        """The behaviour the shared machine requires: busy is not immediately fatal."""
        free, samples, lines = self._wait(
            [_sample(busy=True), _sample(busy=True), _sample(), _sample()]
        )
        self.assertTrue(free, "\n".join(lines))
        self.assertEqual(len(samples), 4)
        self.assertIn("busy", "\n".join(lines))

    def test_a_gpu_that_never_frees_times_out(self) -> None:
        free, samples, _lines = self._wait([_sample(busy=True)] * 40, timeout=100.0)
        self.assertFalse(free)
        self.assertTrue(samples[-1].busy)

    def test_an_unmeasurable_sample_does_not_count_as_clean(self) -> None:
        """Otherwise a broken probe is the fastest way to make the wait pass."""
        free, _samples, lines = self._wait(
            [_sample(), _sample(error="counters unavailable")] * 20, timeout=100.0
        )
        self.assertFalse(free, "\n".join(lines))
        self.assertIn("unmeasured", "\n".join(lines))

    def test_a_clean_streak_must_be_consecutive(self) -> None:
        free, _samples, _lines = self._wait(
            [_sample(), _sample(busy=True), _sample(), _sample(busy=True)] * 10,
            timeout=100.0,
        )
        self.assertFalse(free)


# --------------------------------------------------------------------------
# Start vs. end
# --------------------------------------------------------------------------


def _entry(at: float, busy: bool = False, error: str = None) -> Dict[str, object]:
    contenders = (
        [
            {
                "pid": 500,
                "gpu_percent": 42.0,
                "name": "Godot_v4.7-stable_win64.exe",
                "path": "C:\\g\\G.exe",
                "ours": False,
            }
        ]
        if busy
        else []
    )
    return {"at": at, "error": error, "contenders": contenders, "loads": contenders}


class StartVersusEnd(unittest.TestCase):
    """#881's shape: clean at start, contended while running."""

    def test_a_clean_continuous_series_passes(self) -> None:
        entries = [_entry(index * 60.0) for index in range(1, 11)]
        verdict = contention.evaluate_series(entries, 0.0, 660.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CLEAN)

    def test_contention_that_begins_after_the_preflight_is_caught(self) -> None:
        entries = [_entry(60.0), _entry(120.0), _entry(180.0, busy=True), _entry(240.0, busy=True)]
        verdict = contention.evaluate_series(entries, 0.0, 300.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CONTENDED_MID_RUN)
        self.assertEqual(len(verdict.windows), 1)
        self.assertIn(
            "Godot_v4.7-stable_win64.exe", "\n".join(verdict.reasons)
        )

    def test_contention_is_reported_with_its_window_and_the_offending_image(self) -> None:
        entries = [_entry(60.0, busy=True), _entry(120.0, busy=True), _entry(180.0)]
        verdict = contention.evaluate_series(entries, 0.0, 240.0)
        first, last, described = verdict.windows[0]
        self.assertEqual((first, last), (60.0, 120.0))
        self.assertIn("pid 500", described[0])
        self.assertIn("C:\\g\\G.exe", described[0])

    def test_a_single_busy_sample_is_not_a_contended_run(self) -> None:
        """A blip must not void a two-hour job; that is a false failure too."""
        entries = [_entry(60.0), _entry(120.0, busy=True), _entry(180.0), _entry(240.0)]
        verdict = contention.evaluate_series(entries, 0.0, 300.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CLEAN)

    def test_no_samples_at_all_is_void_not_clean(self) -> None:
        verdict = contention.evaluate_series([], 0.0, 600.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn(contention.VERDICT_UNMEASURED, contention.VOID_VERDICTS)

    def test_a_blind_window_is_void_not_clean(self) -> None:
        """A monitor that died mid-job saw no contention -- and proves nothing.

        This is the vacuous pass the whole module exists to remove: "we observed
        nothing" from an observer that was not running must never read the same
        as "nothing happened".
        """
        entries = [_entry(60.0), _entry(120.0), _entry(120.0 + contention.MAX_SAMPLE_GAP_SEC + 60.0)]
        verdict = contention.evaluate_series(entries, 0.0, 900.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn("gap", "\n".join(verdict.reasons))

    def test_a_gap_before_the_first_sample_counts_too(self) -> None:
        """A sampler that never started until late leaves the same blind window."""
        entries = [_entry(contention.MAX_SAMPLE_GAP_SEC + 120.0)]
        verdict = contention.evaluate_series(
            entries, 0.0, contention.MAX_SAMPLE_GAP_SEC + 180.0
        )
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_a_gap_after_the_last_sample_counts_too(self) -> None:
        entries = [_entry(60.0), _entry(120.0)]
        verdict = contention.evaluate_series(
            entries, 0.0, 120.0 + contention.MAX_SAMPLE_GAP_SEC + 60.0
        )
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_a_series_of_only_failed_samples_is_void(self) -> None:
        entries = [_entry(index * 60.0, error="counters unavailable") for index in range(1, 6)]
        verdict = contention.evaluate_series(entries, 0.0, 360.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_contention_wins_over_an_incomplete_record(self) -> None:
        """A demonstrably contended run is void whether or not the record has holes."""
        entries = [
            _entry(60.0, busy=True),
            _entry(120.0, busy=True),
            _entry(120.0 + contention.MAX_SAMPLE_GAP_SEC + 60.0),
        ]
        verdict = contention.evaluate_series(entries, 0.0, 900.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CONTENDED_MID_RUN)


# --------------------------------------------------------------------------
# Sampling
# --------------------------------------------------------------------------


class SampleParsing(unittest.TestCase):
    def _take(self, payload, smi=(("0 %,"), None)):
        with mock.patch.object(contention, "run_sample_script", return_value=(payload, None)):
            with mock.patch.object(contention, "query_nvidia_smi", return_value=smi):
                return contention.take_sample(job_root_pid=200, busy_percent=15.0)

    PAYLOAD = {
        "counter_error": None,
        "process_error": None,
        "valid_counter_samples": 880,
        "invalid_counter_samples": 3,
        "gpu_percent_by_pid": {"500": 42.0, "400": 90.0},
        "processes": {
            "200": {"ppid": 1, "name": "Runner.Worker.exe", "path": "C:\\r\\Runner.Worker.exe"},
            "400": {"ppid": 200, "name": "godot.exe", "path": "C:\\work\\repo\\bin\\godot.exe"},
            "500": {"ppid": 1, "name": "Godot_v4.7-stable_win64.exe", "path": "C:\\g\\G.exe"},
        },
    }

    def test_only_the_foreign_process_is_a_contender(self) -> None:
        sample = self._take(self.PAYLOAD)
        self.assertTrue(sample.usable)
        self.assertEqual([load.pid for load in sample.contenders], [500])

    def test_an_unreadable_counter_set_is_unusable_not_idle(self) -> None:
        """"We could not see who" must never be recorded as "nobody"."""
        payload = dict(self.PAYLOAD, counter_error="PDH said no")
        sample = self._take(payload)
        self.assertFalse(sample.usable)
        self.assertFalse(sample.busy)
        self.assertIn("PDH said no", sample.error)

    def test_an_unreadable_process_table_is_unusable(self) -> None:
        payload = dict(self.PAYLOAD, process_error="access denied")
        sample = self._take(payload)
        self.assertFalse(sample.usable)

    def test_a_process_below_the_threshold_is_recorded_but_not_a_contender(self) -> None:
        payload = dict(self.PAYLOAD, gpu_percent_by_pid={"500": 5.5})
        sample = self._take(payload)
        self.assertTrue(sample.usable)
        self.assertFalse(sample.busy)
        self.assertEqual([load.pid for load in sample.all_loads], [500])

    def test_the_probe_script_survives_individual_bad_counter_instances(self) -> None:
        """Measured: ~1 query in 10 has an invalid instance among ~890 (process churn).

        PDH invalidates the whole query for one bad instance, so a `-Stop`
        probe turned normal churn into an UNMEASURED verdict every tenth
        sample. The per-sample `Status` filter is the difference between a
        usable monitor and a false-void generator, so it is asserted here.
        """
        self.assertIn("$s.Status -ne 0", contention._SAMPLE_PS)
        self.assertIn("SilentlyContinue", contention._SAMPLE_PS)
        self.assertIn("$validSamples -eq 0", contention._SAMPLE_PS)


class ProbeFailures(unittest.TestCase):
    def test_a_probe_that_cannot_run_is_an_error_not_an_empty_reading(self) -> None:
        with mock.patch.object(
            contention.subprocess, "run", side_effect=OSError("no powershell")
        ):
            payload, error = contention.run_sample_script()
        self.assertIsNone(payload)
        self.assertIn("no powershell", error)

    def test_a_silent_probe_is_an_error(self) -> None:
        with mock.patch.object(
            contention.subprocess,
            "run",
            return_value=mock.Mock(returncode=0, stdout="", stderr=""),
        ):
            payload, error = contention.run_sample_script()
        self.assertIsNone(payload)
        self.assertIn("printed nothing", error)

    def test_nvidia_smi_reporting_no_rows_is_not_an_occupancy_measurement(self) -> None:
        with mock.patch.object(contention.subprocess, "run",
                               return_value=mock.Mock(returncode=0, stdout="\n", stderr="")):
            with mock.patch("shutil.which", return_value="nvidia-smi"):
                rows, error = contention.query_nvidia_smi()
        self.assertEqual(rows, ())
        self.assertIn("no GPU rows", error)


if __name__ == "__main__":
    unittest.main(verbosity=2)
