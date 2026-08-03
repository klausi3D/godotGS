#!/usr/bin/env python3
"""Pins the repaired doctest environment-skip detector in run_module_tests.py (#595).

Two things are pinned here, and the second matters as much as the first:

1. The CURRENT detector counts real doctest output — both the canonical
   `GS_ENV_SKIP:` token and the legacy `MESSAGE: Skip…` prose that #595
   deliberately leaves unconverted.
2. The BASELINE pattern, re-compiled inline below, finds **zero** matches in that
   same output. That is the regression this task exists to fix: the old pattern
   was line-anchored (`(?m)^\\s*Skipping…`), and doctest's ConsoleReporter always
   prefixes a message with `<file>(<line>): MESSAGE: ` via file_line_to_stream()
   (thirdparty/doctest/doctest.h:6051-6056, used by log_message at :6423-6437),
   so the marker can never start a line. The gate had therefore never fired once
   and every environment skip in every strict lane was scored as a pass.
   Asserting the old pattern's inertness is what stops it silently returning.

The exact-count assertions run against a VERBATIM CAPTURE of a real headless run
(`tests/ci/fixtures/doctest_env_skip_sample.txt`), not a hand-written sample. A
hand-written sample is precisely how the previous fixture
(`tests/ci/test_quarantine_manifest.py::_fail_output_with_skip`) certified a
line shape doctest has never emitted.
"""

from __future__ import annotations

import contextlib
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
FIXTURE = ROOT / "tests" / "ci" / "fixtures" / "doctest_env_skip_sample.txt"

# The command that produces FIXTURE. Quoted in every failure message below so a
# missing fixture tells you how to regenerate it instead of tempting anyone to
# type one out by hand.
CAPTURE_COMMAND = (
    'bin/godot.windows.editor.dev.x86_64.console.exe --headless --test '
    '"--test-case=*Painterly*"'
)

# The number of environment-skip markers in FIXTURE, COUNTED BY HAND from the
# captured output and asserted as an exact integer (never "> 0"). None means the
# capture has not been taken yet, which fails closed below rather than passing
# vacuously.
#
# Counted by reading the capture: three `MESSAGE:` lines, one carrying the
# canonical token and two carrying legacy prose --
#   test_painterly_material.cpp(209):   MESSAGE: GS_ENV_SKIP: RenderingDevice unavailable
#   test_gaussian_splat_node.h(1725):   MESSAGE: Skipping test - renderer unavailable (headless mode)
#   test_renderer_pipeline.h(3841):     MESSAGE: Skipping test - Rendering server unavailable
# The same capture also holds two `WARNING: ... - skipping painterly ...` lines,
# which are deliberately NOT markers (see the known asymmetry in
# run_module_tests.py), and doctest's own summary reads
# `test cases: 9 | 9 passed | 0 failed` -- i.e. all three skips were scored as
# passes, which is the defect this slice removes.
EXPECTED_SKIP_MARKERS: int | None = 3
EXPECTED_TOKEN_MARKERS = 1
EXPECTED_PROSE_MARKERS = 2
# Present in the capture and deliberately excluded from the count.
EXPECTED_UNCOUNTED_WARN_LINES = 2

# The pattern as it stood at baseline e9ddb27c285. Re-compiled here rather than
# imported, so that removing it from run_module_tests.py cannot make this
# assertion disappear with it.
BASELINE_SKIP_MARKER_RE = re.compile(r"(?m)^\s*(?:Skipping(?: test)?\s*-\s+.+)$")


def _load_harness():
    spec = importlib.util.spec_from_file_location("run_module_tests", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


harness = _load_harness()

SUMMARY = (
    "[doctest] test cases: 9 | 9 passed | 0 failed | 0 skipped\n"
    "[doctest] assertions: 42 | 42 passed | 0 failed\n"
    "[doctest] Status: SUCCESS!\n"
)


def _markers(text: str) -> int:
    return len(harness.DOCTEST_SKIP_MARKER_RE.findall(text))


class RealCaptureTests(unittest.TestCase):
    """A3 / A4: the exact-count assertions, fed the committed capture."""

    def _fixture_text(self) -> str:
        if not FIXTURE.is_file():
            self.fail(
                f"Missing captured doctest sample: {FIXTURE.relative_to(ROOT)}.\n"
                f"Capture it verbatim with:\n    {CAPTURE_COMMAND}\n"
                f"and set EXPECTED_SKIP_MARKERS in this file to the number of skip "
                f"markers you counted in it. Do NOT hand-write the sample: the fixture "
                f"this task removed was hand-written, and it certified a line shape "
                f"doctest has never emitted (#595)."
            )
        return FIXTURE.read_text(encoding="utf-8", errors="replace")

    def test_detector_counts_every_marker_in_the_capture(self) -> None:
        text = self._fixture_text()
        self.assertIsNotNone(
            EXPECTED_SKIP_MARKERS,
            "EXPECTED_SKIP_MARKERS is unset. Count the skip markers in "
            f"{FIXTURE.name} and write the integer in; an unset expectation would "
            "let this test pass against any detector.",
        )
        passed, failed, p_asserts, f_asserts, skip_markers, found = (
            harness._parse_doctest_results(text + SUMMARY if "test cases:" not in text else text)
        )
        self.assertEqual(EXPECTED_SKIP_MARKERS, skip_markers)

    def test_baseline_pattern_is_inert_on_the_capture(self) -> None:
        """A4: the regression is pinned, so it cannot silently come back."""
        text = self._fixture_text()
        self.assertEqual([], BASELINE_SKIP_MARKER_RE.findall(text))

    def test_capture_exercises_both_detector_branches(self) -> None:
        """A one-branch fixture would let the other branch rot unnoticed."""
        text = self._fixture_text()
        self.assertEqual(
            EXPECTED_TOKEN_MARKERS, len(re.findall(r"MESSAGE:[ \t]*GS_ENV_SKIP:", text))
        )
        self.assertEqual(EXPECTED_PROSE_MARKERS, len(re.findall(r"MESSAGE:[ \t]*Skipp", text)))
        self.assertEqual(EXPECTED_SKIP_MARKERS, EXPECTED_TOKEN_MARKERS + EXPECTED_PROSE_MARKERS)

    def test_capture_proves_the_console_prefix_exists(self) -> None:
        """The empirical proof that a line-anchored pattern cannot work.

        Every marker in a real capture is preceded on its own line by doctest's
        `<file>(<line>): ` prefix. This is the fact the removed fixture invented
        the opposite of.
        """
        text = self._fixture_text()
        marker_lines = [ln for ln in text.splitlines() if "MESSAGE:" in ln and "Skip" in ln or "GS_ENV_SKIP:" in ln]
        self.assertEqual(EXPECTED_SKIP_MARKERS, len(marker_lines))
        for line in marker_lines:
            self.assertRegex(line, r"\(\d+\):\s*MESSAGE:")
            self.assertFalse(
                line.lstrip().startswith("Skipping"),
                f"marker unexpectedly starts its line: {line!r}",
            )

    def test_detection_does_not_depend_on_the_capturing_machine_path(self) -> None:
        """A fixture that only matches the machine it was taken on is worthless.

        The captured paths are absolute and MIX separators
        (`C:\\Projects\\wt-595\\modules/gaussian_splatting/...`). Nothing in the
        detector may key on that, so the count must survive rewriting the root
        to another drive, to a POSIX root, and to a bare relative path.
        """
        text = self._fixture_text()
        original = len(harness.DOCTEST_SKIP_MARKER_RE.findall(text))
        self.assertEqual(EXPECTED_SKIP_MARKERS, original)
        for old, new in (
            ("C:\\Projects\\wt-595\\", "D:\\somewhere else\\"),
            ("C:\\Projects\\wt-595\\", "/home/runner/godotgs/"),
            ("C:\\Projects\\wt-595\\", ""),
            ("\\", "/"),
        ):
            rewritten = text.replace(old, new)
            self.assertEqual(
                original,
                len(harness.DOCTEST_SKIP_MARKER_RE.findall(rewritten)),
                f"count changed after rewriting {old!r} -> {new!r}",
            )

    def test_warn_print_lines_in_the_capture_are_not_counted(self) -> None:
        """The documented static/runtime asymmetry, pinned against real output.

        The capture contains real `WARN_PRINT` skips. They are static-only sites
        by design: counting Godot's `WARNING:` framing at runtime would also
        count production logging (this very capture contains a renderer line
        reading "...collected but skipped because no renderer can be attached").
        """
        text = self._fixture_text()
        warn_lines = [
            ln for ln in text.splitlines() if ln.startswith("WARNING:") and "skipping" in ln
        ]
        self.assertEqual(EXPECTED_UNCOUNTED_WARN_LINES, len(warn_lines))
        for line in warn_lines:
            self.assertEqual(0, len(harness.DOCTEST_SKIP_MARKER_RE.findall(line)), line)

    def test_production_log_line_mentioning_skipped_is_not_counted(self) -> None:
        """The concrete false positive a looser detector would produce."""
        text = self._fixture_text()
        production = [
            ln for ln in text.splitlines() if "collected but skipped" in ln
        ]
        self.assertEqual(1, len(production), "capture no longer contains the production line")
        self.assertEqual(0, len(harness.DOCTEST_SKIP_MARKER_RE.findall(production[0])))

    def test_capture_shows_every_skip_scored_as_a_pass(self) -> None:
        """The defect itself, stated as an assertion against real output."""
        text = self._fixture_text()
        self.assertRegex(text, r"test cases:\s*9\s*\|\s*9 passed\s*\|\s*0 failed")
        self.assertEqual(EXPECTED_SKIP_MARKERS, len(harness.DOCTEST_SKIP_MARKER_RE.findall(text)))


class RealShapeTests(unittest.TestCase):
    """The console shapes the detector must survive.

    Each line here is the exact framing doctest's ConsoleReporter produces:
    `file_line_to_stream()` writes `<path>(<line>): ` (or `<path>:<line>: ` under
    --gnu-file-line), then `log_message()` writes the colour code, `MESSAGE`,
    `": "`, a Color::None reset, and the message body.
    """

    CANONICAL = (
        "modules/gaussian_splatting/tests/test_painterly_pipeline.h(473): "
        "MESSAGE: GS_ENV_SKIP: RenderingDevice unavailable\n"
    )
    LEGACY = (
        "modules/gaussian_splatting/tests/test_gaussian_splat_node.h(944): "
        "MESSAGE: Skipping test - renderer unavailable (headless mode)\n"
    )

    def test_canonical_token_is_counted(self) -> None:
        self.assertEqual(1, _markers(self.CANONICAL))

    def test_legacy_prose_is_counted(self) -> None:
        """The ~354 unconverted sites must stay in the number until slice
        GS-595-B converts them; dropping them would shrink the count while
        growing the hidden surface."""
        self.assertEqual(1, _markers(self.LEGACY))

    def test_canonical_token_is_not_double_counted(self) -> None:
        """Discriminating version of an earlier VACUOUS assertion.

        The previous body was byte-identical to test_canonical_token_is_counted,
        so it asserted nothing extra. This uses a line that carries the token AND
        skip prose after it -- the shape a naive
        `GS_ENV_SKIP:|MESSAGE:.*Skip` union counts twice. One emitted MESSAGE is
        one skip, whatever its text.
        """
        line = (
            "test_x.h(10): MESSAGE: GS_ENV_SKIP: Skipping test - "
            "RenderingDevice unavailable\n"
        )
        self.assertEqual(1, _markers(line))

    def test_bare_token_without_message_framing_is_not_counted(self) -> None:
        """F7: with an allowance of 0, ONE false positive fails a lane.

        The token used to be matched anywhere in the stream, so any log line
        quoting it -- a build log, a grep echoed into CI output, this very
        docstring in a traceback -- counted as a skipped test.
        """
        for line in (
            "GS_ENV_SKIP: this is a bare log line, not a doctest message\n",
            "[module-tests] detector token is GS_ENV_SKIP: (documentation)\n",
            "ERROR: something mentioning GS_ENV_SKIP: in passing\n",
        ):
            self.assertEqual(0, _markers(line), line)

    def test_lowercase_prose_is_counted_like_the_static_guard(self) -> None:
        """F7: the static guard is IGNORECASE; the runtime branch used not to be.

        Measured on this corpus the two agree exactly (354 sites either way), so
        this pins a definitional match rather than a behavioural change.
        """
        self.assertEqual(1, _markers("test_x.h(10): MESSAGE: skipping test - lowercase\n"))
        self.assertEqual(1, _markers("test_x.h(10): MESSAGE: SKIPPED - shouting\n"))

    def test_windows_absolute_path_prefix(self) -> None:
        line = (
            "C:\\Projects\\godotgs-clean\\modules\\gaussian_splatting\\tests\\"
            "test_gpu_sorting.h(632): MESSAGE: GS_ENV_SKIP: streaming unavailable\n"
        )
        self.assertEqual(1, _markers(line))

    def test_gnu_file_line_prefix(self) -> None:
        line = (
            "modules/gaussian_splatting/tests/test_diagnostics.h:135: "
            "MESSAGE: Skipping test - ProjectSettings unavailable\n"
        )
        self.assertEqual(1, _markers(line))

    def test_ansi_colour_between_message_and_body(self) -> None:
        """log_message writes `<< Color::None <<` between `MESSAGE: ` and the
        body, which on a colour-capable terminal is an ANSI escape."""
        line = (
            "test_x.h(10): \x1b[0;37mMESSAGE\x1b[0m: \x1b[0m"
            "Skipping test - RenderingDevice unavailable\n"
        )
        self.assertEqual(1, _markers(line))
        canonical = "test_x.h(10): \x1b[0;37mMESSAGE\x1b[0m: \x1b[0mGS_ENV_SKIP: no device\n"
        self.assertEqual(1, _markers(canonical))

    def test_embedded_prose_form_is_knowingly_not_counted(self) -> None:
        """The runtime detector's documented boundary, matching the static
        guard's shape contract exactly.

        These are real environment skips that this detector does not see. The
        exclusion is pinned so the two detectors cannot drift apart, and so the
        per-lane allowance measured below keeps meaning one specific thing.
        Closing the gap is follow-on GS-595-E.
        """
        for line in (
            "test_ply_importer.h(198): MESSAGE: Cache file not created "
            "(caching may be disabled); skipping version guard test\n",
            "tile_renderer_regression_test.cpp(1645): MESSAGE: [TileRenderer] "
            "RenderingServer not available, skipping regression tests\n",
            "test_node_bootstrap.h(106): MESSAGE: Renderer unavailable "
            "(headless mode) - skipping renderer state checks\n",
        ):
            self.assertEqual(0, _markers(line), line)

    def test_ordinary_message_is_not_counted(self) -> None:
        line = (
            "test_renderer_pipeline.h(3675): MESSAGE: Pre-teardown culler-backed "
            "sample: lod_enabled=true frustum_culling=true\n"
        )
        self.assertEqual(0, _markers(line))

    def test_failed_assertion_text_is_not_counted(self) -> None:
        line = "test_x.h(10): ERROR: CHECK( a == b ) is NOT correct!\n"
        self.assertEqual(0, _markers(line))

    def test_baseline_pattern_is_inert_on_every_real_shape(self) -> None:
        """A4, against the shapes rather than the capture: whatever the capture
        happens to contain, a line-anchored pattern cannot see any of these."""
        for sample in (
            self.CANONICAL,
            self.LEGACY,
            "test_x.h(10): \x1b[0;37mMESSAGE\x1b[0m: \x1b[0mSkipping test - x\n",
            "modules/gaussian_splatting/tests/test_diagnostics.h:135: "
            "MESSAGE: Skipping test - ProjectSettings unavailable\n",
        ):
            self.assertEqual([], BASELINE_SKIP_MARKER_RE.findall(sample), sample)


class ParseWiringTests(unittest.TestCase):
    """The count must reach _parse_doctest_results, not just the regex."""

    def test_parse_doctest_results_reports_the_marker_count(self) -> None:
        output = (
            "test_a.h(1): MESSAGE: GS_ENV_SKIP: RenderingDevice unavailable\n"
            "test_b.h(2): MESSAGE: Skipping test - renderer unavailable\n"
            "test_c.h(3): MESSAGE: not a skip at all\n" + SUMMARY
        )
        (
            passed_tests,
            failed_tests,
            passed_asserts,
            failed_asserts,
            skip_markers,
            summary_found,
        ) = harness._parse_doctest_results(output)
        self.assertTrue(summary_found)
        self.assertEqual(9, passed_tests)
        self.assertEqual(2, skip_markers)

    def test_baseline_output_shape_yields_zero_before_the_fix(self) -> None:
        """The measured baseline behaviour, stated as a test: on output whose
        markers are all `MESSAGE: Skipping …`, the old pattern reports 0 while
        doctest reports every case passed. That combination is the bug."""
        output = (
            "test_a.h(1): MESSAGE: Skipping test - RenderingDevice unavailable\n"
            "test_b.h(2): MESSAGE: Skipping test - streaming unavailable\n"
            "test_c.h(3): MESSAGE: Skipping test - worker thread pool unavailable\n"
            + SUMMARY
        )
        self.assertEqual(0, len(BASELINE_SKIP_MARKER_RE.findall(output)))
        self.assertEqual(3, _markers(output))


REAL_LANE = harness.MODULE_TEST_FILTERS[0][0]


def _valid_allowance_entry(allowed: int, *, expires_days: int = 90) -> dict:
    expiry = datetime.now(timezone.utc) + timedelta(days=expires_days)
    return {
        "allowed": allowed,
        "owner": "gaussian-splatting-module",
        "reason": "pre-existing environment skips frozen by #595; converted in GS-595-B.",
        "issue_url": "https://github.com/klausi3D/godotGS/issues/595",
        "expires_utc": expiry.isoformat(),
    }


@contextlib.contextmanager
def _allowance(mapping: dict):
    """Point the harness at a temporary baseline carrying `mapping`."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "environment_skip_baseline.json"
        path.write_text(
            json.dumps({"files": {}, "runtime_lane_allowance": mapping}), encoding="utf-8"
        )
        saved = harness.ENVIRONMENT_SKIP_BASELINE_PATH
        harness.ENVIRONMENT_SKIP_BASELINE_PATH = path
        try:
            yield
        finally:
            harness.ENVIRONMENT_SKIP_BASELINE_PATH = saved


class LaneAllowanceTests(unittest.TestCase):
    """The tolerance that keeps this slice from converting skips into failures.

    Enforcement is not weakened by it: an absent lane means an allowance of
    zero, every entry must name an owner and an expiry, and an expired entry
    drops to zero rather than being honoured.
    """

    def test_unknown_lane_gets_zero_while_a_named_lane_keeps_its_value(self) -> None:
        """Discriminating version of an earlier VACUOUS assertion.

        The previous test asserted `allowance.get(unknown, 0) == 0`, which is a
        property of `dict.get`, not of the code -- it passed against a mapping
        that gave every lane 999. This one pins both halves: the named lane must
        come back with its measured number, and the unnamed lane must be absent
        (so the caller's `.get(name, 0)` yields 0).
        """
        with _allowance({REAL_LANE: _valid_allowance_entry(4)}):
            allowance = harness._environment_skip_lane_allowance()
        self.assertEqual(4, allowance[REAL_LANE])
        self.assertNotIn("GaussianSplatting [NoSuchLane]", allowance)
        self.assertEqual(0, allowance.get("GaussianSplatting [NoSuchLane]", 0))

    def test_bare_integer_entry_is_rejected(self) -> None:
        """A silencer with no owner and no expiry is not an allowance."""
        with _allowance({REAL_LANE: 3}):
            with self.assertRaises(RuntimeError) as caught:
                harness._environment_skip_lane_allowance()
        self.assertIn("must be an object", str(caught.exception))

    def test_entry_missing_any_required_field_is_rejected(self) -> None:
        for field in ("allowed", "owner", "reason", "issue_url", "expires_utc"):
            entry = _valid_allowance_entry(2)
            entry.pop(field)
            with _allowance({REAL_LANE: entry}):
                with self.assertRaises(RuntimeError) as caught:
                    harness._environment_skip_lane_allowance()
            self.assertIn(field, str(caught.exception))

    def test_expired_entry_drops_to_zero_rather_than_being_honoured(self) -> None:
        """An allowance nobody renews must tighten by itself."""
        entry = _valid_allowance_entry(5, expires_days=-1)
        with _allowance({REAL_LANE: entry}):
            allowance = harness._environment_skip_lane_allowance()
        self.assertEqual(0, allowance[REAL_LANE])

    def test_unknown_lane_name_in_the_file_is_rejected(self) -> None:
        """A stale lane name is an allowance that silently applies to nothing."""
        with _allowance({"GaussianSplatting [LaneThatWasRenamed]": _valid_allowance_entry(1)}):
            with self.assertRaises(RuntimeError) as caught:
                harness._environment_skip_lane_allowance()
        self.assertIn("not in MODULE_TEST_FILTERS", str(caught.exception))

    def test_allowance_rejects_a_negative_or_non_integer_count(self) -> None:
        for bad in (-1, "3", True, 1.5):
            entry = _valid_allowance_entry(0)
            entry["allowed"] = bad
            with _allowance({REAL_LANE: entry}):
                with self.assertRaises(RuntimeError) as caught:
                    harness._environment_skip_lane_allowance()
            self.assertIn("non-negative integer", str(caught.exception))

    def test_missing_baseline_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            saved = harness.ENVIRONMENT_SKIP_BASELINE_PATH
            harness.ENVIRONMENT_SKIP_BASELINE_PATH = Path(tmp) / "absent.json"
            try:
                with self.assertRaises(RuntimeError):
                    harness._environment_skip_lane_allowance()
            finally:
                harness.ENVIRONMENT_SKIP_BASELINE_PATH = saved

    def test_unusable_allowance_becomes_a_lane_failure_not_a_traceback(self) -> None:
        """F9: fail closed, but as a lane failure a human can act on."""
        with tempfile.TemporaryDirectory() as tmp:
            saved = harness.ENVIRONMENT_SKIP_BASELINE_PATH
            harness.ENVIRONMENT_SKIP_BASELINE_PATH = Path(tmp) / "absent.json"
            buffer = io.StringIO()
            try:
                with mock.patch.dict(os.environ, {"CI": "1"}):
                    with contextlib.redirect_stdout(buffer):
                        result = harness._enforce_skipped_marker_policy(
                            REAL_LANE, True, "output", 1
                        )
            finally:
                harness.ENVIRONMENT_SKIP_BASELINE_PATH = saved
        self.assertFalse(result)
        self.assertIn("environment-skip allowance is unusable", buffer.getvalue())

    def test_policy_fails_when_markers_exceed_the_allowance(self) -> None:
        with _allowance({REAL_LANE: _valid_allowance_entry(2)}):
            with mock.patch.dict(os.environ, {"CI": "1"}):
                buffer = io.StringIO()
                with contextlib.redirect_stdout(buffer):
                    over = harness._enforce_skipped_marker_policy(REAL_LANE, True, "out", 3)
                    within = harness._enforce_skipped_marker_policy(REAL_LANE, True, "out", 2)
        self.assertFalse(over)
        self.assertTrue(within)


class BaseForwardingTests(unittest.TestCase):
    """(l) The wrapper must hand the review base to the guard subprocess.

    resolve_base_sha() being correct inside the guard is worth nothing if the
    caller never tells it which base to use: the guard's own fallback chain ends
    at origin/master, which is right only for PRs targeting master. The PRs that
    most need the ratchet are the stacked ones that do not (#821 targets
    gs/595-env-skip-marker, #822 targets gs/650-quarantine-ratchet), and there
    the ratchet would have graded against the wrong branch and reported green.
    """

    def _capture(self, env: dict[str, str], override: str | None = None):
        calls: list[list[str]] = []

        def fake_run(command, *args, **kwargs):
            calls.append(list(command))
            return 0, "", ""

        saved_override = harness._GUARD_BASE_REF_OVERRIDE
        harness._GUARD_BASE_REF_OVERRIDE = override
        with mock.patch.object(harness, "_run_command", fake_run):
            with mock.patch.dict(os.environ, env, clear=False):
                for name in harness.ENVIRONMENT_SKIP_BASE_ENV_VARS:
                    if name not in env:
                        os.environ.pop(name, None)
                # The base requirement now depends on the EVENT, so the ambient
                # GITHUB_EVENT_NAME must be controlled too. Without this, running
                # the suite under `GITHUB_EVENT_NAME=push` silently changes what
                # these cases assert -- which is exactly how it failed once.
                if "GITHUB_EVENT_NAME" not in env:
                    os.environ["GITHUB_EVENT_NAME"] = "pull_request"
                try:
                    ok, output = harness._run_environment_skip_marker_guard()
                finally:
                    harness._GUARD_BASE_REF_OVERRIDE = saved_override
        return ok, output, calls

    def _guard_call(self, calls: list[list[str]]) -> list[str]:
        guard_script = str(harness.ENVIRONMENT_SKIP_GUARD_SCRIPT)
        for command in calls:
            if guard_script in command:
                return command
        self.fail(f"guard script was never invoked; calls={calls}")

    def test_base_from_a_non_master_pr_is_forwarded(self) -> None:
        ok, output, calls = self._capture(
            {"CI": "1", "GITHUB_BASE_SHA": "gs/595-env-skip-marker"}
        )
        self.assertTrue(ok, output)
        command = self._guard_call(calls)
        self.assertIn("--base-ref", command)
        self.assertEqual(
            "gs/595-env-skip-marker", command[command.index("--base-ref") + 1]
        )

    def test_explicit_base_ref_wins(self) -> None:
        ok, output, calls = self._capture(
            {"CI": "1", "GITHUB_BASE_SHA": "ignored"}, override="gs/650-quarantine-ratchet"
        )
        self.assertTrue(ok, output)
        command = self._guard_call(calls)
        self.assertEqual(
            "gs/650-quarantine-ratchet", command[command.index("--base-ref") + 1]
        )

    def test_ci_without_any_base_fails_closed(self) -> None:
        """'Defaulted to master' and 'confirmed against the real base' must not
        share an encoding."""
        ok, output, calls = self._capture({"CI": "1"})
        self.assertFalse(ok)
        self.assertTrue(any("no review base available in CI" in line for line in output), output)
        self.assertEqual([], calls, "the guard must not run at all without a base")

    def test_local_run_without_a_base_is_allowed(self) -> None:
        """Local ergonomics: the guard falls back to its documented defaults,
        and no merge decision rests on a local run."""
        ok, output, calls = self._capture({"CI": ""})
        self.assertTrue(ok, output)
        command = self._guard_call(calls)
        self.assertNotIn("--base-ref", command)


class WorkflowBaseExportTests(unittest.TestCase):
    """Every base-bearing event the gate triggers on must be given a base.

    The guard fails closed without one. agentic_pr_gate.yml triggers on
    `pull_request` AND `merge_group` but exported a base only for the former, so
    every merge-queue run of a required check would have been blocked -- a
    defect introduced by making the guard correctly refuse.

    The event list is DERIVED from the workflow's own `on:` block rather than
    written out here. A hand-written list is how the next event type gets
    missed, and this repo has been bitten by that repeatedly -- including twice
    on this very branch (the hand-written macro list, twice over).
    """

    WORKFLOW = ROOT / ".github" / "workflows" / "agentic_pr_gate.yml"

    def _trigger_events(self) -> set[str]:
        """Event names under the workflow's top-level `on:` key."""
        text = self.WORKFLOW.read_text(encoding="utf-8")
        lines = text.splitlines()
        try:
            start = next(i for i, line in enumerate(lines) if re.match(r"^on:\s*$", line))
        except StopIteration:
            self.fail(f"{self.WORKFLOW.name} has no top-level 'on:' block")
        events: set[str] = set()
        for line in lines[start + 1 :]:
            if line and not line[0].isspace():
                break
            match = re.match(r"^  ([A-Za-z_][A-Za-z0-9_]*):", line)
            if match:
                events.add(match.group(1))
        self.assertTrue(events, "derived no trigger events; the parser is broken")
        return events

    def _guard_step_env(self) -> str:
        """The `env:` block attached to the guard-only step."""
        text = self.WORKFLOW.read_text(encoding="utf-8")
        marker = "run: python tests/ci/run_module_tests.py --guard-only"
        self.assertIn(marker, text, "the guard-only step moved or was renamed")
        # The step's env: block precedes its run: line within the same step.
        step_start = text.rindex("- name:", 0, text.index(marker))
        return text[step_start : text.index(marker)]

    def test_every_base_bearing_trigger_is_covered_by_the_export(self) -> None:
        events = self._trigger_events()
        base_bearing = events & harness.BASE_BEARING_EVENTS
        self.assertTrue(
            base_bearing,
            f"no base-bearing trigger found in {sorted(events)}; if the workflow "
            f"genuinely no longer runs on one, this test should be removed deliberately",
        )
        env_block = self._guard_step_env()
        self.assertIn(
            "GS_CI_BASE_REF",
            env_block,
            "the guard-only step exports no review base; the guard fails closed "
            "without one, so this blocks the gate",
        )
        for event in sorted(base_bearing):
            if event == "pull_request":
                self.assertIn("github.event.pull_request.base.sha", env_block)
            elif event == "merge_group":
                self.assertIn(
                    "github.event.merge_group.base_sha",
                    env_block,
                    "merge_group triggers this workflow but no merge-queue base is "
                    "exported; every merge-queue run would fail closed",
                )
            else:
                self.fail(
                    f"trigger '{event}' is base-bearing but this test does not know how "
                    f"to check its export -- add it rather than letting it pass silently"
                )

    def test_non_base_bearing_events_do_not_require_a_base(self) -> None:
        """push / schedule / workflow_dispatch have no review base at all.

        gaussian_production_gates.yml runs --guard-only on those events too, so
        demanding a base there would fail a required gate on every push.
        """
        for event in ("push", "schedule", "workflow_dispatch"):
            with mock.patch.dict(
                os.environ, {"CI": "1", "GITHUB_EVENT_NAME": event}, clear=False
            ):
                for name in harness.ENVIRONMENT_SKIP_BASE_ENV_VARS:
                    os.environ.pop(name, None)
                saved = harness._GUARD_BASE_REF_OVERRIDE
                harness._GUARD_BASE_REF_OVERRIDE = None
                try:
                    base_ref, failures = harness._environment_skip_base_ref()
                finally:
                    harness._GUARD_BASE_REF_OVERRIDE = saved
            self.assertEqual([], failures, event)
            self.assertIsNone(base_ref, event)

    def test_base_bearing_event_without_a_base_still_fails_closed(self) -> None:
        for event in sorted(harness.BASE_BEARING_EVENTS):
            with mock.patch.dict(
                os.environ, {"CI": "1", "GITHUB_EVENT_NAME": event}, clear=False
            ):
                for name in harness.ENVIRONMENT_SKIP_BASE_ENV_VARS:
                    os.environ.pop(name, None)
                saved = harness._GUARD_BASE_REF_OVERRIDE
                harness._GUARD_BASE_REF_OVERRIDE = None
                try:
                    base_ref, failures = harness._environment_skip_base_ref()
                finally:
                    harness._GUARD_BASE_REF_OVERRIDE = saved
            self.assertIsNone(base_ref, event)
            self.assertTrue(failures, f"{event} should require a base")


if __name__ == "__main__":
    unittest.main(verbosity=2)
