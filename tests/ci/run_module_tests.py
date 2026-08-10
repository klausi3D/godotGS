#!/usr/bin/env python3
"""Run Gaussian Splatting module tests via Godot's built-in test runner.

If the binary was built without tests enabled, behavior is controlled by
strict/warn-only policy (strict fails, warn-only skips).
"""

from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass
from datetime import datetime, timezone
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Iterable


ROOT = Path(__file__).resolve().parents[2]
MODULE_SOURCE_DIR = ROOT / "modules" / "gaussian_splatting"
RENDERER_DIR = MODULE_SOURCE_DIR / "renderer"
BUILD_METADATA_GUARD_SCRIPT = MODULE_SOURCE_DIR / "tests" / "check_build_metadata_consistency.py"
SHADER_DEPENDENCY_GUARD_SCRIPT = MODULE_SOURCE_DIR / "tests" / "check_shader_dependency_contract.py"
PROJECT_SETTINGS_MANIFEST_GUARD_SCRIPT = MODULE_SOURCE_DIR / "tests" / "check_project_settings_manifest.py"
GAUSSIAN_LAYOUT_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_gaussian_layout_sync.py"
CULL_SIGNATURE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_cull_signature_parity.py"
CULL_SIGNATURE_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_cull_signature_parity.py"
METRIC_RESET_PARITY_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_metric_reset_parity.py"
METRIC_RESET_PARITY_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_metric_reset_parity.py"
REJECT_TELEMETRY_PARITY_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_reject_telemetry_parity.py"
REJECT_TELEMETRY_PARITY_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_reject_telemetry_parity.py"
DOC_CLASSES_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_doc_classes_complete.py"
TEST_LINKAGE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_test_linkage.py"
REQUIRE_NULL_DEREF_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_require_null_deref.py"
REQUIRE_NULL_DEREF_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_require_null_deref.py"
ENVIRONMENT_SKIP_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_environment_skip_marker.py"
ENVIRONMENT_SKIP_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_environment_skip_marker.py"
SKIP_MARKER_DETECTOR_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_run_module_tests_skip_marker.py"
LANE_LEDGER_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_run_module_tests_lane_ledger.py"
ENVIRONMENT_SKIP_BASELINE_PATH = ROOT / "tests" / "ci" / "environment_skip_baseline.json"

UNCHECKED_RESIZE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_unchecked_resize.py"
TEST_LANE_COVERAGE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_test_lane_coverage.py"
TEST_LANE_COVERAGE_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_test_lane_coverage.py"
GPU_SORTING_ORDER_COVERAGE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_gpu_sorting_order_coverage.py"
RENDERER_RELEASE_GATE_SCRIPT = ROOT / "tests" / "ci" / "check_renderer_release_gates.py"
RENDERER_CONTRACT_BOUNDARY_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_renderer_contract_boundary.py"
DEVICE_SUBMISSION_CONTRACT_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_device_submission_contract.py"
EDITOR_NODE_POINTER_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_editor_node_pointer_lifetime.py"
RENDERER_RELEASE_GATE_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_renderer_release_gates.py"
BASELINE_QA_REQUIRE_FLAG_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_baseline_qa_require_flag.py"
HISTORY_ARTIFACT_AUDIT_SCRIPT = ROOT / "scripts" / "repo" / "history_artifact_audit.py"
SYNTHETIC_ASSET_PREP_SCRIPT = ROOT / "tests" / "runtime" / "prepare_synthetic_assets.py"
BENCHMARK_ASSET_GUARD_SCRIPT = ROOT / "tests" / "runtime" / "check_benchmark_asset_paths.py"
SOURCE_TREES = (ROOT,)
HEADLESS_GAUSSIAN_SCOPED_TAGS: tuple[str, ...] = (
    # Only tags whose TEST_CASEs are registered at runtime belong here. Phantom
    # tags (zero runtime tests) must NOT appear here because strict lanes fail on
    # zero coverage.
    "Animation",
    "AtomicWrite",  # G2: promoted from the advisory [untagged] lane to a strict blocking lane.
    "ComputeInfra",
    "Config",
    "Container",
    "DataAuthority",  # #846: promoted from the advisory [untagged] lane to a strict blocking lane.
    "Editor",
    "Importer",
    "MalformedCorpus",  # G2: the aggregate malformed-input gate (WorldIO/PLY/SPZ/Persistence).
    "Node",
    "PLY",
    "Persistence",
    "SPZ",  # G2: promoted from the advisory [untagged] lane to a strict blocking lane.
    "SceneTree",
    "SortBenchmark",
    "SortFallback",  # #586: promoted from the advisory [untagged] lane to a strict blocking lane.
    "Synthetic",
    "VRAMBudgetRegulator",
    "ViewTransform",
    "WorldIO",
)
UNTAGGED_GAUSSIAN_EXCLUDE_TAGS: tuple[str, ...] = HEADLESS_GAUSSIAN_SCOPED_TAGS + (
    "GeneratePLY",  # fixture generator with disk side effects; invoked explicitly by prepare_synthetic_assets.py
    "Renderer",  # only aspirational stubs currently; advisory lane below
    "RequiresGPU",
    "Thumbnail",
    "World",
)
UNTAGGED_GAUSSIAN_EXCLUDE_FILTERS: tuple[str, ...] = tuple(
    f"*GaussianSplatting*][{tag}]*" for tag in UNTAGGED_GAUSSIAN_EXCLUDE_TAGS
)
MODULE_TEST_FILTERS: tuple[tuple[str, tuple[str, ...], tuple[str, ...], bool], ...] = (
    # (name, test_case_filters, test_case_exclude_filters, strict)
    # Split the canonical headless Gaussian lane into deterministic subsets so a
    # single-process crash in one area does not erase summary output for the rest
    # of the suite.
    ("GaussianSplatting [Animation]", ("*GaussianSplatting*][Animation]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [ComputeInfra]", ("*GaussianSplatting*][ComputeInfra]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Config]", ("*GaussianSplatting*][Config]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Container]", ("*GaussianSplatting*][Container]*",), ("*][RequiresGPU]*",), True),
    ("Gaussian Diagnostics", ("*Gaussian Diagnostics*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Editor]", ("*GaussianSplatting*][Editor]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Importer]", ("*GaussianSplatting*][Importer]*",), ("*][RequiresGPU]*",), True),
    ("Gaussian Logger", ("*Gaussian Logger*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Node]", ("*GaussianSplatting*][Node]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [PLY]", ("*GaussianSplatting*][PLY]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Persistence]", ("*GaussianSplatting*][Persistence]*",), ("*][RequiresGPU]*",), True),
    (
        "GaussianSplatting [SceneTree]",
        ("*GaussianSplatting*][SceneTree]*",),
        (
            "*][RequiresGPU]*",
            "*][Node][SceneTree]*",
            "*][Container][SceneTree]*",
            "*][World][SceneTree]*",
        ),
        True,
    ),
    ("GaussianSplatting [SortBenchmark]", ("*GaussianSplatting*][SortBenchmark]*",), ("*][RequiresGPU]*",), True),
    # #586: the sort-fallback policy decides whether a frame is presented with incorrect alpha
    # compositing or rejected. Those cases previously reached only the advisory [untagged] lane,
    # so a regression in the reject decision could not fail CI. Strict blocking lane.
    #
    # HELD IN PLACE BY A GUARD, not by this comment (same as [DataAuthority] below):
    # the promotion is two coupled edits -- this tuple plus the
    # HEADLESS_GAUSSIAN_SCOPED_TAGS entry above -- and undoing BOTH, retagging only
    # some of the cases, or flipping this `True` to `False` would drop them back into
    # the advisory net without stranding anything, so no other check would notice.
    # The `[SortFallback]` `STRICT_COVERAGE_CONTRACTS` entry in
    # tests/ci/check_test_lane_coverage.py asserts the property instead: every case in
    # this corpus must reach some strict lane. Measured before that contract existed:
    # dropping both halves left the guard at exit 0.
    ("GaussianSplatting [SortFallback]", ("*GaussianSplatting*][SortFallback]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Synthetic]", ("*GaussianSplatting*][Synthetic]*",), ("*][RequiresGPU]*",), False),
    ("GaussianSplatting [VRAMBudgetRegulator]", ("*GaussianSplatting*][VRAMBudgetRegulator]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [ViewTransform]", ("*GaussianSplatting*][ViewTransform]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [WorldIO]", ("*GaussianSplatting*][WorldIO]*",), ("*][RequiresGPU]*",), True),
    # G2 (exit criterion; ledger #458): the malformed-input corpus gate plus the
    # SPZ and atomic-write lanes, promoted from the advisory [untagged] safety net
    # to strict blocking lanes so a hostile-input or crash-atomicity regression
    # hard-fails CI. [MalformedCorpus] aggregates the per-format malformed cases.
    ("GaussianSplatting [MalformedCorpus]", ("*GaussianSplatting*][MalformedCorpus]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [SPZ]", ("*GaussianSplatting*][SPZ]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [AtomicWrite]", ("*GaussianSplatting*][AtomicWrite]*",), ("*][RequiresGPU]*",), True),
    # #846: same promotion, same reason. The 11 [DataAuthority] cases ran only in
    # the advisory [untagged] lane, where _report_failed_lane() returns True
    # ("advisory lane, continuing"), so a failure could not fail the runner. Five
    # of them are the ONLY executable proof of the defects fixed in #805 (the
    # coherent reset that bumps payload_version and emits `changed`; the failed
    # lane SHRINK that left the lane oversized -- an actual OOB read captured
    # under cdb as c0000005; the getter-allocation refusal that stops a defaulted
    # payload being cached; the transactional materialization; and the merge that
    # refuses rather than substituting defaults). Fail-closed persistence proofs
    # that cannot fail CI are not proofs, so this lane blocks.
    #
    # Promotion gated on measured stability, not on one green run (the reason
    # #805 did not do it): 25 full-lane runs (11/11 cases, 190/190 assertions,
    # zero variance) plus 100 dedicated runs of the one threaded case, "Animated
    # accessors tolerate concurrent payload mutation" -- 60 on a quiet box and 40
    # under 4-way self-contention to shift the interleaving -- all 98/98 with no
    # crash and no assertion-count drift. 125 runs, zero failures.
    #
    # HELD IN PLACE BY A GUARD, not by this comment: the promotion is two coupled
    # edits (this tuple plus the HEADLESS_GAUSSIAN_SCOPED_TAGS entry above), and
    # undoing BOTH -- or retagging only some of the cases -- would drop them back
    # into the advisory net without stranding anything. `STRICT_COVERAGE_CONTRACTS`
    # in tests/ci/check_test_lane_coverage.py asserts the property instead: every
    # case in this corpus must reach some strict lane. Deleting this line without
    # retiring that contract fails the guard batch.
    ("GaussianSplatting [DataAuthority]", ("*GaussianSplatting*][DataAuthority]*",), ("*][RequiresGPU]*",), True),
    # Safety-net lane for unscoped [GaussianSplatting] tests.  Advisory because
    # doctest's --test-case-exclude parsing is unreliable beyond ~10 repeated
    # flags, so the exclude list cannot guarantee precise filtering.  Real
    # coverage lives in the per-tag strict lanes above.
    ("GaussianSplatting [untagged]", ("*GaussianSplatting*",), UNTAGGED_GAUSSIAN_EXCLUDE_FILTERS, False),
    # Use stable description fragments instead of tag prefixes for secondary
    # lanes, as doctest matching can differ depending on how bracketed prefixes
    # are parsed in test names.
    ("GaussianSplatting [Lifetime]", ("*][Lifetime]*",), ("*][RequiresGPU]*",), True),
    ("GaussianSplatting [Renderer]", ("*GaussianSplatting*][Renderer]*",), ("*][RequiresGPU]*",), False),
    # #641: "Shader compilation on local device" is now
    # `[TileRenderer][RequiresGPU]` and runs in the GPU harness's `TileRenderer`
    # batch (tests/ci/run_gpu_harness.py), which is the only lane that can give
    # it a RenderingDevice. The exclude keeps this headless lane from claiming to
    # cover a GPU case.
    #
    # #637 unblocked the repoint this comment used to defer. The four
    # test_tile_async_readback_freshness.cpp cases that SIGSEGV'd under `--test`
    # did so because constructing a TileRenderer needs the global
    # RenderingDevice singleton (upstream ShaderRD's constructor dereferences it,
    # shader_rd.cpp:791); they are now `[RequiresGPU]` and run in the GPU harness,
    # where they pass. They are excluded here by the same `[RequiresGPU]` rule
    # that excludes every other GPU case — NOT by a special-case workaround.
    # The lane now covers the whole host-only `[TileRenderer]` family (prefix-scan
    # ABI/dispatch/CPU-fallback, shared-memory contract, range regression):
    # measured 11 cases / 2,097,742 assertions, all passing.
    ("TileRenderer", ("*[TileRenderer]*",), ("*][RequiresGPU]*",), False),
    ("GPU Memory Stream", ("*Triple Buffering*",), (), False),
    ("Streaming Pipeline", ("*[Streaming Pipeline]*",), (), False),
)
# Renderer-dependent (requires-RD) doctest lane.  Under Godot's --test mode
# every test here will skip because no RenderingDevice is available.  This lane
# exists for future use when a full-engine test harness is added; in the
# meantime it serves as a catalogue of renderer-dependent tests.
REQUIRES_RD_TEST_FILTERS: tuple[tuple[str, tuple[str, ...], tuple[str, ...], bool], ...] = (
    ("GaussianSplatting [requires-RD]", ("*GaussianSplatting*][RequiresGPU]*",), (), False),
)
# Test quarantine manifest (production-readiness C3 / exit criterion G5; ledger
# #458). The manifest is the single tracked home for known-failing headless
# lanes: each entry names a real MODULE_TEST_FILTERS lane whose failure has been
# proven on a base SHA and linked to an issue, so a known failure lives in the
# repo (not in memory) without weakening any gate. It ships EMPTY (entries: [])
# in Slice 1, which is behaviorally inert. See the ADR
# (docs/architecture/adr-test-quarantine-manifest.md) and the non-authoritative
# mirror (docs/reference/test-quarantine.md). The GPU [SceneTree]/[Importer]
# deferrals live in a separate manifest
# (renderer_release_gate_manifest.json:deferred_requires_gpu_waivers, #329).
QUARANTINE_MANIFEST_PATH = ROOT / "tests" / "ci" / "quarantine_manifest.json"
# The mechanism's own unit test, executed by the schema guard so the tolerate /
# stale / coverage-lost / harness-error lane logic runs in the fast
# --guard-only lane (mirrors how the renderer release gate guard runs its test
# script). QUARANTINE_UNITTEST_ACTIVE_ENV is a recursion guard: it is set in the
# child process so a test that calls the guard cannot re-spawn the suite.
QUARANTINE_MANIFEST_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_quarantine_manifest.py"
QUARANTINE_UNITTEST_ACTIVE_ENV = "GS_QUARANTINE_MANIFEST_UNITTEST_ACTIVE"
# Fields every populated entry MUST carry (Slice 2, human-gated). 'lane' must
# equal a real MODULE_TEST_FILTERS name. 'test_case' is REQUIRED (round-3 review):
# a lane bundles many doctest cases, so a lane-only quarantine would tolerate any
# NEW unrelated failure in that lane until the entry expires. 'test_case' is a
# doctest-style wildcard ('*'/'?' only; use '*...*' for a substring) matched
# against the failing doctest case names so only the named failure is tolerated. On a
# whole-lane CRASH (no per-case info) the match cannot be applied and the lane is
# tolerated as a whole - a documented limitation (see docs/reference/
# test-quarantine.md); target the narrowest possible lane in that case.
# 'mitigation' remains optional/descriptive and is not enforced here.
QUARANTINE_REQUIRED_FIELDS: tuple[str, ...] = (
    "lane",
    "test_case",
    "reason",
    "issue_url",
    "base_sha_proven_failing",
    "owner",
    "risk",
    "expires_utc",
)

GS_RUN_GPU_TESTS_ENV = "GS_RUN_GPU_TESTS"
DISALLOWED_TRACKED_ARTIFACT_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("Python cache directory", re.compile(r"(^|/)__pycache__/")),
    ("Python bytecode file", re.compile(r"\.pyc$")),
    ("Root screenshot dump", re.compile(r"^Screenshot [^/]+\.png$")),
    ("Runtime Linux log output", re.compile(r"^tests/runtime/linux_logs/")),
    ("Runtime log output", re.compile(r"^tests/runtime/.*\.log$")),
)
TRACKED_SYNTHETIC_PLY_PATTERN = re.compile(r"^(tests|templates)/.*\.ply$")
REQUIRED_IGNORED_PATH_PROBES: tuple[str, ...] = (
    "tests/runtime/linux_logs/.hygiene_guard_probe.log",
    "tests/runtime/windows_logs/.hygiene_guard_probe.log",
    "baseline_qa_results.json",
    "tests/ci/qa_results.json",
)
ALLOW_SETTING_TOKEN = "GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION"
ALLOW_FS_WRITE_TOKEN = "GS_CI_ALLOW_RENDER_PATH_FS_WRITE"
VALIDATION_MODE_ENV = "GS_CI_VALIDATION_MODE"
# Controls behavior when the Godot binary was built without tests enabled.
# - strict: unavailable tests are fatal.
# - warn-only: unavailable tests are logged and skipped.
# Defaults to strict in CI and warn-only locally.
TEST_AVAILABILITY_MODE_ENV = "GS_CI_MODULE_TEST_AVAILABILITY_MODE"
# Explicit override for local/debug flows that need to bypass unavailable tests.
ALLOW_TESTS_UNAVAILABLE_ENV = "GS_CI_ALLOW_TESTS_UNAVAILABLE"
HISTORY_ARTIFACT_GUARD_MODE_ENV = "GS_CI_HISTORY_ARTIFACT_GUARD_MODE"
HISTORY_ARTIFACT_GUARD_MODES = ("off", "warn", "strict")
HISTORY_ARTIFACT_MATCH_COUNT_RE = re.compile(r"Matched blob entries:\s*(\d+)")
# Environment-skip detection (#595).
#
# The previous pattern was `(?m)^\s*(?:Skipping(?: test)?\s*-\s+.+)$` and it
# matched NOTHING that doctest has ever printed. doctest's ConsoleReporter
# always emits a message through file_line_to_stream() first
# (thirdparty/doctest/doctest.h:6051-6056, 6423-6437), so the real line is
#
#     C:\...\test_painterly_pipeline.h(473): MESSAGE: Skipping test - ...
#
# The marker can therefore never START a line, and a line-anchored regex is
# structurally incapable of seeing it. Measured on a real headless run at
# baseline e9ddb27c285: 3 `MESSAGE: Skipping` lines present, 0 regex matches,
# `test cases: 9 | 9 passed | 0 failed`. The policy below has consequently never
# fired once, and every environment skip in every strict lane has been scored as
# a pass.
#
# The repaired detector is NOT line-anchored and counts two things:
#
#   * the canonical token `GS_ENV_SKIP:` emitted by
#     modules/gaussian_splatting/tests/test_macros.h:GS_ENV_SKIP(). Matched on
#     its own, with no prefix requirement, so it survives a reporter change or a
#     non-console reporter.
#   * the LEGACY free-form `MESSAGE: Skip…` prose, in its PREFIX FORM only -
#     the message must BEGIN with `Skipping`/`Skipped`. #595 deliberately does
#     not rewrite those 354 sites (that is slice GS-595-B), and dropping them
#     from the count would shrink the reported number while growing the hidden
#     surface. They are counted until they are converted.
#
# KNOWN GAP, stated so a zero here is never misread as proof of execution: the
# EMBEDDED FORM is NOT counted - a message that mentions skipping mid-sentence
# rather than at the start, e.g.
#     MESSAGE("[TileRenderer] RenderingServer not available, skipping regression test");
#     MESSAGE("Renderer unavailable (headless mode) - skipping renderer state checks");
# Measured on this tree: 9 such sites in 4 files, and TWO of those files
# (test_shadow_instance_subset.h, test_node_bootstrap.h) contain ONLY embedded
# skips, so they are invisible to both this detector and the static inventory.
# Both hold [SceneTree] cases, i.e. the strict `GaussianSplatting [SceneTree]`
# lane - which can therefore report 0 markers while skipping at runtime. Closing
# the gap is follow-on GS-595-E and must be its own measured step: widening the
# detector here would move the baseline and the enforcement blast radius in the
# same change. The shape contract is written out in full in
# tests/ci/check_environment_skip_marker.py; the two must change together.
#
# Robustness the shape demands: the `<file>(<line>): ` prefix and the
# `--gnu-file-line` `<file>:<line>: ` variant (both simply precede the match),
# absolute Windows paths with drive letters and backslashes (never touched,
# since nothing is anchored), and the ANSI colour escape doctest writes between
# `MESSAGE: ` and the message body (`s << Color::None << mb.m_string`).
#
# BOTH branches require the doctest `MESSAGE:` framing. An earlier version
# matched the bare token `GS_ENV_SKIP:` anywhere in the stream, which meant any
# log line that happened to contain the token counted as a skip -- and with a
# lane allowance of 0, ONE false positive fails a lane. The framing costs
# nothing real (every lane runs the console reporter) and removes that class.
#
# The prose branch is case-INSENSITIVE, matching the static guard's
# `_SKIP_PROSE_PREFIX_RE`. Measured on this corpus the two agree exactly (354
# sites either way), so this closes a definitional mismatch rather than changing
# a number.
#
# KNOWN ASYMMETRY, measured rather than assumed: the static guard counts 2
# `WARN_PRINT` skip sites that this detector does NOT count, because WARN_PRINT
# does not go through doctest -- it reaches the stream as Godot's own
# `WARNING: <text>` framing. A runtime branch for that shape was evaluated and
# REJECTED: 38 WARN_PRINT/ERR_PRINT literals engine-wide mention skipping,
# including in this module's own hot path
# (gaussian_streaming.cpp "[Streaming] Skipping Morton sort..."), and the
# captured fixture itself contains a production line reading "...will be
# collected but skipped because no renderer can be attached." Counting those
# would manufacture skip markers out of ordinary logging and fail lanes for no
# reason. Static-only is the deliberate, documented resolution; the full shape
# contract lives in tests/ci/check_environment_skip_marker.py.
#
# tests/ci/test_run_module_tests_skip_marker.py pins both halves against a
# captured real sample, and re-asserts that the OLD pattern finds zero matches
# in it so this regression cannot silently return.
DOCTEST_ENV_SKIP_TOKEN = "GS_ENV_SKIP:"
_ANSI_ESCAPE = r"(?:\x1b\[[0-9;]*[A-Za-z])*"
DOCTEST_SKIP_MARKER_RE = re.compile(
    rf"MESSAGE{_ANSI_ESCAPE}:[ \t]*{_ANSI_ESCAPE}[ \t]*"
    rf"(?:{re.escape(DOCTEST_ENV_SKIP_TOKEN)}|(?i:Skipp?(?:ing|ed))\b)"
)

# StringName orphan guard: PR 6 of work package #352.
#
# Headless --verbose runs of the engine trigger StringName::cleanup() in
# core/string/string_name.cpp, which prints `Orphan StringName: ...`
# lines for any StringName whose refcount != static_count at exit. The
# Gaussian Splatting module previously surfaced ~19 orphan entries from
# its own cached StringName paths and Dictionary-key sites because the
# module never released those caches at unregister. PR 6 closes the
# module-owned cases; this guard locks the post-fix count in place so a
# future regression cannot silently re-leak module-owned StringNames.
#
# The guard runs the binary on the canonical synthetic-test project
# (which loads the module, registers project settings, and exits via
# --quit). The `--test --test-case=*Synthetic*` doctest mode does NOT
# exercise the orphan report because doctest's exit path is not the same
# as the engine's cleanup path, so the project-with-quit run is what
# actually exercises StringName::cleanup.
#
# Threshold is configurable so the guard can absorb a small bounded set
# of orphans that may come from engine-side Variant infrastructure
# rather than from this module. PR 7 will tighten this to a
# delta-vs-baseline rule of zero. Set GS_STRINGNAME_ORPHAN_THRESHOLD to
# override the default. Set GS_STRINGNAME_ORPHAN_PROJECT to override the
# probe project (default tests/examples/godot/test_project).
STRINGNAME_ORPHAN_GUARD_DEFAULT_PROJECT = (
    ROOT / "tests" / "examples" / "godot" / "test_project"
)
STRINGNAME_ORPHAN_GUARD_PROJECT_ENV = "GS_STRINGNAME_ORPHAN_PROJECT"
# Empirical post-fix module-owned orphan count is 0; a small headroom
# absorbs unrelated engine-side noise.
STRINGNAME_ORPHAN_GUARD_DEFAULT_THRESHOLD = 5
STRINGNAME_ORPHAN_GUARD_THRESHOLD_ENV = "GS_STRINGNAME_ORPHAN_THRESHOLD"
STRINGNAME_ORPHAN_GUARD_BINARY_ENV = "GODOT_BINARY"
STRINGNAME_ORPHAN_GUARD_TIMEOUT_SEC = 120
STRINGNAME_ORPHAN_LINE_RE = re.compile(
    r"^Orphan StringName:\s*(?P<name>[^\(]+?)\s*\(.*\)\s*$", re.MULTILINE
)

SETTING_MUTATION_RE = re.compile(r"->set_setting\s*\(")
FS_WRITE_RULES = (
    ("ProjectSettings save", re.compile(r"\b(?:ps|project_settings|settings)->save\s*\(")),
    ("FileAccess write open", re.compile(r"\bFileAccess::open\s*\(.*FileAccess::(?:WRITE|APPEND|READ_WRITE)\b")),
    ("ResourceSaver save", re.compile(r"\bResourceSaver::save\s*\(")),
    ("DirAccess mutation", re.compile(r"\bDirAccess::(?:make_dir(?:_recursive)?|rename|remove|copy|copy_absolute)\s*\(")),
    ("FileAccess store_* write", re.compile(r"->store_(?:8|16|32|64|float|double|string|line|csv_line|buffer)\s*\(")),
)

STATIC_FORMAT_GUARDS: tuple[tuple[str, Path, tuple[str, ...]], ...] = (
    (
        "tile_compute_rgba8_gate",
        MODULE_SOURCE_DIR / "renderer" / "render_pipeline_stages.cpp",
        (
            r"static RD::DataFormat _resolve_compute_friendly_raster_format\(RD::DataFormat p_format\)",
            r"case RD::DATA_FORMAT_R8G8B8A8_SRGB:\s*.*?return RD::DATA_FORMAT_R8G8B8A8_UNORM;",
            r"const RD::DataFormat raster_output_format = _resolve_compute_friendly_raster_format\(target_format\);",
            r"Tile fallback format override: requested=%d resolved=%d",
        ),
    ),
    (
        "render_output_default_format",
        MODULE_SOURCE_DIR / "renderer" / "render_output_orchestrator.cpp",
        (
            r"if \(target_format == RD::DATA_FORMAT_MAX\)\s*\{\s*target_format = RD::DATA_FORMAT_R8G8B8A8_UNORM;",
        ),
    ),
    (
        "output_copy_format_mismatch_gate",
        MODULE_SOURCE_DIR / "interfaces" / "output_compositor.cpp",
        (
            r"bool format_mismatch = false;",
            r"destination_format\.format != source_format\.format",
            r"bool can_direct_copy = .*?!format_mismatch.*?;",
        ),
    ),
    # The _check_dual_state_sync canonical-vs-derived guardrail was a documented
    # no-op (it null-checked the orchestrators then validated nothing) and was
    # deleted outright rather than silently retained. This guard locks in the
    # honest decision: the deletion note must stay present, so the misleading
    # no-op cannot be reintroduced without a reviewer re-reading why it is gone.
    # If a real check is implemented, replace the note with the real method and
    # update this guard to assert the real validation instead.
    (
        "dual_state_sync_guardrail_removed_not_silently_kept",
        MODULE_SOURCE_DIR / "renderer" / "gaussian_splat_renderer.cpp",
        (
            r"_check_dual_state_sync was removed:.*?validated nothing",
        ),
    ),
    # G2 "all savers atomic" (exit criterion; ledger #458): every final-output
    # writer must route its destination write through gs_atomic_file_write (temp
    # -> fsync -> atomic rename with backup swap), so a crash mid-save cannot
    # truncate the prior good file. The [AtomicWrite] doctest lane proves the
    # helper is crash-atomic; these guards lock in that each writer actually USES
    # it (the PLY cache writer delegates to the world saver, so it is covered
    # transitively). If a writer is intentionally re-plumbed, update the guard.
    (
        "atomic_saver_world_io",
        MODULE_SOURCE_DIR / "io" / "gaussian_splat_world_io.cpp",
        (r"gs_atomic_file_write\s*\(",),
    ),
    (
        "atomic_saver_scene_serializer",
        MODULE_SOURCE_DIR / "persistence" / "gaussian_scene_serializer.cpp",
        (r"gs_atomic_file_write\s*\(",),
    ),
    (
        "atomic_saver_incremental",
        MODULE_SOURCE_DIR / "persistence" / "incremental_saver.cpp",
        (r"gs_atomic_file_write\s*\(",),
    ),
    # #714: the .gsplatworld importer's final-output copy (_copy_binary_file) also
    # writes the artifact the importer produces. It historically used a plain
    # truncating FileAccess::WRITE and bypassed this guard set, so an interrupted
    # copy could destroy an existing generated output while the three guards above
    # stayed green. It now routes through the atomic helper; lock that in.
    (
        "atomic_saver_gsplatworld_importer",
        MODULE_SOURCE_DIR / "io" / "resource_importer_gsplatworld.cpp",
        (r"gs_atomic_file_write\s*\(",),
    ),
)

_approved_tracked_synthetic_ply_fixtures_cache: set[str] | None = None


def _run_command(
    args: list[str], cwd: Path = ROOT, env: dict[str, str] | None = None
) -> tuple[int, str, str]:
    result = subprocess.run(
        args,
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    return result.returncode, result.stdout or "", result.stderr or ""


def _build_doctest_run_args(test_case_filters: tuple[str, ...], test_case_exclude_filters: tuple[str, ...]) -> list[str]:
    # Each option is passed ONCE with its patterns comma-joined, because doctest
    # OVERWRITES a filter option on every repeat instead of accumulating it
    # (thirdparty/doctest/doctest.h parses each occurrence into the same
    # `filters` slot). Passing four separate --test-case-exclude flags therefore
    # applied only the LAST one.
    #
    # Measured on 7c350507f51: the strict "GaussianSplatting [SceneTree]" lane
    # declares four excludes, so only "*][World][SceneTree]*" was in effect and
    # the "*][RequiresGPU]*" exclude was silently dropped -- the lane ran 67
    # cases instead of 26, ~31 of them [RequiresGPU] cases that reached a
    # "renderer unavailable" guard and early-returned. They could not fail, so
    # the leak was invisible: a headless lane quietly executing (and vacuously
    # passing) the GPU corpus it declares it does not cover.
    #
    # Comma-joining also retires the "unreliable beyond ~10 repeated flags"
    # caveat on the [untagged] lane's 23 excludes -- same root cause.
    # No filter pattern may contain a comma; doctest uses it as the separator.
    run_args = ["--headless", "--test"]
    for option, patterns in (
        ("--test-case", test_case_filters),
        ("--test-case-exclude", test_case_exclude_filters),
    ):
        if not patterns:
            continue
        for pattern in patterns:
            assert "," not in pattern, (
                f"{option} pattern must not contain a comma (doctest's separator): {pattern!r}"
            )
        run_args.append(f"{option}={','.join(patterns)}")
    return run_args


def _normalize_process_arg(value: str) -> str:
    # GitHub Actions outputs on Windows can include leading BOM or trailing CR/LF.
    # Also strip embedded NUL bytes defensively before subprocess invocation.
    return str(value).lstrip("\ufeff").replace("\x00", "").strip()


def _resolve_guard_base_ref(explicit_ref: str | None) -> str | None:
    def _resolve_ref(ref: str) -> str | None:
        code, out, _ = _run_command(["git", "rev-parse", "--verify", ref])
        if code == 0:
            return out.strip()
        return None

    def _merge_base(ref: str) -> str | None:
        code, out, _ = _run_command(["git", "merge-base", "HEAD", ref])
        if code == 0:
            return out.strip()
        return None

    if explicit_ref:
        if explicit_ref.lower() == "head":
            return _resolve_ref("HEAD")
        return _resolve_ref(explicit_ref) or _merge_base(explicit_ref)

    candidates = ("HEAD~1", "origin/main", "origin/master", "main", "master")
    for candidate in candidates:
        resolved = _merge_base(candidate)
        if resolved:
            return resolved
        resolved = _resolve_ref(candidate)
        if resolved:
            return resolved
    return None


def _parse_added_lines(diff_text: str) -> list[tuple[int, str]]:
    added: list[tuple[int, str]] = []
    current_new_line: int | None = None
    hunk_re = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")

    for raw_line in diff_text.splitlines():
        hunk_match = hunk_re.match(raw_line)
        if hunk_match:
            current_new_line = int(hunk_match.group(1))
            continue
        if raw_line.startswith("+++ ") or raw_line.startswith("--- "):
            continue
        if current_new_line is None:
            continue

        if raw_line.startswith("+"):
            added.append((current_new_line, raw_line[1:]))
            current_new_line += 1
        elif raw_line.startswith("-"):
            continue
        else:
            current_new_line += 1

    return added


def _check_render_path_guards(base_ref: str | None) -> tuple[bool, list[str]]:
    if not RENDERER_DIR.exists():
        return True, []
    if base_ref is None:
        if os.environ.get("CI"):
            return False, ["Unable to determine git base ref for renderer guard in CI."]
        return True, ["Skipping renderer guard (no git base ref found outside CI)."]

    diff_range = f"{base_ref}...HEAD"
    changed_code, changed_out, changed_err = _run_command([
        "git", "diff", "--name-only", "--diff-filter=AMRTUXB", diff_range, "--", str(RENDERER_DIR.relative_to(ROOT))
    ])
    if changed_code != 0:
        return False, [f"Failed to enumerate renderer diffs: {changed_err.strip()}"]

    changed_files = [line.strip() for line in changed_out.splitlines() if line.strip()]
    if not changed_files:
        return True, []

    violations: list[str] = []
    for rel_path in changed_files:
        code, diff_out, diff_err = _run_command([
            "git", "diff", "--no-color", "--unified=0", diff_range, "--", rel_path
        ])
        if code != 0:
            violations.append(f"{rel_path}: failed to inspect diff ({diff_err.strip()})")
            continue

        for line_no, line_text in _parse_added_lines(diff_out):
            stripped = line_text.strip()
            if not stripped:
                continue

            if SETTING_MUTATION_RE.search(line_text) and ALLOW_SETTING_TOKEN not in line_text:
                violations.append(
                    f"{rel_path}:{line_no}: render-path set_setting mutation requires guard token "
                    f"'{ALLOW_SETTING_TOKEN}' on the same line."
                )

            for label, rule in FS_WRITE_RULES:
                if rule.search(line_text) and ALLOW_FS_WRITE_TOKEN not in line_text:
                    violations.append(
                        f"{rel_path}:{line_no}: {label} requires guard token "
                        f"'{ALLOW_FS_WRITE_TOKEN}' on the same line."
                    )

    return len(violations) == 0, violations


def _run_static_format_guards() -> tuple[bool, list[str]]:
    failures: list[str] = []
    for guard_name, file_path, required_patterns in STATIC_FORMAT_GUARDS:
        rel_path = file_path.relative_to(ROOT)
        if not file_path.is_file():
            failures.append(f"{guard_name}: missing file '{rel_path}'")
            continue
        try:
            contents = file_path.read_text(encoding="utf-8")
        except OSError as exc:
            failures.append(f"{guard_name}: failed reading '{rel_path}': {exc}")
            continue

        for pattern in required_patterns:
            if re.search(pattern, contents, re.MULTILINE | re.DOTALL) is None:
                failures.append(
                    f"{guard_name}: missing expected pattern '{pattern}' in '{rel_path}'"
                )

    return not failures, failures


def _run_tracked_backup_guard() -> tuple[bool, list[str]]:
    source_tree_roots = [str(path.relative_to(ROOT)) for path in SOURCE_TREES]
    code, out, err = _run_command(["git", "ls-files", "--", *source_tree_roots])
    if code != 0:
        return False, [f"Failed to enumerate tracked source files for backup guard: {err.strip()}"]

    tracked_backups = sorted(
        line.strip() for line in out.splitlines() if line.strip().endswith(".backup")
    )
    if not tracked_backups:
        return True, []

    violations = ["Tracked '*.backup' files are not allowed under source trees:"]
    violations.extend(f"  - {path}" for path in tracked_backups)
    violations.append("Remove these files from git tracking (for example, with 'git rm <path>').")
    return False, violations


def _run_tracked_artifact_guard() -> tuple[bool, list[str]]:
    approved_result = _get_approved_tracked_synthetic_ply_fixtures()
    if isinstance(approved_result, list):
        return False, approved_result
    approved_tracked_synthetic_plys = approved_result

    code, out, err = _run_command(["git", "ls-files"])
    if code != 0:
        return False, [f"Failed to enumerate tracked files for artifact guard: {err.strip()}"]

    tracked_paths = [line.strip() for line in out.splitlines() if line.strip()]
    violations: list[str] = []
    for tracked_path in tracked_paths:
        for label, pattern in DISALLOWED_TRACKED_ARTIFACT_PATTERNS:
            if pattern.search(tracked_path):
                violations.append(f"  - {tracked_path} ({label})")
                break
        else:
            if TRACKED_SYNTHETIC_PLY_PATTERN.search(tracked_path) and tracked_path not in approved_tracked_synthetic_plys:
                violations.append(f"  - {tracked_path} (Unexpected tracked synthetic PLY fixture)")

    status_code, status_out, status_err = _run_command(
        ["git", "status", "--porcelain=1", "--untracked-files=all", "--", "tests", "templates"]
    )
    if status_code != 0:
        return False, [f"Failed to inspect synthetic fixture status for artifact guard: {status_err.strip()}"]

    for raw_line in status_out.splitlines():
        if not raw_line:
            continue
        status = raw_line[:2]
        path_text = raw_line[3:].strip()
        if " -> " in path_text:
            path_text = path_text.split(" -> ", 1)[1].strip()
        normalized_path = path_text.replace("\\", "/")
        if not TRACKED_SYNTHETIC_PLY_PATTERN.search(normalized_path):
            continue

        if normalized_path in approved_tracked_synthetic_plys:
            if status != "??":
                violations.append(
                    f"  - {normalized_path} (Approved tracked synthetic PLY fixture is dirty: git status '{status}')"
                )
            continue

        if status == "??":
            violations.append(f"  - {normalized_path} (Unexpected untracked synthetic PLY artifact)")
        else:
            violations.append(f"  - {normalized_path} (Unexpected dirty synthetic PLY artifact: git status '{status}')")

    for probe_path in REQUIRED_IGNORED_PATH_PROBES:
        check_code, _, _ = _run_command(["git", "check-ignore", "--quiet", "--no-index", probe_path])
        if check_code != 0:
            violations.append(
                f"  - Missing ignore rule for '{probe_path}' (required runtime log ignore)."
            )

    if not violations:
        return True, []

    messages = ["Tracked artifact hygiene guard failed:"]
    messages.extend(violations)
    messages.append("Remove tracked artifacts and update .gitignore patterns before merging.")
    return False, messages


def _get_approved_tracked_synthetic_ply_fixtures() -> set[str] | list[str]:
    global _approved_tracked_synthetic_ply_fixtures_cache
    if _approved_tracked_synthetic_ply_fixtures_cache is not None:
        return _approved_tracked_synthetic_ply_fixtures_cache

    if not SYNTHETIC_ASSET_PREP_SCRIPT.is_file():
        return [
            "Tracked artifact hygiene guard failed:",
            f"  - Missing synthetic asset prep script: {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}",
        ]

    try:
        spec = importlib.util.spec_from_file_location(
            "tests.runtime.prepare_synthetic_assets", SYNTHETIC_ASSET_PREP_SCRIPT
        )
        if spec is None or spec.loader is None:
            return [
                "Tracked artifact hygiene guard failed:",
                f"  - Unable to load synthetic asset definitions from {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}",
            ]
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
    except Exception as exc:
        return [
            "Tracked artifact hygiene guard failed:",
            f"  - Failed to import synthetic asset definitions from {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}: {exc}",
        ]

    canonical_specs = getattr(module, "CANONICAL_SPECS", None)
    if canonical_specs is None:
        return [
            "Tracked artifact hygiene guard failed:",
            f"  - Synthetic asset prep script does not expose CANONICAL_SPECS: {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}",
        ]

    approved_paths: set[str] = set()
    for spec_entry in canonical_specs:
        relative_path = getattr(spec_entry, "relative_path", "")
        if not relative_path:
            continue
        normalized_path = str(relative_path).replace("\\", "/")
        if normalized_path.endswith(".ply") and TRACKED_SYNTHETIC_PLY_PATTERN.search(normalized_path):
            approved_paths.add(normalized_path)

    _approved_tracked_synthetic_ply_fixtures_cache = approved_paths
    return approved_paths


def _run_build_metadata_guard() -> tuple[bool, list[str]]:
    if not BUILD_METADATA_GUARD_SCRIPT.is_file():
        return False, [f"Missing build metadata guard script: {BUILD_METADATA_GUARD_SCRIPT.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(BUILD_METADATA_GUARD_SCRIPT)])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]

    if code != 0:
        if not output_lines:
            output_lines = [f"Build metadata guard failed with exit code {code}."]
        return False, output_lines

    return True, output_lines


def _run_shader_dependency_guard() -> tuple[bool, list[str]]:
    if not SHADER_DEPENDENCY_GUARD_SCRIPT.is_file():
        return False, [
            f"Missing shader dependency guard script: {SHADER_DEPENDENCY_GUARD_SCRIPT.relative_to(ROOT)}"
        ]

    code, out, err = _run_command([sys.executable, str(SHADER_DEPENDENCY_GUARD_SCRIPT)])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]

    if code != 0:
        if not output_lines:
            output_lines = [f"Shader dependency guard failed with exit code {code}."]
        return False, output_lines

    return True, output_lines


def _run_project_settings_manifest_guard() -> tuple[bool, list[str]]:
    if not PROJECT_SETTINGS_MANIFEST_GUARD_SCRIPT.is_file():
        return False, [
            f"Missing ProjectSettings manifest guard script: {PROJECT_SETTINGS_MANIFEST_GUARD_SCRIPT.relative_to(ROOT)}"
        ]

    output_lines: list[str] = []
    for args in (
        [sys.executable, str(PROJECT_SETTINGS_MANIFEST_GUARD_SCRIPT), "--self-test"],
        [sys.executable, str(PROJECT_SETTINGS_MANIFEST_GUARD_SCRIPT)],
    ):
        code, out, err = _run_command(args)
        output_lines.extend(line for line in (out + err).splitlines() if line.strip())

        if code != 0:
            if not output_lines:
                output_lines = [f"ProjectSettings manifest guard failed with exit code {code}."]
            return False, output_lines

    return True, output_lines


def _run_gaussian_layout_guard() -> tuple[bool, list[str]]:
    if not GAUSSIAN_LAYOUT_GUARD_SCRIPT.is_file():
        return False, [
            f"Missing Gaussian layout guard script: {GAUSSIAN_LAYOUT_GUARD_SCRIPT.relative_to(ROOT)}"
        ]

    code, out, err = _run_command([sys.executable, str(GAUSSIAN_LAYOUT_GUARD_SCRIPT)])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]

    if code != 0:
        if not output_lines:
            output_lines = [f"Gaussian layout guard failed with exit code {code}."]
        return False, output_lines

    return True, output_lines


def _run_doc_classes_guard() -> tuple[bool, list[str]]:
    if not DOC_CLASSES_GUARD_SCRIPT.is_file():
        return False, [
            f"Missing doc_classes completeness guard script: {DOC_CLASSES_GUARD_SCRIPT.relative_to(ROOT)}"
        ]

    code, out, err = _run_command([sys.executable, str(DOC_CLASSES_GUARD_SCRIPT)])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]

    if code != 0:
        if not output_lines:
            output_lines = [f"doc_classes completeness guard failed with exit code {code}."]
        return False, output_lines

    return True, output_lines


def _run_test_linkage_guard() -> tuple[bool, list[str]]:
    if not TEST_LINKAGE_GUARD_SCRIPT.is_file():
        return False, [
            f"Missing test linkage guard script: {TEST_LINKAGE_GUARD_SCRIPT.relative_to(ROOT)}"
        ]

    code, out, err = _run_command([sys.executable, str(TEST_LINKAGE_GUARD_SCRIPT)])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]

    if code != 0:
        if not output_lines:
            output_lines = [f"Test linkage guard failed with exit code {code}."]
        return False, output_lines

    return True, output_lines


# Set from --base-ref in _run_ci_guard_steps so the guard subprocess can be told
# which base to ratchet against. A module global rather than a parameter because
# the guard-step table calls its runners with no arguments.
_GUARD_BASE_REF_OVERRIDE: str | None = None

# The PR base, in priority order. GITHUB_BASE_SHA / GITHUB_BASE_REF are what
# .github/workflows/agentic_pr_gate.yml has available from
# `github.event.pull_request.base.*`.
ENVIRONMENT_SKIP_BASE_ENV_VARS: tuple[str, ...] = (
    "GS_CI_ENV_SKIP_BASE_REF",
    "GS_CI_BASE_REF",
    "GITHUB_BASE_SHA",
    "GITHUB_BASE_REF",
)


# Events that propose a CHANGE for review and therefore HAVE a base. Anything
# else (push, schedule, workflow_dispatch) has no "review base" at all: HEAD is
# already the integrated state, so demanding one there would block the run rather
# than protect it. gaussian_production_gates.yml runs --guard-only on push,
# schedule, workflow_dispatch and merge_group as well as pull_request, so the
# distinction is load-bearing and not hypothetical.
BASE_BEARING_EVENTS: frozenset[str] = frozenset(
    {"pull_request", "pull_request_target", "merge_group"}
)


def _environment_skip_base_ref() -> tuple[str | None, list[str]]:
    """The review base to hand every base-anchored guard, or a hard failure.

    Shared, not per-guard: there is ONE review base for a diff. The env-skip marker
    ratchet and the unchecked-resize ratchet both grade their baseline against it, and
    the second used to resolve its own -- so `--guard-only --base-ref X` reached one of
    them and not the other, and on a stacked PR they graded different branches. Worse for
    the resize guard than for this one, because an older base can predate its baseline
    file entirely and "absent at base" is that guard's permissive branch.

    For a base-bearing event this MUST be explicit. The guard's own fallback
    chain ends at origin/master, which is correct only for PRs that target
    master -- and the PRs most in need of the ratchet are the stacked ones that
    do not. Letting the fallback run there means "defaulted to master" and
    "confirmed against the real base" produce the same green, which is the
    conflation this whole change exists to remove.

    For an event with no base (push, schedule, workflow_dispatch) returning None
    is correct, not lax: there is no proposed change to grade, the base-relative
    comparison has nothing to say, and the scan-vs-baseline check that catches a
    new skip is still fully enforced. Demanding a base there would fail every
    such run of a required gate.

    Locally, returning None is likewise deliberate: the guard uses its documented
    defaults, and no merge decision rests on a local run.
    """
    if _GUARD_BASE_REF_OVERRIDE:
        return _GUARD_BASE_REF_OVERRIDE, []
    for name in ENVIRONMENT_SKIP_BASE_ENV_VARS:
        value = os.environ.get(name, "").strip()
        if value:
            return value, []
    event = os.environ.get("GITHUB_EVENT_NAME", "").strip()
    if event and event not in BASE_BEARING_EVENTS:
        return None, []
    if _is_ci():
        return None, [
            "Base-anchored guards: no review base available in CI. Set one of "
            f"{', '.join(ENVIRONMENT_SKIP_BASE_ENV_VARS)} (the workflow has "
            "github.event.pull_request.base.sha) or pass --base-ref. Refusing to let the "
            "guard fall back to origin/master: on a PR stacked on a feature branch that "
            "grades the ratchet against the wrong branch, and reports green either way."
        ]
    return None, []


def _run_environment_skip_marker_guard() -> tuple[bool, list[str]]:
    """#595: environment skips are a counted, shrink-only inventory.

    doctest 2.4.12 has no runtime skip API and this build never unwinds, so an
    environment skip is `MESSAGE(...); return;` -- which doctest scores as
    PASSED. The static inventory is therefore the only thing that can tell a
    skipped case from a case that ran, for the many cases no lane executes at
    all. The guard also pins the four canonical macros to the `GS_ENV_SKIP:`
    token that the runtime detector above counts: without that check, reverting
    the macro bodies to free-form MESSAGE() would blind the runtime detector
    while every static count stayed identical.

    Runs the guard's own unit test first, mirroring the REQUIRE null-deref and
    metric-reset parity guards, so the rules are exercised against synthetic
    fixtures even when the real tree happens not to trip them.
    """
    base_ref, base_failures = _environment_skip_base_ref()
    if base_failures:
        return False, base_failures

    reported: list[str] = []
    for label, script, quiet_on_success in (
        ("Environment-skip marker guard unit test", ENVIRONMENT_SKIP_TEST_SCRIPT, True),
        # The DETECTOR's own pin test. It was written and then wired into no
        # lane at all -- the same defect this whole change exists to remove, and
        # the worst possible file to leave unrun: it is the only coverage of the
        # runtime_lane_allowance schema, the expiry-drops-to-zero rule and the
        # over-allowance failure, i.e. every code path that can LOOSEN the gate.
        ("Skip-marker detector pin test", SKIP_MARKER_DETECTOR_TEST_SCRIPT, True),
        ("Environment-skip marker guard", ENVIRONMENT_SKIP_GUARD_SCRIPT, False),
    ):
        if not script.is_file():
            return False, [f"Missing {label} script: {script.relative_to(ROOT)}"]
        # The base MUST reach the guard. Its own resolution falls back to
        # origin/master, which is wrong for any PR that does not target master --
        # and PRs stacked on a feature branch are exactly the ones the ratchet
        # exists to police (#821 targets gs/595-env-skip-marker, #822 targets
        # gs/650-quarantine-ratchet). Without this the ratchet silently graded
        # against master on precisely those PRs.
        extra = ["--base-ref", base_ref] if base_ref and script is ENVIRONMENT_SKIP_GUARD_SCRIPT else []
        code, out, err = _run_command([sys.executable, str(script), *extra])
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, reported + [f"{label} FAILED:"] + output_lines
        # The unit test's per-case chatter is noise on success, but its result
        # must still be visible: a guard whose self-test silently stopped running
        # is exactly the vacuous-pass shape this module keeps eliminating.
        reported.extend([f"{label} passed."] if quiet_on_success else output_lines)
    return True, reported
def _run_unchecked_resize_guard() -> tuple[bool, list[str]]:
    """Ratchet: no NEW unchecked `Vector::resize()` feeding a raw write (#794, #798).

    A failed resize leaves the vector at its PREVIOUS size, so a later `write[]` traps
    in CRASH_BAD_INDEX and a later `ptrw()` write runs past a live allocation with no
    diagnostic at all. This compares the tree against a GENERATED baseline and fails
    only on sites that are not already recorded, so it cannot silently bless a new
    defect while also not claiming the existing set is proven safe.

    Like the environment-skip guard above, its baseline is graded against the REVIEW
    BASE, so the base must reach it. `_environment_skip_base_ref()` is the one resolver
    for both -- there is one review base per diff, and two guards answering that question
    differently would be the bug.
    """
    if not UNCHECKED_RESIZE_GUARD_SCRIPT.is_file():
        return False, [f"Missing unchecked-resize guard: {UNCHECKED_RESIZE_GUARD_SCRIPT.relative_to(ROOT)}"]

    # The base MUST reach the guard, for the same reason it must reach the env-skip
    # guard: this one's own fallback chain also ends at origin/master, and a PR stacked
    # on a feature branch is exactly the case the ratchet exists to police. Worse here
    # than there, because an older wrong base can predate the baseline file entirely --
    # and "absent at base" is this guard's PERMISSIVE branch (no shrink-only reference,
    # so no addition is rejected). Defaulting silently would therefore not merely grade
    # the wrong branch, it would disable the base comparison and still report green.
    base_ref, base_failures = _environment_skip_base_ref()
    if base_failures:
        return False, base_failures

    # Run the guard's OWN self-tests in the same lane, and FIRST. Two review rounds
    # found EIGHT distinct ways to evade this guard: key collision, --regenerate
    # blessing a new site on a net-zero delta, a fixed-window function-scope cap,
    # line-anchored matching missing wrapped calls, an unreadable source passing
    # silently, a trailing comment hiding a statement, a '}' inside a comment or string
    # truncating the function span, and an occurrence ordinal that counted duplicate
    # sites without identifying them. Each is now a self-test. Wiring them here rather
    # than into a separate lane is
    # deliberate: this file already documents a guard that "existed but was wired into
    # NO lane and no runner ... therefore never executed once", and a self-test that
    # does not run is worth exactly nothing.
    self_test = ROOT / "tests" / "ci" / "test_unchecked_resize_guard.py"
    if not self_test.is_file():
        return False, [f"Missing unchecked-resize guard self-test: {self_test.relative_to(ROOT)}"]
    code, out, err = _run_command([sys.executable, str(self_test)])
    if code != 0:
        lines = [line for line in (out + err).splitlines() if line.strip()]
        return False, lines or [f"Unchecked-resize guard self-test failed with exit code {code}."]
    # Report the case count unittest actually ran, never a number typed by hand: the
    # previous literal ("7 cases") was already stale, and a hand-maintained count is a
    # claim about coverage that nothing checks.
    ran = re.search(r"^Ran (\d+) tests?", out + err, re.MULTILINE)
    case_count = f"{ran.group(1)} cases" if ran else "case count not reported"

    extra = ["--base-ref", base_ref] if base_ref else []
    code, out, err = _run_command([sys.executable, str(UNCHECKED_RESIZE_GUARD_SCRIPT), *extra])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]
    if code != 0:
        if not output_lines:
            output_lines = [f"Unchecked-resize guard failed with exit code {code}."]
        return False, output_lines
    return True, [f"Unchecked-resize guard self-test passed ({case_count})."] + output_lines


def _run_require_null_deref_guard() -> tuple[bool, list[str]]:
    """#656: REQUIRE does not abort under DOCTEST_CONFIG_NO_EXCEPTIONS.

    Flags `REQUIRE(<null-ish>)` followed by a dereference of the same symbol,
    which crashes the whole test binary instead of failing one case. Runs the
    guard's own unit test first, so the tree/false-positive discrimination is
    exercised in the fast --guard-only lane (mirrors the metric-reset parity
    guard).
    """
    for label, script in (
        ("REQUIRE null-deref guard unit test", REQUIRE_NULL_DEREF_TEST_SCRIPT),
        ("REQUIRE null-deref guard", REQUIRE_NULL_DEREF_GUARD_SCRIPT),
    ):
        if not script.is_file():
            return False, [f"Missing {label} script: {script.relative_to(ROOT)}"]
        code, out, err = _run_command([sys.executable, str(script)])
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, output_lines
    return True, output_lines


def _run_renderer_contract_boundary_guard() -> tuple[bool, list[str]]:
    """#611: every blocking render-thread dispatch stays behind an instrumented boundary.

    The lock-order inversion this guards has no behavioural lane (every doctest
    process is `--headless --test` and the dispatcher short-circuits), so the
    violation counter is the evidence — and the counter is only complete while no
    new direct `renderer->` dispatch appears in the scene director. Runs the
    guard's own discrimination cases first, mirroring the metric-reset and
    REQUIRE null-deref guards.
    """
    for label, args in (
        ("Renderer-contract boundary guard self-test", ["--self-test"]),
        ("Renderer-contract boundary guard", []),
    ):
        if not RENDERER_CONTRACT_BOUNDARY_GUARD_SCRIPT.is_file():
            return False, [
                f"Missing {label} script: {RENDERER_CONTRACT_BOUNDARY_GUARD_SCRIPT.relative_to(ROOT)}"
            ]
        code, out, err = _run_command(
            [sys.executable, str(RENDERER_CONTRACT_BOUNDARY_GUARD_SCRIPT), *args]
        )
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, output_lines
    return True, output_lines


def _run_device_submission_contract_guard() -> tuple[bool, list[str]]:
    """#685: every local-device submit/blocking readback stays behind gs_device_utils.

    A local RenderingDevice between submit() and sync() has an ended command
    buffer and draw graph; a second submit() is rejected outright and a
    synchronous buffer_get_data()/texture_get_data() faults in the driver
    replaying the graph. Shipping builds are spared only because
    get_primary_rendering_device() returns the MAIN device, on which safe_submit
    is a no-op -- a property of that one function, not of the sort path, and so
    not something the sort path can rely on. The guard keeps the rule true by
    construction. Runs its own discrimination cases first, like its siblings.
    """
    for label, args in (
        ("Device-submission contract guard self-test", ["--self-test"]),
        ("Device-submission contract guard", []),
    ):
        if not DEVICE_SUBMISSION_CONTRACT_GUARD_SCRIPT.is_file():
            return False, [
                f"Missing {label} script: {DEVICE_SUBMISSION_CONTRACT_GUARD_SCRIPT.relative_to(ROOT)}"
            ]
        code, out, err = _run_command(
            [sys.executable, str(DEVICE_SUBMISSION_CONTRACT_GUARD_SCRIPT), *args]
        )
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, output_lines
    return True, output_lines


def _run_editor_node_pointer_lifetime_guard() -> tuple[bool, list[str]]:
    """#698: no raw Node pointer may survive a re-entrant editor call.

    `EditorFileSystem::reimport_file_with_custom_parameters()` emits
    `resources_reimported` synchronously, and a handler can close the scene and
    free the node the caller resolved before the call. A null test on the
    retained pointer does not detect that -- freed memory can still test
    non-null -- so the only correct pattern is to carry an ObjectID across the
    call and re-resolve through ObjectDB before each dereference. Runs its own
    discrimination cases first, like its siblings.
    """
    for label, args in (
        ("Editor node-pointer lifetime guard self-test", ["--self-test"]),
        ("Editor node-pointer lifetime guard", []),
    ):
        if not EDITOR_NODE_POINTER_GUARD_SCRIPT.is_file():
            return False, [
                f"Missing {label} script: {EDITOR_NODE_POINTER_GUARD_SCRIPT.relative_to(ROOT)}"
            ]
        code, out, err = _run_command(
            [sys.executable, str(EDITOR_NODE_POINTER_GUARD_SCRIPT), *args]
        )
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, output_lines
    return True, output_lines


def _run_test_lane_coverage_guard() -> tuple[bool, list[str]]:
    """#520: fail when a registered TEST_CASE matches no lane in any runner.

    Runs as a subprocess (like every sibling guard) rather than in-process: the
    guard IMPORTS this module to read MODULE_TEST_FILTERS / REQUIRES_RD_TEST_FILTERS,
    so calling it in-process would re-enter the runner.
    """
    for label, script in (
        ("Test lane coverage guard unit test", TEST_LANE_COVERAGE_TEST_SCRIPT),
        ("Test lane coverage guard", TEST_LANE_COVERAGE_GUARD_SCRIPT),
    ):
        if not script.is_file():
            return False, [f"Missing {label} script: {script.relative_to(ROOT)}"]
        code, out, err = _run_command([sys.executable, str(script)])
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, output_lines
    return True, output_lines


def _run_cull_signature_parity_guard() -> tuple[bool, list[str]]:
    # Mirrors _run_metric_reset_parity_guard: run the guard against the committed
    # sources, then the guard's own unit test. The guard passing only proves the
    # tree is clean today; the unit test pins the extractor's rules (notably that
    # a LOSSY fold such as `_hash_bool(config.knob > 0, seed)` does NOT count as
    # hashed) against synthetic fixtures, so a future parser change that re-opens
    # a fail-open hole is caught even when the real sources do not exercise it.
    missing = [
        path.relative_to(ROOT)
        for path in (CULL_SIGNATURE_GUARD_SCRIPT, CULL_SIGNATURE_TEST_SCRIPT)
        if not path.is_file()
    ]
    if missing:
        return False, [f"Missing cull-signature parity guard file: {path}" for path in missing]

    output_lines: list[str] = []
    commands = (
        [sys.executable, str(CULL_SIGNATURE_GUARD_SCRIPT)],
        [sys.executable, str(CULL_SIGNATURE_TEST_SCRIPT)],
    )
    for args in commands:
        code, out, err = _run_command(args)
        output_lines.extend(line for line in (out + err).splitlines() if line.strip())
        if code != 0:
            if not output_lines:
                output_lines = [f"Cull-signature parity guard failed with exit code {code}."]
            return False, output_lines

    return True, output_lines


def _run_metric_reset_parity_guard() -> tuple[bool, list[str]]:
    # Mirrors _run_renderer_release_gate_guard: run the guard script against
    # the committed header, then the guard's own unit test (which pins the
    # parser's matching rules against synthetic fixtures, incl. the #627
    # non-reset-mutator counterexample) so a future regex change is caught
    # even if it happens not to flag anything in the current header.
    missing = [
        path.relative_to(ROOT)
        for path in (METRIC_RESET_PARITY_GUARD_SCRIPT, METRIC_RESET_PARITY_TEST_SCRIPT)
        if not path.is_file()
    ]
    if missing:
        return False, [f"Missing metric-reset parity guard file: {path}" for path in missing]

    output_lines: list[str] = []
    commands = (
        [sys.executable, str(METRIC_RESET_PARITY_GUARD_SCRIPT)],
        [sys.executable, str(METRIC_RESET_PARITY_TEST_SCRIPT)],
    )
    for args in commands:
        code, out, err = _run_command(args)
        output_lines.extend(line for line in (out + err).splitlines() if line.strip())
        if code != 0:
            if not output_lines:
                output_lines = [f"Metric-reset parity guard failed with exit code {code}."]
            return False, output_lines

    return True, output_lines


def _run_reject_telemetry_parity_guard() -> tuple[bool, list[str]]:
    # #586 round-4. Sibling of _run_metric_reset_parity_guard, for the OTHER telemetry
    # struct: check_metric_reset_parity.py covers PerformanceMetrics' per-frame reset,
    # this one covers RasterPerformance's per-REJECT invalidation. Two consecutive
    # review rounds found a field missing from _reject_frame()'s hand-written list, so
    # the guard derives the covered set from the real
    # struct -> get_performance() -> getter -> _reject_frame() chain and fails closed on
    # anything it cannot resolve. Its unit test runs too, so a future regex change is
    # caught even if it happens not to flag anything on today's tree.
    missing = [
        path.relative_to(ROOT)
        for path in (REJECT_TELEMETRY_PARITY_GUARD_SCRIPT, REJECT_TELEMETRY_PARITY_TEST_SCRIPT)
        if not path.is_file()
    ]
    if missing:
        return False, [f"Missing reject-telemetry parity guard file: {path}" for path in missing]

    output_lines: list[str] = []
    commands = (
        [sys.executable, str(REJECT_TELEMETRY_PARITY_GUARD_SCRIPT)],
        [sys.executable, str(REJECT_TELEMETRY_PARITY_TEST_SCRIPT)],
    )
    for args in commands:
        code, out, err = _run_command(args)
        output_lines.extend(line for line in (out + err).splitlines() if line.strip())
        if code != 0:
            if not output_lines:
                output_lines = [f"Reject-telemetry parity guard failed with exit code {code}."]
            return False, output_lines

    return True, output_lines


def _run_renderer_release_gate_guard() -> tuple[bool, list[str]]:
    missing = [
        path.relative_to(ROOT)
        for path in (RENDERER_RELEASE_GATE_SCRIPT, RENDERER_RELEASE_GATE_TEST_SCRIPT)
        if not path.is_file()
    ]
    if missing:
        return False, [f"Missing renderer release gate guard file: {path}" for path in missing]

    output_lines: list[str] = []
    commands = (
        [sys.executable, str(RENDERER_RELEASE_GATE_SCRIPT), "--mode", "contract"],
        [sys.executable, str(RENDERER_RELEASE_GATE_TEST_SCRIPT)],
    )
    for args in commands:
        code, out, err = _run_command(args)
        output_lines.extend(line for line in (out + err).splitlines() if line.strip())
        if code != 0:
            if not output_lines:
                output_lines = [f"Renderer release gate guard failed with exit code {code}."]
            return False, output_lines

    return True, output_lines


def _run_baseline_qa_require_flag_guard() -> tuple[bool, list[str]]:
    """Run run_baseline_qa.py's own require-baseline regression test (#596
    follow-up: the switch was previously ignored on the QA-scene-skip path;
    see test_baseline_qa_require_flag.py). Deterministic, no GPU/binary
    required."""
    if not BASELINE_QA_REQUIRE_FLAG_TEST_SCRIPT.is_file():
        return False, [
            f"Missing baseline QA require-flag unit test: "
            f"{BASELINE_QA_REQUIRE_FLAG_TEST_SCRIPT.relative_to(ROOT)}"
        ]

    code, out, err = _run_command([sys.executable, str(BASELINE_QA_REQUIRE_FLAG_TEST_SCRIPT)])
    output_lines = [line for line in (out + err).splitlines() if line.strip()]
    if code != 0:
        if not output_lines:
            output_lines = [f"Baseline QA require-flag unit test failed with exit code {code}."]
        return False, output_lines

    return True, ["Baseline QA require-flag unit test passed."]


def _parse_quarantine_expiry(value: str) -> datetime | None:
    """Parse an ISO-8601 expires_utc into a UTC-aware datetime, or None."""
    try:
        parsed = datetime.fromisoformat(value.strip().replace("Z", "+00:00"))
    except (AttributeError, ValueError):
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def _load_quarantine(path: Path | None = None) -> dict[str, list[dict]]:
    """Return the quarantine map: lane name -> LIST of entries for that lane.

    A lane may carry more than one entry (one per approved failing test_case),
    so entries are grouped into a list per lane rather than one entry per lane.
    A missing file, unreadable/invalid JSON, a non-object root, a non-list
    'entries', or an empty 'entries' all resolve to an empty map, so an empty
    (or absent) manifest is behaviorally inert: no lane is treated as
    quarantined. The schema guard (_run_quarantine_manifest_guard) is the
    enforcement point for malformed manifests; this loader is intentionally
    lenient so it never raises inside the lane loop.
    """
    manifest_path = path if path is not None else QUARANTINE_MANIFEST_PATH
    if not manifest_path.is_file():
        return {}
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict):
        return {}
    entries = data.get("entries")
    if not isinstance(entries, list):
        return {}
    quarantine: dict[str, list[dict]] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        lane = entry.get("lane")
        if isinstance(lane, str) and lane:
            quarantine.setdefault(lane, []).append(entry)
    return quarantine


def _validate_quarantine_entry(
    index: int,
    entry: object,
    valid_lanes: set[str],
    now: datetime,
    seen_lane_cases: set[tuple[str, str]],
) -> list[str]:
    """Validate a single manifest entry; return ASCII-only failure messages.

    Repeated lanes are allowed (multiple entries per lane, one per approved
    test_case); only an exact-duplicate (lane, test_case) pair is rejected.
    """
    label = f"entry[{index}]"
    if not isinstance(entry, dict):
        return [f"Quarantine {label} must be a JSON object."]

    failures: list[str] = []
    lane = entry.get("lane")
    if isinstance(lane, str) and lane:
        label = f"entry[{index}] lane '{lane}'"

    for field in QUARANTINE_REQUIRED_FIELDS:
        value = entry.get(field)
        if not isinstance(value, str) or not value.strip():
            failures.append(f"Quarantine {label} is missing required field '{field}'.")

    if isinstance(lane, str) and lane:
        if lane not in valid_lanes:
            failures.append(
                f"Quarantine {label} names a lane not present in MODULE_TEST_FILTERS."
            )
        test_case = entry.get("test_case")
        if isinstance(test_case, str) and test_case.strip():
            key = (lane, test_case.strip())
            if key in seen_lane_cases:
                failures.append(
                    f"Quarantine {label} duplicates an earlier entry for the same "
                    f"(lane, test_case)."
                )
            seen_lane_cases.add(key)

    expires = entry.get("expires_utc")
    if isinstance(expires, str) and expires.strip():
        parsed = _parse_quarantine_expiry(expires)
        if parsed is None:
            failures.append(
                f"Quarantine {label} has an unparseable expires_utc '{expires}'."
            )
        elif parsed <= now:
            failures.append(
                f"Quarantine {label} is past its expires_utc '{expires}'; "
                "re-verify the failure and refresh the entry or remove it."
            )

    return failures


def _validate_quarantine_manifest_schema() -> tuple[bool, list[str]]:
    """Schema-validate the quarantine manifest file.

    An empty manifest passes trivially. A populated entry must carry every
    required field, must name a lane present in MODULE_TEST_FILTERS, and must
    not be past its expires_utc. All messages are ASCII-only.
    """
    path = QUARANTINE_MANIFEST_PATH
    try:
        rel = path.relative_to(ROOT)
    except ValueError:
        rel = path
    if not path.is_file():
        return True, [f"Quarantine manifest absent ({rel}); no lanes quarantined."]

    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        return False, [f"Quarantine manifest unreadable ({rel}): {exc}"]

    try:
        data = json.loads(raw)
    except ValueError as exc:
        return False, [f"Quarantine manifest is not valid JSON ({rel}): {exc}"]

    if not isinstance(data, dict):
        return False, [f"Quarantine manifest root must be a JSON object ({rel})."]

    failures: list[str] = []
    schema_version = data.get("schema_version")
    if schema_version != 1:
        failures.append(
            f"Quarantine manifest schema_version must be 1 (got {schema_version!r})."
        )

    entries = data.get("entries")
    if not isinstance(entries, list):
        return False, [f"Quarantine manifest 'entries' must be a list ({rel})."]

    valid_lanes = {name for name, *_ in MODULE_TEST_FILTERS}
    now = datetime.now(timezone.utc)
    seen_lane_cases: set[tuple[str, str]] = set()
    for index, entry in enumerate(entries):
        failures.extend(
            _validate_quarantine_entry(index, entry, valid_lanes, now, seen_lane_cases)
        )

    if failures:
        return False, failures

    plural = "entry" if len(entries) == 1 else "entries"
    return True, [
        f"Quarantine manifest schema guard passed ({len(entries)} {plural}, {rel})."
    ]


def _run_quarantine_manifest_unittest() -> tuple[bool, list[str]]:
    """Run the mechanism's own unit test so its lane logic is validated in CI.

    Mirrors _run_renderer_release_gate_guard, which runs its test script. A
    recursion guard (QUARANTINE_UNITTEST_ACTIVE_ENV) makes a nested invocation
    (a test that itself calls the guard) a no-op instead of re-spawning.
    """
    if _env_truthy(os.environ.get(QUARANTINE_UNITTEST_ACTIVE_ENV, "")):
        return True, ["Quarantine manifest unit test skipped (nested guard invocation)."]

    if not QUARANTINE_MANIFEST_TEST_SCRIPT.is_file():
        return False, [
            f"Missing quarantine manifest unit test: "
            f"{QUARANTINE_MANIFEST_TEST_SCRIPT.relative_to(ROOT)}"
        ]

    child_env = dict(os.environ)
    child_env[QUARANTINE_UNITTEST_ACTIVE_ENV] = "1"
    code, out, err = _run_command(
        [sys.executable, str(QUARANTINE_MANIFEST_TEST_SCRIPT)], env=child_env
    )
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Quarantine manifest unit test failed with exit code {code}."]
        return False, output_lines

    return True, ["Quarantine manifest unit test passed."]


def _run_benchmark_fixture_contract_guard() -> tuple[bool, list[str]]:
    """Guard (#669): a benchmark lane refuses an absent or undersized fixture.

    Pure static/unit coverage of the fail-closed fixture contract -- no GPU and no
    engine binary -- so the vacuous-benchmark regression is caught on every PR.
    The failure mode it protects against is a lane that reports a *flattering*
    number (empty scene, ~2400 FPS, passing recommendation) instead of failing.
    """
    script = ROOT / "tests" / "ci" / "test_benchmark_fixture_contract.py"
    if not script.is_file():
        return False, [f"Missing benchmark fixture contract test: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Benchmark fixture contract guard failed with exit code {code}."]
        return False, output_lines

    return True, ["Benchmark fixture contract guard passed."]


def _run_gpu_harness_deferred_contract_guard() -> tuple[bool, list[str]]:
    """Guard (#329): every [RequiresGPU] test runs in a named GPU batch, is
    waived, or sits in the recorded unbatched backlog.

    This lane is deliberately headless-only and needs no GPU: it is pure static
    analysis of the test corpus against run_gpu_harness.py's BatchSpecs and the
    release-gate manifest. It catches the #329 failure mode -- a test that
    executes in no lane at all -- on every PR, not just on the GPU runner.
    """
    script = ROOT / "tests" / "ci" / "test_gpu_harness_deferred_contract.py"
    if not script.is_file():
        return False, [f"Missing GPU harness deferred contract guard: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"GPU harness deferred contract guard failed with exit code {code}."]
        return False, output_lines

    return True, ["GPU harness deferred contract guard passed."]


def _run_export_template_naming_guard() -> tuple[bool, list[str]]:
    """Guard (#825): the export-template jobs resolve the names SConstruct emits.

    Static, headless, no GPU, no build. The first version of
    `build_windows_export_template` globbed `godot.windows.template_release*.x86_64.exe`
    and then demanded one `*.console.exe` among the matches -- but SConstruct
    appends `.console` AFTER the architecture, so the wrapper
    (`godot.windows.template_release.x86_64.console.exe`) never matched, the
    count check threw, and the job could not upload an artifact on any run.

    The failure mode this protects against is the one that hid it: the guard was
    right and its INPUT was wrong, and nothing checked the matching against the
    file names the producer actually writes.
    """
    script = ROOT / "tests" / "ci" / "test_resolve_export_template.py"
    if not script.is_file():
        return False, [f"Missing export template naming test: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Export template naming guard failed with exit code {code}."]
        return False, output_lines

    return True, ["Export template naming guard passed."]


def _run_release_builds_path_filter_guard() -> tuple[bool, list[str]]:
    """Guard (#825): every script release_builds.yml runs also triggers it.

    Static, headless, no GPU, no PyYAML. The workflow's `paths:` filters list
    root-level `"*.py"`, and a single `*` does not span `/`, so a change to
    `tests/ci/resolve_export_template.py` -- which BOTH export-template jobs
    execute -- skipped the workflow entirely. The resolver's unit tests would
    pass while neither real packaging path ran.

    The general hazard: moving logic out of a workflow into a helper module
    improves testability and silently reduces integration coverage, because the
    helper lands outside the paths the workflow watches. This guard derives the
    executed-script set from the workflow rather than trusting the filter list,
    so the next helper cannot reopen the gap unnoticed.
    """
    script = ROOT / "tests" / "ci" / "test_release_builds_path_filters.py"
    if not script.is_file():
        return False, [f"Missing release builds path filter test: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Release builds path filter guard failed with exit code {code}."]
        return False, output_lines

    return True, ["Release builds path filter guard passed."]


def _run_release_builds_runner_trust_guard() -> tuple[bool, list[str]]:
    """Guard (#825): every self-hosted release_builds.yml job is guarded and documented.

    Static, headless, no GPU, no PyYAML. `.github/workflows/AGENTS.md` requires
    self-hosted jobs to carry a fork guard, and requires `README.md` to stay in
    sync with the runner trust policy. The second requirement was carried by a
    hand-written prose list, so when #825 added a second self-hosted job to
    `release_builds.yml` the documented trust boundary silently became partial
    while every check stayed green.

    This guard derives the self-hosted job set from the workflow and checks both
    directions against README, and fails closed on any `runs-on:`/job-level
    `if:` form it cannot model -- an unreadable job must not read as a clean one.
    """
    script = ROOT / "tests" / "ci" / "test_release_builds_runner_trust.py"
    if not script.is_file():
        return False, [f"Missing release builds runner trust test: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Release builds runner trust guard failed with exit code {code}."]
        return False, output_lines

    return True, ["Release builds runner trust guard passed."]


def _run_export_smoke_preset_state_guard() -> tuple[bool, list[str]]:
    """Guard (#825): the export smoke test never destroys a preset it did not create.

    Static, headless, no GPU, no engine binary -- it drives the preset/backup
    state machine over a temp directory.

    `test_project/export_presets.cfg` is gitignored, so losing a developer's own
    copy is *invisible* to `git status`. The first fix moved the file aside
    instead of truncating it, which made the happy path safe and left the
    recovery path worse: with a backup already on disk from a crashed run, the
    unconditional `unlink()` deleted the only surviving original. This guard
    pins all three backup states (absent / present-with-preset /
    present-without-preset) and that a refusal rewrites nothing.
    """
    script = ROOT / "tests" / "runtime" / "test_export_smoke_preset_state.py"
    if not script.is_file():
        return False, [f"Missing export smoke preset state test: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Export smoke preset state guard failed with exit code {code}."]
        return False, output_lines

    return True, ["Export smoke preset state guard passed."]


def _run_runtime_validation_contract_guard() -> tuple[bool, list[str]]:
    """Guard (#787): the runtime summary keeps the diagnostic a crashed scenario emits.

    Static, headless, no GPU. `tests/runtime/test_runtime_validation_proof_contract.py`
    existed but was wired into NO lane and no runner -- `check_test_lane_coverage.py`
    only scans .h/.cpp, so nothing noticed. It therefore never executed once.

    The failure mode it now protects against is the one that hid #787 for a full
    nightly cycle: the summary serialised only `reasons`, whose crash fallback is the
    FIRST stderr line (a benign startup warning), so a fatal out-of-bounds trap was
    reported as "Can't create an accessibility driver" and the message naming the real
    fault was captured and discarded.
    """
    script = ROOT / "tests" / "runtime" / "test_runtime_validation_proof_contract.py"
    if not script.is_file():
        return False, [f"Missing runtime validation contract test: {script.relative_to(ROOT)}"]

    code, out, err = _run_command([sys.executable, str(script)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Runtime validation contract guard failed with exit code {code}."]
        return False, output_lines

    return True, ["Runtime validation contract guard passed."]


def _run_gpu_sorting_order_coverage_guard() -> tuple[bool, list[str]]:
    """Guard (#622): the required GpuSorting batch actually gates sort-ORDER.

    Static, headless, no GPU: it imports run_gpu_harness.py's BATCHES /
    REQUIRED_BATCHES and confirms the Bitonic/Radix sort-order oracles in
    test_gpu_sorting.h are SELECTED by the GpuSorting batch filter and still
    carry their discriminating order assertion. Runs its own --self-test
    discrimination cases first (like its sibling guards) so a parser regression
    that made it vacuous is caught even when the tree is clean.
    """
    for label, args in (
        ("GPU sorting order-coverage guard self-test", ["--self-test"]),
        ("GPU sorting order-coverage guard", []),
    ):
        if not GPU_SORTING_ORDER_COVERAGE_GUARD_SCRIPT.is_file():
            return False, [
                f"Missing {label} script: "
                f"{GPU_SORTING_ORDER_COVERAGE_GUARD_SCRIPT.relative_to(ROOT)}"
            ]
        code, out, err = _run_command(
            [sys.executable, str(GPU_SORTING_ORDER_COVERAGE_GUARD_SCRIPT), *args]
        )
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if code != 0:
            if not output_lines:
                output_lines = [f"{label} failed with exit code {code}."]
            return False, output_lines
    return True, output_lines


def _run_quarantine_manifest_guard() -> tuple[bool, list[str]]:
    """Guard step (runs in the --guard-only lane): schema-validate the manifest
    and then run the mechanism's unit test. Fails on either."""
    schema_ok, messages = _validate_quarantine_manifest_schema()
    if not schema_ok:
        return False, messages
    test_ok, test_messages = _run_quarantine_manifest_unittest()
    return test_ok, messages + test_messages


def _run_lane_ledger_guard() -> tuple[bool, list[str]]:
    """Run the per-lane ledger's own unit test in the fast --guard-only lane (#705).

    Mirrors the skip-marker detector pin test: a test wired into no lane is the
    same defect as an advisory lane wired into no gate. The ledger's exit-code
    PARITY assertions are the coverage that stops this reporting slice from
    quietly becoming a gate, so they must actually run somewhere.
    """
    if not LANE_LEDGER_TEST_SCRIPT.is_file():
        return False, [
            f"Missing lane-ledger unit test: {_display_path(LANE_LEDGER_TEST_SCRIPT)}"
        ]
    code, out, err = _run_command([sys.executable, str(LANE_LEDGER_TEST_SCRIPT)])
    if code != 0:
        output_lines = [line for line in (out + err).splitlines() if line.strip()]
        if not output_lines:
            output_lines = [f"Lane-ledger unit test failed with exit code {code}."]
        return False, ["Lane-ledger unit test FAILED:"] + output_lines
    return True, ["Lane-ledger unit test passed."]


def _tests_unavailable(output: str) -> bool:
    normalized_output = " ".join(output.lower().split())
    markers = (
        "unknown option '--test'",
        "unknown option \"--test\"",
        "unknown option '--test-case'",
        "unknown option '--test-suite'",
        "testing is disabled",
        "tests are disabled",
        "support for tests is disabled",
        "compiled without support for unit test",
    )
    return any(marker in normalized_output for marker in markers)


def _env_truthy(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _is_ci() -> bool:
    return _env_truthy(os.environ.get("CI", ""))


def _resolve_tests_unavailable_mode(explicit_mode: str | None) -> str:
    if explicit_mode in ("strict", "warn-only"):
        return explicit_mode

    shared_mode = os.environ.get(VALIDATION_MODE_ENV, "").strip().lower()
    if shared_mode in ("strict", "warn-only"):
        return shared_mode

    return "strict" if os.environ.get("CI") else "warn-only"


def _resolve_history_artifact_guard_mode() -> tuple[str, str | None]:
    mode = os.environ.get(HISTORY_ARTIFACT_GUARD_MODE_ENV, "").strip().lower()
    if not mode:
        return "warn", None
    if mode in HISTORY_ARTIFACT_GUARD_MODES:
        return mode, None
    return (
        "warn",
        (
            f"Invalid {HISTORY_ARTIFACT_GUARD_MODE_ENV}='{mode}'. "
            "Falling back to 'warn'."
        ),
    )


def _run_history_artifact_guard(mode: str) -> tuple[bool, int, list[str]]:
    messages = [
        f"History artifact guard mode: {mode} (env: {HISTORY_ARTIFACT_GUARD_MODE_ENV})."
    ]

    if mode == "off":
        messages.append("History artifact guard skipped (mode=off).")
        return True, 0, messages

    if not HISTORY_ARTIFACT_AUDIT_SCRIPT.is_file():
        missing_msg = (
            f"Missing history artifact audit script: {HISTORY_ARTIFACT_AUDIT_SCRIPT.relative_to(ROOT)}"
        )
        if mode == "strict":
            return False, 1, messages + [missing_msg]
        messages.append(f"{missing_msg}; skipping history audit in warn mode.")
        return True, 0, messages

    code, out, err = _run_command([sys.executable, str(HISTORY_ARTIFACT_AUDIT_SCRIPT)], cwd=ROOT)
    combined_output = (out + err).strip()
    if combined_output:
        for line in combined_output.splitlines():
            messages.append(f"[history-audit] {line}")

    if code != 0:
        messages.append(f"History artifact guard failed: audit exited with code {code}.")
        return False, 1, messages

    match = HISTORY_ARTIFACT_MATCH_COUNT_RE.search(out + err)
    if match is None:
        messages.append(
            "History artifact guard failed: unable to parse 'Matched blob entries' from audit output."
        )
        return False, 1, messages

    matched_entries = int(match.group(1))
    if matched_entries <= 0:
        messages.append("History artifact guard passed: no matched history artifact entries.")
        return True, 0, messages

    entry_label = "entry" if matched_entries == 1 else "entries"
    if mode == "strict":
        messages.append(
            f"History artifact guard strict failure: found {matched_entries} matched history artifact {entry_label}."
        )
        return False, 3, messages

    messages.append(
        f"History artifact guard warning: found {matched_entries} matched history artifact {entry_label}. "
        "Continuing because mode=warn."
    )
    return True, 0, messages


def _prepare_synthetic_assets() -> tuple[bool, list[str]]:
    if not SYNTHETIC_ASSET_PREP_SCRIPT.is_file():
        return (
            False,
            [f"Missing synthetic asset prep script: {SYNTHETIC_ASSET_PREP_SCRIPT.relative_to(ROOT)}"],
        )

    code, out, err = _run_command(
        [sys.executable, str(SYNTHETIC_ASSET_PREP_SCRIPT), "--quiet"],
        cwd=ROOT,
    )

    messages = ["Preparing synthetic PLY assets for runtime and template lanes."]
    combined = (out + err).strip()
    if combined:
        messages.extend(combined.splitlines())
    if code != 0:
        messages.append(f"Synthetic asset preparation failed with exit code {code}.")
        return False, messages
    return True, messages


def _run_benchmark_asset_guard() -> tuple[bool, list[str]]:
    if not BENCHMARK_ASSET_GUARD_SCRIPT.is_file():
        return (
            False,
            [f"Missing benchmark asset guard script: {BENCHMARK_ASSET_GUARD_SCRIPT.relative_to(ROOT)}"],
        )

    code, out, err = _run_command([sys.executable, str(BENCHMARK_ASSET_GUARD_SCRIPT)], cwd=ROOT)
    output_lines = [line for line in (out + err).splitlines() if line.strip()]
    if code != 0:
        if not output_lines:
            output_lines = [f"Benchmark asset guard failed with exit code {code}."]
        return False, output_lines
    return True, output_lines


class GodotRunResult(tuple):
    """`(ok, skipped, output)` plus the raw process exit code (#705).

    Deliberately a 3-tuple SUBCLASS rather than a 4-field NamedTuple: every
    existing caller and every test stub unpacks exactly three values, and the
    lane ledger must not be bought with a signature break in the one function
    the whole runner funnels through. A stub that returns a plain tuple simply
    has no `returncode` attribute, which the ledger records as UNKNOWN (-1)
    rather than as a fabricated 0.
    """

    # No __slots__: a variable-length built-in subtype (tuple) cannot carry a
    # non-empty __slots__, so the attribute lives in the instance __dict__.

    def __new__(cls, ok: bool, skipped: bool, output: str, returncode: int | None):
        self = super().__new__(cls, (ok, skipped, output))
        self.returncode = returncode
        return self


def _run_godot(godot: str, args: Iterable[str]) -> tuple[bool, bool, str]:
    normalized_godot = _normalize_process_arg(godot)
    command = [normalized_godot]
    command.extend(_normalize_process_arg(arg) for arg in args)

    if not command[0]:
        return GodotRunResult(
            False, False, "ValueError: empty Godot binary path after normalization", None
        )

    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except (FileNotFoundError, PermissionError, OSError) as exc:
        return GodotRunResult(
            False, False, f"{type(exc).__name__}: {exc} (command={command!r})", None
        )

    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0 and _tests_unavailable(output):
        return GodotRunResult(True, True, output, result.returncode)
    return GodotRunResult(result.returncode == 0, False, output, result.returncode)


def _parse_doctest_results(output: str) -> tuple[int, int, int, int, int, bool]:
    """Parse doctest output and return counts plus whether both summary lines were found."""
    tests_match = re.search(r"test cases:\s*\d+\s*\|\s*(\d+)\s*passed\s*\|\s*(\d+)\s*failed", output)
    asserts_match = re.search(r"assertions:\s*\d+\s*\|\s*(\d+)\s*passed\s*\|\s*(\d+)\s*failed", output)
    skip_markers = len(DOCTEST_SKIP_MARKER_RE.findall(output))

    passed_tests = int(tests_match.group(1)) if tests_match else 0
    failed_tests = int(tests_match.group(2)) if tests_match else 0
    passed_asserts = int(asserts_match.group(1)) if asserts_match else 0
    failed_asserts = int(asserts_match.group(2)) if asserts_match else 0

    return (
        passed_tests,
        failed_tests,
        passed_asserts,
        failed_asserts,
        skip_markers,
        tests_match is not None and asserts_match is not None,
    )


# doctest's ConsoleReporter prints a "TEST CASE:  <name>" header
# (logTestStart) before a test case emits its first output, then a
# "<file>(<line>): ERROR: ..." (or "FATAL ERROR:") line for each failed
# assertion. WARNING:/MESSAGE: lines are not failures. See
# thirdparty/doctest/doctest.h (logTestStart / log_assert / failureString).
DOCTEST_TEST_CASE_NAME_RE = re.compile(r"^TEST CASE:\s+(?P<name>.+?)\s*$")
DOCTEST_FAILURE_LINE_RE = re.compile(r"(?:^|\s)(?:FATAL\s+)?ERROR:\s")

# doctest's logTestStart() emits, in this exact order, for EVERY test case:
#   separator_to_stream()   -> a run of '=' (thirdparty/doctest/doctest.h:6011-6015)
#   file_line_to_stream()   -> "<file>(<line>):"                        (:6052-6057)
#   optional "DESCRIPTION: " / "TEST SUITE: " lines                     (:6065-6068)
#   the name line                                                       (:6069-6071)
#
# The name line carries the "TEST CASE:  " prefix ONLY when the name does not
# start with "  Scenario:" -- i.e. BDD SCENARIO() cases print their name bare
# (doctest.h:6069-6071; SCENARIO expands to TEST_CASE("  Scenario: " name) at
# :2926). Anchoring on the separator + file/line header lets us capture that
# bare name too, so a scenario failure is attributed to the scenario instead of
# inheriting the previously named test case.
DOCTEST_SEPARATOR_RE = re.compile(r"^={20,}\s*$")
DOCTEST_FILE_LINE_HEADER_RE = re.compile(r"^\S.*\(\d+\):\s*$")
DOCTEST_TEST_START_META_RE = re.compile(r"^(?:DESCRIPTION|TEST SUITE):\s")


def _parse_failing_doctest_cases(output: str) -> list[str]:
    """Return the ordered, de-duplicated names of doctest test cases that emitted
    a failure (ERROR / FATAL ERROR).

    Attribution is stateful: the current case is taken from the most recent test
    start header, and a following ERROR line marks that case as failing.
    WARNING:/MESSAGE: lines are ignored (not failures).

    BDD SCENARIO cases print their name WITHOUT the "TEST CASE:  " prefix, so
    they are recognised positionally (separator -> "<file>(<line>):" -> name)
    rather than by that prefix. Before this was handled, a scenario failure that
    followed any prefixed failure in the same run silently inherited the earlier
    case's name: for a quarantined lane that meant a brand-new BDD regression was
    attributed to the approved case and the lane was tolerated. The separator
    also clears attribution, so a failure that still cannot be named leaves the
    result unparseable, which the callers fail closed on.
    """
    failing: list[str] = []
    seen: set[str] = set()
    current: str | None = None
    awaiting_file_line = False
    awaiting_name = False
    for line in output.splitlines():
        if DOCTEST_SEPARATOR_RE.match(line):
            # A new test is starting: drop the previous attribution so nothing
            # can be misfiled against it.
            current = None
            awaiting_file_line = True
            awaiting_name = False
            continue
        if awaiting_file_line and DOCTEST_FILE_LINE_HEADER_RE.match(line):
            awaiting_file_line = False
            awaiting_name = True
            continue
        name_match = DOCTEST_TEST_CASE_NAME_RE.match(line)
        if name_match:
            current = name_match.group("name")
            awaiting_file_line = False
            awaiting_name = False
            continue
        if awaiting_name:
            if DOCTEST_TEST_START_META_RE.match(line):
                continue
            if not line.strip():
                continue
            # Unprefixed name line: a BDD scenario. Keep it verbatim (minus
            # trailing whitespace) so manifest 'test_case' globs can match it.
            current = line.rstrip()
            awaiting_name = False
            continue
        if current is not None and DOCTEST_FAILURE_LINE_RE.search(line):
            if current not in seen:
                seen.add(current)
                failing.append(current)
    return failing


def _test_case_matches(pattern: str, case_name: str) -> bool:
    """Match a manifest 'test_case' wildcard against a full doctest test-case name.

    Mirrors doctest's own filter wildcards, which support ONLY '*' and '?'; every
    other character (including the '[' ']' of tag prefixes) is literal. Use
    '*...*' for a substring match, e.g. '*plays a clip*' or
    '[GaussianSplatting][Animation]*'.
    """
    regex = ["^"]
    for char in pattern:
        if char == "*":
            regex.append(".*")
        elif char == "?":
            regex.append(".")
        else:
            regex.append(re.escape(char))
    regex.append("$")
    return re.match("".join(regex), case_name, re.DOTALL) is not None


GuardRunner = Callable[[], tuple[bool, list[str]]]
GuardStep = Callable[[], int | None]
TestRun = tuple[str, list[str], bool]


@dataclass
class DoctestLaneStats:
    passed_tests: int
    passed_asserts: int
    skipped_markers: int
    has_executed_coverage: bool


@dataclass
class DoctestTotals:
    lanes: int = 0
    passed_tests: int = 0
    passed_asserts: int = 0
    skipped_markers: int = 0
    lanes_with_skip_markers: int = 0
    lanes_with_executed_coverage: int = 0
    lanes_unavailable: int = 0
    quarantined_failing: int = 0

    def add_lane_stats(self, stats: DoctestLaneStats) -> None:
        self.passed_tests += stats.passed_tests
        self.passed_asserts += stats.passed_asserts
        self.skipped_markers += stats.skipped_markers
        if stats.skipped_markers > 0:
            self.lanes_with_skip_markers += 1
        if stats.has_executed_coverage:
            self.lanes_with_executed_coverage += 1


# --------------------------------------------------------------------------
# Per-lane result ledger (#705, slice 1).
#
# It REPORTS; it gates nothing. See docs/architecture/adr-advisory-lane-ledger.md.
#
# On an advisory (strict=False) lane, a nonzero exit, a crash, zero coverage and
# self-skipped coverage are all TOLERATED - that lane's own outcome does not fail
# the run - and since doctest exits nonzero whenever a test fails, that covers the
# normal shape of an advisory failure. Two outcomes still fail the run for ANY
# lane regardless of strict: exit 0 while the doctest summary reports failures,
# and exit 0 with no doctest summary at all. "Advisory" is not an unconditional
# exemption from the exit code.
#
# The ledger records what happened per lane so #705/#519 can be armed later
# against a MEASURED value instead of a guessed one.
# --------------------------------------------------------------------------

# 2 (#822 round 10): `gating_failures` -> `fail_outcomes` plus the new
# `run_ending_outcomes` and `exit_code_reported` fields. A rename plus a version
# bump breaks a stale consumer loudly; a silently redefined field does not.
LANE_LEDGER_SCHEMA_VERSION = 2
# "Not known", as distinct from "zero". _parse_doctest_results() returns 0 for
# every count when no doctest summary was found, which makes a crash before any
# output indistinguishable from a lane that ran and passed nothing. The ledger
# never propagates that ambiguity.
#
# COUNTS ONLY. -1 is outside the range of every count, so it cannot collide with
# a real one. It IS inside the range of a process exit code: `subprocess` reports
# a POSIX SIGHUP termination as returncode -1, so "no return code was reported"
# and "the process was killed by signal 1" would be the same value in the
# exit_code field. That is what `exit_code_reported` exists to separate; see
# LaneResult.
LANE_COUNT_UNKNOWN = -1

LANE_OUTCOME_PASS = "PASS"
LANE_OUTCOME_FAIL = "FAIL"
LANE_OUTCOME_ADVISORY_FAIL = "ADVISORY-FAIL"
LANE_OUTCOME_ADVISORY_NO_COVERAGE = "ADVISORY-NO-COVERAGE"
LANE_OUTCOME_UNAVAILABLE = "UNAVAILABLE"
LANE_OUTCOME_QUARANTINE_TOLERATED = "QUARANTINE-TOLERATED"
LANE_OUTCOME_QUARANTINE_REJECTED = "QUARANTINE-REJECTED"
# Additive: a lane the runner never reached because an earlier strict lane
# aborted the run. Printed rather than omitted -- an absent lane reading as a
# passed lane is the very defect this ledger exists to remove.
LANE_OUTCOME_NOT_RUN = "NOT-RUN"
LANE_OUTCOMES: tuple[str, ...] = (
    LANE_OUTCOME_PASS,
    LANE_OUTCOME_FAIL,
    LANE_OUTCOME_ADVISORY_FAIL,
    LANE_OUTCOME_ADVISORY_NO_COVERAGE,
    LANE_OUTCOME_UNAVAILABLE,
    LANE_OUTCOME_QUARANTINE_TOLERATED,
    LANE_OUTCOME_QUARANTINE_REJECTED,
    LANE_OUTCOME_NOT_RUN,
)
# Says what is true of the ADVISORY RESULT, not what the whole run did. "CI
# exited 0" is a run-wide claim this record cannot make: the loop continues past
# an ADVISORY-FAIL, so a LATER strict lane can still fail the run while this
# report sits in the same file asserting success. See totals.run_ending_outcomes
# and lane_loop_exit_code for what the run actually did.
LANE_LEDGER_BASELINE_NOTE = (
    "Reporting only (#705 slice 1): this ledger observes lane outcomes and changes no "
    "exit code. An ADVISORY-RED lane failed, crashed or executed nothing, and that "
    "outcome did not itself fail the run; the run's exit code is decided elsewhere and "
    "may still be nonzero for another lane's reason."
)


@dataclass
class LaneResult:
    """What one lane did, as observed by _execute_lane()."""

    outcome: str
    exit_code: int = LANE_COUNT_UNKNOWN
    # "the subprocess reported a return code at all", NOT "the return code was
    # nonzero" (#822 round 10). Without it, `exit_code=-1` is ambiguous: it is
    # what the ledger records when no return code is available (a lane that was
    # never attempted, a `GodotRunResult` carrying returncode=None because the
    # process could not be launched, or a stub that returns a bare 3-tuple), and
    # it is ALSO the genuine value `subprocess` reports for a POSIX process
    # killed by SIGHUP. Those are different facts about a lane, and the field
    # whose whole purpose is telling a crash from a pass may not conflate them.
    # Read exit_code ONLY when exit_code_reported is true.
    exit_code_reported: bool = False
    # "doctest printed a summary", NOT "something ran". A summary of
    # `0 passed | 0 failed` is reported and executed nothing; the field is named
    # for the observation so the two are never confused. Whether anything
    # actually ran is zero_coverage's job.
    summary_reported: bool = False
    # None == not knowable (no doctest summary), never silently False.
    # Derived from passed + FAILED counts: see _execute_lane().
    zero_coverage: bool | None = None
    passed_tests: int = LANE_COUNT_UNKNOWN
    failed_tests: int = LANE_COUNT_UNKNOWN
    passed_assertions: int = LANE_COUNT_UNKNOWN
    failed_assertions: int = LANE_COUNT_UNKNOWN
    skipped_markers: int = LANE_COUNT_UNKNOWN
    detail: str = ""


@dataclass
class LaneLedgerRecord:
    lane: str
    strict: bool
    outcome: str = LANE_OUTCOME_NOT_RUN
    exit_code: int = LANE_COUNT_UNKNOWN
    # False on a seeded, never-attempted lane, which is exactly right: no
    # process ran, so no return code was reported. See LaneResult.
    exit_code_reported: bool = False
    summary_reported: bool = False
    zero_coverage: bool | None = None
    passed_tests: int = LANE_COUNT_UNKNOWN
    failed_tests: int = LANE_COUNT_UNKNOWN
    passed_assertions: int = LANE_COUNT_UNKNOWN
    failed_assertions: int = LANE_COUNT_UNKNOWN
    skipped_markers: int = LANE_COUNT_UNKNOWN
    # True when this lane's outcome is the one that ended the run.
    ended_run: bool = False
    detail: str = "lane was not attempted"

    @property
    def is_advisory_red(self) -> bool:
        return not self.strict and self.outcome in (
            LANE_OUTCOME_ADVISORY_FAIL,
            LANE_OUTCOME_ADVISORY_NO_COVERAGE,
        )

    @property
    def advisory_red_reason(self) -> str:
        """Why this advisory lane is red, named from what was actually observed.

        "A summary exists, therefore tests failed" is WRONG, and the repo
        already knows it is wrong: _classify_quarantined_lane_outcome() treats a
        clean all-pass summary followed by a nonzero exit as a teardown/harness
        failure, not a test failure. Reporting that shape as reason=failed would
        have this ledger announce "an advisory lane is failing tests" when
        nothing failed - a confidently wrong claim someone would then quote.

        So the reason is derived from the failed COUNTS, the executed coverage
        and the exit status:
        - failed                         the summary reports failed tests/assertions
        - no-coverage                    nothing executed, whatever the outcome or
                                         the exit code
        - nonzero-exit-no-test-failures  tests RAN and passed, yet the process still
                                         exited nonzero (teardown/harness crash);
                                         named for the observation, not a guessed cause
        - crashed                        no doctest summary at all

        The zero-coverage check has to come BEFORE the teardown fallback. A lane
        that exits nonzero after printing a summary in which nothing ran is a
        no-coverage lane; reporting it as `nonzero-exit-no-test-failures` because
        a summary merely exists tells a stdout consumer that tests ran and passed
        when none ran at all - and the aggregate is meanwhile counting it under
        advisory_zero_coverage, so the two would disagree in the same block.

        `is True`, not truthiness: `None` means "not knowable" (no summary) and
        must never be read as zero.
        """
        if self.failed_tests > 0 or self.failed_assertions > 0:
            return "failed"
        if self.outcome == LANE_OUTCOME_ADVISORY_NO_COVERAGE or self.zero_coverage is True:
            return "no-coverage"
        if self.summary_reported:
            # A summary was printed, it reported no failures, and something DID
            # run - yet the lane is red, so it exited nonzero after passing.
            return "nonzero-exit-no-test-failures"
        return "crashed"

    def to_json(self) -> dict:
        return {
            "lane": self.lane,
            "strict": self.strict,
            "outcome": self.outcome,
            "exit_code": self.exit_code,
            "exit_code_reported": self.exit_code_reported,
            "summary_reported": self.summary_reported,
            "zero_coverage": self.zero_coverage,
            "passed_tests": self.passed_tests,
            "failed_tests": self.failed_tests,
            "passed_assertions": self.passed_assertions,
            "failed_assertions": self.failed_assertions,
            "skipped_markers": self.skipped_markers,
            "ended_run": self.ended_run,
            "advisory_red": self.is_advisory_red,
            "detail": self.detail,
        }


@dataclass
class LaneLedgerTotals:
    lanes: int = 0
    strict_lanes: int = 0
    advisory_lanes: int = 0
    advisory_failures: int = 0
    advisory_zero_coverage: int = 0
    quarantine_tolerated: int = 0
    unavailable: int = 0
    quarantine_rejected: int = 0
    # Lanes whose outcome was FAIL. Deliberately NOT called strict_failures: an
    # ADVISORY lane also records FAIL when it exits 0 with a missing or failing
    # doctest summary, so counting FAIL outcomes as "strict failures" could print
    # `strict_lanes=0 strict_failures=1` and attribute an advisory harness
    # anomaly to a strict lane. The field is named for what it counts, and the
    # strict/advisory split below is derived from record.strict rather than from
    # the outcome.
    #
    # It was called `gating_failures` until #822 round 10, and that name was the
    # same over-claim one rung up: FAIL is not the only outcome that ends the
    # run. A strict tests-unavailable lane aborts with UNAVAILABLE, and a stale
    # or invalid quarantine aborts with QUARANTINE-REJECTED; both stop the loop
    # and decide the exit code while recording no FAIL at all, so a run that was
    # gated could publish `gating_failures=0`. Counting them under the old name
    # would have changed its meaning silently, which is how a consumer's parser
    # stays green while its numbers stop being true - so the field is renamed to
    # the narrow thing it measures and the broader question gets its own count
    # below.
    fail_outcomes: int = 0
    fail_outcomes_on_strict_lanes: int = 0
    fail_outcomes_on_advisory_lanes: int = 0
    # Lanes whose outcome ENDED the run, whatever that outcome was: FAIL,
    # UNAVAILABLE under strict tests-unavailable mode, or QUARANTINE-REJECTED.
    # Derived from the record's own `ended_run`, which the lane loop sets from
    # the value it actually broke on, so a future abort path is counted here the
    # day it is added rather than the day someone remembers to list its outcome.
    # This is the field to read for "did a lane stop this run", and the field
    # #705/#519's ratchet must consult.
    run_ending_outcomes: int = 0
    passed: int = 0
    not_run: int = 0

    def to_json(self) -> dict:
        return {
            "lanes": self.lanes,
            "strict_lanes": self.strict_lanes,
            "advisory_lanes": self.advisory_lanes,
            "advisory_failures": self.advisory_failures,
            "advisory_zero_coverage": self.advisory_zero_coverage,
            "quarantine_tolerated": self.quarantine_tolerated,
            "unavailable": self.unavailable,
            "quarantine_rejected": self.quarantine_rejected,
            "fail_outcomes": self.fail_outcomes,
            "fail_outcomes_on_strict_lanes": self.fail_outcomes_on_strict_lanes,
            "fail_outcomes_on_advisory_lanes": self.fail_outcomes_on_advisory_lanes,
            "run_ending_outcomes": self.run_ending_outcomes,
            "passed": self.passed,
            "not_run": self.not_run,
        }


def _self_contradictory_records(records: Iterable[LaneLedgerRecord]) -> list[str]:
    """The INVARIANT behind zero_coverage, checked as a property of every record.

    A record may not simultaneously report "no coverage executed" and a nonzero
    failed count. Failures are proof that coverage executed, so the two together
    mean the ledger is contradicting itself, and a reader (or a future ratchet)
    would have to guess which half to believe.

    This is stated as an invariant rather than as a test of one formula on
    purpose: the passed-count derivation that #822 round 4 removed satisfied
    every case-by-case test written for it while violating this property for the
    single most informative shape there is - a lane in which everything failed.

    `failed_tests > 0` is included, not just `failed_assertions > 0`. Under
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS a case is failed IFF it
    recorded a failed assertion, so a failed test with zero executed assertions
    should be impossible; if it ever appears, that model of doctest is wrong and
    surfacing it loudly is worth more than tolerating it quietly.
    """
    errors: list[str] = []
    for record in records:
        if record.zero_coverage is not True:
            continue
        if record.failed_tests > 0 or record.failed_assertions > 0:
            errors.append(
                f"lane '{record.lane}' reports zero_coverage=1 together with "
                f"failed_tests={record.failed_tests} / "
                f"failed_assertions={record.failed_assertions}. Executed failures are "
                f"proof that coverage ran; a record cannot say both 'nothing ran' and "
                f"'these ran and failed'."
            )
    return errors


def _format_zero_coverage(value: bool | None) -> str:
    if value is None:
        return str(LANE_COUNT_UNKNOWN)
    return "1" if value else "0"


def _format_lane_result_line(record: LaneLedgerRecord) -> str:
    """The stable per-lane grammar.

    The first eight fields are the contracted grammar; `exit_code`,
    `exit_code_reported`, `summary_reported` and `zero_coverage` are an additive
    suffix (a superset is not a weakening). They are what lets a reader tell a
    crash from a pass from an empty lane.

    `exit_code_reported` is not decoration and not redundant with `exit_code`:
    `exit_code=-1` means "unknown" for every count in this ledger, but -1 is a
    return code `subprocess` really does report (POSIX SIGHUP), so the two facts
    are only separable by carrying the availability alongside the value.

    `summary_reported` was called `executed` until #822 round 4. It never meant
    "something ran" - a `0 passed | 0 failed` summary is reported and executes
    nothing - and a field whose name overstates what it observes is the same
    defect as a count derived across a boundary the data does not cross. Renamed
    rather than redefined, deliberately: a rename breaks a parser loudly, a
    silent change of meaning does not.
    """
    return (
        f"[module-tests][lane-result] lane={record.lane} "
        f"strict={1 if record.strict else 0} "
        f"outcome={record.outcome} "
        f"passed_tests={record.passed_tests} "
        f"passed_assertions={record.passed_assertions} "
        f"failed_tests={record.failed_tests} "
        f"failed_assertions={record.failed_assertions} "
        f"skipped_markers={record.skipped_markers} "
        f"exit_code={record.exit_code} "
        f"exit_code_reported={1 if record.exit_code_reported else 0} "
        f"summary_reported={1 if record.summary_reported else 0} "
        f"zero_coverage={_format_zero_coverage(record.zero_coverage)}"
    )


class LaneLedger:
    """One record per configured lane, seeded up front.

    Seeding is the totality mechanism: a control-flow path that forgets to
    record cannot make a lane DISAPPEAR, only leave it visibly NOT-RUN, which
    the integrity check turns into a failure when the run was not aborted.
    """

    def __init__(self, lanes: Iterable[tuple[str, bool]]) -> None:
        self.records: list[LaneLedgerRecord] = [
            LaneLedgerRecord(lane=name, strict=strict) for name, strict in lanes
        ]
        self.integrity_errors: list[str] = []

    def record(self, index: int, result: LaneResult, *, ended_run: bool) -> None:
        if index < 0 or index >= len(self.records):
            self.integrity_errors.append(
                f"lane index {index} is outside the seeded ledger of "
                f"{len(self.records)} lane(s); a lane result was produced for a lane "
                f"that was never seeded."
            )
            return
        record = self.records[index]
        if record.outcome != LANE_OUTCOME_NOT_RUN:
            # Never overwrite: an overwrite is exactly how a FAIL becomes a PASS.
            self.integrity_errors.append(
                f"lane '{record.lane}' was recorded twice (kept "
                f"outcome={record.outcome}, refused outcome={result.outcome})."
            )
            return
        if result.outcome not in LANE_OUTCOMES:
            self.integrity_errors.append(
                f"lane '{record.lane}' produced unknown outcome {result.outcome!r}."
            )
        record.outcome = result.outcome
        record.exit_code = result.exit_code
        record.exit_code_reported = result.exit_code_reported
        record.summary_reported = result.summary_reported
        record.zero_coverage = result.zero_coverage
        record.passed_tests = result.passed_tests
        record.failed_tests = result.failed_tests
        record.passed_assertions = result.passed_assertions
        record.failed_assertions = result.failed_assertions
        record.skipped_markers = result.skipped_markers
        record.ended_run = ended_run
        record.detail = result.detail

    def totals(self) -> LaneLedgerTotals:
        totals = LaneLedgerTotals(lanes=len(self.records))
        for record in self.records:
            if record.strict:
                totals.strict_lanes += 1
            else:
                totals.advisory_lanes += 1
            # Zero coverage is a PROPERTY of the record, not one bucket in a
            # mutually-exclusive outcome chain: an advisory lane can execute
            # nothing while its outcome is ADVISORY-FAIL (a crash whose summary
            # reported 0|0), QUARANTINE-TOLERATED or FAIL. Counting it from the
            # ADVISORY-NO-COVERAGE outcome alone under-reported exactly the thing
            # this ledger exists to expose, and it is the field GS-705-2 is meant
            # to ratchet on. `is True` on purpose: None means "not knowable"
            # (no doctest summary) and must never be silently read as zero.
            if not record.strict and record.zero_coverage is True:
                totals.advisory_zero_coverage += 1
            # Also a PROPERTY, not an outcome bucket, and for the same reason:
            # three different outcomes end the run, and one more may be added.
            if record.ended_run:
                totals.run_ending_outcomes += 1
            if record.outcome == LANE_OUTCOME_PASS:
                totals.passed += 1
            elif record.outcome == LANE_OUTCOME_ADVISORY_FAIL:
                totals.advisory_failures += 1
            elif record.outcome == LANE_OUTCOME_ADVISORY_NO_COVERAGE:
                pass  # counted above, by property
            elif record.outcome == LANE_OUTCOME_QUARANTINE_TOLERATED:
                totals.quarantine_tolerated += 1
            elif record.outcome == LANE_OUTCOME_QUARANTINE_REJECTED:
                totals.quarantine_rejected += 1
            elif record.outcome == LANE_OUTCOME_UNAVAILABLE:
                totals.unavailable += 1
            elif record.outcome == LANE_OUTCOME_FAIL:
                totals.fail_outcomes += 1
                # Split by the lane's DECLARED strictness, never by the outcome:
                # an advisory lane can record FAIL (exit 0 with a missing or
                # failing summary), and charging that to a strict lane would make
                # the published aggregate quotably wrong.
                if record.strict:
                    totals.fail_outcomes_on_strict_lanes += 1
                else:
                    totals.fail_outcomes_on_advisory_lanes += 1
            elif record.outcome == LANE_OUTCOME_NOT_RUN:
                totals.not_run += 1
        return totals

    def check_integrity(self, *, attempted_lanes: int) -> list[str]:
        """Validate every lane the runner ATTEMPTED, abort or no abort.

        `attempted_lanes` is how many lanes the loop reached, so only the lanes
        AFTER an aborting one may legitimately be NOT-RUN. The previous version
        took an `aborted` flag and skipped the whole check whenever it was set,
        which meant a lane whose record went missing BEFORE the abort escaped
        validation entirely - the same "did not run reads as passed" hole this
        ledger exists to close, reopened for exactly the runs where a lane
        already failed.
        """
        errors = list(self.integrity_errors)
        for index, record in enumerate(self.records):
            if index < attempted_lanes and record.outcome == LANE_OUTCOME_NOT_RUN:
                errors.append(
                    f"lane '{record.lane}' was attempted but produced no ledger "
                    f"record (outcome stayed {LANE_OUTCOME_NOT_RUN}); an unrecorded "
                    f"lane reads as a passed lane."
                )
        errors.extend(_self_contradictory_records(self.records))
        return errors

    def print_block(self) -> LaneLedgerTotals:
        for record in self.records:
            print(_format_lane_result_line(record))
        totals = self.totals()
        # Printed UNCONDITIONALLY, including when advisory_failures=0, so that
        # absence of output can never be read as absence of failures.
        print(
            f"[module-tests][lane-ledger] lanes={totals.lanes} "
            f"strict_lanes={totals.strict_lanes} "
            f"advisory_lanes={totals.advisory_lanes} "
            f"advisory_failures={totals.advisory_failures} "
            f"advisory_zero_coverage={totals.advisory_zero_coverage} "
            f"quarantine_tolerated={totals.quarantine_tolerated} "
            f"unavailable={totals.unavailable} "
            f"quarantine_rejected={totals.quarantine_rejected} "
            f"fail_outcomes={totals.fail_outcomes} "
            f"run_ending_outcomes={totals.run_ending_outcomes} "
            f"passed={totals.passed} "
            f"not_run={totals.not_run}"
        )
        for record in self.records:
            if record.is_advisory_red:
                print(
                    f"[module-tests][lane-ledger] ADVISORY-RED lane={record.lane} "
                    f"reason={record.advisory_red_reason}"
                )
        return totals

    def to_json(self, totals: LaneLedgerTotals, *, lane_loop_exit_code: int) -> dict:
        return {
            "schema_version": LANE_LEDGER_SCHEMA_VERSION,
            "baseline_note": LANE_LEDGER_BASELINE_NOTE,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            # The value the LANE LOOP produced. Named narrowly on purpose, and
            # the reason is about the FILE, not about this run's ordering
            # (#822 round 10 corrected the earlier claim here, which had the
            # order backwards): the integrity check runs BEFORE the write, so a
            # ledger that fails it is never written, and a write that fails
            # leaves the previous report in place. In both cases the process
            # exits nonzero while this path holds an OLDER report whose
            # lane_loop_exit_code describes a different run - so a reader must
            # check generated_utc before treating any report as this run's.
            # When the write does succeed, main() returns the lane loop's value
            # unchanged. Calling the field "the run's exit code" would be the
            # same over-claim as asserting CI exited 0.
            "lane_loop_exit_code": lane_loop_exit_code,
            "lanes": [record.to_json() for record in self.records],
            "totals": totals.to_json(),
        }


def _lane_report_probe(path: Path, suffix: str) -> tuple[int, str]:
    """Create a uniquely-named sibling of `path` in the SAME directory.

    Sibling, not the destination itself, and same directory so the eventual
    os.replace() stays on one filesystem. The caller owns the returned fd/name.
    """
    return tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.", suffix=suffix)


def _write_lane_report(path: Path, payload: dict) -> list[str]:
    """Write the JSON ledger atomically. Returns integrity errors (never silent).

    Serialize first, write to a sibling temp file, then os.replace() onto the
    destination. Three failure modes are ruled out by that order:

    - a payload that will not serialize never touches the filesystem at all;
    - a write that dies half-way leaves the temp file, not a truncated report;
    - the destination is either the OLD report or the NEW one, never an empty or
      partial file.

    That matters more here than for an ordinary output file: this report IS the
    evidence, and the runner elsewhere treats an empty report as a red flag. A
    writer that can replace a good measurement with an empty file manufactures
    exactly that red flag while destroying the thing it was meant to preserve.

    os.replace (NOT os.rename) is used deliberately: it is atomic on Windows and
    overwrites an existing destination. The recorded non-atomic-rename hazard in
    this repo is Godot's DirAccess::rename (remove-then-move); that is engine
    code and does not apply to Python's os.replace.
    """
    try:
        text = json.dumps(payload, indent=2, sort_keys=False) + "\n"
    except (TypeError, ValueError) as exc:
        return [
            f"--lane-report payload for {path} is not serializable: "
            f"{type(exc).__name__}: {exc}. The previous report (if any) was left "
            f"untouched; refusing to report success for a run whose ledger was not "
            f"persisted."
        ]

    handle = None
    temp_name = None
    try:
        handle, temp_name = _lane_report_probe(path, ".tmp")
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            handle = None  # now owned by the context manager
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, str(path))
        temp_name = None
    except (OSError, ValueError) as exc:
        return [
            f"--lane-report could not be written to {path}: "
            f"{type(exc).__name__}: {exc}. Refusing to report success for a run whose "
            f"ledger was not persisted."
        ]
    finally:
        if handle is not None:
            with contextlib.suppress(OSError):
                os.close(handle)
        if temp_name is not None:
            # The replace never happened; do not leave scratch beside the report.
            with contextlib.suppress(OSError):
                os.unlink(temp_name)

    print(f"[module-tests][lane-ledger] wrote lane report to {path}")
    return []


def _preflight_lane_report_path(path: Path) -> list[str]:
    """Refuse an unwritable --lane-report path BEFORE spending a full lane run on it.

    NON-DESTRUCTIVE: the probe is a sibling temp file, never the destination. An
    earlier version opened the destination itself in "w" mode, which truncated
    the previous report at second zero -- so a run that was then interrupted, or
    that failed the run-list integrity check before the write, replaced the last
    valid measurement with an empty file. For a tool whose whole purpose is
    producing trustworthy evidence, destroying good evidence to check that we
    could have written some is the worst available failure mode.

    The end-of-run write still fails closed on its own; this only moves the
    diagnosis to second zero instead of after every lane has run.
    """
    # WHAT THIS RULES OUT - and, just as importantly, what it does not.
    #
    # The sibling probe answers "can I create a file NEXT TO this path", which is
    # a neighbouring question, not the one being asked. Two destination classes
    # are invalid ALREADY and the probe structurally cannot observe either, so
    # both used to surface only from os.replace() after every lane had run,
    # defeating the entire purpose of a preflight:
    #
    #   - the destination is an existing directory (or any other non-regular
    #     file): os.replace() onto it raises;
    #   - the destination is an existing file this process may not write (the
    #     Windows read-only attribute, a POSIX mode without write permission):
    #     os.replace() onto a read-only file raises PermissionError on Windows.
    #
    # DELIBERATELY NOT CLAIMED: that a clean preflight means the write will
    # succeed. A destination can be opened by another process without delete
    # sharing (normal on Windows), have its permissions changed, or lose its
    # parent directory between this check and the end-of-run write. Those are
    # unavoidable time-of-check/time-of-use races; no probe can rule them out,
    # and a preflight that implied otherwise would be making a false promise in
    # exactly the cases someone relies on it. This rejects what is KNOWABLY
    # invalid at second zero; the end-of-run write still fails closed for the
    # rest, and both halves are needed.
    if path.is_dir():
        return [
            f"--lane-report path is a directory: {path}. Name the JSON file to "
            f"write, not the directory to write it into."
        ]
    if path.exists() and not path.is_file():
        return [
            f"--lane-report path exists and is not a regular file: {path}. "
            f"os.replace() cannot replace it; name a regular file."
        ]
    # Rejected on EVERY platform, on purpose. On POSIX the rename could still
    # succeed through the parent directory's permissions, so this is stricter
    # than the OS: writing this run's evidence over a file the process is not
    # permitted to write is not something to do silently, and naming the wrong
    # destination is far cheaper to diagnose now than after every lane has run.
    if path.is_file() and not os.access(path, os.W_OK):
        return [
            f"--lane-report destination exists and is not writable: {path}. "
            f"Replacing it would fail only after every lane had run; remove the "
            f"read-only attribute or name a different file."
        ]
    handle = None
    temp_name = None
    try:
        handle, temp_name = _lane_report_probe(path, ".probe")
    except (OSError, ValueError) as exc:
        return [
            f"--lane-report path is not writable: {path} "
            f"({type(exc).__name__}: {exc})."
        ]
    finally:
        if handle is not None:
            with contextlib.suppress(OSError):
                os.close(handle)
        if temp_name is not None:
            with contextlib.suppress(OSError):
                os.unlink(temp_name)
    return []


def _declared_lane_names(run_gpu: bool) -> list[str]:
    """Every lane `_build_module_test_runs()` is required to produce, in order.

    Derived from the SAME two tables and the SAME `run_gpu` flag the builder
    reads, so the expectation cannot drift from the construction. `run_gpu` is
    required rather than defaulted: a default would let a caller silently ask
    for the weaker headless-only expectation on a GPU run, which is the hole
    this function exists to close.
    """
    names = [name for name, *_ in MODULE_TEST_FILTERS]
    if run_gpu:
        names.extend(name for name, *_ in REQUIRES_RD_TEST_FILTERS)
    return names


def _lane_runs_missing_from_module_filters(
    test_runs: Iterable[TestRun], run_gpu: bool
) -> list[str]:
    """Assert the built run list covers the lane declaration tables themselves.

    The ledger's totality guarantee is only worth as much as the list it is
    seeded from. A lane that disappears between the declaration table and the
    loop would produce a complete-looking ledger for an incomplete run, which is
    the same "absence reads as success" defect one level up.

    Both tables are checked. Until #822 round 11 only MODULE_TEST_FILTERS was,
    so on a GPU run - the one configuration where REQUIRES_RD_TEST_FILTERS is
    supposed to execute - deleting or breaking the `if run_gpu:` append in
    `_build_module_test_runs()` left this check GREEN over a run that had
    silently stopped attempting the GPU lanes. `run_gpu` is exactly what decides
    whether those lanes are appended, and it is known at the call site, so it is
    what decides whether they are required here.
    """
    run_names = {name for name, _args, _strict in test_runs}
    missing = [name for name in _declared_lane_names(run_gpu) if name not in run_names]
    if not missing:
        return []
    table = (
        "MODULE_TEST_FILTERS/REQUIRES_RD_TEST_FILTERS"
        if run_gpu
        else "MODULE_TEST_FILTERS"
    )
    return [
        f"lane(s) declared in {table} are absent from the built run list: "
        + ", ".join(missing)
    ]


def _print_module_messages(messages: Iterable[str]) -> None:
    for message in messages:
        prefix = "[module-tests] " if not message.startswith("[module-tests]") else ""
        print(f"{prefix}{message}")


def _print_output_if_present(output: str) -> None:
    stripped_output = output.strip()
    if stripped_output:
        print(stripped_output)


def _run_message_guard(
    runner: GuardRunner,
    failure_summary: str,
    success_summary: str | None = None,
    *,
    success_always: bool = False,
    success_when_empty: bool = False,
) -> int | None:
    ok, messages = runner()
    _print_module_messages(messages)
    if not ok:
        print(f"[module-tests] {failure_summary}")
        return 1
    if success_summary and (success_always or (success_when_empty and not messages)):
        print(f"[module-tests] {success_summary}")
    return None


def _run_history_artifact_guard_step(mode: str) -> int | None:
    ok, exit_code, messages = _run_history_artifact_guard(mode)
    _print_module_messages(messages)
    if not ok:
        print("[module-tests] History artifact guard failed.")
        return exit_code
    return None


def _run_render_guard_step(base_ref: str | None) -> int | None:
    guard_ok, guard_messages = _check_render_path_guards(base_ref)
    _print_module_messages(guard_messages)
    if not guard_ok:
        print("[module-tests] Renderer guard checks failed.")
        return 1
    print("[module-tests] Renderer guard checks passed.")
    return None


def _resolve_stringname_orphan_threshold() -> int:
    raw = os.environ.get(STRINGNAME_ORPHAN_GUARD_THRESHOLD_ENV, "").strip()
    if not raw:
        return STRINGNAME_ORPHAN_GUARD_DEFAULT_THRESHOLD
    try:
        return max(0, int(raw))
    except ValueError:
        return STRINGNAME_ORPHAN_GUARD_DEFAULT_THRESHOLD


def _count_stringname_orphans(output: str) -> tuple[int, list[str]]:
    matches = STRINGNAME_ORPHAN_LINE_RE.findall(output)
    return len(matches), matches


def _emit_stringname_orphan_lifetime_line(
    passed: bool, delta: int, threshold: int, fail_reason: str
) -> None:
    payload = {
        "scenario": "stringname_orphans",
        "passed": passed,
        "stringname_orphan_delta": delta,
        "threshold_orphans": threshold,
        "fail_reason": fail_reason,
    }
    # Stable ordering so PR 7's manifest parser can diff the line verbatim.
    print("[GS-LIFETIME] " + json.dumps(payload, sort_keys=False, separators=(",", ":")))


def _resolve_stringname_orphan_project() -> Path:
    raw = os.environ.get(STRINGNAME_ORPHAN_GUARD_PROJECT_ENV, "").strip()
    if raw:
        candidate = Path(raw)
        if not candidate.is_absolute():
            candidate = (ROOT / candidate).resolve()
        return candidate
    return STRINGNAME_ORPHAN_GUARD_DEFAULT_PROJECT


def _run_stringname_orphan_guard_step(godot: str) -> int | None:
    """Subprocess CI guard for PR 6 of work package #352.

    Spawns the Godot binary headless+verbose against a representative
    project (default tests/examples/godot/test_project) with --quit so
    that StringName::cleanup() runs at exit, then counts the
    ``Orphan StringName:`` lines it emits. Emits one ``[GS-LIFETIME]``
    JSON line shaped to match the stringname_orphans scenario PR 7's
    manifest gate will consume.

    The guard is informational for now: it passes when the orphan count
    is at or below threshold. PR 7 tightens this to ``delta vs baseline
    must be 0``. Missing binary path or missing probe project is
    non-fatal (the guard short-circuits and emits a passing line with a
    sentinel delta of -1 so guard-only mode still completes in
    environments without a built binary or sample project).
    """
    threshold = _resolve_stringname_orphan_threshold()
    project = _resolve_stringname_orphan_project()
    normalized_binary = _normalize_process_arg(godot)
    if not normalized_binary:
        print(
            "[module-tests] StringName orphan guard skipped: no Godot binary "
            f"specified (set --godot-binary or {STRINGNAME_ORPHAN_GUARD_BINARY_ENV})."
        )
        _emit_stringname_orphan_lifetime_line(
            passed=True,
            delta=-1,
            threshold=threshold,
            fail_reason="binary_unspecified",
        )
        return None

    binary_path = Path(normalized_binary)
    # Accept either a direct file path or a command-style name that is
    # resolvable via PATH (e.g. the default `godot`). shutil.which honors
    # PATHEXT on Windows so `godot` can resolve to `godot.exe`/`.bat`.
    path_resolved_binary = shutil.which(normalized_binary)
    if not binary_path.is_file() and path_resolved_binary is None:
        print(
            f"[module-tests] StringName orphan guard skipped: binary not found "
            f"at '{normalized_binary}' and not resolvable via PATH."
        )
        _emit_stringname_orphan_lifetime_line(
            passed=True,
            delta=-1,
            threshold=threshold,
            fail_reason="binary_missing",
        )
        return None
    # Prefer the literal path when it exists; otherwise use the PATH-resolved
    # location so subprocess.run does not need to re-resolve via PATHEXT.
    invocation_binary = (
        normalized_binary if binary_path.is_file() else path_resolved_binary
    )

    if not project.is_dir() or not (project / "project.godot").is_file():
        print(
            f"[module-tests] StringName orphan guard skipped: probe project "
            f"not found at '{project}'."
        )
        _emit_stringname_orphan_lifetime_line(
            passed=True,
            delta=-1,
            threshold=threshold,
            fail_reason="project_missing",
        )
        return None

    command = [
        invocation_binary,
        "--path",
        str(project),
        "--headless",
        "--verbose",
        "--quit",
    ]
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=STRINGNAME_ORPHAN_GUARD_TIMEOUT_SEC,
        )
    except (FileNotFoundError, PermissionError, OSError, subprocess.TimeoutExpired) as exc:
        print(
            f"[module-tests] StringName orphan guard failed to run: "
            f"{type(exc).__name__}: {exc}"
        )
        _emit_stringname_orphan_lifetime_line(
            passed=False,
            delta=-1,
            threshold=threshold,
            fail_reason=f"subprocess_error:{type(exc).__name__}",
        )
        return 1

    combined_output = (result.stdout or "") + (result.stderr or "")
    orphan_count, orphan_names = _count_stringname_orphans(combined_output)

    print(
        f"[module-tests] StringName orphan guard: count={orphan_count} "
        f"threshold={threshold} project={project.name} exit={result.returncode}."
    )
    if orphan_count > 0:
        sample = ", ".join(sorted(set(orphan_names))[:10])
        print(f"[module-tests]   orphan StringNames (deduped, first 10): {sample}")

    # A crashed/failed Godot invocation must not be reported as a passing
    # guard just because the orphan parse stayed under threshold (the
    # teardown path we are validating may not even have executed). Emit a
    # failing [GS-LIFETIME] line so PR 7's manifest parser consumes it
    # uniformly as a failure.
    if result.returncode != 0:
        fail_reason = f"probe_exit:{result.returncode}"
        _emit_stringname_orphan_lifetime_line(
            passed=False,
            delta=orphan_count,
            threshold=threshold,
            fail_reason=fail_reason,
        )
        print(
            f"[module-tests] StringName orphan guard FAILED: probe exited with "
            f"code {result.returncode}."
        )
        return 1

    if orphan_count > threshold:
        fail_reason = (
            f"orphan_count_above_threshold:{orphan_count}>{threshold}"
        )
        _emit_stringname_orphan_lifetime_line(
            passed=False,
            delta=orphan_count,
            threshold=threshold,
            fail_reason=fail_reason,
        )
        print(
            f"[module-tests] StringName orphan guard FAILED: "
            f"{orphan_count} orphans exceeds threshold {threshold}."
        )
        return 1

    _emit_stringname_orphan_lifetime_line(
        passed=True,
        delta=orphan_count,
        threshold=threshold,
        fail_reason="",
    )
    return None


def _run_static_guard_step() -> int | None:
    guards_ok, guard_failures = _run_static_format_guards()
    if not guards_ok:
        print("[module-tests] Static format safety guard(s) failed:")
        for failure in guard_failures:
            print(f"[module-tests]  - {failure}")
        return 1
    print(f"[module-tests] Static format safety guards passed ({len(STATIC_FORMAT_GUARDS)} checks).")
    return None


def _first_guard_failure(steps: Iterable[GuardStep]) -> int | None:
    for step in steps:
        exit_code = step()
        if exit_code is not None:
            return exit_code
    return None


def _run_required_message_guards() -> int | None:
    required_guards: tuple[tuple[GuardRunner, str, str], ...] = (
        (_run_tracked_backup_guard, "Tracked backup-file guard failed.", "Tracked backup-file guard passed."),
        (_run_tracked_artifact_guard, "Tracked artifact hygiene guard failed.", "Tracked artifact hygiene guard passed."),
    )
    for runner, failure_summary, success_summary in required_guards:
        exit_code = _run_message_guard(
            runner,
            failure_summary,
            success_summary,
            success_always=True,
        )
        if exit_code is not None:
            return exit_code
    return None


def _run_optional_message_guards(cli_args: argparse.Namespace) -> int | None:
    """Every guard `--guard-only` runs, one tuple entry each.

    Deleting an entry here is invisible to every guard's own unit test: the
    checker, its runner function and its unit test all survive and all stay
    green, and CI simply stops calling it (measured on #852 -- dropping the
    reject-telemetry entry left 137 unit tests passing at exit 0).

    `GuardRunnerWiringTests` in tests/ci/test_run_module_tests_lane_ledger.py
    closes that for the whole table rather than one entry at a time: it drives
    `_run_ci_guard_steps()` with only the leaf executors stubbed and asserts that
    every zero-argument `_run_*_guard` the module defines was invoked. All 31
    entries are mutation-proven RED individually.
    """
    optional_message_guards: list[tuple[bool, GuardRunner, str, str]] = [
        (
            not cli_args.skip_build_metadata_guard,
            _run_build_metadata_guard,
            "Build metadata guard failed.",
            "Build metadata guard passed.",
        ),
        (
            True,
            _run_shader_dependency_guard,
            "Shader dependency guard failed.",
            "Shader dependency guard passed.",
        ),
        (
            True,
            _run_project_settings_manifest_guard,
            "ProjectSettings manifest guard failed.",
            "ProjectSettings manifest guard passed.",
        ),
        (
            True,
            _run_gaussian_layout_guard,
            "Gaussian layout guard failed.",
            "Gaussian layout guard passed.",
        ),
        (
            True,
            _run_cull_signature_parity_guard,
            "Cull-signature parity guard failed.",
            "Cull-signature parity guard passed.",
        ),
        (
            True,
            _run_metric_reset_parity_guard,
            "Metric-reset parity guard failed.",
            "Metric-reset parity guard passed.",
        ),
        (
            True,
            _run_reject_telemetry_parity_guard,
            "Reject-telemetry parity guard failed.",
            "Reject-telemetry parity guard passed.",
        ),
        (
            True,
            _run_doc_classes_guard,
            "doc_classes completeness guard failed.",
            "doc_classes completeness guard passed.",
        ),
        (
            True,
            _run_test_linkage_guard,
            "Test linkage guard failed.",
            "Test linkage guard passed.",
        ),
        (
            True,
            _run_require_null_deref_guard,
            "REQUIRE null-deref guard failed.",
            "REQUIRE null-deref guard passed.",
        ),
        (
            True,
            _run_environment_skip_marker_guard,
            "Environment-skip marker guard failed.",
            "Environment-skip marker guard passed.",
        ),
        (
            True,
            _run_unchecked_resize_guard,
            "Unchecked-resize guard failed.",
            "Unchecked-resize guard passed.",
        ),
        (
            True,
            _run_test_lane_coverage_guard,
            "Test lane coverage guard failed.",
            "Test lane coverage guard passed.",
        ),
        (
            True,
            _run_renderer_contract_boundary_guard,
            "Renderer-contract boundary guard failed.",
            "Renderer-contract boundary guard passed.",
        ),
        (
            True,
            _run_device_submission_contract_guard,
            "Device-submission contract guard failed.",
            "Device-submission contract guard passed.",
        ),
        (
            True,
            _run_editor_node_pointer_lifetime_guard,
            "Editor node-pointer lifetime guard failed.",
            "Editor node-pointer lifetime guard passed.",
        ),
        (
            True,
            _run_renderer_release_gate_guard,
            "Renderer release gate guard failed.",
            "Renderer release gate guard passed.",
        ),
        (
            True,
            _run_baseline_qa_require_flag_guard,
            "Baseline QA require-flag guard failed.",
            "Baseline QA require-flag guard passed.",
        ),
        (
            True,
            _run_quarantine_manifest_guard,
            "Quarantine manifest guard failed.",
            "Quarantine manifest guard passed.",
        ),
        (
            True,
            _run_lane_ledger_guard,
            "Lane-ledger guard failed.",
            "Lane-ledger guard passed.",
        ),
        (
            True,
            _run_benchmark_asset_guard,
            "Benchmark asset path guard failed.",
            "Benchmark asset path guard passed.",
        ),
        (
            True,
            _run_gpu_harness_deferred_contract_guard,
            "GPU harness deferred contract guard failed.",
            "GPU harness deferred contract guard passed.",
        ),
        (
            True,
            _run_gpu_sorting_order_coverage_guard,
            "GPU sorting order-coverage guard failed.",
            "GPU sorting order-coverage guard passed.",
        ),
        (
            True,
            _run_benchmark_fixture_contract_guard,
            "Benchmark fixture contract guard failed.",
            "Benchmark fixture contract guard passed.",
        ),
        (
            True,
            _run_runtime_validation_contract_guard,
            "Runtime validation contract guard failed.",
            "Runtime validation contract guard passed.",
        ),
        (
            True,
            _run_export_template_naming_guard,
            "Export template naming guard failed.",
            "Export template naming guard passed.",
        ),
        (
            True,
            _run_export_smoke_preset_state_guard,
            "Export smoke preset state guard failed.",
            "Export smoke preset state guard passed.",
        ),
        (
            True,
            _run_release_builds_path_filter_guard,
            "Release builds path filter guard failed.",
            "Release builds path filter guard passed.",
        ),
        (
            True,
            _run_release_builds_runner_trust_guard,
            "Release builds runner trust guard failed.",
            "Release builds runner trust guard passed.",
        ),
    ]
    for enabled, runner, failure_summary, success_summary in optional_message_guards:
        if not enabled:
            continue
        exit_code = _run_message_guard(
            runner,
            failure_summary,
            success_summary,
            success_when_empty=True,
        )
        if exit_code is not None:
            return exit_code
    return None


def _run_configured_render_and_static_guards(cli_args: argparse.Namespace) -> int | None:
    if not cli_args.skip_render_guards:
        base_ref = _resolve_guard_base_ref(cli_args.base_ref)
        exit_code = _run_render_guard_step(base_ref)
        if exit_code is not None:
            return exit_code

    if not cli_args.skip_static_guards:
        exit_code = _run_static_guard_step()
        if exit_code is not None:
            return exit_code

    return None


def _run_ci_guard_steps(cli_args: argparse.Namespace) -> int | None:
    # Publish --base-ref so the env-skip guard subprocess ratchets against the
    # SAME base the render-path guard diffs against, instead of silently
    # resolving its own (which ends at origin/master).
    global _GUARD_BASE_REF_OVERRIDE
    _GUARD_BASE_REF_OVERRIDE = getattr(cli_args, "base_ref", None)

    history_guard_mode, history_guard_mode_warning = _resolve_history_artifact_guard_mode()
    if history_guard_mode_warning:
        print(f"[module-tests] {history_guard_mode_warning}")

    return _first_guard_failure(
        (
            _run_required_message_guards,
            lambda: _run_history_artifact_guard_step(history_guard_mode),
            lambda: _run_optional_message_guards(cli_args),
            lambda: _run_configured_render_and_static_guards(cli_args),
            lambda: _run_stringname_orphan_guard_step(cli_args.godot_binary),
        )
    )


def _build_module_test_runs(run_gpu: bool) -> list[TestRun]:
    test_runs = []
    for name, test_filters, exclude_filters, strict in MODULE_TEST_FILTERS:
        run_args = _build_doctest_run_args(test_filters, exclude_filters)
        test_runs.append((name, run_args, strict))

    if run_gpu:
        for name, test_filters, exclude_filters, strict in REQUIRES_RD_TEST_FILTERS:
            run_args = _build_doctest_run_args(test_filters, exclude_filters)
            test_runs.append((name, run_args, strict))
    return test_runs


def _report_unavailable_lane(
    name: str,
    output: str,
    tests_unavailable_mode: str,
    allow_tests_unavailable: bool,
) -> bool:
    if tests_unavailable_mode == "strict" and not allow_tests_unavailable:
        print(
            f"[module-tests] '{name}' unavailable: binary does not support --test "
            "(build with tests=yes)."
        )
        _print_output_if_present(output)
        print(
            f"[module-tests] Failing because strict mode is active. "
            f"Use --allow-tests-unavailable or {ALLOW_TESTS_UNAVAILABLE_ENV}=1 "
            "for an explicit local opt-out."
        )
        return False

    print(f"[module-tests] Skipping '{name}' (tests not enabled in binary).")
    _print_output_if_present(output)
    return True


def _report_failed_lane(name: str, strict: bool, output: str) -> bool:
    if not strict:
        print(
            f"[module-tests] '{name}' crashed or failed "
            "(advisory lane, continuing)."
        )
        _print_output_if_present(output)
        return True

    print(f"[module-tests] '{name}' failed.")
    _print_output_if_present(output)
    return False


def _report_lane_failure(name: str, reason: str, output: str) -> None:
    print(f"[module-tests] '{name}' failed: {reason}")
    _print_output_if_present(output)


def _report_advisory_no_coverage(
    name: str,
    output: str,
    passed_tests: int,
    passed_asserts: int,
    skipped_markers: int,
) -> None:
    if skipped_markers > 0:
        print(
            f"[module-tests] '{name}' executed only skipped doctest coverage "
            f"(passed_tests={passed_tests}, passed_assertions={passed_asserts}, "
            f"skipped_markers={skipped_markers}); treating this lane as advisory and continuing."
        )
    else:
        print(
            f"[module-tests] '{name}' has no executed coverage "
            f"(passed_tests={passed_tests}, passed_assertions={passed_asserts}); "
            "treating this lane as advisory and continuing."
        )
    _print_output_if_present(output)


def _display_path(path: Path) -> str:
    """Repo-relative when possible, absolute otherwise.

    `Path.relative_to` RAISES for a path outside the tree, so using it directly
    in an error message turns a clear diagnostic into a ValueError traceback --
    including in the unit tests that redirect the baseline at a tempdir.
    """
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _environment_skip_lane_allowance() -> dict[str, int]:
    """Per-lane tolerance for environment-skip markers (#595).

    The tolerance exists only because repairing the detector turned a provably
    inert count into a real one. Converting every pre-existing environment skip
    into a lane failure in the same change is explicitly out of scope for #595
    (that is what slices GS-595-B/C do, one reviewed step at a time), so the
    lanes that already skip need a frozen, derived allowance.

    It lives beside the static inventory in environment_skip_baseline.json under
    `runtime_lane_allowance`, must be MEASURED from a real lane run rather than
    guessed, and may only ever shrink. A lane with no entry has an allowance of
    zero, which is the strictest possible reading: nothing here can loosen a
    lane that is clean today.

    EVERY entry must carry `allowed`, `owner`, `reason`, `issue_url` and
    `expires_utc`, mirroring quarantine_manifest.json's required fields and the
    deferred_requires_gpu_waivers. A bare {"lane": N} map is a silencer with no
    expiry and no one accountable for it, and it would rot exactly the way the
    free-form prose baseline did. An EXPIRED entry is dropped to zero rather than
    honoured, so an allowance that nobody renews tightens by itself instead of
    becoming permanent.

    Fail-closed: an unreadable or malformed file raises rather than silently
    yielding an empty (or, worse, permissive) mapping.
    """
    path = ENVIRONMENT_SKIP_BASELINE_PATH
    label = _display_path(path)
    if not path.is_file():
        raise RuntimeError(
            f"Environment-skip baseline missing: {label}. Refusing to "
            f"evaluate the skip-marker policy against an absent baseline."
        )
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError, UnicodeDecodeError) as exc:
        raise RuntimeError(f"{label}: not readable JSON ({exc}).") from exc
    if not isinstance(data, dict):
        raise RuntimeError(f"{label}: must be a JSON object.")
    allowance = data.get("runtime_lane_allowance", {})
    if not isinstance(allowance, dict):
        raise RuntimeError(f"{label}: 'runtime_lane_allowance' must be an object.")

    known_lanes = {lane for lane, *_ in MODULE_TEST_FILTERS}
    now = datetime.now(timezone.utc)
    out: dict[str, int] = {}
    for lane, entry in allowance.items():
        where = f"{label}: runtime_lane_allowance['{lane}']"
        if not isinstance(entry, dict):
            raise RuntimeError(
                f"{where} must be an object carrying "
                f"{{allowed, owner, reason, issue_url, expires_utc}}, got {entry!r}. "
                f"A bare number is a silencer with no owner and no expiry."
            )
        missing = [
            field
            for field in ("allowed", "owner", "reason", "issue_url", "expires_utc")
            if field not in entry
        ]
        if missing:
            raise RuntimeError(f"{where} is missing required field(s): {', '.join(missing)}.")
        allowed = entry["allowed"]
        if not isinstance(allowed, int) or isinstance(allowed, bool) or allowed < 0:
            raise RuntimeError(f"{where}.allowed must be a non-negative integer, got {allowed!r}.")
        for field in ("owner", "reason", "issue_url"):
            if not isinstance(entry[field], str) or not entry[field].strip():
                raise RuntimeError(f"{where}.{field} must be a non-empty string.")
        try:
            expires = datetime.fromisoformat(str(entry["expires_utc"]))
        except ValueError as exc:
            raise RuntimeError(f"{where}.expires_utc is not an ISO-8601 timestamp ({exc}).") from exc
        if expires.tzinfo is None:
            expires = expires.replace(tzinfo=timezone.utc)
        if lane not in known_lanes:
            raise RuntimeError(
                f"{where} names a lane that is not in MODULE_TEST_FILTERS. A stale lane name "
                f"is an allowance that silently applies to nothing."
            )
        if expires <= now:
            print(
                f"[module-tests] environment-skip allowance for '{lane}' EXPIRED at "
                f"{entry['expires_utc']} (owner {entry['owner']}, {entry['issue_url']}); "
                f"treating it as 0. Renew it deliberately or fix the skips."
            )
            out[str(lane)] = 0
            continue
        out[str(lane)] = allowed
    return out


def _enforce_skipped_marker_policy(name: str, strict: bool, output: str, skipped_markers: int) -> bool:
    if skipped_markers <= 0:
        return True

    print(f"[module-tests] '{name}' reported {skipped_markers} skipped doctest marker(s).")
    if strict and _is_ci():
        try:
            allowance = _environment_skip_lane_allowance().get(name, 0)
        except RuntimeError as exc:
            # Fail CLOSED, but as a lane failure rather than an uncaught
            # traceback: a stack dump reads as "the runner crashed" and invites
            # someone to rerun, where a lane failure reads as "fix the baseline".
            _report_lane_failure(
                name, f"environment-skip allowance is unusable: {exc}", output
            )
            return False
        if skipped_markers > allowance:
            _report_lane_failure(
                name,
                f"skipped doctest coverage is not allowed in CI "
                f"({skipped_markers} marker(s) > allowance {allowance}). Lower the count in "
                f"the source, or record a MEASURED allowance under 'runtime_lane_allowance' "
                f"in tests/ci/environment_skip_baseline.json with an owner (#595).",
                output,
            )
            return False
        print(
            f"[module-tests] '{name}' is within its frozen environment-skip allowance "
            f"({skipped_markers} <= {allowance}); see tests/ci/environment_skip_baseline.json (#595)."
        )
    return True


def _handle_no_executed_coverage(
    name: str,
    strict: bool,
    output: str,
    passed_tests: int,
    passed_asserts: int,
    skipped_markers: int,
) -> DoctestLaneStats | None:
    if not strict:
        _report_advisory_no_coverage(name, output, passed_tests, passed_asserts, skipped_markers)
        return DoctestLaneStats(passed_tests, passed_asserts, skipped_markers, False)

    reason = (
        f"no executed coverage (passed_tests={passed_tests}, passed_assertions={passed_asserts}, "
        f"skipped_markers={skipped_markers})."
    )
    _report_lane_failure(name, reason, output)
    return None


def _validate_successful_lane(name: str, strict: bool, output: str) -> DoctestLaneStats | None:
    (
        passed_tests,
        failed_tests,
        passed_asserts,
        failed_asserts,
        skipped_markers,
        summary_found,
    ) = _parse_doctest_results(output)

    if not summary_found:
        _report_lane_failure(name, "missing doctest summary in output.", output)
        return None

    if failed_tests > 0 or failed_asserts > 0:
        reason = (
            f"{failed_tests} failed test(s), {failed_asserts} failed assertion(s), "
            f"{skipped_markers} skipped doctest marker(s)."
        )
        _report_lane_failure(name, reason, output)
        return None

    if not _enforce_skipped_marker_policy(name, strict, output, skipped_markers):
        return None

    has_executed_coverage = passed_tests > 0 and passed_asserts > 0
    if not has_executed_coverage:
        return _handle_no_executed_coverage(
            name,
            strict,
            output,
            passed_tests,
            passed_asserts,
            skipped_markers,
        )

    return DoctestLaneStats(passed_tests, passed_asserts, skipped_markers, True)


def _print_lane_passed(name: str, stats: DoctestLaneStats) -> None:
    skipped_suffix = ""
    if stats.skipped_markers > 0:
        skipped_suffix = f", {stats.skipped_markers} skipped doctest marker(s)"
    print(
        f"[module-tests] '{name}' passed: {stats.passed_tests} test(s), "
        f"{stats.passed_asserts} assertion(s){skipped_suffix}."
    )


def _print_doctest_totals(totals: DoctestTotals) -> None:
    print(
        f"[module-tests] Gaussian splatting module tests passed "
        f"(lanes={totals.lanes}, lanes_with_coverage={totals.lanes_with_executed_coverage}, "
        f"lanes_with_skips={totals.lanes_with_skip_markers}, lanes_unavailable={totals.lanes_unavailable}, "
        f"quarantined_failing={totals.quarantined_failing}, "
        f"skipped_markers={totals.skipped_markers}, passed_tests={totals.passed_tests}, "
        f"passed_assertions={totals.passed_asserts})."
    )


def _classify_quarantined_lane_outcome(ok: bool, output: str) -> str:
    """Classify a quarantined lane run as one of four outcomes.

    A quarantined entry is only honored when the lane actually RAN AND FAILED a
    real test. Tolerate ONLY a genuine failure signal: a nonzero/crash exit, or
    a doctest summary reporting failed tests or assertions. Everything else on an
    exit-0 run means the quarantine is stale or misconfigured and must fail.

    The doctest SUMMARY is inspected first; the exit code only decides the
    no-summary cases. The only outcomes that are ever tolerated downstream are
    'expected_fail' (a summary whose failures all match test_case) and a genuine
    crash (no summary + nonzero exit); everything else fails.

    - expected_fail: a summary with failed_tests>0 or failed_asserts>0, OR no
      summary with a nonzero/crash exit. The known, quarantined failure -
      tolerated (after case matching) or a whole-lane crash.
    - clean_pass: a summary showing real executed coverage and zero failures,
      REGARDLESS of exit code. The tests pass, so the quarantine is stale; a
      nonzero exit on top means an additional teardown/harness crash. Either way
      it must fail (anti-rot).
    - coverage_lost: a summary but zero executed coverage (the lane's filter no
      longer matches any test). Stale/misconfigured - fail.
    - harness_error: exit 0 with no doctest summary at all. Nothing ran to fail -
      fail.
    """
    (
        passed_tests,
        failed_tests,
        passed_asserts,
        failed_asserts,
        _skipped_markers,
        summary_found,
    ) = _parse_doctest_results(output)

    # Inspect the summary FIRST: a doctest summary is authoritative about what
    # ran, and the exit code must not override it (a clean all-pass summary that
    # then exits nonzero is a stale quarantine plus a teardown crash, not a
    # tolerable failure).
    if summary_found:
        if failed_tests > 0 or failed_asserts > 0:
            return "expected_fail"
        if passed_tests > 0 and passed_asserts > 0:
            return "clean_pass"
        return "coverage_lost"

    # No summary at all: the exit code is the only signal.
    if not ok:
        # Genuine crash before any summary -> tolerable whole-lane failure.
        return "expected_fail"
    return "harness_error"


def _dedupe_preserving_order(values: Iterable[str]) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def _quarantine_issue_refs(entries: list[dict]) -> str:
    """Comma-joined, de-duplicated issue URLs across a lane's entries."""
    refs = _dedupe_preserving_order(
        (entry.get("issue_url") or "unknown-issue") for entry in entries
    )
    return ", ".join(refs) if refs else "unknown-issue"


def _tolerate_quarantined_lane(
    name: str,
    strict: bool,
    issue: str,
    output: str,
    totals: DoctestTotals,
    skipped_markers: int,
    message: str,
) -> int | None:
    """Tolerate a quarantined lane's known failure, or fail if it also introduced
    NEW skipped coverage.

    A quarantine tolerates ONLY its exact known failure - never a NEW skipped
    test. This mirrors _enforce_skipped_marker_policy for non-quarantined strict
    lanes: in strict CI, any skipped doctest marker fails the lane. When the lane
    IS tolerated, the skip counts are still folded into the totals so
    lanes_with_skips / skipped_markers reflect reality instead of a silent 0.
    """
    if skipped_markers > 0 and strict and _is_ci():
        print(
            f"[module-tests][QUARANTINE-UNEXPECTED] '{name}' is quarantined but "
            f"introduced newly skipped coverage ({skipped_markers} skipped marker(s)) "
            f"in strict CI - failing (issue {issue})."
        )
        _print_output_if_present(output)
        return 1

    print(message)
    _print_output_if_present(output)
    totals.quarantined_failing += 1
    # Reflect skipped coverage even when the lane is tolerated, so the totals do
    # not hide newly-skipped tests behind the quarantine.
    totals.skipped_markers += skipped_markers
    if skipped_markers > 0:
        totals.lanes_with_skip_markers += 1
    return None


def _warn_stale_quarantine_entry(name: str, pattern: str, entry: dict) -> None:
    """WARN (not a failure) that an approved entry matched no current failing case.

    The lane is still tolerated, but the entry is either fixed or simply did not
    run this pass - surfacing it prompts a human to re-verify or remove it so a
    fixed quarantine cannot silently re-tolerate a future regression. It is a WARN
    rather than a hard fail because we cannot distinguish "fixed" from "did not run
    this run" (env-skipped / filtered) without parsing passed-case names; the
    expires_utc field remains the hard backstop.
    """
    issue = entry.get("issue_url") or "unknown-issue"
    print(
        f"[module-tests][QUARANTINE-STALE-ENTRY] lane '{name}' entry for test_case "
        f"'{pattern}' matched no current failing case (fixed, or did not run this "
        f"run); review/remove (issue {issue})."
    )


def _handle_quarantined_runnable_failure(
    name: str,
    strict: bool,
    output: str,
    pattern_entries: list[tuple[str, dict]],
    approved_patterns: list[str],
    skipped_markers: int,
    totals: DoctestTotals,
    issue_refs: str,
) -> int | None:
    """Scenario A: the lane RAN and reported per-case failures.

    Tolerate only if every failing case matches at least one approved pattern;
    otherwise fail. When tolerated, WARN on any approved entry that matched no
    current failing case (a fixed / not-run stale entry) via
    _warn_stale_quarantine_entry - the lane is still tolerated (rc unchanged).
    """
    # 'test_case' is required by the schema guard, so no approved pattern here
    # means a misconfigured manifest bypassed the guard; fail closed.
    if not approved_patterns:
        print(
            f"[module-tests][QUARANTINE-UNVERIFIED] '{name}' failed but no manifest "
            f"entry has a test_case to match against; refusing to tolerate a whole "
            f"runnable lane - failing (issue {issue_refs})."
        )
        _print_output_if_present(output)
        return 1

    failing_cases = _parse_failing_doctest_cases(output)
    if not failing_cases:
        print(
            f"[module-tests][QUARANTINE-UNVERIFIED] '{name}' reported failures but no "
            f"failing test-case name could be parsed; cannot confirm they match the "
            f"approved patterns {approved_patterns} - failing (issue {issue_refs})."
        )
        _print_output_if_present(output)
        return 1

    unexpected = [
        case
        for case in failing_cases
        if not any(_test_case_matches(pattern, case) for pattern in approved_patterns)
    ]
    if unexpected:
        print(
            f"[module-tests][QUARANTINE-UNEXPECTED] '{name}' quarantines patterns "
            f"{approved_patterns} but other case(s) failed: {unexpected}; new "
            f"regression - failing (issue {issue_refs})."
        )
        _print_output_if_present(output)
        return 1

    # Every failing case matched an approved pattern. Partition the entries into
    # those whose pattern matched a current failure (still live) and those that
    # matched none (fixed this run, or did not run) - WARN on the latter so a
    # stale entry surfaces for review instead of silently lingering.
    matched_patterns: list[str] = []
    matched_entries: list[dict] = []
    for pattern, entry in pattern_entries:
        if any(_test_case_matches(pattern, case) for case in failing_cases):
            matched_patterns.append(pattern)
            matched_entries.append(entry)
        else:
            _warn_stale_quarantine_entry(name, pattern, entry)

    return _tolerate_quarantined_lane(
        name,
        strict,
        issue_refs,
        output,
        totals,
        skipped_markers,
        f"[module-tests][QUARANTINE] '{name}' failed as expected in matched case(s) "
        f"{failing_cases} (matched patterns {_dedupe_preserving_order(matched_patterns)}, "
        f"issue {_quarantine_issue_refs(matched_entries)}); tolerating.",
    )


def _handle_quarantined_expected_fail(
    name: str,
    strict: bool,
    output: str,
    entries: list[dict],
    totals: DoctestTotals,
    issue_refs: str,
) -> int | None:
    """The quarantined lane failed. Honor the entries only for their named failures.

    A lane may carry several entries, one per approved failing case; the lane's
    approved patterns are the UNION of every entry's 'test_case'.

    Scenario A - the lane RAN and reported per-case failures (summary with
    failed tests/assertions): tolerate ONLY if every failing case matches at
    least one approved pattern. If any failing case matches NONE, that is a new
    regression and the run fails. If failures are reported but no case name can
    be parsed, fail closed (we cannot confirm the failures are the quarantined
    ones). A fully matched failure is still NOT tolerated if it also introduced
    newly skipped coverage in strict CI (handled in _tolerate_quarantined_lane).

    Scenario B - the lane CRASHED (nonzero exit, no per-case summary): a crash
    takes down the whole lane, so per-case matching is impossible and the lane is
    tolerated as a whole. This is a documented limitation (see
    docs/reference/test-quarantine.md): a crash-quarantine can mask a NEW crash
    in the same lane, so such an entry should target the narrowest lane filter.
    """
    (
        _passed_tests,
        failed_tests,
        _passed_asserts,
        failed_asserts,
        skipped_markers,
        summary_found,
    ) = _parse_doctest_results(output)

    # Union of approved test_case patterns across ALL of the lane's entries.
    pattern_entries = [
        ((entry.get("test_case") or "").strip(), entry)
        for entry in entries
        if (entry.get("test_case") or "").strip()
    ]
    approved_patterns = _dedupe_preserving_order(pattern for pattern, _ in pattern_entries)

    runnable_failure = summary_found and (failed_tests > 0 or failed_asserts > 0)
    if not runnable_failure:
        # Scenario B: crash / no parseable per-case failure info.
        return _tolerate_quarantined_lane(
            name,
            strict,
            issue_refs,
            output,
            totals,
            skipped_markers,
            f"[module-tests][QUARANTINE] '{name}' crashed as expected "
            f"(no per-case doctest summary; tolerating the whole lane - a crash "
            f"cannot be narrowed to a single test_case); (issue {issue_refs}).",
        )

    # Scenario A: runnable failure with per-case info (delegated for clarity).
    return _handle_quarantined_runnable_failure(
        name,
        strict,
        output,
        pattern_entries,
        approved_patterns,
        skipped_markers,
        totals,
        issue_refs,
    )


def _handle_quarantined_lane(
    name: str,
    strict: bool,
    ok: bool,
    output: str,
    entries: list[dict],
    totals: DoctestTotals,
) -> int | None:
    """Apply quarantine semantics to one lane (which may carry several entries).
    Returns an exit code to abort on, or None to continue to the next lane."""
    issue_refs = _quarantine_issue_refs(entries)
    entry_word = "entry" if len(entries) == 1 else "entries"
    outcome = _classify_quarantined_lane_outcome(ok, output)

    if outcome == "clean_pass":
        if ok:
            print(
                f"[module-tests][QUARANTINE-STALE] '{name}' is quarantined but PASSED "
                f"- delete its manifest {entry_word} (issue {issue_refs})."
            )
        else:
            print(
                f"[module-tests][QUARANTINE-STALE] '{name}' passed all tests (nonzero "
                f"exit indicates a teardown/harness failure) - delete the {entry_word} / "
                f"investigate the crash (issue {issue_refs})."
            )
        _print_output_if_present(output)
        return 1

    if outcome == "coverage_lost":
        print(
            f"[module-tests][QUARANTINE] '{name}' is quarantined but exercised no "
            f"failing test (0 coverage; stale/misconfigured {entry_word}) - failing "
            f"(issue {issue_refs})."
        )
        _print_output_if_present(output)
        return 1

    if outcome == "harness_error":
        print(
            f"[module-tests][QUARANTINE] '{name}' is quarantined but produced no doctest "
            f"summary (exit 0 harness error); refusing to treat as an expected failure "
            f"(issue {issue_refs})."
        )
        _print_output_if_present(output)
        return 1

    return _handle_quarantined_expected_fail(
        name, strict, output, entries, totals, issue_refs
    )


def _lane_counts_from_output(output: str) -> tuple[int, int, int, int, int, bool]:
    """Doctest counts for the ledger, with UNKNOWN kept distinct from zero.

    `_parse_doctest_results()` reports 0 for every count when it found no
    summary. Passing that on would make "crashed before printing anything"
    indistinguishable from "ran and passed nothing" -- the same
    absence-reads-as-success confusion the ledger exists to remove. The skipped
    marker count is a direct scan of the output and stays exact either way.
    """
    (
        passed_tests,
        failed_tests,
        passed_asserts,
        failed_asserts,
        skipped_markers,
        summary_found,
    ) = _parse_doctest_results(output)
    if not summary_found:
        return (
            LANE_COUNT_UNKNOWN,
            LANE_COUNT_UNKNOWN,
            LANE_COUNT_UNKNOWN,
            LANE_COUNT_UNKNOWN,
            skipped_markers,
            False,
        )
    return passed_tests, failed_tests, passed_asserts, failed_asserts, skipped_markers, True


def _execute_lane(
    godot: str,
    name: str,
    run_args: list[str],
    strict: bool,
    tests_unavailable_mode: str,
    allow_tests_unavailable: bool,
    quarantine: dict[str, list[dict]],
    totals: DoctestTotals,
) -> tuple[int | None, LaneResult]:
    """Run ONE lane. Returns (exit code to abort on or None, ledger result).

    The return type is the totality mechanism: every control-flow path must
    hand back a LaneResult, so a future `return 1` that forgets the ledger is a
    loud unpacking error rather than a silently missing lane. Baseline
    behaviour -- every printed message and every exit code -- is unchanged; this
    function only *observes* what the baseline already decided.
    """
    run_result = _run_godot(godot, run_args)
    ok, skipped, output = run_result
    raw_returncode = getattr(run_result, "returncode", None)
    # The availability is carried, not encoded into the value: -1 is a return
    # code POSIX processes really produce (SIGHUP), so a lane whose process was
    # signalled and a lane for which no return code exists would otherwise be
    # the same record. See LaneResult.exit_code_reported.
    exit_code_reported = raw_returncode is not None
    lane_exit_code = LANE_COUNT_UNKNOWN if raw_returncode is None else int(raw_returncode)
    (
        passed_tests,
        failed_tests,
        passed_asserts,
        failed_asserts,
        skipped_markers,
        summary_found,
    ) = _lane_counts_from_output(output)

    # EXECUTED coverage is passed + failed, never passed alone. A lane in which
    # every test fails has both PASSED counts at zero while having executed the
    # most coverage of any shape there is; deriving "nothing ran" from the passed
    # counts would file the maximally-informative case under "no coverage" - the
    # exact inverse of what this field exists to expose, and it would read as an
    # improvement to any future ratchet armed on it (#822 round 4).
    executed_tests = LANE_COUNT_UNKNOWN if not summary_found else passed_tests + failed_tests
    executed_assertions = (
        LANE_COUNT_UNKNOWN if not summary_found else passed_asserts + failed_asserts
    )

    def result(outcome: str, detail: str) -> LaneResult:
        return LaneResult(
            outcome=outcome,
            exit_code=lane_exit_code,
            exit_code_reported=exit_code_reported,
            summary_reported=summary_found,
            zero_coverage=(
                None
                if not summary_found
                else not (executed_tests > 0 and executed_assertions > 0)
            ),
            passed_tests=passed_tests,
            failed_tests=failed_tests,
            passed_assertions=passed_asserts,
            failed_assertions=failed_asserts,
            skipped_markers=skipped_markers,
            detail=detail,
        )

    if skipped:
        if not _report_unavailable_lane(name, output, tests_unavailable_mode, allow_tests_unavailable):
            return 1, result(
                LANE_OUTCOME_UNAVAILABLE,
                "binary does not support --test and strict tests-unavailable mode failed the run",
            )
        totals.lanes_unavailable += 1
        return None, result(
            LANE_OUTCOME_UNAVAILABLE, "binary does not support --test; lane skipped"
        )

    lane_entries = quarantine.get(name)
    if lane_entries:
        exit_code = _handle_quarantined_lane(name, strict, ok, output, lane_entries, totals)
        if exit_code is not None:
            return exit_code, result(
                LANE_OUTCOME_QUARANTINE_REJECTED,
                "quarantine entry not honoured; see the [module-tests][QUARANTINE*] line above",
            )
        return None, result(
            LANE_OUTCOME_QUARANTINE_TOLERATED,
            "known failure tolerated by tests/ci/quarantine_manifest.json",
        )

    if not ok:
        if _report_failed_lane(name, strict, output):
            return None, result(
                LANE_OUTCOME_ADVISORY_FAIL,
                "advisory lane failed or crashed; this outcome did not itself fail the run",
            )
        return 1, result(LANE_OUTCOME_FAIL, "strict lane failed or crashed")

    stats = _validate_successful_lane(name, strict, output)
    if stats is None:
        return 1, result(
            LANE_OUTCOME_FAIL,
            "exit 0, but the doctest summary was missing, reported failures, or the lane "
            "violated the strict-CI skipped-marker policy",
        )
    totals.add_lane_stats(stats)
    if stats.has_executed_coverage:
        _print_lane_passed(name, stats)
        return None, result(LANE_OUTCOME_PASS, "passed with executed coverage")
    # Only an advisory lane can reach here: _handle_no_executed_coverage()
    # returns None (-> FAIL above) for a strict lane with no executed coverage.
    return None, result(
        LANE_OUTCOME_ADVISORY_NO_COVERAGE,
        # Passed counts are the right basis HERE and only here: this path is
        # reached solely when _validate_successful_lane() has already
        # established failed_tests == failed_assertions == 0, so passed IS the
        # executed total. The ledger's own zero_coverage field cannot make that
        # assumption, because it also describes failing lanes.
        "advisory lane executed no coverage (0 passed tests or 0 passed assertions, "
        "with no failures on this path)",
    )


def _run_doctest_lanes(
    godot: str,
    test_runs: Iterable[TestRun],
    tests_unavailable_mode: str,
    allow_tests_unavailable: bool,
    lane_report_path: Path | None = None,
) -> int:
    runs = list(test_runs)
    totals = DoctestTotals()
    ledger = LaneLedger((name, strict) for name, _run_args, strict in runs)
    quarantine = _load_quarantine()
    exit_code = 0
    attempted_lanes = 0
    for index, (name, run_args, strict) in enumerate(runs):
        totals.lanes += 1
        attempted_lanes += 1
        lane_exit_code, lane_result = _execute_lane(
            godot,
            name,
            run_args,
            strict,
            tests_unavailable_mode,
            allow_tests_unavailable,
            quarantine,
            totals,
        )
        ledger.record(index, lane_result, ended_run=lane_exit_code is not None)
        if lane_exit_code is not None:
            exit_code = lane_exit_code
            break
    else:
        _print_doctest_totals(totals)

    ledger_totals = ledger.print_block()
    integrity_errors = ledger.check_integrity(attempted_lanes=attempted_lanes)
    if lane_report_path is not None:
        if integrity_errors:
            # The atomic write guarantees the destination is never empty or
            # partial; it does NOT decide whether this ledger is worth keeping.
            # A ledger that failed its own integrity check is known-untrustworthy,
            # so overwriting the last valid measurement with it destroys good
            # evidence in favour of bad - the same trade the round-2 preflight fix
            # refused. The full block is on stdout above either way.
            print(
                f"[module-tests][lane-ledger][INTEGRITY] refusing to write "
                f"{lane_report_path}: this ledger failed its own integrity check "
                f"(see the lines below). Any previous report at that path is left "
                f"untouched; the full block is on stdout above."
            )
        else:
            integrity_errors.extend(
                _write_lane_report(
                    lane_report_path,
                    ledger.to_json(ledger_totals, lane_loop_exit_code=exit_code),
                )
            )
    if integrity_errors:
        # The ledger gates no TEST outcome, but it must not report success for a
        # run whose own record is incomplete or unpersisted.
        for message in integrity_errors:
            print(f"[module-tests][lane-ledger][INTEGRITY] {message}")
        return exit_code if exit_code != 0 else 1
    return exit_code


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Gaussian Splatting module tests and CI guards.")
    parser.add_argument("--godot-binary", default=os.environ.get("GODOT_BINARY", "godot"),
                        help="Path to Godot binary (default: GODOT_BINARY env or 'godot').")
    parser.add_argument("--base-ref", default=os.environ.get("GS_RENDER_GUARD_BASE"),
                        help="Git base ref/commit for render-path guard diff (default: auto-detected).")
    parser.add_argument("--guard-only", "--guards-only", action="store_true",
                        help="Run guards only and skip Godot test execution.")
    parser.add_argument("--skip-render-guards", action="store_true",
                        help="Skip render-path mutation guard checks.")
    parser.add_argument("--skip-static-guards", action="store_true",
                        help="Skip static format safety guard checks.")
    parser.add_argument("--skip-build-metadata-guard", action="store_true",
                        help="Skip SCons/CMake/doc metadata consistency guard checks.")
    parser.add_argument(
        "--tests-unavailable-mode",
        choices=("strict", "warn-only"),
        default=os.environ.get(TEST_AVAILABILITY_MODE_ENV, "").strip().lower() or None,
        help=(
            "Behavior when the binary has no test runner support "
            f"(default: {TEST_AVAILABILITY_MODE_ENV}, then {VALIDATION_MODE_ENV}, then CI-aware fallback)."
        ),
    )
    parser.add_argument(
        "--allow-tests-unavailable",
        action="store_true",
        help=(
            "Explicitly allow skipping unavailable tests (non-fatal), "
            f"equivalent to setting {ALLOW_TESTS_UNAVAILABLE_ENV}=1."
        ),
    )
    parser.add_argument(
        "--gpu",
        action="store_true",
        help=(
            "Run the renderer-dependent (requires-RD) doctest lane (opt-in). "
            "Under Godot's --test mode all tests in this lane skip because no "
            "RenderingDevice is created.  Useful as a smoke-check that the "
            "tests still compile and skip gracefully. "
            f"Equivalent to setting {GS_RUN_GPU_TESTS_ENV}=1."
        ),
    )
    parser.add_argument(
        "--lane-report",
        metavar="PATH",
        default=None,
        help=(
            "Write the per-lane result ledger (#705) as JSON to PATH. Optional; "
            "omitting it changes nothing. The file is a build output and must stay "
            "untracked. Rejected together with --guard-only, where it could only "
            "produce an empty report."
        ),
    )
    args = parser.parse_args()
    if args.lane_report is not None and args.guard_only:
        # An empty ledger from a run that executed no lane reads as "no lane
        # failed". Refuse rather than emit it.
        parser.error("--lane-report cannot be combined with --guard-only: no lane runs.")
    return args


def main() -> int:
    cli_args = _parse_args()
    godot = _normalize_process_arg(cli_args.godot_binary)
    tests_unavailable_mode = _resolve_tests_unavailable_mode(cli_args.tests_unavailable_mode)
    allow_tests_unavailable = cli_args.allow_tests_unavailable or _env_truthy(
        os.environ.get(ALLOW_TESTS_UNAVAILABLE_ENV, "")
    )
    guard_exit_code = _run_ci_guard_steps(cli_args)
    if guard_exit_code is not None:
        return guard_exit_code

    if cli_args.guard_only:
        print("[module-tests] Guard-only mode complete.")
        return 0

    synthetic_assets_ok, synthetic_asset_messages = _prepare_synthetic_assets()
    for message in synthetic_asset_messages:
        print(f"[module-tests] {message}")
    if not synthetic_assets_ok:
        print("[module-tests] Synthetic asset preparation failed.")
        return 1

    print(
        f"[module-tests] Tests-unavailable mode: {tests_unavailable_mode}"
        f"{' (explicit override enabled)' if allow_tests_unavailable else ''}."
    )

    lane_report_path: Path | None = None
    if cli_args.lane_report is not None:
        lane_report_path = Path(cli_args.lane_report)
        preflight_errors = _preflight_lane_report_path(lane_report_path)
        if preflight_errors:
            for message in preflight_errors:
                print(f"[module-tests][lane-ledger][INTEGRITY] {message}")
            return 1

    run_gpu = os.environ.get(GS_RUN_GPU_TESTS_ENV, "0") == "1" or cli_args.gpu
    test_runs = _build_module_test_runs(run_gpu)
    coverage_errors = _lane_runs_missing_from_module_filters(test_runs, run_gpu)
    if coverage_errors:
        for message in coverage_errors:
            print(f"[module-tests][lane-ledger][INTEGRITY] {message}")
        return 1

    return _run_doctest_lanes(
        godot,
        test_runs,
        tests_unavailable_mode,
        allow_tests_unavailable,
        lane_report_path,
    )


if __name__ == "__main__":
    sys.exit(main())
