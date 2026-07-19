#!/usr/bin/env python3
"""
Supervisor for the Gaussian Splatting GPU test harness.

Drives the Godot test binary's --gs-gpu-test entrypoint in per-batch
subprocesses so a driver hang or GPU OOM during one batch can't corrupt
the next. Each batch is a doctest --test-case= filter set. Aggregates
per-batch results into a JSON report and returns max(batch_rc) so the
caller (baseline_qa.yml or a local developer) sees a non-zero exit
whenever any GPU test fails.

This is Phase 2B of the GPU harness rollout. The harness itself
(gs_gpu_test_runner.cpp) is Phase 2A; CI integration in baseline_qa.yml
is Phase 3.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_REPORT_PATH = ROOT / "tests" / "ci" / "gpu_harness_report.json"
DEFAULT_TIMEOUT_SECONDS = 60
REPORT_SCHEMA_VERSION = 1

# Each batch maps a name to one or more doctest --test-case patterns.
# The CompositorHazard batch is the only one that has an actually-running
# test today (test_output_compositor_composite_hazard.h). The rest are
# catalogued so future [RequiresGPU] migrations land in a known batch
# without re-shaping the supervisor; they currently report "0 tests
# matched" which the supervisor treats as success (advisory) per batch.
#
# Intentionally no catch-all "*[RequiresGPU]*" batch: tests that aren't in a
# named batch stay invisible until they're added explicitly, and a wildcard
# would silently absorb every future tag family (including [Importer], which
# still cannot run here).
#
# #329 note: this comment used to claim the [SceneTree]+[RequiresGPU] cases
# "crash with an access violation because SceneTree isn't initialized". That
# diagnosis was wrong on both counts, and the hardcoded count ("26") had
# silently drifted to 28 — which is why no number is hardcoded anywhere in this
# file any more. See DEFERRED_SCENETREE_NOTE below.
@dataclass(frozen=True)
class BatchSpec:
    name: str
    filters: tuple[str, ...]
    # Optional per-batch timeout override. None = use global --timeout. Set when
    # a single batch is known to take measurably longer (or shorter) than the
    # default, so a hung batch can't burn the budget reserved for later ones.
    timeout_seconds: Optional[int] = None
    # doctest `--test-case-exclude=` patterns subtracted from `filters` (#329).
    #
    # Use ONLY to carve out cases that are known-broken and declared in
    # docs/reference/renderer_release_gate_manifest.json:deferred_requires_gpu_waivers.
    # A guard (tests/ci/test_gpu_harness_deferred_contract.py) requires every
    # exclude here to have a matching waiver entry, so an exclusion cannot be
    # used to quietly drop a case that merely became inconvenient.
    #
    # The excluded case rejoins the batch automatically once its waiver is
    # removed, which is the direction we want drift to flow.
    excludes: tuple[str, ...] = ()


BATCHES: tuple[BatchSpec, ...] = (
    BatchSpec("CompositorHazard", ("*HazardRepro*",)),
    BatchSpec("OutputCompositor", ("*OutputCompositor*][RequiresGPU]*",)),
    BatchSpec("ComputeInfrastructure", ("*ComputeInfra*][RequiresGPU]*",)),
    BatchSpec("TileRenderer", ("*TileRenderer*][RequiresGPU]*",)),
    BatchSpec("GpuSorting", ("*Sort*][RequiresGPU]*",)),
    BatchSpec("MemoryStream", ("*MemoryStream*][RequiresGPU]*",)),
    BatchSpec("Streaming", ("*Streaming*][RequiresGPU]*",)),
    # #641: the sync-policy device-contract tests in
    # modules/gaussian_splatting/tests/test_integration.cpp. They need a real
    # local RenderingDevice, which only this harness provides; before #641 they
    # were untagged and matched no lane's filter in either run_module_tests.py or
    # here, so they had never executed once since being written. NOT in
    # REQUIRED_BATCHES: one of the two ("...main-device no-submit/no-sync
    # contract") still cannot run anywhere — it needs a main-window
    # RenderingDevice that no lane creates — so requiring a matching, executing
    # case here would encode a promise the harness cannot keep.
    BatchSpec("Integration", ("*Integration*][RequiresGPU]*",)),
    # Render route/stage cascade failure-injection coverage for #351. These tests carry no
    # subsystem bracket tag, so the batch matches on the descriptive phrase. The phrases are
    # deliberately NOT wrapped in "[...]" brackets: the manifest contract check matches with
    # fnmatch (where "[RequiresGPU]" would be a character class), while doctest treats
    # brackets literally, so only a bracket-free phrase matches identically in both.
    #
    # Counts only the cascade tests that ACTUALLY execute their assertions under the GPU
    # harness (no SceneTree): the resident cull/sort-skip case, the resident raster-failure
    # case, the streaming-requested hard-fail case, and the serial failure-injection case —
    # genuine resident + streaming + serial route/stage coverage as #351 requires. The
    # streaming leg is the "Streaming-requested failure hard-fails" case: it forces
    # route_policy=STREAMING with the streaming system released and asserts a typed
    # COMMON.SKIP.STREAMING_NOT_READY.* route that does not bounce to resident. It gates only
    # on RenderingServer/ProjectSettings/GaussianSplatManager/RenderingDevice (all present
    # under the harness), so it runs its assertions rather than early-returning.
    #
    # The separate "Stage results report streaming not-ready" case stays EXCLUDED: it depends
    # on a SceneTree (via ScopedWorldStreamingRenderer) but is not [SceneTree]-tagged, so the
    # test listener (tests/test_main.cpp) never gives it a SceneTree and it early-returns —
    # which doctest counts as a PASS, not a skip. Counting it would let this required batch be
    # satisfied without exercising the streaming cascade (Codex #418). Its stage-status
    # assertions remain host/contract coverage until it is given a SceneTree-free fixture.
    BatchSpec("RendererPipeline", (
            "*Stage results report cull*",
            "*Stage results report raster*",
            "*Streaming-requested failure hard-fails*",
            "*Serial instancing failure injection*",
    )),
    # Lifetime batch: the renderer/RID leak + shutdown-lifetime proof scenarios that
    # carry #352's "zero rid_leak_bytes" evidence. Re-enabled now that the --gs-gpu-test
    # runner starts WorkerThreadPool (issue #392 fix in gs_gpu_test_runner.cpp), so the
    # GPU scenarios run their assertions instead of skipping via
    # REQUIRE_WORKER_THREAD_POOL(). Bracket-free phrases (fnmatch treats [tags] as char
    # classes). The host-only stringname_orphans + monotonicity scenarios and the
    # no-device failed_init scenario run on the module-test lane, not here.
    BatchSpec("Lifetime", (
            "*renderer_instance lifetime proof*",
            "*scene_director_reload lifetime proof*",
            "*asset_attach_detach lifetime proof*",
            "*tile_shader_recompile frees old shaders*",
    )),
    # ------------------------------------------------------------------
    # #329: the [SceneTree]+[RequiresGPU] corpus, which until now executed in
    # NO lane at all — excluded from every headless lane in run_module_tests.py
    # and matching none of the named batches above.
    #
    # Two harness bugs (both fixed in gs_gpu_test_runner.cpp, not worked around
    # here) were keeping them out:
    #
    #   1. `gs_gpu_test_main` never called DisplayServerMock::register_mock_driver().
    #      The globally-registered GodotTestCaseListener DOES build a SceneTree for
    #      these cases, but it first looks for a DisplayServer driver named "mock";
    #      finding none it created nothing, then called RenderingServerDefault::init()
    #      with no DisplayServer behind it -> null deref in RendererCompositor::create.
    #      That — not "SceneTree isn't initialized" — was the access violation.
    #   2. Teardown order destroyed the RenderingDevice before module
    #      uninitialization freed renderer GPU resources through it. Cases ran
    #      green and crashed on the way out.
    #
    # Filters are the literal tag triples (doctest treats brackets literally),
    # so a newly added [SceneTree] GPU case joins the right batch automatically
    # instead of silently landing nowhere — the failure mode this issue is about.
    #
    # NOT in REQUIRED_BATCHES yet: some cases are still genuinely broken and are
    # excluded below, each with a waiver in the release-gate manifest. Promote
    # these to required once the waiver list is empty (see #329 follow-ups).
    #
    # No count is written here on purpose — that is exactly the "26 that had
    # silently become 28" drift this file already warns about below. Ask the
    # source instead:
    #   python tests/ci/test_gpu_harness_deferred_contract.py --print-summary
    #
    # timeout_seconds=180 (#677). This batch is the largest in the harness: 18
    # executing cases at ~2.4-2.9 s each is ~45 s of case time, and the runner's
    # RenderingDevice bring-up/teardown adds ~35 s, for ~82 s wall measured on
    # ccbef8cc482 (RTX 3090). Under the 60 s default it was cut off mid-run at
    # exactly 9 of 18 cases with rc=124 and no doctest summary line — so half the
    # corpus #329/#660 landed was still not executing, and the batch's parsed
    # totals read 0/0. It is a BUDGET problem, not a hang: given enough time the
    # batch completes cleanly (18/18 passed, 175/175 assertions, Status: SUCCESS).
    # 180 s leaves ~2.2x headroom for a loaded CI runner. Do NOT raise this to
    # paper over a genuine hang — a hang must be diagnosed, not budgeted.
    # No excludes: the whole [Node][SceneTree][RequiresGPU] corpus executes.
    #
    # "Shared renderer hides node-local debug settings" and "Shared renderer
    # preserves local painterly and color grading state" were excluded here until
    # they were fixed as genuine PRODUCT bugs (P2 shared-renderer gating): the
    # peer-set convergence hook in GaussianSplatNode3D and the painterly P2 gate
    # in GaussianSplatNodeRendererHelper::apply_renderer_settings.
    #
    # The last four exclusions were removed in the #329 waiver-reduction pass.
    # All four were TEST defects, not product defects:
    #   * "instance buffer drops hidden nodes", "Scene sphere effectors build
    #     per-instance selection masks" and "instance buffer tracks per-node
    #     opacity" all matched instance rows by translation_x, which is the NODE
    #     transform origin (gaussian_splat_scene_director.cpp:1705), while
    #     offsetting the splat inside the asset and never calling set_position.
    #     Every lookup therefore missed: one case asserted 0 == 0 vacuously, one
    #     crashed the batch by indexing with -1 (#656), and one silently
    #     early-returned past all of its assertions while reported green.
    #   * "full-fidelity override only follows attached assets" set a
    #     full-fidelity asset ACTIVE in both halves, which short-circuits
    #     _renderer_requests_conservative_full_fidelity_runtime
    #     (gaussian_splat_renderer.cpp:136-139) before the director is consulted,
    #     so the two halves were indistinguishable to the predicate.
    #   * "ignores hidden full-fidelity assets" asserted the OPPOSITE of the
    #     documented no-flicker contract (gaussian_splat_renderer.cpp:150-153)
    #     and is now "full-fidelity collection ignores node visibility".
    # A third, independent defect was found in the two full-fidelity cases: they
    # set visibility_threshold to 0.2f, which set_visibility_threshold clamps to
    # 0.1f (render_quality_orchestrator.cpp:242), so the read-back assertion was
    # unsatisfiable regardless of the override.
    # Each fix was mutation-checked: reverting the pinned behaviour fails the case.
    #
    # timeout_seconds raised 180 -> 300 with this change. The batch grew from 18
    # to 22 executing cases (175 -> 285 assertions) and measured 127.7 s, 133.8 s
    # and 153.3 s wall across three consecutive runs on an RTX 3090. Against the
    # slowest observed run, 180 s was only 1.17x -- under the 1.5x floor the
    # NodeSceneTree budget guard in test_gpu_harness_deferred_contract.py encodes,
    # and squarely in #677 silent-truncation territory on a loaded runner. 300 s
    # is ~1.96x. This is a re-budget for genuinely more work, NOT headroom for a
    # hang: every one of the 22 cases completes and the batch reports 22/22.
    BatchSpec("NodeSceneTree", ("*[Node][SceneTree][RequiresGPU]*",), timeout_seconds=300),
    BatchSpec("WorldSceneTree", ("*[World][SceneTree][RequiresGPU]*",), excludes=(
            # GPU sort sync-contract violation: "device already submitted, call
            # sync to wait until done", then a fault inside
            # GPUSortingPipeline::_capture_instance_count_sync -> buffer_get_data
            # -> _flush_and_stall_for_all_frames.
            "*World submission renders through the resident instanced route*",
            "*Resident rejection preserves resident diagnostics*",
            # VK_ERROR_DEVICE_LOST — RenderingDeviceDriverVulkan::on_device_lost
            # during device finalize. A real GPU fault, not a harness artifact.
            "*Explicit resident quantization rejection falls back*",
    )),
    # Two filters, not one, because doctest treats "[" and "]" literally: a case
    # tagged [SceneDirector][WorldSubmission][SceneTree][RequiresGPU] does NOT match
    # "*[SceneDirector][SceneTree][RequiresGPU]*". The world-submission cases in
    # test_scene_director_submission_scaffolding.h carry that extra bracket, so
    # without the second filter they would carry a [RequiresGPU] tag while matching
    # no batch — i.e. they would move from "vacuously green in the strict lane" to
    # "stranded", which is not an improvement. Adding the tag order here rather than
    # flattening the test names keeps [WorldSubmission] usable as a tag.
    #
    # timeout_seconds=180. This batch grew from 2 executing cases (17 assertions,
    # 12.6 s wall) to 12 (166 assertions, 73.3 s wall measured on 7c350507f51,
    # RTX 3090) when ten renderer-dependent cases were retagged out of the strict
    # [SceneTree] lane, where they had been early-returning past every assertion.
    # Under the 60 s default the batch was cut off with rc=124 and no doctest
    # summary, so its parsed totals read 0/0. Verified a BUDGET problem, not a
    # hang: given no timeout the same filter completes cleanly. Do NOT raise this
    # to paper over a genuine hang -- a hang must be diagnosed, not budgeted.
    #
    # #675 added two device-backed world-submission cases (cross-scenario eviction
    # restore, arbitration-rejection end state). Re-measured on 91a03b83efe + that
    # change, RTX 3090: 14/14 cases, 206/206 assertions, 76.9-100.9 s wall across
    # runs (run-to-run variance on this batch is large -- device bring-up
    # dominates). Against the slowest observed 100.9 s, 180 s is ~1.78x, still
    # clear of the >=1.5x headroom the NodeSceneTree budget guard in
    # test_gpu_harness_deferred_contract.py encodes as the minimum.
    BatchSpec("SceneDirectorSceneTree", (
            "*[SceneDirector][SceneTree][RequiresGPU]*",
            "*[SceneDirector][WorldSubmission][SceneTree][RequiresGPU]*",
    ), timeout_seconds=180),
)

# Batches whose filter MUST resolve to at least one matching doctest test case
# for the supervisor to report success. Without this, a test rename or removal
# can silently zero-out a batch — doctest exits 0 with `0 test cases matched`,
# the supervisor sees `max_rc == 0 && parsed_failures == 0` and passes the gate,
# and the visual gate effectively stops running while still showing green in CI.
#
# CompositorHazard is the canonical regression test for the #256 black-blocks
# hazard (the entire reason for the scratch-copy path and this harness). If its
# filter ever stops matching, the gate must fail loudly rather than greenly
# skip — anyone touching the renderer should see immediate signal.
#
# RendererPipeline carries #351's route/stage cascade failure-injection coverage
# (resident, streaming, serial). It is required so a rename/removal of those tests
# fails the gate loudly instead of silently dropping the route-contract coverage.
#
# Lifetime carries #352's renderer GPU-resource leak / shutdown-lifetime proof
# (renderer_instance, scene_director_reload, asset_attach_detach, tile_shader_recompile).
# It is required (allow_rid_leaks=false in the manifest) so a leak regression — or a
# rename/removal of the scenarios — fails the gate loudly: this batch IS the
# "candidate GPU harness report includes zero rid_leak_bytes for required batches"
# evidence #352 demands. Runnable now that the runner starts WorkerThreadPool (#392).
REQUIRED_BATCHES: frozenset[str] = frozenset({"CompositorHazard", "RendererPipeline", "Lifetime"})

# #329: the deferred-case count is DERIVED, never hardcoded.
#
# The previous version of this file stated "26 [SceneTree]+[RequiresGPU] tests
# deferred" in a prose comment. The corpus grew to 28 and nothing noticed,
# because a number in a Python comment is not checked by anything. Both the
# count and the identity of every still-deferred case now come from
# docs/reference/renderer_release_gate_manifest.json:deferred_requires_gpu_waivers,
# which tests/ci/test_gpu_harness_deferred_contract.py cross-checks against the
# real test corpus and against the `excludes` above.
#
# Ask the source, not this comment:
#   python tests/ci/test_gpu_harness_deferred_contract.py --print-summary
DEFERRED_SCENETREE_NOTE = (
    "The [SceneTree]+[RequiresGPU] corpus runs in the NodeSceneTree / WorldSceneTree / "
    "SceneDirectorSceneTree batches. Cases still deferred are exactly the BatchSpec.excludes "
    "entries, each of which must carry a waiver in "
    "docs/reference/renderer_release_gate_manifest.json:deferred_requires_gpu_waivers."
)

# Validate at import time that every required batch name actually exists in
# BATCHES — without this, renaming a batch but forgetting to update the
# REQUIRED_BATCHES set would silently green the gate for selections that
# don't include the (now-vanished) required name. Hard error at import time
# is preferable to a silently broken safety net at runtime.
assert REQUIRED_BATCHES <= {spec.name for spec in BATCHES}, (
    f"REQUIRED_BATCHES references unknown batches: "
    f"{REQUIRED_BATCHES - {spec.name for spec in BATCHES}}"
)

# Matches the listener line emitted by gs_gpu_test_runner.cpp:
#   `[GS-GPU][RID-LEAK?] bytes=N test=advisory`
# The listener can't call CHECK_MESSAGE (doctest reporters run outside test
# assertion context), so leaks surface as stdout lines. We scrape them here
# and fold into the gate decision.
RID_LEAK_RE = re.compile(r"\[GS-GPU\]\[RID-LEAK\?\] bytes=(\d+)")

# Matches the per-scenario lifetime line emitted by the lifetime-proof fixture
# (modules/gaussian_splatting/tests/test_renderer_lifetime_proof.h):
#   `[GS-LIFETIME] {"scenario":"renderer_instance","passed":true,...}`
# A skipped scenario (RendererLifetimeFixture::mark_skipped) and a
# scope-exited-without-finalize (crashed_or_aborted) BOTH emit this line with
# `"passed":false` while doctest still counts the test case as PASSED (a skip
# raises no failed assertion). For a REQUIRED batch that means a scenario could
# silently stop exercising its path — hollow coverage, the same class as the
# #418 streaming-leg finding — yet satisfy the doctest-rc / empty-batch / leak
# gate. We scrape these lines here and treat any non-passing required-batch
# scenario as a gate failure (Codex PR #419 Finding 2). This mirrors the
# manifest's gpu_harness_policy.required_batches[].allow_skips=false intent.
GS_LIFETIME_RE = re.compile(r"\[GS-LIFETIME\]\s*(.+?)\s*$")

# Doctest summary lines we parse:
#   "[doctest] test cases:  N | M passed | K failed | L skipped"
#   "[doctest] assertions: N | M passed | K failed |"
#   "[doctest] Status: SUCCESS!" or "FAILURE!"
TEST_CASES_RE = re.compile(
    r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+)\s+passed\s*\|\s*(\d+)\s+failed(?:\s*\|\s*(\d+)\s+skipped)?"
)
ASSERTIONS_RE = re.compile(
    r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+)\s+passed\s*\|\s*(\d+)\s+failed"
)
STATUS_RE = re.compile(r"\[doctest\] Status:\s*(\w+)")

# Supervisor-level exit codes (orthogonal to harness exit codes 64-71).
SUPERVISOR_EXIT_OK = 0
SUPERVISOR_EXIT_HARNESS_FAILED = 1
SUPERVISOR_EXIT_TIMEOUT = 124
SUPERVISOR_EXIT_BAD_INVOCATION = 2
SUPERVISOR_EXIT_GODOT_MISSING = 3


@dataclass
class BatchResult:
    name: str
    filters: tuple[str, ...]
    excludes: tuple[str, ...] = ()
    rc: int = -1
    wall_seconds: float = 0.0
    timed_out: bool = False
    test_cases_total: int = 0
    test_cases_passed: int = 0
    test_cases_failed: int = 0
    test_cases_skipped: int = 0
    assertions_total: int = 0
    assertions_passed: int = 0
    assertions_failed: int = 0
    status: str = "UNKNOWN"
    stdout_tail: str = ""
    stderr_tail: str = ""
    rid_leak_bytes: int = 0
    summary_parse_ok: bool = False
    # Scenarios from [GS-LIFETIME] lines that reported passed != true (a real
    # failure, a mark_skipped(), or a scope-exited-without-finalize). Each entry
    # is "scenario: fail_reason". Folded into the gate for REQUIRED batches so a
    # skipped lifetime scenario can't satisfy the gate without exercising its
    # path (Codex PR #419 Finding 2).
    lifetime_failed_scenarios: list[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "filters": list(self.filters),
            "excludes": list(self.excludes),
            "rc": self.rc,
            "wall_seconds": round(self.wall_seconds, 3),
            "timed_out": self.timed_out,
            "test_cases": {
                "total": self.test_cases_total,
                "passed": self.test_cases_passed,
                "failed": self.test_cases_failed,
                "skipped": self.test_cases_skipped,
            },
            "assertions": {
                "total": self.assertions_total,
                "passed": self.assertions_passed,
                "failed": self.assertions_failed,
            },
            "status": self.status,
            "stdout_tail": self.stdout_tail,
            "stderr_tail": self.stderr_tail,
            "rid_leak_bytes": self.rid_leak_bytes,
            "summary_parse_ok": self.summary_parse_ok,
            "lifetime_failed_scenarios": list(self.lifetime_failed_scenarios),
        }


def _parse_summary(stdout: str, result: BatchResult) -> None:
    tc_match = TEST_CASES_RE.search(stdout)
    if tc_match:
        result.test_cases_total = int(tc_match.group(1))
        result.test_cases_passed = int(tc_match.group(2))
        result.test_cases_failed = int(tc_match.group(3))
        if tc_match.group(4):
            result.test_cases_skipped = int(tc_match.group(4))

    a_match = ASSERTIONS_RE.search(stdout)
    if a_match:
        result.assertions_total = int(a_match.group(1))
        result.assertions_passed = int(a_match.group(2))
        result.assertions_failed = int(a_match.group(3))

    s_match = STATUS_RE.search(stdout)
    if s_match:
        result.status = s_match.group(1)

    # Track whether ALL three summary regexes matched. Locale shifts, ANSI-color
    # piping, or doctest output-format changes can silently break parsing —
    # without this signal, parsed_failures stays 0 and the gate goes green for
    # a binary that actually emitted `Status: FAILURE!`.
    result.summary_parse_ok = bool(tc_match and a_match and s_match)

    # Sum bytes across every RID-leak advisory the listener emitted during this
    # batch. Listener reporters can't fail tests directly (they run outside
    # assertion context), so we scrape the well-known stdout marker and let the
    # gate decision in main() escalate.
    total_leak = 0
    for lm in RID_LEAK_RE.finditer(stdout):
        try:
            total_leak += int(lm.group(1))
        except ValueError:
            pass
    result.rid_leak_bytes = total_leak

    # Scrape [GS-LIFETIME] scenario lines. A skip (mark_skipped) or a
    # scope-exited-without-finalize both emit passed=false while doctest still
    # counts the case as PASSED, so without this a required lifetime scenario
    # could silently stop running and still green the gate. Collect any
    # non-passing scenario; main() escalates these for REQUIRED batches only.
    # (Codex PR #419 Finding 2.) A line we can't parse as JSON is itself
    # suspect (the contract is a single JSON object after the marker), so we
    # record it as an unparseable-lifetime failure rather than ignoring it.
    for line in stdout.splitlines():
        m = GS_LIFETIME_RE.search(line.strip())
        if not m:
            continue
        try:
            payload = json.loads(m.group(1))
        except (ValueError, TypeError):
            result.lifetime_failed_scenarios.append(
                f"<unparseable lifetime line>: {line.strip()[:200]}"
            )
            continue
        if payload.get("passed") is not True:
            scenario = payload.get("scenario", "<unknown>")
            fail_reason = payload.get("fail_reason") or "no fail_reason reported"
            result.lifetime_failed_scenarios.append(f"{scenario}: {fail_reason}")


def _run_batch(
    godot_binary: Path,
    name: str,
    filters: tuple[str, ...],
    excludes: tuple[str, ...],
    timeout_sec: int,
    extra_args: list[str],
) -> BatchResult:
    # Note: doctest's banner-suppression flag is --no-intro / --dt-no-intro;
    # --no-header is silently ignored. Leaving the banner in stdout for now —
    # the supervisor's _parse_summary regex matches the summary lines regardless,
    # and the banner is useful when triaging a failing run.
    args: list[str] = [str(godot_binary), "--gs-gpu-test"]
    # doctest's argv parser honors only the FIRST `--test-case=` occurrence and
    # silently drops subsequent ones, so we must pack multiple filters into a
    # single comma-separated value rather than emitting one arg per filter.
    # Today every batch has a single filter, but if anyone adds a multi-filter
    # batch in the future the old loop would silently skip everything past the
    # first pattern with the supervisor still reporting SUCCESS. The
    # comma-separated form is the doctest-supported multi-pattern syntax.
    if filters:
        args.append(f"--test-case={','.join(filters)}")
    # Same packing rule applies to excludes: one comma-separated arg, not one
    # arg per pattern.
    if excludes:
        args.append(f"--test-case-exclude={','.join(excludes)}")
    args.extend(extra_args)

    result = BatchResult(name=name, filters=filters, excludes=excludes)
    started = datetime.datetime.now(datetime.timezone.utc)
    print(
        f"[run_gpu_harness] >> batch={name} filters={list(filters)} "
        f"excludes={list(excludes)} timeout={timeout_sec}s",
        flush=True,
    )

    # Stream stdout/stderr through bounded ring buffers rather than
    # subprocess.run(capture_output=True)'s unbounded buffering. A test that
    # prints in a tight loop (easy to do accidentally in a renderer test)
    # would otherwise accumulate the entire stdout into memory until the
    # timeout fires — by which point the supervisor itself can be OOM-killed.
    # The ring buffer trims to ~64 KiB per stream so we still have generous
    # tail context for triage, but failure modes are bounded.
    #
    # Threaded reader pattern (not selectors): Python's selectors module
    # supports only sockets on Windows (WSAStartup error otherwise), and
    # this supervisor must run on the self-hosted Windows runner. A
    # dedicated reader thread per pipe is the portable approach.
    import threading

    BUFFER_BYTES_MAX = 64 * 1024
    stdout_buf = bytearray()
    stderr_buf = bytearray()
    buf_lock = threading.Lock()

    def _pump(stream, dst: bytearray) -> None:
        try:
            while True:
                chunk = stream.read(4096)
                if not chunk:
                    return
                with buf_lock:
                    dst.extend(chunk)
                    if len(dst) > BUFFER_BYTES_MAX:
                        del dst[: len(dst) - BUFFER_BYTES_MAX]
        except (OSError, ValueError):
            # Pipe closed mid-read (terminate/kill) — drop the rest, this is
            # an expected termination path, not an error to surface.
            return

    proc: Optional[subprocess.Popen] = None
    stdout_thread: Optional[threading.Thread] = None
    stderr_thread: Optional[threading.Thread] = None
    timed_out = False
    try:
        proc = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        stdout_thread = threading.Thread(target=_pump, args=(proc.stdout, stdout_buf), daemon=True)
        stderr_thread = threading.Thread(target=_pump, args=(proc.stderr, stderr_buf), daemon=True)
        stdout_thread.start()
        stderr_thread.start()
        try:
            result.rc = proc.wait(timeout=timeout_sec)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            result.rc = SUPERVISOR_EXIT_TIMEOUT
            result.timed_out = True
            result.status = "TIMEOUT"

        # Give the reader threads a brief grace period to drain whatever the
        # child wrote between the last read and the pipe closing on exit.
        stdout_thread.join(timeout=2.0)
        stderr_thread.join(timeout=2.0)

        with buf_lock:
            stdout_text = stdout_buf.decode("utf-8", errors="replace")
            stderr_text = stderr_buf.decode("utf-8", errors="replace")
        # The buffer was already trimmed to BUFFER_BYTES_MAX; tail to 4 KiB
        # for the JSON report to keep artifacts small.
        result.stdout_tail = stdout_text[-4096:]
        result.stderr_tail = stderr_text[-4096:]
        # Parse what we have (full output on success, partial on timeout —
        # if the batch crashed late and we caught the summary in flight,
        # we surface what we got).
        _parse_summary(stdout_text, result)
    except FileNotFoundError as exc:
        # Surfaces when --godot points at a non-existent binary; treat as
        # supervisor misconfiguration rather than a test failure.
        result.rc = SUPERVISOR_EXIT_GODOT_MISSING
        result.status = "GODOT_MISSING"
        result.stderr_tail = f"FileNotFoundError: {exc}"
    finally:
        if proc is not None and proc.poll() is None:
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                    proc.wait()
                except Exception:
                    pass

    elapsed = (datetime.datetime.now(datetime.timezone.utc) - started).total_seconds()
    result.wall_seconds = elapsed

    summary = (
        f"[run_gpu_harness] << batch={name} rc={result.rc} "
        f"cases={result.test_cases_passed}/{result.test_cases_total} "
        f"asserts={result.assertions_passed}/{result.assertions_total} "
        f"wall={elapsed:.1f}s status={result.status}"
    )
    print(summary, flush=True)
    return result


def _select_batches(filter_names: Optional[set[str]]) -> tuple[BatchSpec, ...]:
    if not filter_names:
        return BATCHES
    return tuple(spec for spec in BATCHES if spec.name in filter_names)


def _build_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--godot",
        required=False,
        help="Path to the Godot test binary (with tests=yes). "
        "Defaults to bin/godot.windows.editor.dev.x86_64.console.exe under the repo root.",
    )
    parser.add_argument(
        "--report",
        default=str(DEFAULT_REPORT_PATH),
        help=f"JSON report output path (default: {DEFAULT_REPORT_PATH.relative_to(ROOT)}).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"Per-batch subprocess wall-clock timeout in seconds (default: {DEFAULT_TIMEOUT_SECONDS}).",
    )
    parser.add_argument(
        "--batch",
        action="append",
        default=None,
        help="Restrict to specific batch name(s). Repeatable. If omitted, all batches run.",
    )
    parser.add_argument(
        "--list-batches",
        action="store_true",
        help="Print the batch catalogue and exit.",
    )
    parser.add_argument(
        "--baseline-mode",
        choices=("compare", "update"),
        default=None,
        help="If set, exports GS_VISUAL_BASELINE_MODE for the harness. "
        "If omitted, the harness inherits whatever the calling environment provides.",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Extra argv to forward to the harness (e.g., --extra-arg=--success). Repeatable.",
    )
    return parser


def _resolve_godot(arg_value: Optional[str]) -> Optional[Path]:
    if arg_value:
        candidate = Path(arg_value)
        if not candidate.is_absolute():
            candidate = (ROOT / candidate).resolve()
        return candidate if candidate.is_file() else None
    # Under CI we never accept the fallback list: a stale binary left behind by a
    # previous run could silently pass with the new commits' tests not actually
    # exercised (matches feedback_verify_binary_timestamp.md — scons can exit 0
    # without re-linking, so file presence doesn't prove this commit was built).
    # CI must pass --godot explicitly so the workflow controls the binary path.
    if os.environ.get("GITHUB_ACTIONS") == "true" or os.environ.get("CI") == "true":
        return None
    candidates = (
        ROOT / "bin" / "godot.windows.editor.dev.x86_64.console.exe",
        ROOT / "bin" / "godot.windows.editor.dev.x86_64.exe",
        ROOT / "bin" / "godot.windows.editor.x86_64.console.exe",
        ROOT / "bin" / "godot.windows.editor.x86_64.exe",
        ROOT / "bin" / "godot.linuxbsd.editor.dev.x86_64",
        ROOT / "bin" / "godot.linuxbsd.editor.x86_64",
    )
    for c in candidates:
        if c.is_file():
            return c
    return None


def main() -> int:
    parser = _build_argparser()
    args = parser.parse_args()

    if args.list_batches:
        print("Available batches:")
        for spec in BATCHES:
            timeout_note = f" timeout={spec.timeout_seconds}s" if spec.timeout_seconds else ""
            print(f"  {spec.name:24s} {list(spec.filters)}{timeout_note}")
        return SUPERVISOR_EXIT_OK

    godot = _resolve_godot(args.godot)
    if godot is None:
        in_ci = os.environ.get("GITHUB_ACTIONS") == "true" or os.environ.get("CI") == "true"
        if in_ci and not args.godot:
            sys.stderr.write(
                "[run_gpu_harness] FATAL: --godot is required in CI. The supervisor "
                "refuses the bin/ fallback list under GITHUB_ACTIONS or CI=true to "
                "prevent a stale binary from a previous run silently passing the gate.\n"
            )
        else:
            sys.stderr.write(
                "[run_gpu_harness] FATAL: could not resolve Godot test binary. "
                "Pass --godot PATH explicitly.\n"
            )
        return SUPERVISOR_EXIT_GODOT_MISSING

    # Record the binary mtime in the report so post-mortem can detect a stale
    # binary even outside CI (a build that cache-hit and skipped linking would
    # leave the mtime unchanged across runs).
    try:
        godot_mtime_utc = datetime.datetime.fromtimestamp(
            godot.stat().st_mtime, tz=datetime.timezone.utc
        ).isoformat()
    except OSError:
        godot_mtime_utc = ""

    if args.baseline_mode:
        os.environ["GS_VISUAL_BASELINE_MODE"] = args.baseline_mode

    selected = _select_batches(set(args.batch) if args.batch else None)
    if not selected:
        sys.stderr.write(
            f"[run_gpu_harness] FATAL: no batches matched --batch filter {args.batch}\n"
        )
        return SUPERVISOR_EXIT_BAD_INVOCATION

    run_started = datetime.datetime.now(datetime.timezone.utc)
    print(f"[run_gpu_harness] godot={godot} timeout={args.timeout}s batches={len(selected)}", flush=True)
    if args.baseline_mode:
        print(f"[run_gpu_harness] GS_VISUAL_BASELINE_MODE={args.baseline_mode}", flush=True)

    results: list[BatchResult] = []
    # Track the worst batch rc by *absolute non-zero* magnitude so we capture
    # POSIX signal-kill negative codes (e.g. SIGSEGV = -11) as failures. A
    # naive `if r.rc > max_rc` starting from 0 silently swallowed those on
    # Linux/Mac, reporting `max_rc=0` for a segfault. We preserve the original
    # sign in the report for diagnosis but the gate decision flips on any
    # non-zero.
    max_rc = 0
    for spec in selected:
        # Per-batch timeout overrides --timeout when set on the BatchSpec.
        batch_timeout = spec.timeout_seconds if spec.timeout_seconds is not None else args.timeout
        r = _run_batch(godot, spec.name, spec.filters, spec.excludes, batch_timeout, args.extra_arg)
        results.append(r)
        if r.rc != 0 and abs(r.rc) > abs(max_rc):
            max_rc = r.rc

    run_ended = datetime.datetime.now(datetime.timezone.utc)

    totals = {
        "test_cases_total": sum(r.test_cases_total for r in results),
        "test_cases_passed": sum(r.test_cases_passed for r in results),
        "test_cases_failed": sum(r.test_cases_failed for r in results),
        "test_cases_skipped": sum(r.test_cases_skipped for r in results),
        "assertions_total": sum(r.assertions_total for r in results),
        "assertions_passed": sum(r.assertions_passed for r in results),
        "assertions_failed": sum(r.assertions_failed for r in results),
    }

    # Gate decision considers subprocess rc, parsed doctest totals, AND that
    # every required batch actually ran at least one test case. If
    # `_parse_summary` regex stops matching (doctest output change, locale
    # issue) the totals stay at zero — without this check, a harness binary
    # returning 0 but emitting `[doctest] Status: FAILURE!` would pass the gate.
    # If a required batch's filter stops matching (test rename/removal),
    # doctest exits 0 with `0 test cases matched` — without the required-batch
    # check below, the gate would silently pass while exercising nothing.
    parsed_failures = totals["test_cases_failed"] + totals["assertions_failed"]
    empty_required_batches: list[str] = []
    # Lifetime scenarios in REQUIRED batches that reported passed=false (a real
    # failure, a mark_skipped, or a scope-exited-without-finalize). doctest
    # counts a skip as PASSED, so this is the only signal that catches a
    # required lifetime scenario silently ceasing to exercise its path
    # (hollow coverage; Codex PR #419 Finding 2). Format: "Batch/scenario: reason".
    required_lifetime_failures: list[str] = []
    if selected_required := REQUIRED_BATCHES.intersection({spec.name for spec in selected}):
        for r in results:
            if r.name in selected_required and r.test_cases_total <= 0:
                empty_required_batches.append(r.name)
                print(
                    f"[run_gpu_harness] FATAL: required batch '{r.name}' matched 0 test cases "
                    f"(filters={list(r.filters)}). The visual gate must not pass while the "
                    "canonical regression test is silently skipped — fix the filter or remove "
                    "this batch from REQUIRED_BATCHES.",
                    flush=True,
                )
            if r.name in selected_required and r.lifetime_failed_scenarios:
                for entry in r.lifetime_failed_scenarios:
                    required_lifetime_failures.append(f"{r.name}/{entry}")
                    print(
                        f"[run_gpu_harness] FATAL: required batch '{r.name}' lifetime scenario "
                        f"reported passed=false ({entry}). A skipped or failed lifetime scenario "
                        "must not satisfy the gate without exercising its path — fix the scenario "
                        "or its harness wiring (Codex PR #419 Finding 2).",
                        flush=True,
                    )
    # Per-batch RID-leak totals scraped from the listener's stdout marker.
    # The listener's threshold was raised to 4 MiB in #335 (above the ~2.25 MiB
    # of allocator-pool overhead observed on the hazard test on NVIDIA/Vulkan/
    # release), so any non-zero value here is a real leak signal — fold into
    # gate_failed below. If a future driver/runner's allocator pool trips the
    # listener on the canonical hazard test, retune the threshold in
    # gs_gpu_test_runner.cpp rather than the supervisor side.
    # #329 measurement note. Two distinct things were conflated here before:
    #
    #   * MIS-ATTRIBUTION (fixed): the SceneDirector retained a
    #     Ref<GaussianSplatRenderer> per scenario that outlived each case's
    #     SceneTree, and the leak listener sampled before SceneTree teardown.
    #     Together those reported 2,720,433,428 bytes across the new [SceneTree]
    #     batches. Fixed in gs_gpu_test_runner.cpp (release_all_worlds() before
    #     sampling, listener demoted to priority 0 so it samples last).
    #
    #   * DEFERRED-FREE LAG (fixed in #662, was misdiagnosed as "allocator
    #     high-water growth"): the residual was NOT allocator pool warmup.
    #     MEMORY_BUFFERS/MEMORY_TEXTURES are exact live-object counters
    #     (rendering_device.h:1580-1581), but `rd->free()` only QUEUES the
    #     release; the counters drop in `_free_pending_resources()`, which runs
    #     solely from `_begin_frame()` (rendering_device.cpp:6469). This
    #     harness's device has no swapchain and no main loop, so nothing ever
    #     advanced a frame and correctly-freed resources still read as leaked.
    #     That produced the order-dependence (first allocating case pays for
    #     everything queued; later cases read an inflated baseline, go negative,
    #     and are suppressed) and the whack-a-mole on exclusion. The listener now
    #     drains via swap_buffers(false) before sampling both endpoints.
    #
    # Measured on base 8914756deca: the whole-harness total was 179,390,524 B.
    # After the deferred-free drain AND the real leak fix below it is 0.
    # Of the untouched TileRenderer batch's 35,389,440 B, exactly half
    # (17,694,720 B) was deferred-free lag and half was a GENUINE leak:
    # `TileRenderer::TileResolveStage::destroy_resolve_textures()` freed its
    # main-device external resolved textures through the LOCAL device, which
    # silently no-ops, orphaning 2 textures per resolving TileRenderer. The
    # engine's own `2 RIDs of type "Texture" were leaked` report at device
    # finalize confirmed it independently. Fixed in tile_render_resolve.cpp.
    #
    # The gate is verified to still DISCRIMINATE: a deliberately leaked
    # 2048x1024 RGBA8 texture on the main device is reported as exactly
    # 8,388,608 B and fails the gate, because a genuinely leaked RID is never
    # queued for free and therefore survives any number of drains.
    # Do not "fix" a future non-zero value by retuning the threshold.
    total_rid_leak_bytes = sum(r.rid_leak_bytes for r in results)

    # Batches that hit their wall-clock timeout (#677). A timed-out batch is a
    # NON-RESULT, not a pass: doctest never printed its summary line, so this
    # batch's `test_cases_*` parse as 0 and its `rid_leak_bytes` reflects only
    # whatever the listener managed to emit before the kill. Those partial zeros
    # are then summed into `totals` and `total_rid_leak_bytes` below exactly as
    # if they were complete measurements.
    #
    # The gate already fails in this situation, because `_run_batch` sets
    # rc=SUPERVISOR_EXIT_TIMEOUT and `max_rc != 0` flips `gate_failed` — that part
    # was never broken. What WAS missing is attribution: the run-level `DONE` line
    # and the JSON report's top-level fields said only `max_rc=124`, naming no
    # batch, while `total_rid_leak_bytes=0` sat next to it reading like a clean
    # result. That is how a timed-out batch's non-measurement gets quoted as a
    # measurement (#677, and the reading that produced #662's corrected claim).
    #
    # So: name the batch loudly here, carry it in the report, and mark the
    # aggregates non-authoritative so no downstream consumer can quote a total
    # that silently excludes a batch which never finished.
    timed_out_batches = [r.name for r in results if r.timed_out]
    for name in timed_out_batches:
        print(
            f"[run_gpu_harness] FATAL: batch '{name}' TIMED OUT - it produced no doctest "
            "summary, so its test-case counts parsed as 0 and its leak bytes are partial. "
            "This is an absence of measurement, NOT a clean batch: the run totals and "
            "total_rid_leak_bytes below EXCLUDE whatever it did not reach. Do not quote "
            "them as complete. Raise this batch's BatchSpec.timeout_seconds if it is merely "
            "slow, or diagnose the hang - do not leave it timing out.",
            flush=True,
        )

    # Batches where the doctest summary regex failed to match. Locale shifts,
    # ANSI-color piping, or a doctest version change can break parsing —
    # without this, parsed_failures stays 0 for batches where the harness
    # actually emitted `Status: FAILURE!` but we couldn't read it. Skip the
    # check for timed-out batches (they intentionally interrupt mid-output)
    # and batches that crashed before producing output (rc != 0 already
    # flips the gate, and a parse miss on a crash is uninformative).
    summary_parse_failures = [
        r.name for r in results
        if not r.timed_out and r.rc == 0 and not r.summary_parse_ok
    ]

    # `timed_out_batches` is listed explicitly even though `max_rc` already covers
    # it today. The rc path is incidental — it holds only because
    # SUPERVISOR_EXIT_TIMEOUT happens to be non-zero. Stating the condition
    # directly means a future refactor of the rc bookkeeping cannot silently make
    # an unrun batch gate-clean (#677).
    gate_failed = (
        (max_rc != 0)
        or (parsed_failures > 0)
        or bool(empty_required_batches)
        or bool(summary_parse_failures)
        or (total_rid_leak_bytes > 0)
        or bool(required_lifetime_failures)
        or bool(timed_out_batches)
    )
    # Preserve the worst batch's exit code as the supervisor exit when a batch
    # itself failed (the module header promises "returns max(batch_rc)"). The
    # absolute-magnitude comparison above means `max_rc` carries POSIX signal
    # codes (e.g. SIGSEGV = -11) AND the SUPERVISOR_EXIT_TIMEOUT=124 we inject
    # for hung batches, so callers/CI can distinguish timeouts from segfaults
    # from assertion failures by exit status alone. Only fall back to the
    # generic SUPERVISOR_EXIT_HARNESS_FAILED=1 when the gate failed for a
    # non-rc reason (parsed_failures or empty_required_batches) and no batch
    # itself returned non-zero.
    if not gate_failed:
        supervisor_exit = SUPERVISOR_EXIT_OK
    elif max_rc != 0:
        supervisor_exit = max_rc
    else:
        supervisor_exit = SUPERVISOR_EXIT_HARNESS_FAILED

    report = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "timestamp_started_utc": run_started.isoformat(),
        "timestamp_finished_utc": run_ended.isoformat(),
        "wall_seconds": round((run_ended - run_started).total_seconds(), 3),
        "godot_binary": str(godot),
        "godot_binary_mtime_utc": godot_mtime_utc,
        "baseline_mode": args.baseline_mode or os.environ.get("GS_VISUAL_BASELINE_MODE", "compare"),
        "max_batch_rc": max_rc,
        "parsed_failures": parsed_failures,
        "empty_required_batches": empty_required_batches,
        "summary_parse_failures": summary_parse_failures,
        "required_lifetime_failures": required_lifetime_failures,
        "timed_out_batches": timed_out_batches,
        # False when any batch timed out: `totals` and `total_rid_leak_bytes` then
        # sum only the batches that finished, so they UNDERSTATE both coverage and
        # leaks. Downstream consumers (the baseline_qa.yml step summary, any leak
        # comparison across two runs) must not present them as complete figures
        # while this is False (#677).
        "totals_authoritative": not timed_out_batches,
        "total_rid_leak_bytes": total_rid_leak_bytes,
        "supervisor_exit": supervisor_exit,
        "totals": totals,
        "batches": [r.to_dict() for r in results],
    }

    report_path = Path(args.report)
    if not report_path.is_absolute():
        report_path = (ROOT / report_path).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    # Atomic write: serialize to a UNIQUELY-NAMED sibling temp file then
    # os.replace into the final path. Prevents downstream readers (e.g. the
    # workflow's PowerShell ConvertFrom-Json summary step) from blowing up on
    # a half-written report if the supervisor is killed mid-write.
    #
    # The temp file name must be unique per invocation: a fixed `.tmp` suffix
    # would let two concurrent supervisors clobber each other's temp file
    # before either os.replace lands, defeating the atomicity guarantee even
    # if the final `os.replace` itself is atomic. tempfile.mkstemp gives us
    # an O_CREAT|O_EXCL guarantee from the OS so the name is collision-free
    # even under heavy contention (local parallel runs, overlapping CI
    # retries, etc.). Keep the temp file in the same directory as the final
    # report so os.replace stays an atomic same-filesystem rename.
    import tempfile

    payload = json.dumps(report, indent=2).encode("utf-8")
    fd, tmp_path_str = tempfile.mkstemp(
        prefix=report_path.stem + ".",
        suffix=".tmp",
        dir=str(report_path.parent),
    )
    tmp_path = Path(tmp_path_str)
    try:
        with os.fdopen(fd, "wb") as tmp_file:
            tmp_file.write(payload)
            tmp_file.flush()
            os.fsync(tmp_file.fileno())
        os.replace(tmp_path, report_path)
    except Exception:
        # Clean up the temp file on any failure before re-raising — without
        # this, a failed write would leak a `.tmp` file next to the final
        # report on every error.
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise
    print(f"[run_gpu_harness] wrote {report_path}", flush=True)

    extra_suffix_parts: list[str] = []
    if timed_out_batches:
        # Named first: it qualifies every other number on the DONE line (#677).
        extra_suffix_parts.append(
            f"TIMED_OUT={timed_out_batches} (totals below are INCOMPLETE)"
        )
    if empty_required_batches:
        extra_suffix_parts.append(f"empty_required={empty_required_batches}")
    if total_rid_leak_bytes > 0:
        extra_suffix_parts.append(f"rid_leak_bytes={total_rid_leak_bytes}")
    if summary_parse_failures:
        extra_suffix_parts.append(f"summary_parse_failures={summary_parse_failures}")
    if required_lifetime_failures:
        extra_suffix_parts.append(f"required_lifetime_failures={required_lifetime_failures}")
    extra_suffix = (" " + " ".join(extra_suffix_parts)) if extra_suffix_parts else ""
    print(
        f"[run_gpu_harness] DONE max_rc={max_rc} cases={totals['test_cases_passed']}/{totals['test_cases_total']} "
        f"asserts={totals['assertions_passed']}/{totals['assertions_total']} failures={parsed_failures}"
        f"{extra_suffix}",
        flush=True,
    )

    return supervisor_exit


if __name__ == "__main__":
    sys.exit(main())
