#!/usr/bin/env python3
"""
Baseline QA Test Runner for Gaussian Splatting CI
Runs the core test scripts and validates results with proper error reporting
"""

import argparse
import copy
import re
import subprocess
import sys
import json
import os
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_QA_BASELINE_PATH = ROOT / "tests" / "ci" / "baselines" / "qa_results.json"
DEFAULT_BASELINE_REPORT_PATH = ROOT / "baseline_qa_regression_report.json"
DEFAULT_BASELINE_SUMMARY_PATH = ROOT / "baseline_qa_regression_summary.md"
SYNTHETIC_ASSET_PREP_SCRIPT = ROOT / "tests" / "runtime" / "prepare_synthetic_assets.py"

MINIMUM_SSIM_DROP = 0.02
MINIMUM_FPS_RATIO = 0.85
MAXIMUM_TIME_RATIO = 1.20
# Floor for the measured red-over-blue pixel dominance that proves back-to-front
# sort order. Chosen so the committed baselines (0.404 measured on CI's -O0
# build, 0.440 on an optimized build) stay comfortably inside it while the
# resulting absolute floor -- 0.404 * 0.70 = 0.283 -- remains well ABOVE the
# scenes' own 0.15 acceptance gate. That ordering is the point: if this rule
# were looser than the scene's own assertion it would add nothing.
MINIMUM_DOMINANCE_RATIO = 0.70
TEST_CATEGORIES = ("ply", "pipeline", "sorting", "runtime", "module", "qa")
CATEGORY_ALIASES = {"all": None}
CLI_CATEGORY_CHOICES = tuple(CATEGORY_ALIASES.keys()) + TEST_CATEGORIES

# Windows display driver + Vulkan renderer + safe render thread. The headless
# display driver cannot create a RenderingDevice, so anything that has to
# render (or capture what it rendered) needs these; the Windows driver works
# even on the service-mode self-hosted runner, while --render-thread separate
# does not (see #104). Single-sourced so the `type: godot` dispatch and the QA
# Scene Suite argv cannot drift apart.
GPU_DISPLAY_ARGS = (
    "--display-driver",
    "windows",
    "--rendering-driver",
    "vulkan",
    "--render-thread",
    "safe",
)

# A missing QA baseline is a coverage gap, not a pass. This script must not
# fabricate one or silently treat "nothing to compare against" as success
# without saying so loudly. --require-qa-baseline (or this env var) turns the
# gap into a hard failure; baseline_qa.yml's qa-visual lane sets it.
REQUIRE_QA_BASELINE_ENV = "GS_CI_REQUIRE_QA_BASELINE"
QA_BASELINE_COVERAGE_GAP_NOTE = (
    "QA baseline regression detection is not enforced: no golden baseline was "
    "found. This is a coverage gap, not a verified pass. Pass "
    "--require-qa-baseline (or set "
    f"{REQUIRE_QA_BASELINE_ENV}=1) to make this fail instead of skip."
)

# The QA Scene Suite only produces real signal when it can create a
# RenderingDevice and read back a viewport. Under --headless every capture
# returns null, every SSIM scene reports "no capture", and the run is
# laundered into a skip. A lane that promises a GPU must therefore also
# promise capture: --qa-require-capture runs the suite on the real display
# AND refuses to accept a skip as a pass.
QA_REQUIRE_CAPTURE_ENV = "GS_CI_GPU_REQUIRED"
QA_CAPTURE_REQUIRED_BUT_SKIPPED_MESSAGE = (
    "QA Scene Suite produced no results, but capture was declared REQUIRED "
    "(--qa-require-capture / "
    f"{QA_REQUIRE_CAPTURE_ENV}=1). On a lane that promises a GPU, a suite that "
    "never captured is a failure of that promise, not a legitimate skip. "
    "Refusing to launder it into a pass."
)

# Metrics whose value is a property of the machine, not of the renderer.
# Pinning them in a committed baseline makes the gate measure runner
# contention: a blocking lane would go red because CI was busy, not because
# anything regressed (see #630/#624 for the same trap in the frame-time gate).
# The scenes still assert their own absolute budgets internally
# (qa_performance_budget: min_fps=20 vs ~1200 observed), which is where a
# perf assertion belongs.
NON_DETERMINISTIC_BASELINE_METRIC_MARKERS = ("fps", "frame_time", "_ms")

# A metric whose value is a serialized numeric tuple — Godot renders Color and
# Vector* as "(1.0, 0.4976, 0.5603, 1.0)" — is a MEASUREMENT that merely looks
# like a string. It must never be compared for exact equality.
#
# Learned from CI, not from theory: the first run of this lane on the
# self-hosted runner failed on qa_sort_depth_order.center_color, baseline
# '(1.0, 0.4976, 0.5603, 1.0)' vs current '(1.0, 0.498, 0.5956, 1.0)'. Both
# runs were correct — the local baseline came from an optimized editor build
# and CI builds dev_build=yes (-O0), and the rasterized blue channel differs by
# ~6% between them. Exact-matching a rendered pixel value pins the build
# configuration, not the renderer's behaviour. The scenes still assert the
# pixel relationship themselves (red_minus_blue >= 0.15 and r > 0.4, which CI
# passed at 0.404), which is where a tolerance-bearing assertion belongs.
SERIALIZED_NUMERIC_TUPLE_RE = re.compile(r"^\(\s*-?\d+(\.\d+)?(\s*,\s*-?\d+(\.\d+)?)+\s*\)$")


REQUIRE_QA_BASELINE_INERT_MESSAGE = (
    "--require-qa-baseline/"
    f"{REQUIRE_QA_BASELINE_ENV} was requested, but this invocation does not run "
    "the 'qa' category, so QA-baseline enforcement cannot apply to anything. "
    "Refusing to report a pass for an enforcement request that is structurally "
    "inert. Select the 'qa' category (e.g. --category qa or --categories "
    "qa,...) or drop the switch."
)


def resolve_qa_ran(
    *,
    category: Optional[str],
    categories: Optional[set],
    quick: bool,
) -> bool:
    """Whether this invocation actually runs the ``qa`` category.

    Everything about QA-baseline comparison/enforcement is gated on this, so
    it is the single fact that decides whether --require-qa-baseline can do
    anything at all. `category` and `categories` are already normalized;
    ``None`` inside them means "all categories".
    """
    if categories is not None:
        return (None in categories) or ("qa" in categories)
    if category is not None:
        return category == "qa"
    return not quick


def require_baseline_applies(require_baseline_effective: bool, qa_ran: bool) -> bool:
    """False when enforcement was requested but is structurally inert.

    Asking for --require-qa-baseline on an invocation that never runs the
    ``qa`` category used to be a silent no-op: the request evaporated and the
    run still exited 0 — the exact laundering this switch exists to prevent.
    Callers must fail the run in that case rather than ignore it. No workflow
    passes the flag or sets the env var today (see resolve_qa_ran's callers
    and the workflow inventory in _record_qa_baseline_skipped), so failing
    closed here cannot break an existing lane; it only stops a future
    misconfiguration from landing quietly.
    """
    if require_baseline_effective and not qa_ran:
        return False
    return True


def _env_flag(name: str, default: bool = False) -> bool:
    """Mirror the GDScript CI harness's env-flag parsing (tests/ci/test_gpu_sorting_ci.gd)."""
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _format_metric_value(value: Any) -> str:
    """Render a metric for a human-readable regression line.

    Regressions are no longer numeric-only: path-identity metrics (route_uid,
    data_source, stage_*_status) are compared for exact equality, and a missing
    metric surfaces as None. The reporter previously forced every field through
    float(), so the first string regression it ever saw crashed the run with a
    ValueError instead of printing the failure — a red path that could not
    report red.
    """
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return repr(value)
    return f"{float(value):.6f}"


def is_serialized_numeric_tuple(value: Any) -> bool:
    """True for a Color/Vector rendered as a string, e.g. "(1.0, 0.5, 0.6, 1.0)".

    Such a value is a measurement wearing a string's clothes. See
    SERIALIZED_NUMERIC_TUPLE_RE for the CI failure that motivated this.
    """
    return isinstance(value, str) and bool(SERIALIZED_NUMERIC_TUPLE_RE.match(value.strip()))


def is_non_deterministic_baseline_metric(metric_name: str, value: Any = None) -> bool:
    """True for metrics whose value describes the runner or the build, not the
    renderer's behaviour.

    Two independent tests, because two different things leak in:

    * by NAME — NON_DETERMINISTIC_BASELINE_METRIC_MARKERS catches fps and
      timings. A predicate rather than a hardcoded list because the QA scenes
      emit names dynamically (`near_*`/`far_*` in qa_sort_multi_instance), so a
      frozen list would silently miss new ones — the failure mode called out in
      the repo's derive-don't-enumerate guidance.
    * by VALUE SHAPE — a serialized numeric tuple is a rendered pixel value,
      which differs between an optimized local build and CI's -O0 dev build.
      Name-based classification could not have caught `center_color`: nothing
      about that name says "float vector".
    """
    lowered = metric_name.lower()
    if any(marker in lowered for marker in NON_DETERMINISTIC_BASELINE_METRIC_MARKERS):
        return True
    return is_serialized_numeric_tuple(value)


def validate_baseline_candidate(
    results: List[Dict[str, Any]], *, already_sanitized: bool = False
) -> List[str]:
    """Reasons this QA output must NOT be frozen as a golden baseline.

    Empty list == acceptable. This exists because promoting a bad run is
    silently unrecoverable: the comparator only ever checks
    ``current >= baseline - 0.02`` for SSIM, so a baseline that recorded
    ``ssim_min = 0.0`` (the value calculate_ssim used to return for a NULL
    capture) can never fail again. The gate would be permanently green while
    testing nothing — the exact vacuous-pass shape this lane exists to
    eliminate. Every rejection below describes a run that did not actually
    render, so its numbers are not truth about the renderer.

    ``already_sanitized`` marks the input as the OUTPUT of
    strip_non_deterministic_metrics() rather than a raw run. It relaxes exactly
    one rule: a metric-less scene. On a raw run that means "this scene captured
    nothing" and must be rejected; on a sanitized baseline it can simply mean
    every metric the scene reports was machine-dependent and was stripped
    (qa_performance_budget reports only FPS and frame-time figures). Without
    the distinction the function is not idempotent over its own output — it
    would reject the very baseline it just approved.
    """
    reasons: List[str] = []
    if not results:
        reasons.append("QA output contains no scene results at all.")
        return reasons

    for entry in results:
        scene = str(entry.get("scene", "<unnamed>"))
        if not entry.get("passed", False):
            reasons.append(f"{scene}: scene FAILED; a failing run must never become the baseline.")
        if entry.get("skipped", False):
            reasons.append(f"{scene}: scene SKIPPED; a skip frozen as a baseline is a permanent blind spot.")
        message = str(entry.get("message", ""))
        if "[QA_SKIP]" in message:
            reasons.append(f"{scene}: self-skipped ({message.strip()}); it never rendered.")

        metrics = entry.get("metrics")
        if not isinstance(metrics, dict):
            reasons.append(f"{scene}: 'metrics' is not an object, so it contributes nothing comparable.")
            continue
        if not metrics:
            if not already_sanitized:
                reasons.append(f"{scene}: reported no metrics, so it contributes nothing comparable.")
            continue

        comparable = {
            name: value
            for name, value in metrics.items()
            if not is_non_deterministic_baseline_metric(name, value)
        }
        ssim_thresholds = [
            name for name in comparable
            if "ssim" in name.lower() and "threshold" in name.lower()
        ]
        ssim_measurements = [
            name for name in comparable
            if "ssim" in name.lower() and "threshold" not in name.lower()
        ]
        if ssim_thresholds and not ssim_measurements:
            reasons.append(
                f"{scene}: SSIM acceptance threshold(s) {', '.join(sorted(ssim_thresholds))} "
                "are present but no SSIM measurement was recorded; this scene captured nothing comparable."
            )
        for name, value in comparable.items():
            lowered_name = name.lower()
            if "ssim" in lowered_name and "threshold" not in lowered_name:
                if not isinstance(value, (int, float)) or isinstance(value, bool):
                    reasons.append(f"{scene}.{name}: SSIM metric is not numeric ({value!r}).")
                elif value != value:  # NaN — calculate_ssim's capture-failure sentinel.
                    reasons.append(f"{scene}.{name}: SSIM is NaN, meaning the capture failed.")
                elif value <= 0.0:
                    reasons.append(
                        f"{scene}.{name}: SSIM is {value}, which no successful capture produces; "
                        "this run did not render."
                    )
            if lowered_name.endswith("tie_break_margin"):
                if not isinstance(value, (int, float)) or isinstance(value, bool):
                    reasons.append(f"{scene}.{name}: tie-break margin is not numeric ({value!r}).")
                elif value != value or value <= 0.0:
                    reasons.append(
                        f"{scene}.{name}: tie-break margin is {value}; "
                        "a non-positive margin cannot prove which ordering signal won."
                    )
    return reasons


def strip_non_deterministic_metrics(payload: Dict[str, Any]) -> Tuple[Dict[str, Any], List[str]]:
    """Return a copy of a QA payload with machine-dependent metrics removed.

    A committed baseline is compared by a BLOCKING lane, so anything that
    tracks runner contention rather than renderer behaviour has to go or the
    gate reports load, not regressions. Returns the sanitized payload and the
    sorted list of dropped ``scene.metric`` keys so the drop is auditable
    instead of invisible.

    A scene can legitimately end up with an EMPTY metrics dict here — every
    metric qa_performance_budget reports is an FPS or frame-time figure. The
    entry is deliberately kept rather than removed: it still pins the scene's
    existence, so the comparator's ``missing_scenes`` check fails if the scene
    silently drops out of the suite. (validate_baseline_candidate() rejects a
    metric-less scene, but it runs on the raw pre-strip run, where that means
    "captured nothing" — a different fact from "captured only machine-dependent
    numbers".)
    """
    sanitized = copy.deepcopy(payload)
    dropped: List[str] = []
    for entry in sanitized.get("results", []):
        metrics = entry.get("metrics")
        if not isinstance(metrics, dict):
            continue
        scene = str(entry.get("scene", "<unnamed>"))
        for name in sorted(metrics):
            if is_non_deterministic_baseline_metric(name, metrics[name]):
                del metrics[name]
                dropped.append(f"{scene}.{name}")
    return sanitized, dropped


def _ci_warning(message: str) -> None:
    """Print a [WARN] line plus, under GitHub Actions, a `::warning::`
    annotation — so a coverage gap shows up in the Checks UI/PR summary
    instead of only in step logs someone has to open and scroll through."""
    print(f"[WARN] {message}")
    if os.environ.get("GITHUB_ACTIONS") == "true":
        # https://docs.github.com/actions/using-workflows/workflow-commands-for-github-actions
        escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::warning title=QA baseline coverage gap::{escaped}")


def resolve_root_path(path_value: str) -> Path:
    """Resolve relative paths from repository root for stable CI behavior."""
    path = Path(path_value)
    if path.is_absolute():
        return path
    return ROOT / path


def normalize_test_category(category: Optional[str]) -> Optional[str]:
    """Normalize CLI category aliases to canonical internal categories."""
    if category is None:
        return None
    if category in TEST_CATEGORIES:
        return category
    if category in CATEGORY_ALIASES:
        return CATEGORY_ALIASES[category]
    valid_categories = ", ".join(CLI_CATEGORY_CHOICES)
    raise ValueError(
        f"Unsupported category '{category}'. Expected one of: {valid_categories}"
    )


# #790 asked for `--godot-binary` to be forwarded here. Measured, it cannot be:
# this runner OWNS the corpus that is pinned to the Python-fallback fixture. Its
# `qa` category is the blocking visual/SSIM gate in `.github/workflows/baseline_qa.yml`,
# its baseline (`tests/ci/baselines/qa_results.json`) records
# `source_splat_count: 1024`, and the world-vs-instance A/B it runs compares
# `test_splats.ply` against the committed 1024-splat `test_splats.gsplatworld`,
# refusing to score when the two disagree (`scripts/qa_route_capture_base.gd`).
# The `sorting` and `qa` steps share one workspace, so regenerating at the C++
# count in either would break the gate. Flipping this is a baseline change:
# rebake the world and re-measure the QA baseline first.
FIXTURE_CORPUS_BLOCKER = (
    "the QA scene suite's committed expectations are pinned to it "
    "(tests/ci/baselines/qa_results.json records source_splat_count=1024 and the "
    "committed test_splats.gsplatworld is a 1024-splat bake); regenerating at the C++ "
    "count requires rebaking the world and re-measuring the QA baseline first (#790)"
)


def prepare_synthetic_assets() -> None:
    if not SYNTHETIC_ASSET_PREP_SCRIPT.is_file():
        raise RuntimeError(
            f"Missing synthetic asset prep script: {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}"
        )

    command = [sys.executable, str(SYNTHETIC_ASSET_PREP_SCRIPT), "--quiet"]
    print(f"[INFO] Fixture generator: Python fallback by design -- {FIXTURE_CORPUS_BLOCKER}.")
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        cwd=ROOT,
        check=False,
    )
    # Echo prep output even on success: under --quiet the only thing it prints is
    # the low-fidelity warning, and swallowing that is how the downgrade stayed
    # invisible (#790).
    for line in (result.stdout or "").splitlines():
        if line.strip():
            print(line)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        if not detail:
            detail = f"exit code {result.returncode}"
        raise RuntimeError(f"Synthetic asset prep failed: {detail}")


class BaselineQARunner:
    def __init__(self, godot_binary: str = "godot", qa_require_capture: bool = False):
        self.godot_binary = godot_binary
        # When True the QA Scene Suite runs on the real display instead of
        # --headless, so its viewport captures produce actual images. See
        # QA_REQUIRE_CAPTURE_ENV.
        self.qa_require_capture = qa_require_capture
        self.test_results = {
            "start_time": time.time(),
            "end_time": 0,
            "total_tests": 0,
            "passed_tests": 0,
            "failed_tests": 0,
            "skipped_tests": 0,
            "tests": [],
            "summary": {
                "overall_status": "running",
                "qa_baseline": {"status": "not_run"},
            },
        }

    @staticmethod
    def _is_expected_headless_qa_skip(test_name: str, command: List[str], stdout: str, stderr: str) -> bool:
        if test_name != "QA Scene Suite":
            return False
        if "--headless" not in command:
            return False

        merged_output = f"{stdout}\n{stderr}"
        required_markers = (
            "Failed to create primary local RenderingDevice",
            "Failed to create shared local RenderingDevice",
        )
        if not all(marker in merged_output for marker in required_markers):
            return False

        allowed_error_prefixes = (
            'ERROR: Parameter "t" is null.',
            "ERROR: [RENDERER][ERROR] [GaussianSplatSceneDirector] Unable to acquire local RenderingDevice for shared renderer",
            "ERROR: [RENDERER][ERROR] [GaussianSplatSceneDirector] Unable to acquire primary RenderingDevice for shared renderer",
        )
        for line in merged_output.splitlines():
            stripped = line.strip()
            if stripped.startswith("ERROR:") and not any(stripped.startswith(prefix) for prefix in allowed_error_prefixes):
                return False

        return True

    def _record_qa_baseline_skipped(
        self,
        qa_results_path: Path,
        baseline_path: Path,
        report_path: Optional[Path],
        summary_path: Optional[Path],
        reason: str,
        require_baseline: bool = False,
        require_capture: bool = False,
    ) -> bool:
        """Record that the QA Scene Suite itself did not produce results (e.g.
        no RenderingDevice in this headless environment).

        Two orthogonal switches police two different facts here, and BOTH
        must fail closed or the skip laundered a pass:

        ``require_baseline`` (--require-qa-baseline /
        GS_CI_REQUIRE_QA_BASELINE) polices whether a baseline file exists at
        all. An earlier revision returned True unconditionally and never
        looked at it, so turning the switch on had zero effect whenever the
        suite itself skipped.

        ``require_capture`` (--qa-require-capture / GS_CI_GPU_REQUIRED)
        polices whether the suite actually rendered. An earlier revision of
        this docstring argued the skip was always "legitimately not
        applicable" because "no amount of --require-qa-baseline changes
        whether a RenderingDevice exists". That was true only while no lane
        promised a RenderingDevice. It is now false: baseline_qa.yml's
        qa-visual lane runs on the self-hosted GPU runner and passes
        --qa-require-capture, so a suite that produced no results there means
        the GPU lane failed to render — a hard failure, not a skip. Without
        this branch the worst case is silent: baseline present + suite
        skipped + require_baseline on would return True, and a blocking lane
        would go green having compared nothing.
        """
        baseline_exists = baseline_path.exists()
        baseline_enforced_and_missing = require_baseline and not baseline_exists
        capture_enforced_and_absent = bool(require_capture)
        hard_failure = baseline_enforced_and_missing or capture_enforced_and_absent

        comparison: Dict[str, Any] = {
            "status": "failed" if hard_failure else "skipped",
            "coverage_gap": True,
            "require_capture": require_capture,
            "mode": "compare",
            "qa_results_path": str(qa_results_path),
            "baseline_path": str(baseline_path),
            "baseline_exists": baseline_exists,
            "require_baseline": require_baseline,
            "thresholds": {
                "ssim_min_delta": MINIMUM_SSIM_DROP,
                "fps_min_ratio": MINIMUM_FPS_RATIO,
                "time_max_ratio": MAXIMUM_TIME_RATIO,
            },
            "scenes_checked": 0,
            "metrics_checked": 0,
            "missing_scenes": [],
            "new_scenes": [],
            "regressions": [],
            "notes": [reason],
            "timestamp_unix": time.time(),
        }

        if hard_failure:
            if capture_enforced_and_absent:
                message = f"{QA_CAPTURE_REQUIRED_BUT_SKIPPED_MESSAGE} Reason given: {reason}."
            else:
                message = (
                    f"QA Scene Suite produced no results ({reason}) AND QA baseline "
                    f"missing at {baseline_path}, with --require-qa-baseline/"
                    f"{REQUIRE_QA_BASELINE_ENV} set. Refusing to launder a missing "
                    "baseline into a pass just because the suite also skipped."
                )
            comparison["notes"].append(message)
            print(f"[FAIL] {message}")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return False

        comparison["notes"].append(QA_BASELINE_COVERAGE_GAP_NOTE)
        _ci_warning(f"{reason} (skipping comparison). {QA_BASELINE_COVERAGE_GAP_NOTE}")
        self.test_results["summary"]["qa_baseline"] = comparison
        self._write_baseline_artifacts(comparison, report_path, summary_path)
        return True

    def run_test(self, test: Dict, timeout: Optional[int] = None) -> Tuple[bool, str, Dict]:
        """Run a single test entry (Godot script or arbitrary command)."""

        test_name = test["name"]
        test_type = test.get("type", "godot")
        timeout = test.get("timeout", timeout or 180)
        cwd = ROOT

        if test_type == "godot":
            command = [self.godot_binary]
            if test.get("requires_gpu", False):
                command.extend(GPU_DISPLAY_ARGS)
            else:
                command.append("--headless")
            command.extend(["--verbose", "--script", test["script"]])
            descriptor = test["script"]
        else:
            command = test["command"]
            descriptor = " ".join(command)

        print(f"\n[TEST] Running {test_name}...")
        print(f"   Command: {' '.join(command)}")

        test_start = time.time()

        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
                cwd=cwd,
            )

            test_duration = time.time() - test_start
            success = result.returncode == 0
            output = (result.stdout or "") + (result.stderr or "")
            details = self._parse_test_output(output)
            test_status = "passed" if success else "failed"
            expected_headless_qa_skip = False
            if not success:
                expected_headless_qa_skip = self._is_expected_headless_qa_skip(
                    test_name,
                    command,
                    result.stdout or "",
                    result.stderr or "",
                )
                if expected_headless_qa_skip:
                    details["skip_reason"] = "QA Scene Suite requires local RenderingDevice when run with current headless configuration."
                    test_status = "skipped"
                    success = True

            test_result = {
                "name": test_name,
                "type": test_type,
                "descriptor": descriptor,
                "success": success,
                "status": test_status,
                "exit_code": result.returncode,
                "duration": test_duration,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "details": details,
                "skipped": expected_headless_qa_skip,
            }

            if success:
                if expected_headless_qa_skip:
                    print(f"   [SKIP] SKIPPED ({test_duration:.1f}s) - local RenderingDevice unavailable in headless mode")
                    self.test_results["skipped_tests"] += 1
                else:
                    print(f"   [PASS] PASSED ({test_duration:.1f}s)")
                    self.test_results["passed_tests"] += 1
            else:
                print(f"   [FAIL] FAILED ({test_duration:.1f}s) - Exit code: {result.returncode}")
                if result.stderr:
                    print(f"   Error: {result.stderr[:200]}...")
                self.test_results["failed_tests"] += 1

            self.test_results["tests"].append(test_result)
            return success, output, details

        except subprocess.TimeoutExpired:
            test_duration = time.time() - test_start
            error_msg = f"Test timed out after {timeout}s"
            print(f"   [TIMEOUT] TIMEOUT ({test_duration:.1f}s)")

            test_result = {
                "name": test_name,
                "type": test_type,
                "descriptor": descriptor,
                "success": False,
                "exit_code": -1,
                "duration": test_duration,
                "stdout": "",
                "stderr": error_msg,
                "details": {"error": error_msg},
            }

            self.test_results["failed_tests"] += 1
            self.test_results["tests"].append(test_result)
            return False, error_msg, {"error": error_msg}

        except Exception as e:
            test_duration = time.time() - test_start
            error_msg = f"Exception running test: {str(e)}"
            print(f"   [EXCEPTION] EXCEPTION ({test_duration:.1f}s): {str(e)}")

            test_result = {
                "name": test_name,
                "type": test_type,
                "descriptor": descriptor,
                "success": False,
                "exit_code": -2,
                "duration": test_duration,
                "stdout": "",
                "stderr": error_msg,
                "details": {"error": error_msg},
            }

            self.test_results["failed_tests"] += 1
            self.test_results["tests"].append(test_result)
            return False, error_msg, {"error": error_msg}

    def _parse_test_output(self, output: str) -> Dict:
        """Parse test output for key metrics and details"""
        details = {}

        # Look for test results patterns
        lines = output.split('\n')
        for line in lines:
            if "Total Tests:" in line:
                try:
                    details["total_tests"] = int(line.split(":")[-1].strip())
                except:
                    pass
            elif "Passed:" in line:
                try:
                    details["passed_tests"] = int(line.split(":")[-1].strip())
                except:
                    pass
            elif "Failed:" in line:
                try:
                    details["failed_tests"] = int(line.split(":")[-1].strip())
                except:
                    pass
            elif "sort_time=" in line:
                try:
                    # Extract sort time from performance output
                    parts = line.split("sort_time=")
                    if len(parts) > 1:
                        time_str = parts[1].split("ms")[0]
                        details["sort_time_ms"] = float(time_str)
                except:
                    pass
            elif "throughput=" in line:
                try:
                    # Extract throughput
                    parts = line.split("throughput=")
                    if len(parts) > 1:
                        throughput_str = parts[1].split("M/s")[0]
                        throughput_mps = float(throughput_str)
                        details["throughput_mps"] = throughput_mps
                except:
                    pass

        return details

    def run_all_tests(self, quick: bool = False, category: Optional[str] = None, categories: Optional[set] = None) -> bool:
        """Run baseline QA tests with optional filtering."""
        print("=== Baseline QA Test Suite ===")
        try:
            prepare_synthetic_assets()
            print("[PASS] Synthetic asset prep complete.")
        except RuntimeError as exc:
            print(f"[FAIL] {exc}")
            self.test_results["total_tests"] = 0
            self.test_results["failed_tests"] = 1
            self.test_results["end_time"] = time.time()
            return False
        try:
            category = normalize_test_category(category)
        except ValueError as exc:
            print(f"[FAIL] {exc}")
            self.test_results["total_tests"] = 0
            self.test_results["end_time"] = time.time()
            return False

        tests = self._build_test_table()

        selected_tests = tests
        if categories:
            selected_tests = [test for test in tests if test.get("category") in categories]
        elif category:
            selected_tests = [test for test in tests if test.get("category") == category]
        elif quick:
            quick_categories = {"ply", "sorting"}
            selected_tests = [test for test in tests if test.get("category") in quick_categories]
        return self._execute_selected_tests(selected_tests)

    def _build_test_table(self) -> List[Dict]:
        """The full inventory of runnable tests, independent of selection.

        Split out of run_all_tests() so the QA Scene Suite's argv can be
        asserted directly: whether that entry launches on the real display or
        under --headless decides whether the whole category can capture
        anything, and that is too important to be verifiable only by running
        Godot.
        """
        qa_output_path = str((ROOT / "tests" / "ci" / "qa_results.json").resolve())
        return [
            {
                "name": "PLY Loader Tests",
                "type": "godot",
                "script": "tests/ci/test_ply_loader_ci.gd",
                "category": "ply",
            },
            {
                "name": "Importer Zero-Init Tests",
                "type": "godot",
                "script": "tests/ci/test_importer_zero_init_ci.gd",
                "category": "import",
            },
            {
                "name": "PLY Pipeline Tests",
                "type": "godot",
                "script": "tests/ci/test_ply_pipeline_ci.gd",
                "category": "pipeline",
            },
            {
                "name": "GPU Sorting Tests",
                "type": "godot",
                "script": "tests/ci/test_gpu_sorting_ci.gd",
                "category": "sorting",
                "requires_gpu": True,
            },
            {
                "name": "Runtime Validation Suite",
                "type": "command",
                "command": [
                    sys.executable,
                    "tests/runtime/run_runtime_validation.py",
                    "--profile",
                    "headless-ci",
                    "--godot-binary",
                    self.godot_binary,
                ],
                "timeout": 600,
                "category": "runtime",
            },
            {
                "name": "Module Test Suite (GaussianSplatting)",
                "type": "command",
                "command": [sys.executable, "tests/ci/run_module_tests.py", "--godot-binary", self.godot_binary],
                "timeout": 900,
                "category": "module",
            },
            {
                "name": "QA Scene Suite",
                "type": "command",
                # --headless cannot create a RenderingDevice, so every
                # capture_viewport() in the suite returns null and the whole
                # category degrades into a laundered skip. Only run headless
                # when the caller has NOT declared that capture is required.
                "command": [
                    self.godot_binary,
                    *(GPU_DISPLAY_ARGS if self.qa_require_capture else ("--headless",)),
                    "--path",
                    "tests/examples/godot/test_project",
                    "--script",
                    "res://scripts/qa_test_runner.gd",
                    "--qa-output",
                    qa_output_path,
                ],
                "timeout": 900,
                "category": "qa",
            },
        ]

    def _execute_selected_tests(self, selected_tests: List[Dict]) -> bool:
        if not selected_tests:
            print("No tests selected.")
            self.test_results["total_tests"] = 0
            self.test_results["end_time"] = time.time()
            return True

        self.test_results["total_tests"] = len(selected_tests)

        for test in selected_tests:
            self.run_test(test)

        self.test_results["end_time"] = time.time()
        return self.test_results["failed_tests"] == 0

    def _load_json_file(self, path: Path, label: str) -> Optional[Dict[str, Any]]:
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            print(f"[WARN] Failed to parse {label} JSON at {path}: {exc}")
            return None

    def _metric_rule(self, metric_name: str) -> Optional[Dict[str, Any]]:
        metric = metric_name.lower()
        # ORDER MATTERS, and getting it wrong silently defeats the rule below.
        # An acceptance THRESHOLD a scene was configured with is a contract, not
        # a measurement: lowering it makes the scene permit renders it used to
        # reject, while still passing. It must therefore be pinned exactly —
        # but `ssim_threshold` also contains "ssim", so if the generic
        # measurement rule is consulted first the threshold inherits SSIM's
        # 0.02 tolerance and a 0.98 -> 0.97 weakening slips through. Contracts
        # are classified before measurements for exactly that reason.
        if metric.endswith("_dominance_margin") or metric.endswith("_threshold"):
            return {"kind": "exact_contract"}
        if "ssim" in metric:
            return {"kind": "minimum_delta", "value": MINIMUM_SSIM_DROP}
        if "fps" in metric:
            return {"kind": "minimum_ratio", "value": MINIMUM_FPS_RATIO}
        if "frame_time" in metric or metric.endswith("_ms") or metric.endswith("_time_ms"):
            return {"kind": "maximum_ratio", "value": MAXIMUM_TIME_RATIO}
        # Measured pixel dominance is a sort-order signal, but it moves with the
        # build, so compare it by ratio rather than equality. For the depth-order
        # scenes the floor stays above their own 0.15 acceptance gate. For the
        # tie-break scene this also prevents an almost-equal red/green pixel from
        # retaining the same binary winner while its ordering signal collapses.
        # The committed optimized-build margin is 0.0078431442 and CI measured
        # 0.0078431368, well inside the same observed-noise allowance.
        if metric.endswith("red_minus_blue") or metric.endswith("tie_break_margin"):
            return {"kind": "minimum_ratio", "value": MINIMUM_DOMINANCE_RATIO}
        return None

    def _build_baseline_summary_markdown(self, comparison: Dict[str, Any]) -> str:
        status = str(comparison.get("status", "unknown"))
        coverage_gap = bool(comparison.get("coverage_gap", False))
        # `status == "failed"` must always win over the coverage-gap label:
        # a hard failure (e.g. --require-qa-baseline set with no baseline,
        # even on the QA-scene-skip path) must never render as the softer
        # "[NO BASELINE - COVERAGE GAP]" bucket just because coverage_gap is
        # also true for that comparison. Coverage_gap only relabels a
        # genuine *skip* (nothing to compare, not required to fail).
        if status == "failed":
            icon = "[FAIL]"
        elif coverage_gap:
            icon = "[NO BASELINE - COVERAGE GAP]"
        elif status in {"passed", "updated"}:
            icon = "[PASS]"
        elif status == "skipped":
            icon = "[WARN]"
        else:
            icon = "[FAIL]"
        lines = [
            "# QA Baseline Regression Summary",
            "",
            f"- Status: {icon} `{status}`",
            f"- Coverage gap (no baseline to enforce against): `{coverage_gap}`",
            f"- Mode: `{comparison.get('mode', 'unknown')}`",
            f"- Baseline path: `{comparison.get('baseline_path', '')}`",
            f"- Baseline present: `{comparison.get('baseline_exists', False)}`",
            f"- Scenes checked: `{comparison.get('scenes_checked', 0)}`",
            f"- Metrics checked: `{comparison.get('metrics_checked', 0)}`",
            f"- Regressions: `{len(comparison.get('regressions', []))}`",
            "",
        ]

        missing_scenes = comparison.get("missing_scenes", [])
        if missing_scenes:
            lines.append("## Missing Scenes")
            for scene in missing_scenes:
                lines.append(f"- `{scene}`")
            lines.append("")

        regressions = comparison.get("regressions", [])
        if regressions:
            lines.extend(
                [
                    "## Regressions",
                    "| Scene | Metric | Baseline | Current | Rule |",
                    "| --- | --- | ---: | ---: | --- |",
                ]
            )
            for entry in regressions:
                lines.append(
                    "| {scene} | {metric} | {baseline} | {current} | {rule} |".format(
                        scene=entry.get("scene", ""),
                        metric=entry.get("metric", ""),
                        baseline=_format_metric_value(entry.get("baseline")),
                        current=_format_metric_value(entry.get("current")),
                        rule=entry.get("rule", ""),
                    )
                )
            lines.append("")

        new_scenes = comparison.get("new_scenes", [])
        if new_scenes:
            lines.append("## New Scenes (not in baseline)")
            for scene in new_scenes:
                lines.append(f"- `{scene}`")
            lines.append("")

        notes = comparison.get("notes", [])
        if notes:
            lines.append("## Notes")
            for note in notes:
                lines.append(f"- {note}")
            lines.append("")

        return "\n".join(lines).rstrip() + "\n"

    def _write_baseline_artifacts(
        self,
        comparison: Dict[str, Any],
        report_path: Optional[Path],
        summary_path: Optional[Path],
    ) -> None:
        if report_path is not None:
            try:
                report_path.parent.mkdir(parents=True, exist_ok=True)
                report_path.write_text(json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
                print(f"[REPORT] QA baseline report saved to {report_path}")
            except Exception as exc:
                print(f"[WARN] Could not write QA baseline report: {exc}")

        if summary_path is not None:
            try:
                summary_path.parent.mkdir(parents=True, exist_ok=True)
                summary_path.write_text(self._build_baseline_summary_markdown(comparison), encoding="utf-8")
                print(f"[NOTE] QA baseline summary saved to {summary_path}")
            except Exception as exc:
                print(f"[WARN] Could not write QA baseline summary: {exc}")

    def compare_qa_baseline(
        self,
        qa_results_path: Path,
        baseline_path: Path,
        update_baseline: bool = False,
        require_baseline: bool = False,
        report_path: Optional[Path] = None,
        summary_path: Optional[Path] = None,
    ) -> bool:
        """Store or compare QA results against baseline snapshots."""
        comparison: Dict[str, Any] = {
            "status": "not_run",
            "coverage_gap": False,
            "mode": "update" if update_baseline else "compare",
            "qa_results_path": str(qa_results_path),
            "baseline_path": str(baseline_path),
            "baseline_exists": baseline_path.exists(),
            "require_baseline": require_baseline,
            "thresholds": {
                "ssim_min_delta": MINIMUM_SSIM_DROP,
                "fps_min_ratio": MINIMUM_FPS_RATIO,
                "time_max_ratio": MAXIMUM_TIME_RATIO,
            },
            "scenes_checked": 0,
            "metrics_checked": 0,
            "missing_scenes": [],
            "new_scenes": [],
            "regressions": [],
            "notes": [],
            "timestamp_unix": time.time(),
        }

        if not qa_results_path.exists():
            message = f"QA results not found at {qa_results_path}"
            print(f"[WARN] {message}")
            comparison["status"] = "failed"
            comparison["notes"].append(message)
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return False

        current = self._load_json_file(qa_results_path, "QA results")
        if current is None:
            comparison["status"] = "failed"
            comparison["notes"].append("QA results JSON is invalid.")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return False

        current_results = current.get("results", [])
        if not isinstance(current_results, list):
            comparison["status"] = "failed"
            comparison["notes"].append("QA results payload missing list field 'results'.")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return False

        # A scene that self-skips still reports passed=true, so the suite exits
        # 0 and CI would call this lane green while that scene verified nothing.
        # When the lane promised a GPU, a skip means the promise broke.
        if self.qa_require_capture:
            skipped_scenes = [
                str(entry.get("scene", "<unnamed>"))
                for entry in current_results
                if entry.get("skipped", False) or "[QA_SKIP]" in str(entry.get("message", ""))
            ]
            if skipped_scenes:
                message = (
                    f"{len(skipped_scenes)} QA scene(s) self-skipped on a lane that declared "
                    "capture REQUIRED (--qa-require-capture / "
                    f"{QA_REQUIRE_CAPTURE_ENV}=1): {', '.join(skipped_scenes)}. A skip on a GPU "
                    "lane means the scene verified nothing; refusing to report it as a pass."
                )
                print(f"[FAIL] {message}")
                comparison["status"] = "failed"
                comparison["notes"].append(message)
                comparison["skipped_scenes"] = skipped_scenes
                self.test_results["summary"]["qa_baseline"] = comparison
                self._write_baseline_artifacts(comparison, report_path, summary_path)
                return False

        if update_baseline:
            # Refuse to freeze a run that did not actually render. Writing one
            # is silently unrecoverable: the comparator's SSIM rule is
            # `current >= baseline - 0.02`, so a baseline holding ssim 0.0 can
            # never fail again and the blocking lane stays green forever while
            # testing nothing.
            rejections = validate_baseline_candidate(current_results)
            if rejections:
                message = (
                    f"Refusing to write a QA baseline from this run ({len(rejections)} "
                    "problem(s)); a baseline captured from a non-rendering run makes the "
                    "gate permanently unfailable."
                )
                print(f"[FAIL] {message}")
                for reason in rejections:
                    print(f"   - {reason}")
                comparison["status"] = "failed"
                comparison["notes"].append(message)
                comparison["notes"].extend(rejections)
                self.test_results["summary"]["qa_baseline"] = comparison
                self._write_baseline_artifacts(comparison, report_path, summary_path)
                return False

            sanitized, dropped = strip_non_deterministic_metrics(current)
            # Re-validate what will actually be written, not just what was
            # measured. Stripping is the only step between the two, so this
            # catches a sanitizer that ever starts removing more than it
            # should — the file on disk is what the blocking lane compares
            # against, so the file is what has to be proven sound.
            post_strip = validate_baseline_candidate(sanitized.get("results", []), already_sanitized=True)
            if post_strip:
                message = (
                    "Sanitizing the QA baseline produced a payload that is no longer valid "
                    f"({len(post_strip)} problem(s)); refusing to write it."
                )
                print(f"[FAIL] {message}")
                for reason in post_strip:
                    print(f"   - {reason}")
                comparison["status"] = "failed"
                comparison["notes"].append(message)
                comparison["notes"].extend(post_strip)
                self.test_results["summary"]["qa_baseline"] = comparison
                self._write_baseline_artifacts(comparison, report_path, summary_path)
                return False

            baseline_path.parent.mkdir(parents=True, exist_ok=True)
            baseline_path.write_text(json.dumps(sanitized, indent=2) + "\n", encoding="utf-8")
            comparison["baseline_exists"] = True
            comparison["status"] = "updated"
            comparison["scenes_checked"] = len(current_results)
            comparison["dropped_metrics"] = dropped
            comparison["notes"].append(f"Baseline refreshed from current QA output ({len(current_results)} scenes).")
            if dropped:
                comparison["notes"].append(
                    f"Dropped {len(dropped)} machine-dependent metric(s) so the blocking "
                    "compare measures the renderer, not runner contention: "
                    f"{', '.join(dropped)}."
                )
            print(f"[PASS] QA baseline updated at {baseline_path}")
            if dropped:
                print(f"   Dropped {len(dropped)} machine-dependent metric(s): {', '.join(dropped)}")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return True

        if not baseline_path.exists():
            message = f"QA baseline missing at {baseline_path}"
            if require_baseline:
                print(f"[FAIL] {message}")
                comparison["status"] = "failed"
                comparison["notes"].append(f"{message} (required)")
                self.test_results["summary"]["qa_baseline"] = comparison
                self._write_baseline_artifacts(comparison, report_path, summary_path)
                return False

            comparison["status"] = "skipped"
            comparison["coverage_gap"] = True
            comparison["notes"].append(f"{message} (comparison skipped)")
            comparison["notes"].append(QA_BASELINE_COVERAGE_GAP_NOTE)
            _ci_warning(f"{message} (skipping comparison). {QA_BASELINE_COVERAGE_GAP_NOTE}")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return True

        baseline = self._load_json_file(baseline_path, "QA baseline")
        if baseline is None:
            comparison["status"] = "failed"
            comparison["notes"].append("QA baseline JSON is invalid.")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return False

        baseline_results = baseline.get("results", [])
        if not isinstance(baseline_results, list):
            comparison["status"] = "failed"
            comparison["notes"].append("QA baseline payload missing list field 'results'.")
            self.test_results["summary"]["qa_baseline"] = comparison
            self._write_baseline_artifacts(comparison, report_path, summary_path)
            return False

        baseline_map = {
            str(entry.get("scene", "")).strip(): entry
            for entry in baseline_results
            if str(entry.get("scene", "")).strip()
        }
        current_map = {
            str(entry.get("scene", "")).strip(): entry
            for entry in current_results
            if str(entry.get("scene", "")).strip()
        }

        comparison["scenes_checked"] = len(baseline_map)
        comparison["missing_scenes"] = sorted([scene for scene in baseline_map if scene not in current_map])
        comparison["new_scenes"] = sorted([scene for scene in current_map if scene not in baseline_map])

        for scene_name in sorted(baseline_map.keys()):
            if scene_name not in current_map:
                continue
            baseline_metrics = baseline_map[scene_name].get("metrics", {}) or {}
            current_metrics = current_map[scene_name].get("metrics", {}) or {}
            if not isinstance(baseline_metrics, dict) or not isinstance(current_metrics, dict):
                continue

            for metric_name in sorted(baseline_metrics.keys()):
                baseline_value = baseline_metrics.get(metric_name)
                current_value = current_metrics.get(metric_name)

                # Path-identity metrics (route_uid, data_source, raster_path,
                # stage_*_status, and the boolean seen/ok flags) name WHICH CODE
                # PATH ran. They are deterministic by construction — measured
                # identical across four independent runs — so they are compared
                # for exact equality rather than tolerance. This is the highest-
                # value comparison in the baseline: a route silently degrading
                # to a SKIP is exactly what made qa_visual_diff score a perfect
                # 1.0 while rendering nothing (#785), and a numeric tolerance
                # can never see it.
                #
                # Lists are compared the same way: `warnings` and
                # `sorted_indices_preview` are deterministic collections, and
                # equality is the only sensible relation on them.
                if isinstance(baseline_value, (str, bool, list)):
                    comparison["metrics_checked"] += 1
                    # JSON booleans need an explicit type contract: Python
                    # considers True == 1 and False == 0. Equality alone would
                    # therefore let a pinned boolean silently become numeric.
                    if type(current_value) is not type(baseline_value) or current_value != baseline_value:
                        comparison["regressions"].append(
                            {
                                "scene": scene_name,
                                "metric": metric_name,
                                "baseline": baseline_value,
                                "current": current_value,
                                "threshold": baseline_value,
                                "rule": "current type and value == baseline (path identity)",
                            }
                        )
                    continue

                # None in the baseline is a recorded absence, not a pin.
                if baseline_value is None:
                    comparison.setdefault("unchecked_metrics", []).append(f"{scene_name}.{metric_name}")
                    continue

                # A pinned numeric metric that stops being emitted, gets renamed,
                # or changes type must FAIL, not be skipped. The old `continue`
                # meant a refactor could silently delete the SSIM regression
                # contract: the scene still exits 0, the metric simply vanishes,
                # and the comparison reports "passed" having checked nothing.
                # That is the same disappearing-coverage shape this lane exists
                # to eliminate, so it is checked before any rule lookup — a
                # metric the baseline pins is a contract regardless of whether
                # this script currently knows how to compare its value.
                if not isinstance(baseline_value, (int, float)):
                    comparison["metrics_checked"] += 1
                    comparison["regressions"].append(
                        {
                            "scene": scene_name,
                            "metric": metric_name,
                            "baseline": baseline_value,
                            "current": current_value,
                            "threshold": baseline_value,
                            "rule": "baseline metric has an uncomparable type",
                        }
                    )
                    continue
                # `bool` is a subclass of `int` in Python, so a metric that
                # degrades from 1.0 to `true` would satisfy the isinstance check
                # and float(True) == 1.0 would then sail through the SSIM
                # comparison — the type contract disappearing without a sound.
                # Excluded explicitly rather than relying on the str/bool/list
                # branch above, which only fires when the BASELINE is a bool.
                if isinstance(current_value, bool) or not isinstance(current_value, (int, float)):
                    comparison["metrics_checked"] += 1
                    comparison["regressions"].append(
                        {
                            "scene": scene_name,
                            "metric": metric_name,
                            "baseline": baseline_value,
                            "current": current_value,
                            "threshold": baseline_value,
                            "rule": "metric missing or no longer numeric (was pinned by the baseline)",
                        }
                    )
                    continue

                rule = self._metric_rule(metric_name)
                if rule is None:
                    # Not silently dropped. A numeric metric nobody knows how to
                    # compare is a coverage gap, and an unrecorded coverage gap
                    # reads as "checked and fine" to anyone looking at
                    # metrics_checked. Record it so the gap is countable.
                    comparison.setdefault("unchecked_metrics", []).append(f"{scene_name}.{metric_name}")
                    continue

                baseline_num = float(baseline_value)
                current_num = float(current_value)
                comparison["metrics_checked"] += 1
                rule_kind = str(rule["kind"])

                if rule_kind == "exact_contract":
                    # A configured acceptance threshold, not a measurement:
                    # lowering it makes the scene assert less while still
                    # passing, which no tolerance can detect.
                    if current_num != baseline_num:
                        comparison["regressions"].append(
                            {
                                "scene": scene_name,
                                "metric": metric_name,
                                "baseline": baseline_num,
                                "current": current_num,
                                "threshold": baseline_num,
                                "rule": "current == baseline (acceptance contract)",
                            }
                        )
                    continue

                rule_value = float(rule["value"])

                passes = True
                threshold = baseline_num
                rule_text = ""
                if rule_kind == "minimum_delta":
                    threshold = baseline_num - rule_value
                    rule_text = f"current >= baseline - {rule_value:.3f}"
                    passes = current_num >= threshold
                elif rule_kind == "minimum_ratio":
                    if abs(baseline_num) < 1e-9:
                        continue
                    threshold = baseline_num * rule_value
                    rule_text = f"current >= baseline * {rule_value:.3f}"
                    passes = current_num >= threshold
                elif rule_kind == "maximum_ratio":
                    if abs(baseline_num) < 1e-9:
                        continue
                    threshold = baseline_num * rule_value
                    rule_text = f"current <= baseline * {rule_value:.3f}"
                    passes = current_num <= threshold

                if not passes:
                    comparison["regressions"].append(
                        {
                            "scene": scene_name,
                            "metric": metric_name,
                            "baseline": baseline_num,
                            "current": current_num,
                            "threshold": threshold,
                            "rule": rule_text,
                        }
                    )

        has_regressions = (
            bool(comparison["regressions"])
            or bool(comparison["missing_scenes"])
            or bool(comparison["new_scenes"])
        )
        comparison["status"] = "failed" if has_regressions else "passed"
        self.test_results["summary"]["qa_baseline"] = comparison
        self._write_baseline_artifacts(comparison, report_path, summary_path)

        if has_regressions:
            print("\n[FAIL] QA baseline regression detected:")
            for scene_name in comparison["missing_scenes"]:
                print(f"   Missing current results for {scene_name}")
            for scene_name in comparison["new_scenes"]:
                print(f"   Current QA scene has no committed baseline: {scene_name}")
            for entry in comparison["regressions"]:
                print(
                    "   {scene}: {metric} baseline={baseline} current={current} threshold={threshold} ({rule})".format(
                        scene=entry["scene"],
                        metric=entry["metric"],
                        baseline=_format_metric_value(entry["baseline"]),
                        current=_format_metric_value(entry["current"]),
                        threshold=_format_metric_value(entry["threshold"]),
                        rule=str(entry["rule"]),
                    )
                )
            return False

        print(
            "[PASS] QA baseline comparison passed "
            f"({comparison['scenes_checked']} scenes, {comparison['metrics_checked']} metrics checked)"
        )
        if comparison["new_scenes"]:
            print(f"[INFO] Detected {len(comparison['new_scenes'])} new scene(s) not in baseline")
        return True

    def generate_report(self) -> None:
        """Generate comprehensive test report"""
        duration = self.test_results["end_time"] - self.test_results["start_time"]

        print("\n" + "="*60)
        print("BASELINE QA TEST REPORT")
        print("="*60)
        total_tests = self.test_results["total_tests"]
        passed_tests = self.test_results["passed_tests"]
        failed_tests = self.test_results["failed_tests"]
        skipped_tests = self.test_results["skipped_tests"]

        print(f"Total Tests: {total_tests}")
        print(f"Passed: {passed_tests}")
        print(f"Failed: {failed_tests}")
        print(f"Skipped: {skipped_tests}")
        print(f"Duration: {duration:.1f} seconds")
        success_denominator = max(1, total_tests - skipped_tests)
        success_rate = (passed_tests / success_denominator * 100.0) if total_tests else 100.0
        print(f"Success Rate: {success_rate:.1f}%")
        self.test_results["summary"]["duration_seconds"] = duration
        self.test_results["summary"]["success_rate"] = success_rate
        baseline_status = self.test_results["summary"].get("qa_baseline", {})
        baseline_failed = isinstance(baseline_status, dict) and baseline_status.get("status") == "failed"
        overall_passed = failed_tests == 0 and not baseline_failed
        self.test_results["summary"]["overall_status"] = "passed" if overall_passed else "failed"

        # Detailed results
        print("\nDETAILED RESULTS:")
        for test in self.test_results["tests"]:
            test_status = test.get("status", "passed" if test.get("success") else "failed")
            if test_status == "skipped":
                status = "[SKIP] SKIP"
            elif test["success"]:
                status = "[PASS] PASS"
            else:
                status = "[FAIL] FAIL"
            print(f"  {status} {test['name']} ({test['duration']:.1f}s)")

            if test_status == "skipped":
                skip_reason = test.get("details", {}).get("skip_reason")
                if skip_reason:
                    print(f"       Reason: {skip_reason}")
            elif not test["success"]:
                print(f"       Exit Code: {test['exit_code']}")
                if test['stderr']:
                    print(f"       Error: {test['stderr'][:100]}...")

        # Performance summary
        print("\nPERFORMANCE METRICS:")
        for test in self.test_results["tests"]:
            if "sort_time_ms" in test["details"]:
                print(f"  {test['name']}: {test['details']['sort_time_ms']:.2f}ms sort time")
            throughput_mps = test["details"].get("throughput_mps")
            if throughput_mps is not None:
                print(f"  {test['name']}: {throughput_mps:.1f}M splats/second")

        if isinstance(baseline_status, dict) and baseline_status.get("status") != "not_run":
            print("\nQA BASELINE:")
            print(
                "  Status: {status} | scenes={scenes} | metrics={metrics} | regressions={regressions}".format(
                    status=baseline_status.get("status", "unknown"),
                    scenes=baseline_status.get("scenes_checked", 0),
                    metrics=baseline_status.get("metrics_checked", 0),
                    regressions=len(baseline_status.get("regressions", [])),
                )
            )

        # Save JSON report
        self._save_json_report()

        # CI summary
        if overall_passed:
            print("\n[PASS] ALL BASELINE QA TESTS PASSED!")
        elif failed_tests == 0 and baseline_failed:
            print("\n[WARN] QA BASELINE REGRESSION CHECK FAILED")
        else:
            print(f"\n[WARN] {self.test_results['failed_tests']} TEST(S) FAILED")

    def _save_json_report(self) -> None:
        """Save detailed JSON report for CI artifacts"""
        try:
            report_path = ROOT / "baseline_qa_results.json"
            with report_path.open("w", encoding="utf-8") as f:
                json.dump(self.test_results, f, indent=2)
            print(f"\n[REPORT] Detailed report saved to {report_path}")
        except Exception as e:
            print(f"[WARN] Could not save JSON report: {e}")

    def print_actionable_failures(self) -> None:
        """Print actionable error messages for failed tests"""
        failed_tests = [t for t in self.test_results["tests"] if not t["success"]]

        if not failed_tests:
            return

        print("\n" + "="*60)
        print("ACTIONABLE FAILURE ANALYSIS")
        print("="*60)

        for test in failed_tests:
            print(f"\n[FAIL] {test['name']} FAILED")
            descriptor = test.get('descriptor', '')
            if descriptor:
                print(f"   Command: {descriptor}")
            print(f"   Exit Code: {test['exit_code']}")
            print(f"   Duration: {test['duration']:.1f}s")

            # Analyze failure type
            if test['exit_code'] == -1:
                print("   [ISSUE] ISSUE: Test timed out")
                print("   [ACTION] ACTION: Check for infinite loops or very slow operations")
            elif test['exit_code'] == -2:
                print("   [ISSUE] ISSUE: Exception during test execution")
                print("   [ACTION] ACTION: Check test script syntax and dependencies")
            elif "RenderingDevice" in test.get('stderr', ''):
                print("   [ISSUE] ISSUE: GPU context not available")
                print("   [ACTION] ACTION: This is expected in headless CI - test should handle gracefully")
            elif "Failed to create" in test.get('stderr', ''):
                print("   [ISSUE] ISSUE: Object creation failed")
                print("   [ACTION] ACTION: Check if required modules are compiled and available")
            else:
                print("   [ISSUE] ISSUE: Functional test failure")
                print("   [ACTION] ACTION: Review test output and fix underlying implementation")

            if test['stderr']:
                print(f"   [NOTE] ERROR OUTPUT:")
                print(f"      {test['stderr'][:300]}...")


def main(argv: Optional[List[str]] = None):
    """Main entry point for baseline QA runner"""

    parser = argparse.ArgumentParser(description="Baseline QA Test Runner for Gaussian Splatting CI")
    parser.add_argument("--godot", help="Override path to the Godot binary.")
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run a fast subset of checks (PLY loader + GPU sorting).",
    )
    parser.add_argument(
        "--category",
        choices=CLI_CATEGORY_CHOICES,
        help="Run only the specified test category. Use 'all' for the full suite.",
    )
    parser.add_argument(
        "--categories",
        help="Comma-separated list of test categories to run (e.g., 'ply,pipeline,runtime,module').",
    )
    parser.add_argument(
        "--qa-baseline",
        default=str(DEFAULT_QA_BASELINE_PATH.relative_to(ROOT)),
        help="Path to QA baseline JSON.",
    )
    parser.add_argument(
        "--update-qa-baseline",
        action="store_true",
        help="Update QA baseline with latest results.",
    )
    parser.add_argument(
        "--require-qa-baseline",
        action="store_true",
        help=(
            "Fail if baseline file is unavailable in compare mode, instead of "
            "skipping with a coverage-gap warning. Equivalent to setting "
            f"{REQUIRE_QA_BASELINE_ENV}=1. Set by baseline_qa.yml's qa-visual "
            "lane against tests/ci/baselines/qa_results.json."
        ),
    )
    parser.add_argument(
        "--qa-require-capture",
        action="store_true",
        help=(
            "Run the QA Scene Suite on the real display (Windows/Vulkan) instead "
            "of --headless, and treat a suite that produced no results as a "
            f"failure rather than a skip. Equivalent to {QA_REQUIRE_CAPTURE_ENV}=1. "
            "Headless has no RenderingDevice, so every viewport capture returns "
            "null and the SSIM scenes cannot report anything real; a lane that "
            "promises a GPU must pass this or it verifies nothing."
        ),
    )
    parser.add_argument(
        "--baseline-report",
        default=str(DEFAULT_BASELINE_REPORT_PATH.relative_to(ROOT)),
        help="Path to machine-readable QA baseline comparison JSON artifact.",
    )
    parser.add_argument(
        "--baseline-summary",
        default=str(DEFAULT_BASELINE_SUMMARY_PATH.relative_to(ROOT)),
        help="Path to human-readable QA baseline comparison Markdown artifact.",
    )
    args = parser.parse_args(argv)
    category = normalize_test_category(args.category)
    category_arg_provided = args.category is not None

    godot_binary = args.godot or os.environ.get('GODOT_BINARY', 'godot')
    if args.godot:
        os.environ['GODOT_BINARY'] = args.godot

    # Validate binary exists
    try:
        result = subprocess.run([godot_binary, '--version'],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=10)
        if result.returncode != 0:
            print(f"[FAIL] Godot binary not working: {godot_binary}")
            sys.exit(1)
        stdout_line = (result.stdout or "").strip()
        print(f"[INFO] Using Godot: {stdout_line}")
    except Exception as e:
        print(f"[FAIL] Could not find Godot binary '{godot_binary}': {e}")
        print("[ACTION] Set GODOT_BINARY environment variable or ensure 'godot' is in PATH")
        sys.exit(1)

    if args.quick and category_arg_provided:
        print("[WARN] Ignoring --quick because --category was provided.")
    # --require-qa-baseline and the env var are equivalent switches (see
    # REQUIRE_QA_BASELINE_ENV above) so enforcement can be flipped on from a
    # workflow env block without a code/argv change once a real baseline exists.
    require_qa_baseline_flag = args.require_qa_baseline or _env_flag(REQUIRE_QA_BASELINE_ENV)
    if args.update_qa_baseline and require_qa_baseline_flag:
        print(
            "[WARN] Ignoring --require-qa-baseline/"
            f"{REQUIRE_QA_BASELINE_ENV} because --update-qa-baseline was provided."
        )

    # --qa-require-capture and GS_CI_GPU_REQUIRED are equivalent switches, so a
    # GPU lane that already exports the env var gets capture enforcement without
    # an argv change. This decides BOTH how the suite is launched (real display
    # vs --headless) and whether a no-results run may be laundered into a skip.
    #
    # Deliberately NOT given the structurally-inert check that
    # require_baseline_applies() enforces for --require-qa-baseline. The env var
    # predates this flag and already means "this lane has a GPU": baseline_qa.yml's
    # gpu-tests lane sets GS_CI_GPU_REQUIRED=1 while running --categories sorting,
    # so failing closed on "capture required but qa not selected" would break a
    # green lane that never asked for QA. The mis-wiring this would have caught —
    # a qa lane that silently stops running qa — is already caught by
    # --require-qa-baseline, which the same lane sets.
    qa_require_capture = args.qa_require_capture or _env_flag(QA_REQUIRE_CAPTURE_ENV)

    # Resolve --categories (plural) into a set if provided
    categories_set = None
    if args.categories:
        categories_set = {normalize_test_category(c.strip()) for c in args.categories.split(",")}

    # Run tests
    run_quick = args.quick and not category_arg_provided
    runner = BaselineQARunner(godot_binary, qa_require_capture=qa_require_capture)
    success = runner.run_all_tests(
        quick=run_quick,
        category=category,
        categories=categories_set,
    )

    qa_results_path = ROOT / "tests" / "ci" / "qa_results.json"
    qa_baseline_path = resolve_root_path(args.qa_baseline)
    baseline_report_path = resolve_root_path(args.baseline_report)
    baseline_summary_path = resolve_root_path(args.baseline_summary)
    qa_ran = resolve_qa_ran(category=category, categories=categories_set, quick=run_quick)
    require_baseline_effective = require_qa_baseline_flag and not args.update_qa_baseline
    if not require_baseline_applies(require_baseline_effective, qa_ran):
        print(f"[FAIL] {REQUIRE_QA_BASELINE_INERT_MESSAGE}")
        success = False

    if qa_ran:
        qa_scene_result = next((test for test in runner.test_results["tests"] if test.get("name") == "QA Scene Suite"), None)
        qa_scene_skipped = bool(qa_scene_result and qa_scene_result.get("status") == "skipped")
        if qa_scene_skipped and args.update_qa_baseline:
            print("[FAIL] QA baseline update requested, but QA Scene Suite was skipped.")
            qa_ok = False
        elif qa_scene_skipped:
            skip_reason = (
                (qa_scene_result or {})
                .get("details", {})
                .get("skip_reason", "QA Scene Suite skipped; QA baseline comparison not applicable.")
            )
            qa_ok = runner._record_qa_baseline_skipped(
                qa_results_path=qa_results_path,
                baseline_path=qa_baseline_path,
                report_path=baseline_report_path,
                summary_path=baseline_summary_path,
                reason=skip_reason,
                require_baseline=require_baseline_effective,
                require_capture=qa_require_capture,
            )
        else:
            qa_ok = runner.compare_qa_baseline(
                qa_results_path=qa_results_path,
                baseline_path=qa_baseline_path,
                update_baseline=args.update_qa_baseline,
                require_baseline=require_baseline_effective,
                report_path=baseline_report_path,
                summary_path=baseline_summary_path,
            )
        success = success and qa_ok

    # Generate reports
    runner.generate_report()
    runner.print_actionable_failures()

    # Exit with appropriate code
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
