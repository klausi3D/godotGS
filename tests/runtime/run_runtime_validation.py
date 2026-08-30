#!/usr/bin/env python3
"""Unified runtime validation runner.

Builds and executes lightweight C++ harnesses alongside GDScript runtime checks.

Examples:
  - Default release-ready profile run:
      python3 tests/runtime/run_runtime_validation.py
  - Blocking streaming GPU gate:
      python3 tests/runtime/run_runtime_validation.py --profile streaming-gpu-ci
  - Profile-based stress lane:
      python3 tests/runtime/run_runtime_validation.py --profile stress-only

Completion-marker contract (T3 / #891, ADR adr-phase1-guard-hardening.md section 4)
-----------------------------------------------------------------------------------

`passed` is NOT the fall-through. A scenario is classified `passed` only when its
output carries a positive completion marker:

    [RUNTIME_PASS] {"scenario": "<registry name>", "assertions": <int>}

emitted by the scenario itself (the harness never synthesises the marker it
checks; GDScript scenarios emit it through tests/runtime/gs_runtime_report.gd).
Classification order is: nonzero exit > [RUNTIME_FAIL] > [RUNTIME_SKIP] >
completion evaluation. Within completion evaluation:

  - A present marker with a malformed payload (not one JSON object per line,
    missing/mistyped fields) is a FAILURE, not a missing-optional.
  - The payload's `scenario` field must equal the registry name of the scenario
    being classified; a mismatch is a FAILURE. This makes the marker
    non-transferable: a copy-pasted emitter cannot vouch for another scenario.
  - More than one [RUNTIME_PASS] line is a FAILURE (duplicate/ambiguous claim).
  - `assertions == 0` is a FAILURE unless the payload also carries a
    `no_assertions_reason` string AND the scenario has an unexpired entry in the
    `zero_assertion_allowlist` of runtime_scenarios.json (quarantine-manifest
    shape: scenario, reason, issue_url, owner, expires_utc). The allowlist
    fails in both directions: an entry whose scenario reports `assertions > 0`
    fails as STALE and must be removed in the same change that made the
    scenario assert again; an EXPIRED entry fails config validation at load on
    every run (not only runs that select the exempted scenario) and no longer
    exempts. Unknown scenario names in the allowlist fail config validation
    (membership is pinned).
  - No [RUNTIME_PASS] marker at all is recorded with the advisory status
    `no_completion_marker` (ladder step 1). It is visible in the report,
    counted in the summary (`summary["no_completion_marker"]`), printed by the
    summary printer, and does NOT fail the run: the status is added to the
    summary-schema vocabulary, is excluded from `summary["failed"]`, and so
    satisfies both terms of the exit expression (failed == 0 and schema_valid)
    at once. The fail-closed flip (ladder step 2) is a LATER PR gated on the
    soak below; at the flip this status becomes a failure with reason
    "no completion marker" and is removed from the schema vocabulary.

Soak criterion for the fail-closed flip (ADR section 4.6, settled Q4) -- measured
over the per-profile report artifacts preserved by CI (distinct --report-path
per invocation in gaussian_production_gates.yml; release_ci_runtime.yml already
uploads its own):

  1. Per-profile completeness: 5 consecutive runs of each CI-invoked profile
     (headless-ci, streaming-gpu-ci, release-ci) whose preserved report shows
     `summary["no_completion_marker"] == 0` and a completion marker recorded
     for every scenario that profile selected.
  2. Union coverage: the soaked profiles must between them select all 11
     GDS_TESTS scenarios; the flip PR asserts this in code, derived from
     runtime_scenarios.json and GDS_TESTS, failing loudly and naming any
     orphaned scenario.

The two C++ scenarios run in no CI lane (--skip-cpp is universal); their arming
evidence is a recorded run without --skip-cpp produced by the flip PR itself
(ADR section 4.6.2), never the soak.

Assertion-floor forward reference (#914 amendment A1): step 2 pins the
per-scenario assertion counts observed during the soak as a shrink-only floor.
That floor does not exist at this step, so the A1 self-certification hole (one
diff lowering the floor while removing assertions) is not exploitable yet; the
flip PR that creates the floor MUST ratchet it against the immutable review
base (the `git show "$GS_PR_BASE_SHA:<floor file>"` pattern used by
agentic_pr_gate.yml, failing closed when the base copy cannot be resolved),
never against its own head. The ADR text amendment itself is tracked on #914.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_DIR = ROOT / "tests" / "runtime"
BUILD_DIR = RUNTIME_DIR / "build"
SYNTHETIC_ASSET_PREP_SCRIPT = RUNTIME_DIR / "prepare_synthetic_assets.py"

SKIP_MARKER = "[RUNTIME_SKIP]"
FAIL_MARKER = "[RUNTIME_FAIL]"
METRICS_MARKER = "[RUNTIME_METRICS]"
# T3 (#891): the positive completion marker. See the module docstring for the
# full contract; emitted by scenarios via tests/runtime/gs_runtime_report.gd.
PASS_MARKER = "[RUNTIME_PASS]"
# Ladder step 1 advisory status for "ran, exited 0, produced no completion
# marker". Recorded and printed, counted in summary["no_completion_marker"],
# part of the summary-schema vocabulary, and NOT counted as failed. The
# fail-closed flip (ADR section 4.6 step 2) turns this into a failure and
# removes the status from the vocabulary (no producer remains after the flip:
# allowlisted zero-assertion scenarios classify as "passed").
NO_COMPLETION_MARKER_STATUS = "no_completion_marker"
DEFAULT_SCENARIO_CONFIG = RUNTIME_DIR / "runtime_scenarios.json"

# #787: how many trailing output lines a non-passing scenario contributes to the summary.
# Fatal engine traps print their diagnostic last, so the tail is the load-bearing part.
OUTPUT_TAIL_LINES = 40

# #787: exit codes the harness synthesizes itself when the child never reported one -- a
# timeout (124) or a failure to launch (127). Those results legitimately carry a reason with
# no captured output, so the "a failed scenario must retain its tail" rule must not apply to
# them or every silent timeout would be reported as a schema violation.
HARNESS_SYNTHESIZED_EXIT_CODES = frozenset({124, 127})


@dataclass
class TestResult:
    name: str
    command: List[str]
    duration: float
    exit_code: int
    stdout: str
    stderr: str
    status: str = "failed"
    reasons: List[str] = field(default_factory=list)
    metrics: Dict[str, object] = field(default_factory=dict)
    # T3 (#891): what the completion evaluation observed. Always serialised so
    # the soak can read marker presence and assertion counts out of the report.
    completion: Dict[str, object] = field(default_factory=dict)

    @property
    def success(self) -> bool:
        return self.status == "passed"

    def output_tail(self, max_lines: int = OUTPUT_TAIL_LINES) -> List[str]:
        """Last lines of the scenario's combined output, for non-passing runs.

        #787: a scenario that dies on a fatal engine trap prints the diagnostic that names
        the defect (e.g. `FATAL: Index i = N is out of bounds (size = M)`) immediately before
        aborting -- but the summary only ever serialised `reasons`, and on a crash `reasons`
        falls back to the FIRST non-empty stderr line, which is a benign startup warning.
        The nightly GPU streaming crash therefore reported "Can't create an accessibility
        driver" for a fatal out-of-bounds write, and the message identifying the real fault
        was captured and then thrown away. Keep the TAIL, where fatal errors actually land.
        """
        if self.success:
            return []
        combined = f"{self.stdout}\n{self.stderr}"
        lines = [line.rstrip() for line in combined.splitlines() if line.strip()]
        return lines[-max_lines:]


@dataclass(frozen=True)
class GodotRunConfig:
    binary: str
    mode: str
    timeout: int
    extra_args: tuple[str, ...]
    fail_on_skip: bool
    allow_skip_tests: tuple[str, ...]
    project_path: Optional[Path]


@dataclass(frozen=True)
class CppBuildConfig:
    link_mode: str
    include_dirs: tuple[Path, ...] = ()
    compile_flags: tuple[str, ...] = ()
    link_flags: tuple[str, ...] = ()


CPP_TESTS: Dict[str, Path] = {
    "Runtime Modifications": RUNTIME_DIR / "test_runtime_modifications.cpp",
    "Animation Persistence": RUNTIME_DIR / "test_animation_persistence.cpp",
}

GDS_TESTS: Dict[str, Path] = {
    "Interactive State": RUNTIME_DIR / "test_interactive_state.gd",
    "GPU Streaming Stress": RUNTIME_DIR / "test_gpu_streaming_stress.gd",
    "Engine Capability Sanity": RUNTIME_DIR / "test_engine_capabilities.gd",
    "Scene Effector Runtime Controls": RUNTIME_DIR / "test_scene_effector_runtime_controls.gd",
    "World Streaming Gate": RUNTIME_DIR / "test_world_streaming_gate.gd",
    "Streaming Residency API": RUNTIME_DIR / "test_streaming_residency_api.gd",
    "Streaming GPU Tier Budget Contract": RUNTIME_DIR / "test_streaming_gpu_tier_budget_contract.gd",
    "Canonical Node Asset Render": RUNTIME_DIR / "test_canonical_node_asset_render.gd",
    "Data Flow Recent Window": RUNTIME_DIR / "test_data_flow_recent_window.gd",
    "Pipeline Trace Freshness": RUNTIME_DIR / "test_pipeline_trace_freshness.gd",
    "Monitor Lifecycle Hardening": RUNTIME_DIR / "test_monitor_lifecycle_hardening.gd",
}


def ensure_build_dir() -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)


# #790 asked for `--godot-binary` to be forwarded here too. It cannot be, for the
# same measured reason as tests/ci/run_module_tests.py: this harness runs in the
# module-validation workspace, whose evidence step
# (tests/ci/collect_production_evidence.ps1) executes the QA scene suite, and that
# suite's committed expectations are pinned to the 1024-splat Python-fallback
# test_splats.ply. Six harness scripts here load that fixture directly
# (test_canonical_node_asset_render.gd, test_data_flow_recent_window.gd,
# test_pipeline_trace_freshness.gd, test_scene_effector_runtime_controls.gd,
# test_world_streaming_gate.gd) or the world baked from it
# (test_mixed_residency_routing.gd). Flipping the corpus is a baseline change,
# not a wiring fix.
FIXTURE_CORPUS_BLOCKER = (
    "the QA corpus this workspace also runs is pinned to it "
    "(tests/ci/baselines/qa_results.json records source_splat_count=1024 and the "
    "committed test_splats.gsplatworld is a 1024-splat bake); regenerating at the C++ "
    "count requires rebaking the world and re-measuring the QA baseline first (#790)"
)


def ensure_synthetic_assets() -> None:
    if not SYNTHETIC_ASSET_PREP_SCRIPT.is_file():
        raise RuntimeError(
            f"Missing synthetic asset prep script: {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}"
        )

    command = [sys.executable, str(SYNTHETIC_ASSET_PREP_SCRIPT), "--quiet"]
    print(f"[runtime] Fixture generator: Python fallback by design -- {FIXTURE_CORPUS_BLOCKER}.")
    print(f"[runtime] Preparing synthetic assets: {_format_command(command)}")
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            cwd=ROOT,
        )
    except (OSError, PermissionError) as exc:
        raise RuntimeError(f"Synthetic asset prep failed to launch: {type(exc).__name__}: {exc}") from exc

    # Echo prep output on success too: under --quiet the only thing it prints is
    # the low-fidelity warning (#790), and a warning nobody sees is not a warning.
    for line in (completed.stdout or "").splitlines():
        if line.strip():
            print(line)

    if completed.returncode != 0:
        output = ((completed.stdout or "") + (completed.stderr or "")).strip()
        detail = _first_non_empty_line(output) or f"exit code {completed.returncode}"
        raise RuntimeError(f"Synthetic asset prep failed: {detail}")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Gaussian runtime validation harnesses.")
    parser.add_argument(
        "--godot-binary",
        default=os.environ.get("GODOT_BINARY", "godot"),
        help="Path to Godot binary (default: GODOT_BINARY env or 'godot').",
    )
    parser.add_argument(
        "--gd-mode",
        choices=("headless", "non-headless", "windows-vulkan"),
        default=None,
        help=(
            "Override the profile-selected Godot runtime mode. "
            "Defaults to the selected profile's gd_mode, otherwise GS_RUNTIME_GD_MODE."
        ),
    )
    parser.add_argument(
        "--project-path",
        default=None,
        help="Optional project path passed as '--path <dir>' for GDScript runs.",
    )
    parser.add_argument(
        "--godot-arg",
        action="append",
        default=[],
        help="Extra Godot argument (repeatable).",
    )
    parser.add_argument(
        "--gd-timeout",
        type=int,
        default=300,
        help="Timeout in seconds for each GDScript runtime test.",
    )
    parser.add_argument(
        "--cpp-timeout",
        type=int,
        default=300,
        help="Timeout in seconds for each compiled C++ harness.",
    )
    parser.add_argument(
        "--cpp-link-mode",
        choices=("standalone", "module-linked"),
        default=os.environ.get("GS_RUNTIME_CPP_LINK_MODE", "standalone"),
        help=(
            "C++ harness link mode. 'standalone' keeps mock-only harness behavior. "
            "'module-linked' requires --cpp-build-manifest."
        ),
    )
    parser.add_argument(
        "--cpp-build-manifest",
        default=None,
        help=(
            "Path to JSON manifest for module-linked C++ harness builds "
            "(include_dirs, compile_flags, link_flags)."
        ),
    )
    parser.add_argument("--skip-cpp", action="store_true", help="Skip C++ harnesses.")
    parser.add_argument("--skip-gd", action="store_true", help="Skip GDScript runtime harnesses.")
    parser.add_argument(
        "--scenario-config",
        default=str(DEFAULT_SCENARIO_CONFIG),
        help=(
            "Path to runtime scenario configuration JSON file "
            f"(default: {DEFAULT_SCENARIO_CONFIG.relative_to(ROOT)})."
        ),
    )
    parser.add_argument(
        "--report-path",
        default=str(RUNTIME_DIR / "runtime_validation_report.json"),
        help="Path to write the JSON validation report (default: tests/runtime/runtime_validation_report.json).",
    )
    parser.add_argument(
        "--profile",
        default=None,
        help="Runtime scenario profile name (from --scenario-config).",
    )
    parser.add_argument(
        "--list-profiles",
        action="store_true",
        help="List available runtime scenario profiles and exit.",
    )
    parser.add_argument(
        "--gd-script",
        action="append",
        default=[],
        help=(
            "Run only the specified GDScript runtime path (relative to repo root). "
            "Repeatable."
        ),
    )
    parser.add_argument(
        "--gd-test",
        action="append",
        default=[],
        help=(
            "Run only named GDScript runtime tests from profile/config (repeatable). "
            "Mutually exclusive with --gd-script."
        ),
    )
    parser.add_argument(
        "--cpp-test",
        action="append",
        default=[],
        help="Run only named C++ runtime harness tests from profile/config (repeatable).",
    )
    parser.add_argument(
        "--fail-on-skip",
        action="store_true",
        help="Treat runtime skip markers as failures.",
    )
    parser.add_argument(
        "--allow-skips",
        action="store_true",
        help="Allow runtime skip markers without failing the run.",
    )
    parser.add_argument(
        "--allow-skip-test",
        action="append",
        default=[],
        help=(
            "Allow listed runtime tests to skip without failing, even when --fail-on-skip is set. "
            "Repeatable."
        ),
    )
    args = parser.parse_args()
    valid_cpp_link_modes = {"standalone", "module-linked"}
    if args.cpp_link_mode not in valid_cpp_link_modes:
        parser.error(
            "Invalid --cpp-link-mode '{mode}'. Expected one of: {valid}.".format(
                mode=args.cpp_link_mode,
                valid=", ".join(sorted(valid_cpp_link_modes)),
            )
        )
    if args.fail_on_skip and args.allow_skips:
        parser.error("Use only one of --fail-on-skip or --allow-skips.")
    if args.gd_script and args.gd_test:
        parser.error("Use only one of --gd-script or --gd-test.")
    return args


def _extract_marker_reasons(output: str, marker: str) -> List[str]:
    reasons: List[str] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        idx = line.find(marker)
        if idx == -1:
            continue
        reason = line[idx + len(marker):].strip(" :-")
        reasons.append(reason if reason else line)
    return reasons


def _coerce_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def _first_non_empty_line(text: str) -> Optional[str]:
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line:
            return line
    return None


def _extract_metrics_payload(output: str) -> Dict[str, object]:
    metrics: Dict[str, object] = {}
    for raw_line in output.splitlines():
        marker_index = raw_line.find(METRICS_MARKER)
        if marker_index == -1:
            continue
        payload = raw_line[marker_index + len(METRICS_MARKER):].strip()
        if not payload:
            continue
        try:
            parsed = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, dict):
            metrics = parsed
    return metrics


def _extract_completion_payloads(output: str) -> List[str]:
    """Raw payload text of every line that BEGINS with [RUNTIME_PASS].

    Codex review (PR #915 round 1): unlike the FAIL/SKIP extractors, which only
    make a run stricter when they over-match, this extractor mints a positive
    completion proof -- accepting the token anywhere in a line (find()) would
    let an incidental echo (an engine log line quoting the marker, a scenario
    printing its own documentation) classify a scenario as passed. Both real
    emitters (gs_runtime_report.gd print(); the C++ harnesses' std::cout) write
    the marker at column 0 -- verified against captured producer output, not
    assumed -- so only leading whitespace is tolerated.
    """
    payloads: List[str] = []
    for raw_line in output.splitlines():
        line = raw_line.lstrip()
        if not line.startswith(PASS_MARKER):
            continue
        payloads.append(line[len(PASS_MARKER):].strip())
    return payloads


def _parse_allowlist_expiry(expires_utc: str) -> Optional[datetime]:
    """Parse a zero_assertion_allowlist expires_utc value; None when invalid."""
    text = expires_utc.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def _evaluate_completion(
        result: TestResult,
        combined_output: str,
        zero_assertion_allowlist: Dict[str, Dict[str, object]],
) -> TestResult:
    """Apply the completion-marker contract (module docstring) to a clean run.

    Reached only for exit 0 with no failure and no skip markers. `passed` is
    reachable exclusively from here; the pre-#891 fall-through is deleted.
    """
    payloads = _extract_completion_payloads(combined_output)
    result.completion = {"marker_present": bool(payloads), "assertions": None}

    def _fail(reason: str) -> TestResult:
        result.status = "failed"
        result.reasons = [reason]
        return result

    if not payloads:
        # Ladder step 1 (advisory): recorded, counted, printed -- not a failure.
        result.status = NO_COMPLETION_MARKER_STATUS
        result.reasons = [
            "No [RUNTIME_PASS] completion marker was emitted; the scenario ran but proved "
            "nothing it reported. Advisory until the fail-closed flip (ADR "
            "adr-phase1-guard-hardening.md section 4.6)."
        ]
        return result

    if len(payloads) > 1:
        return _fail(
            f"Expected exactly one [RUNTIME_PASS] completion marker, found {len(payloads)}; "
            "an ambiguous completion claim is a failure."
        )

    payload_text = payloads[0]
    if not payload_text:
        return _fail("[RUNTIME_PASS] marker carried no payload; expected one JSON object on the marker line.")
    try:
        payload = json.loads(payload_text)
    except json.JSONDecodeError as exc:
        return _fail(f"[RUNTIME_PASS] payload is not valid JSON ({exc}); a malformed payload is a failure.")
    if not isinstance(payload, dict):
        return _fail("[RUNTIME_PASS] payload must be a JSON object.")

    scenario = payload.get("scenario")
    if not isinstance(scenario, str) or not scenario.strip():
        return _fail("[RUNTIME_PASS] payload field 'scenario' must be a non-empty string.")
    if scenario != result.name:
        return _fail(
            f"[RUNTIME_PASS] scenario mismatch: marker names '{scenario}' but the scenario "
            f"under classification is '{result.name}'. A completion marker is non-transferable."
        )

    assertions = payload.get("assertions")
    if not isinstance(assertions, int) or isinstance(assertions, bool) or assertions < 0:
        return _fail("[RUNTIME_PASS] payload field 'assertions' must be a non-negative integer.")
    result.completion["assertions"] = assertions

    allow_entry = zero_assertion_allowlist.get(result.name)

    if assertions == 0:
        no_assertions_reason = payload.get("no_assertions_reason")
        if not isinstance(no_assertions_reason, str) or not no_assertions_reason.strip():
            return _fail(
                "Scenario reported assertions=0 without a 'no_assertions_reason' string in the "
                "[RUNTIME_PASS] payload; a scenario that asserts nothing must say why."
            )
        if allow_entry is None:
            return _fail(
                "Scenario reported assertions=0 but has no zero_assertion_allowlist entry in "
                "runtime_scenarios.json; an untracked exemption is not available."
            )
        expiry = _parse_allowlist_expiry(str(allow_entry.get("expires_utc", "")))
        if expiry is None:
            return _fail(
                f"zero_assertion_allowlist entry for '{result.name}' has an unparseable "
                "expires_utc; failing closed."
            )
        if datetime.now(timezone.utc) >= expiry:
            return _fail(
                f"zero_assertion_allowlist entry for '{result.name}' expired at "
                f"{allow_entry.get('expires_utc')}; renew deliberately or make the scenario assert."
            )
        result.status = "passed"
        result.completion["allowlisted_zero_assertions"] = True
        result.reasons = []
        return result

    if allow_entry is not None:
        # Both-directions staleness (ADR section 4.3): an exemption whose
        # scenario has resumed asserting is a licence sitting unused. The diff
        # that makes the scenario assert again must remove the entry.
        return _fail(
            f"Stale zero_assertion_allowlist entry: '{result.name}' reported "
            f"assertions={assertions} (> 0). Remove the allowlist entry."
        )

    result.status = "passed"
    result.reasons = []
    return result


def _classify_result(
        result: TestResult,
        *,
        fail_on_skip: bool,
        allow_skip_tests: set[str],
        zero_assertion_allowlist: Optional[Dict[str, Dict[str, object]]] = None,
) -> TestResult:
    combined_output = f"{result.stdout}\n{result.stderr}"
    result.metrics = _extract_metrics_payload(combined_output)
    explicit_failures = _extract_marker_reasons(combined_output, FAIL_MARKER)
    skip_reasons = _extract_marker_reasons(combined_output, SKIP_MARKER)

    if result.exit_code != 0:
        result.status = "failed"
        if explicit_failures:
            result.reasons = explicit_failures
            return result
        if result.reasons:
            return result
        fallback = (
            _first_non_empty_line(result.stderr)
            or _first_non_empty_line(result.stdout)
            or f"Exited with code {result.exit_code}"
        )
        result.reasons = [fallback]
        return result

    if explicit_failures:
        result.status = "failed"
        result.reasons = explicit_failures
        return result

    if skip_reasons:
        if result.name in allow_skip_tests:
            result.status = "skipped"
            result.reasons = [f"Allowed runtime skip: {reason}" for reason in skip_reasons]
            return result
        if fail_on_skip:
            result.status = "failed"
            result.reasons = [f"Unexpected runtime skip: {reason}" for reason in skip_reasons]
        else:
            result.status = "skipped"
            result.reasons = skip_reasons
        return result

    # T3 (#891): `passed` is no longer the fall-through. A clean exit must
    # positively prove completion; see _evaluate_completion.
    return _evaluate_completion(
        result,
        combined_output,
        zero_assertion_allowlist if zero_assertion_allowlist is not None else {},
    )


def _format_command(command: Iterable[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def run_command(name: str, command: List[str], *, cwd: Optional[Path], timeout: int) -> TestResult:
    print(f"\n[runtime] Running {name}")
    print(f"[runtime] Command: {_format_command(command)}")
    start = time.time()
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            # Decode the child's streams explicitly. `text=True` alone uses the
            # host locale (cp1252 on the Windows self-hosted runner), so ONE
            # byte the codepage cannot map raises UnicodeDecodeError inside
            # subprocess's reader thread and discards the ENTIRE captured
            # stream. Observed locally on this gate: the run reported
            # `metrics: {}` and an empty output tail on a test that had printed
            # both. Everything this harness derives from output -- the metrics
            # payload, the [RUNTIME_FAIL] reasons, [RUNTIME_SKIP], and the #883
            # [RUNTIME_ADVISORY] lines -- silently disappears, leaving only the
            # exit code. errors="replace" keeps the rest of the stream instead
            # of trading all of it for one unmappable byte.
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            cwd=cwd or ROOT,
        )
    except subprocess.TimeoutExpired as exc:
        duration = time.time() - start
        stdout = _coerce_text(exc.stdout)
        stderr = _coerce_text(exc.stderr)
        print(f"[runtime] [FAIL] {name} timed out after {timeout}s")
        return TestResult(
            name=name,
            command=command,
            duration=duration,
            exit_code=124,
            stdout=stdout,
            stderr=stderr,
            status="failed",
            reasons=[f"Timed out after {timeout}s"],
        )
    except (OSError, PermissionError) as exc:
        duration = time.time() - start
        print(f"[runtime] [FAIL] {name} failed to launch: {type(exc).__name__}: {exc}")
        return TestResult(
            name=name,
            command=command,
            duration=duration,
            exit_code=127,
            stdout="",
            stderr=f"{type(exc).__name__}: {exc}",
            status="failed",
            reasons=[f"{type(exc).__name__}: {exc}"],
        )

    duration = time.time() - start
    status = "ok" if completed.returncode == 0 else f"exit={completed.returncode}"
    print(f"[runtime] Completed {name} ({status}) in {duration:.1f}s")
    return TestResult(
        name=name,
        command=command,
        duration=duration,
        exit_code=completed.returncode,
        stdout=_coerce_text(completed.stdout),
        stderr=_coerce_text(completed.stderr),
    )


def _load_cpp_build_config(link_mode: str, manifest_path_value: Optional[str]) -> CppBuildConfig:
    if link_mode == "standalone":
        return CppBuildConfig(link_mode=link_mode)

    if not manifest_path_value:
        raise ValueError("module-linked C++ mode requires --cpp-build-manifest.")

    manifest_path = Path(manifest_path_value)
    if not manifest_path.is_absolute():
        manifest_path = (ROOT / manifest_path).resolve()
    else:
        manifest_path = manifest_path.resolve()

    with manifest_path.open("r", encoding="utf-8") as handle:
        raw = json.load(handle)

    if not isinstance(raw, dict):
        raise ValueError("C++ build manifest must be a JSON object.")

    include_values = raw.get("include_dirs", [])
    compile_values = raw.get("compile_flags", [])
    link_values = raw.get("link_flags", [])
    if not isinstance(include_values, list) or not all(isinstance(value, str) for value in include_values):
        raise ValueError("Manifest field 'include_dirs' must be a string list.")
    if not isinstance(compile_values, list) or not all(isinstance(value, str) for value in compile_values):
        raise ValueError("Manifest field 'compile_flags' must be a string list.")
    if not isinstance(link_values, list) or not all(isinstance(value, str) for value in link_values):
        raise ValueError("Manifest field 'link_flags' must be a string list.")

    include_dirs: list[Path] = []
    for include_dir in include_values:
        path = Path(include_dir)
        if not path.is_absolute():
            path = (ROOT / path).resolve()
        else:
            path = path.resolve()
        include_dirs.append(path)

    return CppBuildConfig(
        link_mode=link_mode,
        include_dirs=tuple(include_dirs),
        compile_flags=tuple(str(value) for value in compile_values),
        link_flags=tuple(str(value) for value in link_values),
    )


def compile_cpp_test(name: str, source: Path, cpp_build_config: CppBuildConfig) -> Path:
    ensure_build_dir()
    output = BUILD_DIR / source.stem
    command: List[str] = [
        os.environ.get("CXX", "g++"),
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
    ]
    for include_dir in cpp_build_config.include_dirs:
        command.append(f"-I{include_dir}")
    command.extend(cpp_build_config.compile_flags)
    command.extend([str(source), "-o", str(output)])
    command.extend(cpp_build_config.link_flags)
    print(f"\n[runtime] Compiling {name}: {source.relative_to(ROOT)}")
    print(f"[runtime] C++ link mode: {cpp_build_config.link_mode}")
    print(f"[runtime] Command: {_format_command(command)}")
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except (OSError, PermissionError) as exc:
        raise RuntimeError(f"Compiler invocation failed: {type(exc).__name__}: {exc}") from exc

    if result.returncode != 0:
        detail = (
            _first_non_empty_line(result.stderr)
            or _first_non_empty_line(result.stdout)
            or f"Compiler exited with code {result.returncode}"
        )
        raise RuntimeError(detail)

    print(f"[runtime] Built {output.relative_to(ROOT)}")
    return output


def run_cpp_harnesses(
        timeout: int,
        selected_tests: Iterable[str],
        cpp_build_config: CppBuildConfig,
        zero_assertion_allowlist: Optional[Dict[str, Dict[str, object]]] = None,
) -> List[TestResult]:
    try:
        cpp_tests = _resolve_named_test_map(CPP_TESTS, selected_tests, kind="C++ runtime test")
    except ValueError as exc:
        return [
            TestResult(
                name="C++ Runtime Selection",
                command=[],
                duration=0.0,
                exit_code=1,
                stdout="",
                stderr=str(exc),
                status="failed",
                reasons=[str(exc)],
            )
        ]

    results: List[TestResult] = []
    for name, source in cpp_tests.items():
        try:
            binary = compile_cpp_test(name, source, cpp_build_config)
        except RuntimeError as exc:
            result = TestResult(
                name=name,
                command=[os.environ.get("CXX", "g++"), str(source)],
                duration=0.0,
                exit_code=1,
                stdout="",
                stderr=str(exc),
                status="failed",
                reasons=[f"Compilation failed: {exc}"],
            )
            results.append(result)
            continue

        result = run_command(name, [str(binary)], cwd=ROOT, timeout=timeout)
        results.append(
            _classify_result(
                result,
                fail_on_skip=False,
                allow_skip_tests=set(),
                zero_assertion_allowlist=zero_assertion_allowlist,
            )
        )
    return results


def _godot_binary_is_available(godot_binary: str) -> Optional[str]:
    try:
        probe = subprocess.run(
            [godot_binary, "--version"],
            check=False,
            capture_output=True,
            text=True,
        )
    except (OSError, PermissionError) as exc:
        return f"{type(exc).__name__}: {exc}"

    if probe.returncode != 0:
        output = (probe.stdout or "") + (probe.stderr or "")
        reason = _first_non_empty_line(output) or f"exit code {probe.returncode}"
        return f"Godot binary probe failed: {reason}"
    return None


def _resolve_mode_args(mode: str) -> List[str]:
    if mode == "headless":
        return ["--headless"]
    if mode == "windows-vulkan":
        return ["--render-thread", "safe", "--display-driver", "windows", "--rendering-driver", "vulkan"]
    if mode == "non-headless":
        # Resolve "non-headless" per host platform so the default release-ci
        # profile stays portable across Windows/Linux/macOS while still picking
        # an explicit vulkan driver. Without a driver flag, Godot 4.5 falls
        # back to OpenGL Compatibility on Windows, which has no RenderingDevice
        # singleton and silently fails any test that needs one (driver-roulette
        # observed in #382: 9/12 vs 5/12 pass rates on the same machine).
        import platform as _platform
        system = _platform.system()
        if system == "Windows":
            return ["--render-thread", "safe", "--display-driver", "windows", "--rendering-driver", "vulkan"]
        if system == "Linux":
            return ["--render-thread", "safe", "--display-driver", "x11", "--rendering-driver", "vulkan"]
        if system == "Darwin":
            return ["--render-thread", "safe", "--display-driver", "macos", "--rendering-driver", "vulkan"]
        # Unknown platform: leave the driver flag off rather than guess; CI
        # for unsupported platforms can fall back to the prior behavior.
        return []
    return []


def _build_godot_command(config: GodotRunConfig, script: Path) -> List[str]:
    command = [config.binary, *_resolve_mode_args(config.mode)]
    if config.project_path is not None:
        command.extend(["--path", str(config.project_path)])
    command.extend(config.extra_args)
    script_arg: Path
    if config.project_path is None:
        script_arg = script.relative_to(ROOT)
    else:
        try:
            script_arg = script.relative_to(config.project_path)
        except ValueError:
            script_arg = script
    command.extend(["--verbose", "--script", str(script_arg)])
    return command


def _resolve_gd_test_map(selected_scripts: Iterable[str]) -> Dict[str, Path]:
    selected_list = [entry.strip() for entry in selected_scripts if entry and entry.strip()]
    if not selected_list:
        return dict(GDS_TESTS)

    resolved: Dict[str, Path] = {}
    # T3 (#891): reverse map so registered scripts keep their registry name.
    registry_names = {path.resolve(): name for name, path in GDS_TESTS.items()}
    for script_path_str in selected_list:
        script_path = Path(script_path_str)
        if not script_path.is_absolute():
            script_path = (ROOT / script_path).resolve()
        else:
            script_path = script_path.resolve()

        if not script_path.exists():
            raise FileNotFoundError(f"GDScript runtime test not found: {script_path_str}")

        try:
            relative = script_path.relative_to(ROOT)
        except ValueError as exc:
            raise FileNotFoundError(
                f"GDScript runtime test must be inside repository: {script_path_str}"
            ) from exc

        # T3 (#891): a registered script keeps its registry name so the
        # completion marker it emits (bound to the registry name) matches the
        # scenario identity under --gd-script runs too. Only unregistered
        # ad-hoc scripts fall back to the filename-derived display name.
        display_name = registry_names.get(
            script_path, relative.stem.replace("_", " ").title()
        )
        resolved[display_name] = script_path
    return resolved


def _resolve_named_test_map(
        all_tests: Dict[str, Path],
        selected_names: Iterable[str],
        *,
        kind: str,
) -> Dict[str, Path]:
    selected_list = [entry.strip() for entry in selected_names if entry and entry.strip()]
    if not selected_list:
        # Empty profile selection must stay empty; this allows profiles to explicitly disable suites.
        return {}

    resolved: Dict[str, Path] = {}
    unknown: List[str] = []
    for name in selected_list:
        if name not in all_tests:
            unknown.append(name)
            continue
        resolved[name] = all_tests[name]

    if unknown:
        available = ", ".join(sorted(all_tests.keys()))
        missing = ", ".join(sorted(unknown))
        raise ValueError(f"Unknown {kind} name(s): {missing}. Available: {available}")

    return resolved


def _load_scenario_config(path: Path) -> Dict[str, object]:
    with path.open("r", encoding="utf-8") as handle:
        raw = json.load(handle)

    if not isinstance(raw, dict):
        raise ValueError("Scenario config must be a JSON object.")

    version = raw.get("version")
    if not isinstance(version, int):
        raise ValueError("Scenario config field 'version' must be an integer.")

    default_profile = raw.get("default_profile")
    if not isinstance(default_profile, str) or not default_profile.strip():
        raise ValueError("Scenario config field 'default_profile' must be a non-empty string.")

    profiles = raw.get("profiles")
    if not isinstance(profiles, dict) or not profiles:
        raise ValueError("Scenario config field 'profiles' must be a non-empty object.")

    for profile_name, profile_config in profiles.items():
        if not isinstance(profile_name, str) or not profile_name.strip():
            raise ValueError("Scenario profile names must be non-empty strings.")
        if not isinstance(profile_config, dict):
            raise ValueError(f"Profile '{profile_name}' must be a JSON object.")

        cpp_tests = profile_config.get("cpp_tests", [])
        gd_tests = profile_config.get("gd_tests", [])
        godot_args = profile_config.get("godot_args", [])

        if not isinstance(cpp_tests, list) or not all(isinstance(value, str) for value in cpp_tests):
            raise ValueError(f"Profile '{profile_name}' field 'cpp_tests' must be a string list.")
        if not isinstance(gd_tests, list) or not all(isinstance(value, str) for value in gd_tests):
            raise ValueError(f"Profile '{profile_name}' field 'gd_tests' must be a string list.")
        if not isinstance(godot_args, list) or not all(isinstance(value, str) for value in godot_args):
            raise ValueError(f"Profile '{profile_name}' field 'godot_args' must be a string list.")

        if "cpp_timeout" in profile_config and not isinstance(profile_config["cpp_timeout"], int):
            raise ValueError(f"Profile '{profile_name}' field 'cpp_timeout' must be an integer.")
        if "gd_timeout" in profile_config and not isinstance(profile_config["gd_timeout"], int):
            raise ValueError(f"Profile '{profile_name}' field 'gd_timeout' must be an integer.")
        if "fail_on_skip" in profile_config and not isinstance(profile_config["fail_on_skip"], bool):
            raise ValueError(f"Profile '{profile_name}' field 'fail_on_skip' must be a boolean.")
        if "requires_renderer_proof" in profile_config and not isinstance(
            profile_config["requires_renderer_proof"], bool
        ):
            raise ValueError(f"Profile '{profile_name}' field 'requires_renderer_proof' must be a boolean.")
        if "gd_mode" in profile_config:
            gd_mode = profile_config["gd_mode"]
            valid_modes = {"headless", "non-headless", "windows-vulkan"}
            if not isinstance(gd_mode, str) or gd_mode not in valid_modes:
                raise ValueError(
                    f"Profile '{profile_name}' field 'gd_mode' must be one of: {', '.join(sorted(valid_modes))}."
                )

    if default_profile not in profiles:
        raise ValueError(
            f"default_profile '{default_profile}' is not defined in profiles: "
            f"{', '.join(sorted(str(name) for name in profiles.keys()))}"
        )

    _validate_zero_assertion_allowlist(raw.get("zero_assertion_allowlist", []))

    return raw


# T3 (#891, ADR section 4.3): required fields of a zero_assertion_allowlist
# entry -- the programme's one waiver idiom (quarantine-manifest shape).
ZERO_ASSERTION_ALLOWLIST_FIELDS = ("scenario", "reason", "issue_url", "owner", "expires_utc")


def _validate_zero_assertion_allowlist(raw_entries: object) -> None:
    """Fail closed on any malformed zero_assertion_allowlist.

    Entry lifecycle (stated at design time, ADR section 2.10):
      - ENTRY: one diff adds the allowlist record (all fields, a real issue,
        an expiry) together with the scenario change that makes it emit
        `assertions: 0` plus a `no_assertions_reason`. A record without the
        emission is unused; the emission without a record fails the run.
      - EXIT: one diff removes the record together with the scenario change
        that makes it assert again. Either half alone is loud: a scenario
        asserting past a live record fails as STALE, and a zero-assertion
        scenario without a record fails as untracked. Expiry passing is the
        third loud exit, and it is loud HERE, at config load, on every run of
        every profile -- not only when the exempted scenario is selected --
        until the entry is renewed deliberately or removed.
    Membership is pinned: an entry naming a scenario outside the registry is a
    config error, so a renamed scenario cannot silently keep an exemption.
    """
    if not isinstance(raw_entries, list):
        raise ValueError("Scenario config field 'zero_assertion_allowlist' must be a list.")
    known_scenarios = set(GDS_TESTS.keys()) | set(CPP_TESTS.keys())
    seen: set[str] = set()
    for index, entry in enumerate(raw_entries):
        prefix = f"zero_assertion_allowlist[{index}]"
        if not isinstance(entry, dict):
            raise ValueError(f"{prefix} must be a JSON object.")
        for field_name in ZERO_ASSERTION_ALLOWLIST_FIELDS:
            value = entry.get(field_name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{prefix} field '{field_name}' must be a non-empty string.")
        unknown_fields = sorted(set(entry.keys()) - set(ZERO_ASSERTION_ALLOWLIST_FIELDS))
        if unknown_fields:
            raise ValueError(f"{prefix} carries unknown field(s): {', '.join(unknown_fields)}.")
        scenario = str(entry["scenario"])
        if scenario not in known_scenarios:
            raise ValueError(
                f"{prefix} names unknown scenario '{scenario}'; allowlist membership is "
                "pinned to the registry (GDS_TESTS + CPP_TESTS)."
            )
        if scenario in seen:
            raise ValueError(f"{prefix} duplicates scenario '{scenario}'.")
        seen.add(scenario)
        expiry = _parse_allowlist_expiry(str(entry["expires_utc"]))
        if expiry is None:
            raise ValueError(
                f"{prefix} field 'expires_utc' is not a parseable ISO-8601 UTC timestamp."
            )
        # Codex review (PR #915 round 1): expiry must be loud on EVERY run, not
        # only on runs whose profile selects the exempted scenario -- otherwise
        # an expired entry for an unselected scenario survives silently, which
        # is the renewed-on-reflex rot the expiry exists to prevent. Same
        # posture as the quarantine manifest: staleness fails at load.
        if datetime.now(timezone.utc) >= expiry:
            raise ValueError(
                f"{prefix} for scenario '{scenario}' expired at {entry['expires_utc']}; "
                "renew it deliberately (new expiry + review) or remove it."
            )


def _load_zero_assertion_allowlist(scenario_config: Dict[str, object]) -> Dict[str, Dict[str, object]]:
    """Allowlist keyed by scenario name; config was already validated on load."""
    entries = scenario_config.get("zero_assertion_allowlist", [])
    allowlist: Dict[str, Dict[str, object]] = {}
    if isinstance(entries, list):
        for entry in entries:
            if isinstance(entry, dict) and isinstance(entry.get("scenario"), str):
                allowlist[entry["scenario"]] = entry
    return allowlist


def _build_renderer_proof_summary(
    results: List[TestResult],
    *,
    required: bool,
) -> Dict[str, object]:
    proof_tests: List[Dict[str, object]] = []
    for result in results:
        metrics = result.metrics if isinstance(result.metrics, dict) else {}
        proof_kind = metrics.get("renderer_proof_kind")
        proof_status = metrics.get("renderer_proof_status")
        if proof_kind is None and proof_status is None:
            continue
        status_text = str(proof_status or "unknown").strip() or "unknown"
        proof_tests.append(
            {
                "name": result.name,
                "result_status": result.status,
                "proof_kind": str(proof_kind or "unknown"),
                "proof_status": status_text,
                "reason": str(metrics.get("reason", "")),
                "asset_path": str(metrics.get("asset_path", "")),
                "visible_splats_max": metrics.get("visible_splats_max"),
                "visual_luma_variance_max": metrics.get("visual_luma_variance_max"),
                "visual_luma_range_max": metrics.get("visual_luma_range_max"),
                "visual_non_background_samples_max": metrics.get("visual_non_background_samples_max"),
                "visual_non_background_ratio_max": metrics.get("visual_non_background_ratio_max"),
            }
        )

    passed = [entry for entry in proof_tests if entry["proof_status"] == "passed"]
    unavailable = [
        entry
        for entry in proof_tests
        if str(entry["proof_status"]).startswith("skipped")
        or str(entry["proof_status"]).startswith("unavailable")
    ]
    failed = [
        entry
        for entry in proof_tests
        if str(entry["proof_status"]) in {"failed", "failure", "error"}
    ]

    failure_reasons: List[str] = []
    if required and not proof_tests:
        failure_reasons.append("No renderer proof metrics were emitted by the selected runtime tests.")
    if required and not passed:
        failure_reasons.append("No renderer proof test reported renderer_proof_status=passed.")
    if required and unavailable:
        names = ", ".join(str(entry["name"]) for entry in unavailable)
        failure_reasons.append(f"Required renderer proof was unavailable or skipped: {names}.")
    if required and failed:
        names = ", ".join(str(entry["name"]) for entry in failed)
        failure_reasons.append(f"Required renderer proof failed: {names}.")

    status = "failed" if failure_reasons else ("passed" if required else "not_required")
    return {
        "required": required,
        "status": status,
        "passed": len(passed),
        "unavailable": len(unavailable),
        "failed": len(failed),
        "total": len(proof_tests),
        "failure_reasons": failure_reasons,
        "proof_tests": proof_tests,
    }


def _print_profiles(config: Dict[str, object], *, source: Path) -> None:
    profiles = config.get("profiles", {})
    default_profile = config.get("default_profile", "")
    print(f"[runtime] Scenario config: {source}")
    print("[runtime] Available profiles:")
    for profile_name in sorted(profiles.keys()):
        profile_config = profiles[profile_name]
        description = ""
        if isinstance(profile_config, dict):
            description = str(profile_config.get("description", "")).strip()
        default_tag = " (default)" if profile_name == default_profile else ""
        if description:
            print(f"  - {profile_name}{default_tag}: {description}")
        else:
            print(f"  - {profile_name}{default_tag}")


def _validate_summary_schema(summary: Dict[str, object]) -> List[str]:
    errors: List[str] = []
    required_fields = ("total", "passed", "failed", "skipped", "no_completion_marker", "duration", "tests")
    for field_name in required_fields:
        if field_name not in summary:
            errors.append(f"Missing summary field '{field_name}'.")

    for integer_field in ("total", "passed", "failed", "skipped", "no_completion_marker"):
        value = summary.get(integer_field)
        if not isinstance(value, int):
            errors.append(f"Field '{integer_field}' must be an integer.")
        elif value < 0:
            errors.append(f"Field '{integer_field}' must be >= 0.")

    duration = summary.get("duration")
    if not isinstance(duration, (int, float)):
        errors.append("Field 'duration' must be numeric.")
    elif duration < 0:
        errors.append("Field 'duration' must be >= 0.")

    tests = summary.get("tests")
    if not isinstance(tests, list):
        errors.append("Field 'tests' must be a list.")
        return errors

    # T3 (#891): the advisory no-completion-marker status is schema-legal during
    # ladder step 1 so an advisory scenario cannot flip schema_valid and re-fail
    # the run through the second term of the exit expression. The fail-closed
    # flip removes it from this vocabulary.
    allowed_statuses = {"passed", "failed", "skipped", NO_COMPLETION_MARKER_STATUS}
    for index, test_entry in enumerate(tests):
        prefix = f"tests[{index}]"
        if not isinstance(test_entry, dict):
            errors.append(f"{prefix} must be an object.")
            continue
        for required_key in ("name", "status", "reasons", "command", "duration", "exit_code", "metrics", "completion", "output_tail"):
            if required_key not in test_entry:
                errors.append(f"{prefix} missing '{required_key}'.")
        name = test_entry.get("name")
        if not isinstance(name, str) or not name.strip():
            errors.append(f"{prefix}.name must be a non-empty string.")
        status = test_entry.get("status")
        if not isinstance(status, str) or status not in allowed_statuses:
            errors.append(f"{prefix}.status must be one of {sorted(allowed_statuses)}.")
        reasons = test_entry.get("reasons")
        if not isinstance(reasons, list) or not all(isinstance(reason, str) for reason in reasons):
            errors.append(f"{prefix}.reasons must be a string list.")
        command = test_entry.get("command")
        if not isinstance(command, list) or not all(isinstance(part, str) for part in command):
            errors.append(f"{prefix}.command must be a string list.")
        test_duration = test_entry.get("duration")
        if not isinstance(test_duration, (int, float)) or test_duration < 0:
            errors.append(f"{prefix}.duration must be numeric >= 0.")
        exit_code = test_entry.get("exit_code")
        if not isinstance(exit_code, int):
            errors.append(f"{prefix}.exit_code must be an integer.")
        metrics = test_entry.get("metrics")
        if not isinstance(metrics, dict):
            errors.append(f"{prefix}.metrics must be an object.")
        completion = test_entry.get("completion")
        if not isinstance(completion, dict):
            errors.append(f"{prefix}.completion must be an object.")
        output_tail = test_entry.get("output_tail")
        if not isinstance(output_tail, list) or not all(isinstance(line, str) for line in output_tail):
            errors.append(f"{prefix}.output_tail must be a string list.")
        # #787: a failing scenario that produced output must carry it. Without this the key
        # could regress to an always-empty list and the summary would look well-formed while
        # losing the only diagnostic a crashed run ever emits. Timeouts and launch failures are
        # exempt: the harness synthesizes those reasons itself and the child may genuinely have
        # emitted nothing.
        elif (
            status == "failed"
            and not output_tail
            and test_entry.get("reasons")
            and exit_code not in HARNESS_SYNTHESIZED_EXIT_CODES
        ):
            errors.append(f"{prefix}.output_tail must not be empty for a failed scenario.")

    if isinstance(tests, list):
        expected_total = summary.get("total")
        if isinstance(expected_total, int) and expected_total != len(tests):
            errors.append(
                f"Field 'total' ({expected_total}) does not match number of test entries ({len(tests)})."
            )
        # T3 (#891): the four status buckets must partition the run, so a new
        # status can never silently drop scenarios out of every counter.
        bucket_fields = ("passed", "failed", "skipped", "no_completion_marker")
        bucket_values = [summary.get(field_name) for field_name in bucket_fields]
        if isinstance(expected_total, int) and all(isinstance(v, int) for v in bucket_values):
            if sum(bucket_values) != expected_total:
                errors.append(
                    "Status buckets {buckets} sum to {total_buckets}, but 'total' is {total}.".format(
                        buckets="+".join(bucket_fields),
                        total_buckets=sum(bucket_values),
                        total=expected_total,
                    )
                )

    renderer_proof = summary.get("renderer_proof")
    if renderer_proof is not None:
        if not isinstance(renderer_proof, dict):
            errors.append("Field 'renderer_proof' must be an object when present.")
        else:
            status = renderer_proof.get("status")
            if status not in {"not_required", "passed", "failed"}:
                errors.append("renderer_proof.status must be one of ['failed', 'not_required', 'passed'].")
            required = renderer_proof.get("required")
            if not isinstance(required, bool):
                errors.append("renderer_proof.required must be a boolean.")
            proof_tests = renderer_proof.get("proof_tests")
            if not isinstance(proof_tests, list):
                errors.append("renderer_proof.proof_tests must be a list.")
            failure_reasons = renderer_proof.get("failure_reasons")
            if not isinstance(failure_reasons, list) or not all(
                isinstance(reason, str) for reason in failure_reasons
            ):
                errors.append("renderer_proof.failure_reasons must be a string list.")

    return errors


def run_gd_tests(
        config: GodotRunConfig,
        selected_scripts: Iterable[str],
        selected_tests: Iterable[str],
        zero_assertion_allowlist: Optional[Dict[str, Dict[str, object]]] = None,
) -> List[TestResult]:
    gd_scripts = [entry for entry in selected_scripts if entry and entry.strip()]
    if gd_scripts:
        try:
            gd_tests = _resolve_gd_test_map(gd_scripts)
        except FileNotFoundError as exc:
            return [
                TestResult(
                    name="GDScript Runtime Selection",
                    command=[],
                    duration=0.0,
                    exit_code=1,
                    stdout="",
                    stderr=str(exc),
                    status="failed",
                    reasons=[str(exc)],
                )
            ]
    else:
        try:
            gd_tests = _resolve_named_test_map(GDS_TESTS, selected_tests, kind="GDScript runtime test")
        except ValueError as exc:
            return [
                TestResult(
                    name="GDScript Runtime Selection",
                    command=[],
                    duration=0.0,
                    exit_code=1,
                    stdout="",
                    stderr=str(exc),
                    status="failed",
                    reasons=[str(exc)],
                )
            ]

    availability_error = _godot_binary_is_available(config.binary)
    if availability_error:
        results: List[TestResult] = []
        for name, script in gd_tests.items():
            command = _build_godot_command(config, script)
            results.append(
                TestResult(
                    name=name,
                    command=command,
                    duration=0.0,
                    exit_code=127,
                    stdout="",
                    stderr=availability_error,
                    status="failed",
                    reasons=[availability_error],
                )
            )
        return results

    print(
        "[runtime] GDScript mode="
        f"{config.mode} fail_on_skip={'yes' if config.fail_on_skip else 'no'} "
        f"allow_skip_tests={len(config.allow_skip_tests)}"
    )
    results: List[TestResult] = []
    allow_skip_tests = set(config.allow_skip_tests)
    for name, script in gd_tests.items():
        command = _build_godot_command(config, script)
        result = run_command(name, command, cwd=ROOT, timeout=config.timeout)
        results.append(
            _classify_result(
                result,
                fail_on_skip=config.fail_on_skip,
                allow_skip_tests=allow_skip_tests,
                zero_assertion_allowlist=zero_assertion_allowlist,
            )
        )
    return results


def summarise(results: List[TestResult]) -> Dict[str, object]:
    summary = {
        "total": len(results),
        "passed": sum(1 for r in results if r.status == "passed"),
        "failed": sum(1 for r in results if r.status == "failed"),
        "skipped": sum(1 for r in results if r.status == "skipped"),
        # T3 (#891): advisory "ran, proved nothing" count. Deliberately NOT part
        # of "failed" during ladder step 1; the soak reads this field.
        "no_completion_marker": sum(
            1 for r in results if r.status == NO_COMPLETION_MARKER_STATUS
        ),
        "duration": sum(r.duration for r in results),
        "tests": [
            {
                "name": r.name,
                "status": r.status,
                "reasons": r.reasons,
                "command": r.command,
                "duration": r.duration,
                "exit_code": r.exit_code,
                "metrics": r.metrics,
                # T3 (#891): marker presence + assertion count, for the soak
                # and the step-2 assertion floor.
                "completion": r.completion,
                # #787: always present so the key is uniformly validatable; populated only for
                # non-passing scenarios, where it is the only record of why the run died.
                "output_tail": r.output_tail(),
            }
            for r in results
        ],
    }
    return summary


def _print_summary(summary: Dict[str, object]) -> None:
    print("\n=== Runtime Validation Summary ===")
    print(
        "[runtime] total={total} passed={passed} failed={failed} skipped={skipped} "
        "no_completion_marker={no_marker} duration={duration:.1f}s".format(
            total=summary["total"],
            passed=summary["passed"],
            failed=summary["failed"],
            skipped=summary["skipped"],
            no_marker=summary.get("no_completion_marker", 0),
            duration=summary["duration"],
        )
    )
    # T3 (#891) ladder step 1: an advisory result must be visibly advisory.
    advisory_tests = [
        entry
        for entry in summary.get("tests", [])
        if isinstance(entry, dict) and entry.get("status") == NO_COMPLETION_MARKER_STATUS
    ]
    for entry in advisory_tests:
        print(
            "[runtime] [ADVISORY] {name}: ran with no [RUNTIME_PASS] completion marker; "
            "this becomes a failure at the fail-closed flip (ADR section 4.6 step 2).".format(
                name=entry.get("name"),
            )
        )
    schema_valid = bool(summary.get("schema_valid", False))
    if schema_valid:
        print("[runtime] summary schema validation: ok")
    else:
        print("[runtime] summary schema validation: FAILED")
    print(json.dumps(summary, indent=2))


def main() -> int:
    args = _parse_args()
    ensure_build_dir()
    try:
        ensure_synthetic_assets()
    except RuntimeError as exc:
        print(f"[runtime] [FAIL] {exc}")
        return 1

    report_path = Path(args.report_path)
    if not report_path.is_absolute():
        report_path = (ROOT / report_path).resolve()
    else:
        report_path = report_path.resolve()

    scenario_config_path = Path(args.scenario_config)
    if not scenario_config_path.is_absolute():
        scenario_config_path = (ROOT / scenario_config_path).resolve()
    else:
        scenario_config_path = scenario_config_path.resolve()

    try:
        scenario_config = _load_scenario_config(scenario_config_path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"[runtime] [FAIL] invalid scenario config: {exc}")
        return 1

    if args.list_profiles:
        _print_profiles(scenario_config, source=scenario_config_path)
        return 0

    cpp_build_config = CppBuildConfig(link_mode=args.cpp_link_mode)

    raw_profiles = scenario_config.get("profiles", {})
    if not isinstance(raw_profiles, dict):
        print("[runtime] [FAIL] scenario config has invalid 'profiles' section.")
        return 1

    profile_name = args.profile or str(scenario_config.get("default_profile", "")).strip()
    if not profile_name:
        print("[runtime] [FAIL] no profile selected and scenario config has no default_profile.")
        return 1
    if profile_name not in raw_profiles:
        print(
            "[runtime] [FAIL] unknown profile '{profile}'. available: {available}".format(
                profile=profile_name,
                available=", ".join(sorted(str(name) for name in raw_profiles.keys())),
            )
        )
        return 1

    profile_config = raw_profiles[profile_name]
    if not isinstance(profile_config, dict):
        print(f"[runtime] [FAIL] scenario profile '{profile_name}' must be an object.")
        return 1

    profile_cpp_tests = [str(name) for name in profile_config.get("cpp_tests", [])]
    profile_gd_tests = [str(name) for name in profile_config.get("gd_tests", [])]
    profile_godot_args = tuple(str(value) for value in profile_config.get("godot_args", []))
    profile_gd_mode = str(profile_config.get("gd_mode", "")).strip()
    requires_renderer_proof = bool(profile_config.get("requires_renderer_proof", False))
    cpp_timeout = int(profile_config.get("cpp_timeout", args.cpp_timeout))
    gd_timeout = int(profile_config.get("gd_timeout", args.gd_timeout))
    env_gd_mode = os.environ.get("GS_RUNTIME_GD_MODE")
    resolved_gd_mode = args.gd_mode or profile_gd_mode or env_gd_mode or "headless"
    if env_gd_mode and not args.gd_mode and profile_gd_mode and env_gd_mode != profile_gd_mode:
        print(
            "[runtime] Ignoring GS_RUNTIME_GD_MODE={env!r}; profile '{profile}' requires gd_mode={mode!r}.".format(
                env=env_gd_mode,
                profile=profile_name,
                mode=profile_gd_mode,
            )
        )

    selected_cpp_tests = args.cpp_test if args.cpp_test else profile_cpp_tests
    selected_gd_tests = args.gd_test if args.gd_test else profile_gd_tests
    should_run_cpp_harnesses = not args.skip_cpp and len(selected_cpp_tests) > 0
    if should_run_cpp_harnesses:
        try:
            cpp_build_config = _load_cpp_build_config(args.cpp_link_mode, args.cpp_build_manifest)
        except (OSError, json.JSONDecodeError, ValueError) as exc:
            print(f"[runtime] [FAIL] invalid C++ build configuration: {exc}")
            return 1
    allow_skip_tests = tuple(dict.fromkeys(str(name) for name in args.allow_skip_test if str(name).strip()))

    default_fail_on_skip = resolved_gd_mode != "headless"
    if args.fail_on_skip:
        fail_on_skip = True
    elif args.allow_skips:
        fail_on_skip = False
    elif "fail_on_skip" in profile_config:
        fail_on_skip = bool(profile_config.get("fail_on_skip"))
    else:
        fail_on_skip = default_fail_on_skip

    project_path = Path(args.project_path).resolve() if args.project_path else None
    gd_config = GodotRunConfig(
        binary=args.godot_binary,
        mode=resolved_gd_mode,
        timeout=gd_timeout,
        extra_args=tuple(profile_godot_args) + tuple(args.godot_arg),
        fail_on_skip=fail_on_skip,
        allow_skip_tests=allow_skip_tests,
        project_path=project_path,
    )

    try:
        config_display = scenario_config_path.relative_to(ROOT)
    except ValueError:
        config_display = scenario_config_path
    print(
        f"[runtime] profile='{profile_name}' scenario_config='{config_display}' "
        f"cpp_tests={len(selected_cpp_tests)} gd_tests={len(selected_gd_tests)} "
        f"gd_mode={resolved_gd_mode} cpp_link_mode={cpp_build_config.link_mode}"
    )

    zero_assertion_allowlist = _load_zero_assertion_allowlist(scenario_config)

    all_results: List[TestResult] = []
    if should_run_cpp_harnesses:
        all_results.extend(
            run_cpp_harnesses(
                timeout=cpp_timeout,
                selected_tests=selected_cpp_tests,
                cpp_build_config=cpp_build_config,
                zero_assertion_allowlist=zero_assertion_allowlist,
            )
        )
    if not args.skip_gd:
        all_results.extend(
            run_gd_tests(
                gd_config,
                args.gd_script,
                selected_gd_tests,
                zero_assertion_allowlist=zero_assertion_allowlist,
            )
        )

    summary = summarise(all_results)
    summary["profile"] = profile_name
    summary["gd_mode"] = resolved_gd_mode
    summary["scenario_config"] = str(config_display)
    summary["renderer_proof"] = _build_renderer_proof_summary(
        all_results,
        required=requires_renderer_proof,
    )
    schema_errors = _validate_summary_schema(summary)
    summary["schema_valid"] = len(schema_errors) == 0
    summary["schema_errors"] = schema_errors
    if schema_errors:
        for error in schema_errors:
            print(f"[runtime] [FAIL] schema: {error}")
    _print_summary(summary)

    report_path.parent.mkdir(parents=True, exist_ok=True)
    with report_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)
    try:
        report_display = report_path.relative_to(ROOT)
    except ValueError:
        report_display = report_path
    print(f"[runtime] Report saved to {report_display}")

    renderer_proof = summary.get("renderer_proof", {})
    renderer_proof_failed = isinstance(renderer_proof, dict) and renderer_proof.get("status") == "failed"
    if renderer_proof_failed:
        for reason in renderer_proof.get("failure_reasons", []):
            print(f"[runtime] [FAIL] renderer proof: {reason}")

    return 0 if summary["failed"] == 0 and summary["schema_valid"] and not renderer_proof_failed else 1


if __name__ == "__main__":
    sys.exit(main())
