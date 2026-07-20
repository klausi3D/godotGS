#!/usr/bin/env python3
"""Run Gaussian Splatting module tests via Godot's built-in test runner.

If the binary was built without tests enabled, behavior is controlled by
strict/warn-only policy (strict fails, warn-only skips).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
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
DOC_CLASSES_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_doc_classes_complete.py"
TEST_LINKAGE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_test_linkage.py"
REQUIRE_NULL_DEREF_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_require_null_deref.py"
REQUIRE_NULL_DEREF_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_require_null_deref.py"
TEST_LANE_COVERAGE_GUARD_SCRIPT = ROOT / "tests" / "ci" / "check_test_lane_coverage.py"
TEST_LANE_COVERAGE_TEST_SCRIPT = ROOT / "tests" / "ci" / "test_check_test_lane_coverage.py"
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
    "Editor",
    "Importer",
    "MalformedCorpus",  # G2: the aggregate malformed-input gate (WorldIO/PLY/SPZ/Persistence).
    "Node",
    "PLY",
    "Persistence",
    "SPZ",  # G2: promoted from the advisory [untagged] lane to a strict blocking lane.
    "SceneTree",
    "SortBenchmark",
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
DOCTEST_SKIP_MARKER_RE = re.compile(r"(?m)^\s*(?:Skipping(?: test)?\s*-\s+.+)$")

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
    # helper is crash-atomic; these guards lock in that each saver actually USES
    # it (the PLY cache writer delegates to the world saver, so it is covered
    # transitively). If a saver is intentionally re-plumbed, update the guard.
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


def _run_quarantine_manifest_guard() -> tuple[bool, list[str]]:
    """Guard step (runs in the --guard-only lane): schema-validate the manifest
    and then run the mechanism's unit test. Fails on either."""
    schema_ok, messages = _validate_quarantine_manifest_schema()
    if not schema_ok:
        return False, messages
    test_ok, test_messages = _run_quarantine_manifest_unittest()
    return test_ok, messages + test_messages


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


def _run_godot(godot: str, args: Iterable[str]) -> tuple[bool, bool, str]:
    normalized_godot = _normalize_process_arg(godot)
    command = [normalized_godot]
    command.extend(_normalize_process_arg(arg) for arg in args)

    if not command[0]:
        return False, False, "ValueError: empty Godot binary path after normalization"

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
        return False, False, f"{type(exc).__name__}: {exc} (command={command!r})"

    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0 and _tests_unavailable(output):
        return True, True, output
    return result.returncode == 0, False, output


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
            _run_benchmark_fixture_contract_guard,
            "Benchmark fixture contract guard failed.",
            "Benchmark fixture contract guard passed.",
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


def _enforce_skipped_marker_policy(name: str, strict: bool, output: str, skipped_markers: int) -> bool:
    if skipped_markers <= 0:
        return True

    print(f"[module-tests] '{name}' reported {skipped_markers} skipped doctest marker(s).")
    if strict and _is_ci():
        _report_lane_failure(name, "skipped doctest coverage is not allowed in CI.", output)
        return False
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


def _run_doctest_lanes(
    godot: str,
    test_runs: Iterable[TestRun],
    tests_unavailable_mode: str,
    allow_tests_unavailable: bool,
) -> int:
    totals = DoctestTotals()
    quarantine = _load_quarantine()
    for name, run_args, strict in test_runs:
        totals.lanes += 1
        ok, skipped, output = _run_godot(godot, run_args)
        if skipped:
            if not _report_unavailable_lane(name, output, tests_unavailable_mode, allow_tests_unavailable):
                return 1
            totals.lanes_unavailable += 1
            continue

        lane_entries = quarantine.get(name)
        if lane_entries:
            exit_code = _handle_quarantined_lane(
                name, strict, ok, output, lane_entries, totals
            )
            if exit_code is not None:
                return exit_code
            continue

        if not ok:
            if _report_failed_lane(name, strict, output):
                continue
            return 1

        stats = _validate_successful_lane(name, strict, output)
        if stats is None:
            return 1
        totals.add_lane_stats(stats)
        if stats.has_executed_coverage:
            _print_lane_passed(name, stats)

    _print_doctest_totals(totals)
    return 0


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
    return parser.parse_args()


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

    run_gpu = os.environ.get(GS_RUN_GPU_TESTS_ENV, "0") == "1" or cli_args.gpu
    return _run_doctest_lanes(
        godot,
        _build_module_test_runs(run_gpu),
        tests_unavailable_mode,
        allow_tests_unavailable,
    )


if __name__ == "__main__":
    sys.exit(main())
