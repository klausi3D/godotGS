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

import contextlib
import json
import os
import posixpath
import sys
import tempfile
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

#: Longest run observed of the GPU job with the SHORTEST timeout, which is what
#: the wait bound has to fit alongside. Measured over the 12 most recent
#: successful `baseline_qa.yml` runs: `gpu-harness` (60 min budget) took 0.5-14.2
#: min, `gpu-tests` (120 min budget) 5.7-18.1 min.
MEASURED_ON = "12 successful baseline_qa.yml runs, 2026-08-13"
OBSERVED_GPU_JOB_MINUTES_MAX = 14.2

#: Twice the worst observed run. A wait is affordable only if the job can still
#: finish after waiting the whole bound, and "still finish" has to carry margin:
#: the runs measured above were themselves on a healthy machine, and the case
#: that consumes the wait is by definition a machine that was NOT healthy.
HEADROOM_MARGIN = 2.0
REQUIRED_HEADROOM_SEC = HEADROOM_MARGIN * OBSERVED_GPU_JOB_MINUTES_MAX * 60


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

        The bound this asserts is DERIVED, not restated. The assertion used to
        say "the shortest GPU job timeout is 120 minutes" and compare against
        that literal -- and it was already false when it was written, because
        `baseline_qa.yml`'s `gpu-harness` is bounded at 60 (#882 review). A
        constant cannot notice a tighter job joining the pool; the workflows can.

        And the relation that matters is not a fraction of the timeout, it is
        HEADROOM: what is left for the job's own work after a maximal wait. A
        wait is affordable exactly when the job can still finish inside its
        timeout having waited the whole bound, so that is what is asserted.
        """
        shortest_minutes, binding_job, declared = gpu_guard.shortest_gpu_job_timeout_minutes()
        self.assertGreater(
            declared,
            0,
            "Not one GPU job declares a `timeout-minutes:`, so every job fell back "
            f"to GitHub's {gpu_guard.DEFAULT_JOB_TIMEOUT_MINUTES}-minute default and "
            "this bound is being checked against a number no workflow states. That "
            "is a broken parser, not a relaxed policy.",
        )
        self.assertGreater(contention.DEFAULT_WAIT_TIMEOUT_SEC, 0)

        headroom_sec = shortest_minutes * 60 - contention.DEFAULT_WAIT_TIMEOUT_SEC
        self.assertGreaterEqual(
            headroom_sec,
            REQUIRED_HEADROOM_SEC,
            f"A job that waits the full {contention.DEFAULT_WAIT_TIMEOUT_SEC:.0f}s and "
            f"then runs normally has {headroom_sec / 60:.0f} min left inside "
            f"{binding_job[0]}'s {binding_job[1]!r} budget of {shortest_minutes} min, "
            f"but that job has been measured taking up to {OBSERVED_GPU_JOB_MINUTES_MAX} "
            f"min ({MEASURED_ON}). Below {REQUIRED_HEADROOM_SEC / 60:.0f} min of headroom "
            "a busy runner stops producing an interpretable RUNNER BUSY verdict and "
            "starts producing a job timeout instead -- the uninterpretable red this "
            "whole guard exists to remove. Either shorten DEFAULT_WAIT_TIMEOUT_SEC or "
            "raise that job's timeout; do not lower the headroom to fit.",
        )

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

    def test_a_sibling_directory_is_not_inside_the_workspace(self) -> None:
        r"""`startswith` alone says `C:\work\repo2` is inside `C:\work\repo`.

        It is not, and the error runs the wrong way: an unrelated process in a
        neighbouring directory would be classified as ours and its GPU load
        would stop counting as contention.
        """
        table = dict(self.TABLE)
        table[600] = {"ppid": 1, "name": "other.exe", "path": "C:\\work\\repo2\\bin\\other.exe"}
        loads = contention.classify(
            {600: 90.0}, table, ours_pids=set(), roots=("C:\\work\\repo",)
        )
        self.assertFalse(loads[0].ours)

    def test_an_uninterpretable_image_path_is_foreign_not_ours(self) -> None:
        """Fail closed. `abspath` used to join it to the cwd -- which is the repo.

        Classifying an unreadable path as *ours* removes it from contention
        accounting, so the failure mode was invisible by construction.
        """
        table = dict(self.TABLE)
        table[700] = {"ppid": 1, "name": "odd.exe", "path": "some-relative-thing.exe"}
        loads = contention.classify(
            {700: 90.0}, table, ours_pids=set(), roots=("C:\\work\\repo",)
        )
        self.assertFalse(loads[0].ours)


class WindowsPathsOnAnyHost(unittest.TestCase):
    r"""These paths are Windows paths wherever this code runs (#882 review).

    The guard runs for real only on the Windows runner, but its unit tests run
    on `ubuntu-latest` in the required `agentic-pr-gate` lane. There,
    `posixpath.abspath("C:\\g\\G.exe")` returns `"<cwd>/C:\\g\\G.exe"` -- the
    Windows path silently reinterpreted as a *relative POSIX* one, rooted in the
    current directory, which during a CI job is the repository and therefore one
    of `ci_roots()`. A foreign `Godot_v4.7-stable_win64.exe` came out classified
    as ours.

    The fix is in the logic, not in the tests, because the defect is not a test
    artefact: `abspath` on any unparsed path fails **open**, in the single
    direction that hides contention. So the rules follow the shape of the data
    and the tests pin that on both host flavours.
    """

    def _posix_host(self):
        return mock.patch.object(contention, "_HOST_PATH", posixpath)

    def test_a_windows_path_normalises_by_windows_rules_on_a_posix_host(self) -> None:
        with self._posix_host():
            self.assertEqual(
                contention.normalise_image_path("C:\\g\\G.exe"), "c:\\g\\g.exe"
            )
            self.assertEqual(
                contention.normalise_image_path("C:/g/G.exe"), "c:\\g\\g.exe"
            )

    def test_a_unc_path_is_windows_style_too(self) -> None:
        with self._posix_host():
            self.assertEqual(
                contention.normalise_image_path("\\\\host\\share\\a.exe"),
                "\\\\host\\share\\a.exe",
            )

    def test_a_relative_path_never_becomes_absolute(self) -> None:
        """The whole defect in one assertion: no cwd is ever joined in."""
        with self._posix_host():
            self.assertIsNone(contention.normalise_image_path("relative\\thing.exe"))
            self.assertIsNone(contention.normalise_image_path("thing.exe"))
            self.assertIsNone(contention.normalise_image_path(""))
            self.assertIsNone(contention.normalise_image_path("   "))

    def test_a_foreign_windows_image_is_foreign_on_a_posix_host(self) -> None:
        """The exact `agentic-pr-gate` failure, pinned."""
        with self._posix_host():
            with mock.patch.object(
                contention, "ci_roots", return_value=("/home/runner/work/godotGS/godotGS",)
            ):
                loads = contention.classify(
                    {500: 42.0},
                    {500: {"ppid": 1, "name": "Godot_v4.7-stable_win64.exe",
                           "path": "C:\\g\\G.exe"}},
                    ours_pids=set(),
                    roots=contention.ci_roots(),
                )
        self.assertFalse(
            loads[0].ours,
            "a Windows image path was matched against a POSIX repository root; on the "
            "Linux guard lane this reclassified foreign GPU load as the job's own.",
        )

    def test_a_posix_image_under_a_posix_root_is_still_ours(self) -> None:
        """The fix must not make everything foreign -- that is red, not correct."""
        with self._posix_host():
            loads = contention.classify(
                {400: 42.0},
                {400: {"ppid": 1, "name": "godot", "path": "/home/runner/work/repo/bin/godot"}},
                ours_pids=set(),
                roots=("/home/runner/work/repo",),
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


#: The pid the preflight recorded for this job's sampler. Fixtures carry it, and
#: carry the sampler's provenance, because production samples do: a fixture that
#: lets the code under test take a path production never takes is a hiding place,
#: not a fixture.
SAMPLER_PID = 4242


def _entry(
    at: float,
    busy: bool = False,
    error: str = None,
    source: str = contention.SOURCE_SAMPLER,
    writer_pid: int = SAMPLER_PID,
) -> Dict[str, object]:
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
    return {
        "at": at,
        "error": error,
        "contenders": contenders,
        "loads": contenders,
        "source": source,
        "writer_pid": writer_pid,
    }


def _series(entries, monitored_from, ended_at, **kwargs):
    """`evaluate_series` as the postflight calls it: the sampler is this job's.

    Coverage is only what `SAMPLER_PID`'s sampler wrote, so every test that is
    about something else has to supply that pid, exactly as the postflight does
    from the session record.
    """
    kwargs.setdefault("sampler_pid", SAMPLER_PID)
    return contention.evaluate_series(entries, monitored_from, ended_at, **kwargs)


class StartVersusEnd(unittest.TestCase):
    """#881's shape: clean at start, contended while running."""

    def test_a_clean_continuous_series_passes(self) -> None:
        entries = [_entry(index * 60.0) for index in range(1, 11)]
        verdict = _series(entries, 0.0, 660.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CLEAN)

    def test_contention_that_begins_after_the_preflight_is_caught(self) -> None:
        entries = [_entry(60.0), _entry(120.0), _entry(180.0, busy=True), _entry(240.0, busy=True)]
        verdict = _series(entries, 0.0, 300.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CONTENDED_MID_RUN)
        self.assertEqual(len(verdict.windows), 1)
        self.assertIn(
            "Godot_v4.7-stable_win64.exe", "\n".join(verdict.reasons)
        )

    def test_contention_is_reported_with_its_window_and_the_offending_image(self) -> None:
        entries = [_entry(60.0, busy=True), _entry(120.0, busy=True), _entry(180.0)]
        verdict = _series(entries, 0.0, 240.0)
        first, last, described = verdict.windows[0]
        self.assertEqual((first, last), (60.0, 120.0))
        self.assertIn("pid 500", described[0])
        self.assertIn("C:\\g\\G.exe", described[0])

    def test_a_single_busy_sample_is_not_a_contended_run(self) -> None:
        """A blip must not void a two-hour job; that is a false failure too."""
        entries = [_entry(60.0), _entry(120.0, busy=True), _entry(180.0), _entry(240.0)]
        verdict = _series(entries, 0.0, 300.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CLEAN)

    def test_no_samples_at_all_is_void_not_clean(self) -> None:
        verdict = _series([], 0.0, 600.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn(contention.VERDICT_UNMEASURED, contention.VOID_VERDICTS)

    def test_a_blind_window_is_void_not_clean(self) -> None:
        """A monitor that died mid-job saw no contention -- and proves nothing.

        This is the vacuous pass the whole module exists to remove: "we observed
        nothing" from an observer that was not running must never read the same
        as "nothing happened".
        """
        entries = [_entry(60.0), _entry(120.0), _entry(120.0 + contention.MAX_SAMPLE_GAP_SEC + 60.0)]
        verdict = _series(entries, 0.0, 900.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn("gap", "\n".join(verdict.reasons))

    def test_a_gap_before_the_first_sample_counts_too(self) -> None:
        """A sampler that never started until late leaves the same blind window."""
        entries = [_entry(contention.MAX_SAMPLE_GAP_SEC + 120.0)]
        verdict = _series(
            entries, 0.0, contention.MAX_SAMPLE_GAP_SEC + 180.0
        )
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_a_gap_after_the_last_sample_counts_too(self) -> None:
        entries = [_entry(60.0), _entry(120.0)]
        verdict = _series(
            entries, 0.0, 120.0 + contention.MAX_SAMPLE_GAP_SEC + 60.0
        )
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_a_series_of_only_failed_samples_is_void(self) -> None:
        entries = [_entry(index * 60.0, error="counters unavailable") for index in range(1, 6)]
        verdict = _series(entries, 0.0, 360.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_a_long_successful_wait_is_not_an_unobserved_gap(self) -> None:
        """The bounded wait must not void the runs it exists to rescue (#882 review).

        Coverage used to be measured from when the *preflight* started. A wait
        longer than `MAX_SAMPLE_GAP_SEC` then left a >5-minute stretch with no
        sample in it, the postflight read that as a blind window, and the
        verdict came back `UNMEASURED` -- voiding every job that waited between
        roughly 5 and 15 minutes and then ran on a genuinely free machine. The
        wait is not unobserved: it is densely sampled, into `wait_samples`.
        """
        waited = contention.MAX_SAMPLE_GAP_SEC + 240.0
        entries = [_entry(waited + index * 60.0) for index in range(1, 6)]
        ended = waited + 360.0

        from_monitoring = _series(entries, waited, ended)
        self.assertEqual(from_monitoring.verdict, contention.VERDICT_CLEAN)

        # The same series judged from the old start point, to show the two are
        # not equivalent and this test would not pass either way.
        from_preflight_start = _series(entries, 0.0, ended)
        self.assertEqual(from_preflight_start.verdict, contention.VERDICT_UNMEASURED)

    def test_failed_probes_do_not_stand_in_for_coverage(self) -> None:
        """An interval where nothing was known must not read as one where nothing happened.

        Error entries used to keep their timestamps in the continuity
        calculation, so an arbitrarily long run of failed probes between two
        good samples still produced `CLEAN`.
        """
        blind = contention.MAX_SAMPLE_GAP_SEC + 120.0
        entries = [_entry(60.0)]
        entries += [
            _entry(60.0 + step, error="counters unavailable")
            for step in range(60, int(blind), 60)
        ]
        entries.append(_entry(60.0 + blind))
        verdict = _series(entries, 0.0, 60.0 + blind + 30.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn("no usable monitoring sample", "\n".join(verdict.reasons))

    def test_a_failed_probe_does_not_split_a_contended_window(self) -> None:
        """A failure to observe must not be able to erase what was observed.

        With error entries in the streak, one failed probe between two busy
        samples broke a real contended window into two single-sample streaks,
        neither reaching the required count -- so contention was dropped
        *because* measurement failed.
        """
        entries = [
            _entry(60.0, busy=True),
            _entry(120.0, error="counters unavailable"),
            _entry(180.0, busy=True),
            _entry(240.0),
        ]
        verdict = _series(entries, 0.0, 300.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CONTENDED_MID_RUN)
        self.assertEqual(len(verdict.windows), 1)

    def test_a_few_scattered_failed_probes_still_pass(self) -> None:
        """Excluding errors must not make every real run UNMEASURED.

        One failed probe in ten is the measured rate on this runner, so a rule
        that voided a run for any error at all would be a false-void generator.
        """
        entries = []
        for index in range(1, 13):
            entries.append(_entry(index * 60.0, error="blip" if index == 5 else None))
        verdict = _series(entries, 0.0, 13 * 60.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CLEAN, verdict.reasons)

    def test_contention_wins_over_an_incomplete_record(self) -> None:
        """A demonstrably contended run is void whether or not the record has holes."""
        entries = [
            _entry(60.0, busy=True),
            _entry(120.0, busy=True),
            _entry(120.0 + contention.MAX_SAMPLE_GAP_SEC + 60.0),
        ]
        verdict = _series(entries, 0.0, 900.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CONTENDED_MID_RUN)


# --------------------------------------------------------------------------
# Sampling
# --------------------------------------------------------------------------


class PostflightReadsTheMonitoredWindow(unittest.TestCase):
    """End-to-end for the wait/coverage split, through the real CLI.

    Timestamps are realistic epochs, not `0.0`. The first version of this test
    used `started_at: 0.0` and did not discriminate: the reader was
    `float(session.get("started_at") or end_sample.at)`, and `0.0` is falsy, so
    the two timestamps collapsed onto each other and the mutation that reverts
    this fix survived. A fixture value that lets the code under test take a
    different path than production does is not a fixture, it is a hiding place.
    """

    #: Any plausible wall-clock epoch; the point is only that it is not falsy.
    EPOCH = 1_700_000_000.0

    def _run(self, session: Dict[str, object], entries: List[Dict[str, object]]) -> int:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / contention.SESSION_FILE).write_text(
                json.dumps(session), encoding="utf-8"
            )
            (base / contention.SAMPLES_FILE).write_text(
                "".join(json.dumps(entry) + "\n" for entry in entries), encoding="utf-8"
            )
            clean = _sample(at=session["monitored_from"] + 400.0)
            with mock.patch.object(contention, "take_sample", return_value=clean):
                return contention.main(["postflight", "--record-dir", str(base)])

    def test_a_job_that_waited_ten_minutes_and_then_ran_clean_passes(self) -> None:
        waited = contention.MAX_SAMPLE_GAP_SEC + 300.0
        session = {
            "start_verdict": contention.VERDICT_CLEAN,
            "started_at": self.EPOCH,
            "monitored_from": self.EPOCH + waited,
            "job_root_pid": 200,
            "sampler_pid": SAMPLER_PID,
            "wait_samples": [
                {"at": self.EPOCH + step} for step in range(0, int(waited), 20)
            ],
        }
        entries = [
            _entry(self.EPOCH + waited + index * 60.0) for index in range(1, 7)
        ]
        self.assertEqual(self._run(session, entries), 0)

    def test_the_same_job_is_void_when_the_sampler_never_started(self) -> None:
        """No `monitored_from` means there was no monitored interval at all.

        The closing sample this postflight takes is by itself a one-sample
        series over a zero-length window, which scores as perfect continuous
        coverage -- a measurement of the last instant standing in for a
        measurement of the job.
        """
        session = {
            "start_verdict": contention.VERDICT_CLEAN,
            "started_at": self.EPOCH,
            "job_root_pid": 200,
            "sampler_error": "could not start the background sampler",
        }
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / contention.SESSION_FILE).write_text(json.dumps(session), encoding="utf-8")
            with mock.patch.object(
                contention, "take_sample", return_value=_sample(at=self.EPOCH + 900.0)
            ):
                code = contention.main(["postflight", "--record-dir", str(base)])
        self.assertEqual(code, contention.EXIT_RUNNER_BUSY)


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


# --------------------------------------------------------------------------
# "Monitoring was active" is a positive fact
# --------------------------------------------------------------------------


class MonitoringIsAPositiveFact(unittest.TestCase):
    """Coverage may rest only on evidence the sampler itself produced.

    Two routes into "monitoring never ran, and the job reported CLEAN" have now
    been found (#882 review):

    1. the sampler never started, so the only entry in the series is the
       postflight's own closing sample -- a one-sample series over a zero-length
       window, which scores as perfect continuous coverage;
    2. the sampler started (`Popen` returned a pid) and died before its first
       append, which leaves the *same* empty series for the postflight to fill.

    Both end in one place, so they are closed in one place rather than as two
    special cases: a sample counts as coverage only if this job's sampler wrote
    it. A third route to the same end state -- however it arises -- is closed by
    the same rule, because the rule is stated over what the evidence *is* rather
    than over the ways it can be missing.

    Every case here is paired with its opposite. A rule that voided everything
    would satisfy the first half of each pair and be worthless.
    """

    T0 = 1_700_000_000.0

    def test_a_sample_records_who_wrote_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / contention.SAMPLES_FILE
            contention.append_sample(path, _sample(at=self.T0), contention.SOURCE_SAMPLER)
            entry = contention.read_samples(path)[0]
        self.assertEqual(entry["source"], contention.SOURCE_SAMPLER)
        self.assertEqual(entry["writer_pid"], os.getpid())
        self.assertTrue(contention.written_by_sampler(entry, os.getpid()))
        # The pid is the discriminator the postflight cannot satisfy: it runs in
        # a different process, so its own closing sample can never match.
        self.assertFalse(contention.written_by_sampler(entry, os.getpid() + 1))

    def test_the_postflights_own_closing_sample_is_not_coverage(self) -> None:
        """Route 2, at the unit: a series of exactly one postflight sample."""
        entries = [
            _entry(self.T0 + 120.0, source=contention.SOURCE_POSTFLIGHT, writer_pid=99)
        ]
        verdict = _series(entries, self.T0, self.T0 + 120.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn("background sampler", "\n".join(verdict.reasons))

    def test_the_same_window_covered_by_the_sampler_is_clean(self) -> None:
        """The other direction, which is the one that keeps the fix honest."""
        entries = [_entry(self.T0 + step) for step in (30.0, 90.0)]
        entries.append(
            _entry(self.T0 + 120.0, source=contention.SOURCE_POSTFLIGHT, writer_pid=99)
        )
        verdict = _series(entries, self.T0, self.T0 + 120.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CLEAN, verdict.reasons)

    def test_a_full_series_of_postflight_samples_is_still_not_coverage(self) -> None:
        """It is provenance that decides, not sample count or gap size.

        Twelve evenly spaced samples with no gap anywhere -- everything the
        continuity rule asks for -- and still void, because no monitor produced
        them.
        """
        entries = [
            _entry(
                self.T0 + index * 60.0,
                source=contention.SOURCE_POSTFLIGHT,
                writer_pid=99,
            )
            for index in range(1, 13)
        ]
        verdict = _series(entries, self.T0, self.T0 + 13 * 60.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)

    def test_an_orphaned_samplers_samples_are_not_this_jobs_coverage(self) -> None:
        """#882 finding B, seen from the postflight side.

        An orphan writes genuine sampler samples -- of the same machine, in the
        same file -- but it was not monitoring *this* job, and it did not cover
        this job's window by design.
        """
        entries = [_entry(self.T0 + index * 60.0, writer_pid=SAMPLER_PID + 7) for index in range(1, 6)]
        verdict = _series(entries, self.T0, self.T0 + 360.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_UNMEASURED)
        self.assertIn(str(SAMPLER_PID + 7), "\n".join(verdict.reasons))

    def test_contention_is_read_from_every_sample_whoever_wrote_it(self) -> None:
        """The asymmetry is deliberate: filtering evidence could only go green.

        A reading of this machine during this window says what the machine was
        doing regardless of who took it, so contention counts it. Coverage is a
        claim about being monitored, so it does not.
        """
        entries = [
            _entry(self.T0 + 60.0, busy=True, writer_pid=SAMPLER_PID + 7),
            _entry(self.T0 + 120.0, busy=True, source=contention.SOURCE_POSTFLIGHT, writer_pid=99),
        ]
        verdict = _series(entries, self.T0, self.T0 + 180.0)
        self.assertEqual(verdict.verdict, contention.VERDICT_CONTENDED_MID_RUN)

    def test_no_recorded_sampler_means_nothing_can_be_coverage(self) -> None:
        """`sampler_pid` absent is answered False, not "match anything"."""
        entries = [_entry(self.T0 + index * 60.0) for index in range(1, 6)]
        self.assertEqual(
            contention.evaluate_series(
                entries, self.T0, self.T0 + 360.0, sampler_pid=None
            ).verdict,
            contention.VERDICT_UNMEASURED,
        )
        self.assertEqual(
            _series(entries, self.T0, self.T0 + 360.0).verdict,
            contention.VERDICT_CLEAN,
        )

    def test_the_preflight_waits_for_the_samplers_first_written_sample(self) -> None:
        """Readiness is the sample, not the spawn -- and it is bounded."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / contention.SAMPLES_FILE

            # Nothing written at all: the wait gives up and says so.
            missing_at, error = contention.await_sampler_ready(
                path, SAMPLER_PID, timeout_sec=0.0, sleep=lambda _seconds: None
            )
            self.assertIsNone(missing_at)
            self.assertIn("wrote no sample", error)

            # A sample from someone else's sampler is not this sampler starting.
            path.write_text(
                json.dumps(_entry(self.T0, writer_pid=SAMPLER_PID + 7)) + "\n",
                encoding="utf-8",
            )
            orphan_at, orphan_error = contention.await_sampler_ready(
                path, SAMPLER_PID, timeout_sec=0.0, sleep=lambda _seconds: None
            )
            self.assertIsNone(orphan_at)
            self.assertIsNotNone(orphan_error)

            # Its own first sample releases the wait, and dates the interval.
            with path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(_entry(self.T0 + 5.0)) + "\n")
            ready_at, ready_error = contention.await_sampler_ready(
                path, SAMPLER_PID, timeout_sec=0.0, sleep=lambda _seconds: None
            )
            self.assertIsNone(ready_error)
            self.assertEqual(ready_at, self.T0 + 5.0)

    def test_a_sampler_that_died_before_its_first_append_is_void_end_to_end(self) -> None:
        """Codex's reproduction, through the real CLI.

        Session with `monitored_from` and a sampler pid, no JSONL series at all,
        postflight 120 s later taking a clean closing sample. Before the fix this
        printed PASS and exited 0.
        """
        session = {
            "start_verdict": contention.VERDICT_CLEAN,
            "started_at": self.T0,
            "monitored_from": self.T0 + 5.0,
            "sampler_pid": SAMPLER_PID,
            "sampler_error": None,
            "job_root_pid": 200,
        }
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / contention.SESSION_FILE).write_text(json.dumps(session), encoding="utf-8")
            with mock.patch.object(
                contention, "take_sample", return_value=_sample(at=self.T0 + 125.0)
            ):
                code = contention.main(["postflight", "--record-dir", str(base)])
        self.assertEqual(code, contention.EXIT_RUNNER_BUSY)


# --------------------------------------------------------------------------
# Orphaned samplers on a persistent runner
# --------------------------------------------------------------------------


OUR_SAMPLER_COMMAND = (
    f'"C:\\Python\\python.exe" "{Path(contention.__file__).resolve()}" sample '
    "--record-dir {directory} --job-root-pid 4 --poll-interval-sec 60.0"
)


class OrphanedSamplersAreStoppedNotInherited(unittest.TestCase):
    """A killed job's sampler must not outlive it into the next job's series.

    A GPU job that hits its 60/120-minute timeout never runs its postflight, so
    nothing writes the stop file and its detached sampler keeps sampling for up
    to three hours. The next job on this persistent runner deletes the shared
    files and starts its own -- and two samplers then append to one series.

    The termination has to be *safe*, which is the harder half: pids are
    recycled, and this runner is also the maintainer's workstation, so the pid a
    stale session names may now be their editor. Killing a stranger would be a
    worse defect than the orphan.
    """

    def _session(self, directory: Path, **fields) -> Path:
        payload = {"sampler_pid": 4242, "start_verdict": contention.VERDICT_CLEAN}
        payload.update(fields)
        (directory / contention.SESSION_FILE).write_text(
            json.dumps(payload), encoding="utf-8"
        )
        return directory

    def _stop(self, directory: Path, probe, killed: List[int]):
        def terminate(pid: int):
            killed.append(pid)
            return None

        # A fake clock, so the "it never died" case exercises the real timeout
        # without spending it.
        clock = {"now": 0.0}

        def advance(seconds: float) -> None:
            clock["now"] += max(seconds, 1.0)

        with mock.patch.object(contention, "probe_process", side_effect=probe):
            with mock.patch.object(contention, "terminate_process", side_effect=terminate):
                return contention.stop_orphaned_sampler(
                    directory, now=lambda: clock["now"], sleep=advance
                )

    def test_identity_needs_this_script_this_phase_and_this_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            ours = OUR_SAMPLER_COMMAND.format(directory=base)
            self.assertTrue(contention.is_this_guards_sampler(ours, base))
            # Right script, right phase, *different* record directory: another
            # job's sampler, not this directory's orphan.
            self.assertFalse(
                contention.is_this_guards_sampler(
                    OUR_SAMPLER_COMMAND.format(directory=base / "elsewhere"), base
                )
            )
            # Right script and directory, different phase.
            self.assertFalse(
                contention.is_this_guards_sampler(ours.replace(" sample ", " postflight "), base)
            )
            # A python process in the same directory that is not this script.
            self.assertFalse(
                contention.is_this_guards_sampler(
                    f'"C:\\Python\\python.exe" "other.py" sample --record-dir {base}', base
                )
            )
            self.assertFalse(contention.is_this_guards_sampler(None, base))
            self.assertFalse(contention.is_this_guards_sampler("", base))

    def test_the_previous_jobs_sampler_is_terminated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = self._session(Path(directory))
            ours = OUR_SAMPLER_COMMAND.format(directory=base)
            killed: List[int] = []
            probes = iter([(True, ours), (False, None)])
            lines, unresolved = self._stop(base, lambda _pid: next(probes), killed)
        self.assertEqual(killed, [4242])
        self.assertIsNone(unresolved)
        self.assertIn("orphaned sampler", "\n".join(lines))

    def test_a_recycled_pid_owned_by_someone_else_is_left_alone(self) -> None:
        """The direction that matters more: never kill a stranger.

        A pid that now belongs to another process is also proof the sampler that
        held it has exited, so there is nothing left to stop.
        """
        stranger = (
            '"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe" '
            "--type=renderer --lang=en-GB"
        )
        with tempfile.TemporaryDirectory() as directory:
            base = self._session(Path(directory))
            killed: List[int] = []
            lines, unresolved = self._stop(base, lambda _pid: (True, stranger), killed)
        self.assertEqual(killed, [])
        self.assertIsNone(unresolved)
        self.assertIn("recycled", "\n".join(lines))
        self.assertIn("chrome.exe", "\n".join(lines))

    def test_a_live_pid_that_cannot_be_identified_is_left_alone_and_recorded(self) -> None:
        """"Running but unreadable" is not "running our sampler", and not "gone"."""
        with tempfile.TemporaryDirectory() as directory:
            base = self._session(Path(directory))
            killed: List[int] = []
            lines, unresolved = self._stop(base, lambda _pid: (True, None), killed)
        self.assertEqual(killed, [])
        self.assertIsNotNone(unresolved)
        self.assertIn("NOT terminating", "\n".join(lines))

    def test_a_dead_pid_needs_no_action(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = self._session(Path(directory))
            killed: List[int] = []
            lines, unresolved = self._stop(base, lambda _pid: (False, None), killed)
        self.assertEqual(killed, [])
        self.assertIsNone(unresolved)

    def test_an_orphan_that_survives_termination_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = self._session(Path(directory))
            ours = OUR_SAMPLER_COMMAND.format(directory=base)
            killed: List[int] = []
            lines, unresolved = self._stop(base, lambda _pid: (True, ours), killed)
        self.assertEqual(killed, [4242])
        self.assertIn("could not be stopped", unresolved or "")
        self.assertIn("WARNING", "\n".join(lines))

    def test_no_previous_session_is_not_an_orphan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            killed: List[int] = []
            lines, unresolved = self._stop(Path(directory), lambda _pid: (True, None), killed)
        self.assertEqual((lines, unresolved, killed), ([], None, []))

    def test_our_own_pid_is_never_a_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = self._session(Path(directory), sampler_pid=os.getpid())
            killed: List[int] = []
            self._stop(base, lambda _pid: (True, None), killed)
        self.assertEqual(killed, [])


class PreflightOrderAndReadiness(unittest.TestCase):
    """The preflight, run offline, in the order the fix depends on."""

    T0 = 1_700_000_000.0

    def _preflight(self, ready, base: Path, seen: Dict[str, object]):
        def stop_orphan(directory, **_kwargs):
            # Recorded at call time: deleting the shared series while a previous
            # sampler is still writing does not stop it writing.
            seen["samples_present_when_orphan_stopped"] = (
                directory / contention.SAMPLES_FILE
            ).is_file()
            return ["  (orphan check)"], "pid 1 could not be identified"

        with contextlib.ExitStack() as stack:
            patch = lambda name, **kw: stack.enter_context(  # noqa: E731
                mock.patch.object(contention, name, **kw)
            )
            patch("stop_orphaned_sampler", side_effect=stop_orphan)
            patch("run_sample_script", return_value=({"processes": {}}, None))
            patch(
                "wait_for_free_gpu",
                return_value=(True, [_sample(at=self.T0)], []),
            )
            patch("spawn_sampler", return_value=(SAMPLER_PID, None))
            # A callable `ready` stands in the readiness wait's place so a case
            # can observe the world *during* the wait -- which is the only place
            # the persist-before-wait property is visible.
            if callable(ready):
                patch("await_sampler_ready", side_effect=ready)
            else:
                patch("await_sampler_ready", return_value=ready)
            patch("probe_process", return_value=(True, "unidentifiable"))
            terminated: List[int] = []
            patch("terminate_process", side_effect=terminated.append)
            code = contention.main(["preflight", "--record-dir", str(base)])
        seen["terminated"] = terminated
        return code, json.loads((base / contention.SESSION_FILE).read_text(encoding="utf-8"))

    def test_the_orphan_is_stopped_before_the_shared_series_is_deleted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / contention.SAMPLES_FILE).write_text("{}\n", encoding="utf-8")
            seen: Dict[str, object] = {}
            code, session = self._preflight((self.T0 + 3.0, None), base, seen)
        self.assertEqual(code, 0)
        self.assertTrue(seen["samples_present_when_orphan_stopped"])
        # And the unresolved orphan is recorded rather than dropped.
        self.assertEqual(session["orphan_sampler_error"], "pid 1 could not be identified")

    def test_a_confirmed_sampler_dates_the_monitored_interval_by_its_first_sample(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            code, session = self._preflight((self.T0 + 3.0, None), base, {})
        self.assertEqual(code, 0)
        self.assertEqual(session["monitored_from"], self.T0 + 3.0)
        self.assertIsNone(session["sampler_error"])

    def test_a_sampler_that_never_wrote_records_no_monitored_interval(self) -> None:
        """And the stray child is stopped rather than left on the runner."""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            seen: Dict[str, object] = {}
            code, session = self._preflight((None, "wrote no sample within 90s"), base, seen)
        self.assertEqual(code, 0)  # the job may still run; the postflight voids it
        self.assertNotIn("monitored_from", session)
        self.assertIn("wrote no sample", session["sampler_error"])
        self.assertEqual(seen["terminated"], [])  # unidentifiable -> never killed

    def test_the_sampler_pid_is_on_disk_before_the_readiness_wait(self) -> None:
        """A preflight killed mid-wait must still leave a stoppable pid (#882 review).

        `spawn_sampler()` returns a detached child with a lifetime of up to
        SAMPLER_MAX_LIFETIME_SEC, and the readiness wait after it can run for
        SAMPLER_READY_TIMEOUT_SEC (90 s). If the pid is only in memory for that
        window, a cancelled or killed preflight leaves the child alive with
        nothing recording it: the next job's orphan check reads a stale or absent
        session, stops nothing, and shares the JSONL with a sampler it cannot see.

        So this reads the session record from *inside* the wait -- the exact
        instant the interruption would land -- rather than after it.
        """
        during: Dict[str, object] = {}

        def read_the_record_mid_wait(samples_path, sampler_pid, **_kwargs):
            path = Path(samples_path).parent / contention.SESSION_FILE
            during["exists"] = path.is_file()
            during["pid"] = (
                json.loads(path.read_text(encoding="utf-8")).get("sampler_pid")
                if path.is_file()
                else None
            )
            return (self.T0 + 3.0, None)

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            code, session = self._preflight(read_the_record_mid_wait, base, {})

        self.assertEqual(code, 0)
        self.assertTrue(
            during["exists"],
            "The preflight entered the readiness wait with no session record on "
            "disk at all, so a preflight interrupted during the wait would leave "
            "its detached sampler with no pid anywhere for the next job to stop.",
        )
        self.assertEqual(
            during["pid"],
            SAMPLER_PID,
            "The session record on disk during the readiness wait does not name "
            f"the sampler that was just spawned (pid {SAMPLER_PID}). Whatever is "
            "recorded there is what the next job's orphan check will act on, so "
            "an interrupted preflight orphans this sampler onto the runner.",
        )
        # And the normal path is unaffected: the wait's result still lands in the
        # record that survives the preflight.
        self.assertEqual(session["sampler_pid"], SAMPLER_PID)
        self.assertEqual(session["monitored_from"], self.T0 + 3.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
