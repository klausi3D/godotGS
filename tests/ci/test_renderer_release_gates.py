#!/usr/bin/env python3
"""Unit tests for renderer release gate checker."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_renderer_release_gates.py"
spec = importlib.util.spec_from_file_location("check_renderer_release_gates", SCRIPT)
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _artifact(root: Path, rel_path: str, content: str = "artifact\n") -> dict[str, Any]:
    path = root / rel_path
    _write(path, content)
    return {
        "path": rel_path,
        "sha256": _sha256(path),
        "godot_binary_commit": "abc",
        "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
    }


def _base_manifest(root: Path) -> dict[str, Any]:
    _write(
        root / "modules/gaussian_splatting/tests/test_gpu.h",
        'TEST_CASE("[GaussianSplatting][RequiresGPU][HazardRepro] Smoke") {}\n',
    )
    tests = checker._extract_requires_gpu_tests(root)
    count, digest = checker._requires_gpu_snapshot(tests)
    for rel in (
        "docs/reference/renderer-release-gates.md",
        "docs/development/known-public-alpha-limitations.md",
        "docs/performance/index.md",
        "docs/assets/data/benchmark_latest.json",
        "tests/visual_baselines/README.md",
        "tests/runtime/runtime_scenarios.json",
        "tests/visual_baselines/composite_hazard_256x256.png",
        ".github/workflows/gaussian_production_gates.yml",
        ".github/workflows/baseline_qa.yml",
        ".github/workflows/release_builds.yml",
    ):
        _write(root / rel, "guards\nmodule-validation\nRun GPU harness\nbuild_linux\nbuild_windows\npublish_release\n")
    _write(
        root / "tests/ci/run_gpu_harness.py",
        "from dataclasses import dataclass\n"
        "@dataclass(frozen=True)\n"
        "class BatchSpec:\n"
        "    name: str\n"
        "    filters: tuple[str, ...]\n"
        "BATCHES = (BatchSpec('CompositorHazard', ('*HazardRepro*',)),)\n",
    )
    return {
        "schema_version": 1,
        "public_alpha_predicate": {
            "channel": "public-alpha",
            "accepted_tag_patterns": ["v*-alpha*"],
            "candidate_manifest_field": "release_channel=public-alpha",
            "disallow_path_filter_downgrade": True,
            "disallow_manual_downgrade": True,
            "disallow_missing_gpu_runner_downgrade": True,
            "disallow_open_world_advisory_only": True,
            "required_issue_query": {
                "classification_labels_any": ["priority:P0", "priority:P1", "release blocker"],
                "blocking_labels_any": ["priority:P0", "release blocker"],
                "alpha_relevant_p1_labels_all": ["priority:P1", "alpha-relevant"],
                "allowed_classifications": ["blocking", "accepted_alpha_limitation", "deferred"],
            },
        },
        "public_alpha_issue_ledger": {
            "scope": "open P0, P1, and release-blocker issues from the candidate issue snapshot",
            "allowed_statuses": ["blocking", "accepted_alpha_limitation", "deferred"],
            "tracked_issues": [
                {
                    "number": 369,
                    "title": "Audit P0: repair opaque qlty check failures blocking renderer PRs",
                    "status": "deferred",
                    "goal": "Keep opaque external quality checks out of the local public-alpha gate unless branch protection makes them required.",
                    "rationale": "The qlty check is an external advisory signal in this repository policy.",
                    "evidence_required": [
                        "GitHub branch protection required_status_checks is empty or absent",
                        "qlty check remains documented as non-blocking external signal",
                    ],
                }
            ],
        },
        "external_status_check_policy": {
            "branch_protection_required_status_checks": [],
            "required_for_public_alpha": [],
            "non_blocking_checks": [
                {
                    "context": "qlty check",
                    "issue": 369,
                    "classification": "deferred",
                    "reason": "External qlty check is not required by branch protection in the current repo policy.",
                    "decision_evidence": [
                        "master branch protection required_status_checks is null",
                        "No qlty configuration is tracked in the repo",
                    ],
                    "local_gate_behavior": "Do not fail the local renderer release gate solely on qlty check status.",
                }
            ],
        },
        "known_limitations_page": "docs/development/known-public-alpha-limitations.md",
        "references": [
            "docs/reference/renderer-release-gates.md",
            "docs/development/known-public-alpha-limitations.md",
            "docs/performance/index.md",
            "docs/assets/data/benchmark_latest.json",
            "tests/visual_baselines/README.md",
            "tests/runtime/runtime_scenarios.json",
        ],
        "requires_gpu_test_snapshot": {
            "test_count": count,
            "sha256": digest,
            "deferred_tags_any": ["SceneTree", "Importer"],
            "deferred_count": 0,
            "closure_policy": "deferred RequiresGPU coverage blocks closure",
        },
        "deferred_requires_gpu_waivers": [],
        "gpu_harness_policy": {
            "script": "tests/ci/run_gpu_harness.py",
            "required_batches": [
                {
                    "name": "CompositorHazard",
                    "minimum_test_cases": 1,
                    "allow_skips": False,
                    "allow_timeout": False,
                    "allow_rid_leaks": False,
                    "reference_artifacts": ["tests/visual_baselines/composite_hazard_256x256.png"],
                }
            ],
        },
        "workflow_policy": {
            "machine_enforcement_scope": "workflow-presence-and-required-job-markers",
            "documented_non_enforced_rules": [
                "manual workflow inputs must not bypass public-alpha readiness",
                "path filters must not downgrade public-alpha readiness",
            ],
            "required_workflows": [
                {
                    "file": ".github/workflows/gaussian_production_gates.yml",
                    "required_jobs": ["guards", "module-validation"],
                },
                {
                    "file": ".github/workflows/baseline_qa.yml",
                    "required_jobs": ["Run GPU harness"],
                },
                {
                    "file": ".github/workflows/release_builds.yml",
                    "required_jobs": ["build_linux", "build_windows", "publish_release"],
                },
            ]
        },
        "visual_acceptance": {
            "tracked_actual_png_allowed": False,
            "blocking_references": [
                {
                    "id": "compositor_hazard_256",
                    "path": "tests/visual_baselines/composite_hazard_256x256.png",
                    "required": True,
                }
            ],
            "candidate_rules": {
                "capture_count_min": 1,
                "capture_reference_match_count_must_equal_capture_count": True,
                "capture_ssim_min_must_be_non_null": True,
                "capture_psnr_min_must_be_non_null": True,
            },
        },
        "benchmark_acceptance": {
            "candidate_required_lanes": ["static_baseline"],
            "required_fields_non_null": ["lane_id", "commit_sha", "godot_binary_commit", "godot_binary_mtime_utc"],
        },
        "artifact_requirements": {
            "required_groups": ["linux_release_archive", "known_limitations_page"],
            "each_artifact_requires": ["path", "sha256", "godot_binary_commit", "godot_binary_mtime_utc"],
        },
        "lifetime_accounting_proof": {
            "stdout_marker": "[GS-LIFETIME] ",
            "required_scenarios": [
                "renderer_instance",
                "failed_init",
                "scene_director_reload",
                "asset_attach_detach",
                "stringname_orphans",
            ],
            "scenario_metric_fields": {
                "renderer_instance": "rd_bytes_leaked",
                "failed_init": "rd_bytes_leaked",
                "scene_director_reload": "rd_bytes_leaked",
                "asset_attach_detach": "rd_bytes_leaked",
            },
            "thresholds_bytes": {
                "renderer_instance": 4194304,
                "failed_init": 4194304,
                "scene_director_reload": 262144,
                "asset_attach_detach": 65536,
            },
            "thresholds_counts": {
                "stringname_orphans_max": 5,
            },
            "test_binary_lane": "GaussianSplatting [Lifetime]",
            "gpu_harness_batch": "Lifetime",
            "advisory_fields": ["stringname_orphan_delta"],
            "advisory_sentinel_value": -1,
            "advisory_fields_strict_for": {
                "stringname_orphans": ["stringname_orphan_delta"],
            },
            "source_prs": [386, 389],
            "owner_issue": 352,
        },
    }


def _issue(number: int, labels: list[str], state: str = "OPEN") -> dict[str, Any]:
    return {
        "number": number,
        "state": state,
        "labels": [{"name": label} for label in labels],
    }


def _base_issue_snapshot(*extra: dict[str, Any]) -> list[dict[str, Any]]:
    return [_issue(369, ["priority:P0"])] + list(extra)


def _valid_candidate_evidence(root: Path) -> dict[str, Any]:
    _write(
        root / "gpu_report.json",
        json.dumps(
            {
                # Top-level run verdict. The release gate consumes these by name
                # rather than re-deriving them from the batch rows (#692), so a
                # valid candidate report must carry a clean verdict.
                "supervisor_exit": 0,
                "max_batch_rc": 0,
                "parsed_failures": 0,
                "total_rid_leak_bytes": 0,
                "totals_authoritative": True,
                "zero_assertion_required_batches": [],
                "hollow_required_cases": [],
                "case_audit_missing_batches": [],
                "empty_required_batches": [],
                "summary_parse_failures": [],
                "required_lifetime_failures": [],
                "timed_out_batches": [],
                "batches": [
                    {
                        "name": "CompositorHazard",
                        "timed_out": False,
                        "summary_parse_ok": True,
                        "rc": 0,
                        # skipped=2007, not 0: doctest's `skipped` column is the
                        # whole corpus minus this batch's filter reach, so a real
                        # report can never carry 0 here (#695). Fixtures that said
                        # 0 are what let the old skip gate pass its tests while
                        # rejecting every real run.
                        "test_cases": {"total": 1, "failed": 0, "skipped": 2007},
                        "assertions": {"failed": 0},
                        "rid_leak_bytes": 0,
                        "zero_assertion_cases": [],
                        "case_assert_audit_ok": True,
                        "cases_started": 1,
                    }
                ],
            }
        ),
    )
    _write(
        root / "bench.json",
        json.dumps(
            [
                {
                    "lane_id": "static_baseline",
                    "commit_sha": "abc",
                    "godot_binary_commit": "abc",
                    "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                    "capture_count": 1,
                    "capture_reference_match_count": 1,
                    "capture_threshold_pass_count": 1,
                    "capture_ssim_min": 0.99,
                    "capture_psnr_min": 40.0,
                    "gpu_timing_available": False,
                    "gpu_frame_time_source": "unavailable",
                    "exit_code": 0,
                    "report_valid": True,
                    "visible_output_valid": True,
                    "visual_reference_match": True,
                    "proof_valid": True,
                    "lane_valid": True,
                }
            ]
        ),
    )
    return {
        "release_channel": "public-alpha",
        "commit": "abc",
        "commit_time_utc": "2026-05-19T10:00:00Z",
        "gpu_harness_report": "gpu_report.json",
        "benchmark_report": "bench.json",
        "artifacts": {
            "linux_release_archive": _artifact(root, "artifacts/linux_release_archive.tar.xz"),
            "known_limitations_page": {
                "path": "docs/development/known-public-alpha-limitations.md",
                "sha256": _sha256(root / "docs/development/known-public-alpha-limitations.md"),
                "godot_binary_commit": "abc",
                "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
            },
        },
    }


class RendererReleaseGateTests(unittest.TestCase):
    def test_repository_contract_passes(self) -> None:
        manifest = checker._load_json(ROOT / "docs/reference/renderer_release_gate_manifest.json")
        failures = checker.validate_contract(ROOT, manifest)
        self.assertEqual([], failures)

    def test_new_requires_gpu_test_fails_snapshot_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "modules/gaussian_splatting/tests/test_new_gpu.h",
                'TEST_CASE("[GaussianSplatting][RequiresGPU] New") {}\n',
            )
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("RequiresGPU test snapshot drift" in item for item in failures))

    def test_missing_visual_reference_fails_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            (root / "tests/visual_baselines/composite_hazard_256x256.png").unlink()
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("reference missing" in item for item in failures))

    def test_tracked_actual_pngs_detect_root_visual_baseline_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            actual = root / "tests/visual_baselines/root.actual.png"
            _write(actual, "actual\n")
            subprocess.run(["git", "init"], cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(
                ["git", "add", "tests/visual_baselines/root.actual.png"],
                cwd=root,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            self.assertIn("tests/visual_baselines/root.actual.png", checker._tracked_actual_pngs(root))

    def test_missing_known_limitations_page_fails_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            (root / "docs/development/known-public-alpha-limitations.md").unlink()
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("known limitations page is missing" in item for item in failures))

    def test_required_batch_matching_zero_tests_fails_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "tests/ci/run_gpu_harness.py",
                "from dataclasses import dataclass\n"
                "@dataclass(frozen=True)\n"
                "class BatchSpec:\n"
                "    name: str\n"
                "    filters: tuple[str, ...]\n"
                "BATCHES = (BatchSpec('CompositorHazard', ('*NoMatch*',)),)\n",
            )
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("matches 0 tests" in item for item in failures))

    def test_workflow_policy_rejects_unenforced_claims(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["workflow_policy"]["required_workflows"][0]["manual_input_may_disable"] = False
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("not machine-enforced" in item for item in failures))

    def test_issue_ledger_rejects_missing_evidence_requirements(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"][0].pop("evidence_required")
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("classification missing evidence_required" in item for item in failures))

    def test_external_status_policy_keeps_qlty_non_blocking(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["external_status_check_policy"]["non_blocking_checks"][0]["required_for_public_alpha"] = True
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("cannot be both non-blocking and public-alpha required" in item for item in failures))

    def test_external_status_policy_requires_qlty_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["external_status_check_policy"]["non_blocking_checks"] = []
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("must classify qlty check as required or non-blocking" in item for item in failures))

    def test_external_status_policy_allows_qlty_to_be_required(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["external_status_check_policy"]["required_for_public_alpha"] = ["qlty check"]
            manifest["external_status_check_policy"]["non_blocking_checks"] = []
            failures = checker.validate_contract(root, manifest)
            self.assertEqual([], [failure for failure in failures if "qlty check" in failure])

    def test_external_status_policy_rejects_non_string_status_contexts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["external_status_check_policy"]["branch_protection_required_status_checks"] = [{"context": "bad"}]
            manifest["external_status_check_policy"]["required_for_public_alpha"] = [42]
            manifest["external_status_check_policy"]["non_blocking_checks"][0]["context"] = {"context": "bad"}
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(
                any("branch_protection_required_status_checks[0] must be a non-empty string" in item for item in failures)
            )
            self.assertTrue(any("required_for_public_alpha[0] must be a non-empty string" in item for item in failures))
            self.assertTrue(any("context must be a non-empty string" in item for item in failures))

    def test_candidate_requires_issue_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            failures = checker.validate_candidate(root, manifest, evidence)
            self.assertTrue(any("issue snapshot missing" in item for item in failures))

    def test_candidate_open_issue_snapshot_may_omit_resolved_manifest_tracked_issues(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            evidence["resolved_manifest_issues"] = [_issue(360, ["priority:P0"], state="CLOSED")]
            failures = checker.validate_candidate(root, manifest, evidence, [])
            self.assertEqual([], failures)

    def test_candidate_rejects_omitted_manifest_blocker_without_closure_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            failures = checker.validate_candidate(root, manifest, evidence, [])
            self.assertTrue(any("missing manifest-tracked blocking issue #360" in item for item in failures))

    def test_candidate_rejects_open_closure_proof_for_omitted_manifest_blocker(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            evidence["resolved_manifest_issues"] = [_issue(360, ["priority:P0"], state="OPEN")]
            failures = checker.validate_candidate(root, manifest, evidence, [])
            self.assertTrue(any("resolved_manifest_issues issue #360 must have state CLOSED" in item for item in failures))

    def test_candidate_rejects_malformed_closure_state_for_omitted_manifest_blocker(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            evidence["resolved_manifest_issues"] = [_issue(360, ["priority:P0"], state="NOT_A_GITHUB_STATE")]
            failures = checker.validate_candidate(root, manifest, evidence, [])
            self.assertTrue(any("resolved_manifest_issues issue #360 must have state CLOSED" in item for item in failures))

    def test_candidate_rejects_malformed_resolved_manifest_issues(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["resolved_manifest_issues"] = "closed"
            failures = checker.validate_candidate(root, manifest, evidence, _base_issue_snapshot())
            self.assertTrue(any("resolved_manifest_issues must be an issue snapshot object or list" in item for item in failures))

    def test_candidate_accepts_embedded_issue_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["issue_snapshot"] = _base_issue_snapshot()
            failures = checker.validate_candidate(root, manifest, evidence)
            self.assertEqual([], failures)

    def test_candidate_rejects_wrong_channel_and_tag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["release_channel"] = "nightly"
            evidence["release_tag"] = "nightly-20260519"
            failures = checker.validate_candidate(root, manifest, evidence, [])
            self.assertTrue(any("selector is not public-alpha" in item for item in failures))

    def test_candidate_accepts_alpha_tag_selector(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["release_channel"] = "nightly"
            evidence["release_tag"] = "v1.0.0-alpha.1"
            failures = checker.validate_candidate(root, manifest, evidence, _base_issue_snapshot())
            self.assertEqual([], failures)

    def test_issue_status_policy_must_match_manifest_ledger(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_predicate"]["required_issue_query"]["allowed_classifications"] = [
                "blocking",
                "accepted_alpha_limitation",
                "post_alpha",
            ]
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("allowed_classifications must be" in item for item in failures))

    def test_candidate_requires_commit_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence.pop("commit")
            evidence.pop("commit_time_utc")
            failures = checker.validate_candidate(root, manifest, evidence, [])
            self.assertTrue(any("candidate evidence missing commit" in item for item in failures))
            self.assertTrue(any("candidate evidence missing commit_time_utc" in item for item in failures))

    def test_candidate_rejects_invalid_commit_time_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["commit_time_utc"] = "not-a-timestamp"
            failures = checker._candidate_commit_metadata_failures(evidence)
            self.assertTrue(any("invalid commit_time_utc" in item for item in failures))

    def test_deferred_gpu_waivers_must_match_deferred_tests(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            tests = checker._extract_requires_gpu_tests(root)
            deferred_name = tests[0]["name"]
            manifest["requires_gpu_test_snapshot"]["deferred_tags_any"] = ["HazardRepro"]
            manifest["requires_gpu_test_snapshot"]["deferred_count"] = 1
            manifest["deferred_requires_gpu_waivers"] = [{"test_name": "Unrelated deferred test"}]

            failures = checker._validate_candidate_deferred_waivers(manifest, tests)
            self.assertTrue(any(f"missing waiver: {deferred_name}" in item for item in failures))
            self.assertTrue(any("does not match a deferred test" in item for item in failures))

    def test_deferred_gpu_waivers_reject_duplicates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            tests = checker._extract_requires_gpu_tests(root)
            deferred_name = tests[0]["name"]
            manifest["requires_gpu_test_snapshot"]["deferred_tags_any"] = ["HazardRepro"]
            manifest["requires_gpu_test_snapshot"]["deferred_count"] = 1
            manifest["deferred_requires_gpu_waivers"] = [
                {"test_name": deferred_name},
                {"test_name": deferred_name},
            ]

            failures = checker._validate_candidate_deferred_waivers(manifest, tests)
            self.assertTrue(any("waiver duplicated" in item for item in failures))

    def test_candidate_fails_timeout_and_null_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": True,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            _write(
                root / "bench.json",
                json.dumps([{"lane_id": "static_baseline", "commit_sha": None, "capture_count": 0}]),
            )
            evidence = {
                "commit": "abc",
                "commit_time_utc": "2026-05-19T10:00:00Z",
                "gpu_harness_report": "gpu_report.json",
                "benchmark_report": "bench.json",
                "artifacts": {
                    "linux_release_archive": {
                        "path": "x",
                        "sha256": "y",
                        "godot_binary_commit": "abc",
                        "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                    },
                    "known_limitations_page": {
                        "path": "x",
                        "sha256": "y",
                        "godot_binary_commit": "abc",
                        "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                    },
                },
            }
            failures = checker.validate_candidate(root, manifest, evidence)
            self.assertTrue(any("timed out" in item for item in failures))
            self.assertTrue(any("null/missing commit_sha" in item for item in failures))
            self.assertTrue(any("has no visual captures" in item for item in failures))

    def _gpu_report_with(self, root: Path, **overrides: Any) -> dict[str, Any]:
        """Write a candidate gpu_report.json that is clean except for overrides.

        The baseline here is a report the gate ACCEPTS, so any failure a test
        observes is attributable to the override under test and not to fixture
        noise.

        `test_cases.skipped` is 2007, NOT 0 (#695). 2007 is what a real
        CompositorHazard batch measures on an RTX 3090: doctest's `skipped`
        column is the whole ~2008-case corpus minus the one case this batch's
        filter selects. The old fixtures said 0 -- a value the harness cannot
        emit for a filtered batch -- and that single unrealistic number is why
        `--mode candidate` shipped for months rejecting every real report while
        its tests were green. Keep this realistic.
        """
        report: dict[str, Any] = {
            "supervisor_exit": 0,
            "max_batch_rc": 0,
            "parsed_failures": 0,
            "total_rid_leak_bytes": 0,
            "totals_authoritative": True,
            "zero_assertion_required_batches": [],
            "hollow_required_cases": [],
            "case_audit_missing_batches": [],
            "empty_required_batches": [],
            "summary_parse_failures": [],
            "required_lifetime_failures": [],
            "timed_out_batches": [],
            "batches": [
                {
                    "name": "CompositorHazard",
                    "timed_out": False,
                    "summary_parse_ok": True,
                    "rc": 0,
                    "test_cases": {"total": 1, "failed": 0, "skipped": 2007},
                    "assertions": {"failed": 0},
                    "rid_leak_bytes": 0,
                    "zero_assertion_cases": [],
                    "case_assert_audit_ok": True,
                    "cases_started": 1,
                }
            ],
        }
        report.update(overrides)
        _write(root / "gpu_report.json", json.dumps(report))
        return report

    def test_candidate_gpu_report_rejects_zero_assertion_required_batches(self) -> None:
        """#692 follow-up: the release gate must consume the zero-assertion signal.

        The harness detects a required batch that asserted nothing and fails the
        run. Before this, `check_renderer_release_gates.py` validated only
        minimum test-case count, rc/failed counters and summary parse -- so that
        same report could be supplied as release-candidate evidence and PASS.
        That is a laundering path, and it is the same defect shape as #692
        itself: a signal that is produced and not consumed where it matters.

        Note the batch rows here are entirely clean. Nothing is re-derivable
        from them -- the only evidence of the problem is the top-level field,
        which is exactly why it has to be read.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            self._gpu_report_with(
                root, zero_assertion_required_batches=["RendererPipeline"]
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)

            self.assertTrue(
                failures,
                "A report carrying zero_assertion_required_batches must be rejected.",
            )
            self.assertTrue(
                any("zero_assertion_required_batches" in item for item in failures),
                f"The failure must name the field. Got: {failures}",
            )
            self.assertTrue(
                any("RendererPipeline" in item for item in failures),
                f"The failure must name the offending batch. Got: {failures}",
            )

    def test_candidate_gpu_report_accepts_clean_verdict(self) -> None:
        """Discriminates: the same fixture without the signal must PASS.

        Without this, the rejection test above could pass for any reason at all
        (a malformed fixture, an unrelated failure) and still look like proof.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            self._gpu_report_with(root)
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertEqual(
                [], failures,
                "A report with a clean run verdict must be accepted; otherwise the "
                "rejection test above proves nothing.",
            )

    def test_candidate_gpu_report_requires_supervisor_exit(self) -> None:
        """Fail-closed: deleting the verdict must not launder the report.

        Consuming named signal fields is only as strong as their presence. If a
        stripped report were treated as clean, the laundering path would reopen
        by deletion. `supervisor_exit` is the harness's own overall verdict, so
        requiring it also covers gate reasons added after this code was written.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            report = self._gpu_report_with(root)
            del report["supervisor_exit"]
            del report["zero_assertion_required_batches"]
            _write(root / "gpu_report.json", json.dumps(report))
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(
                any("supervisor_exit" in item for item in failures),
                f"A report with no run verdict must be refused. Got: {failures}",
            )

    def test_candidate_gpu_report_rejects_nonzero_supervisor_exit(self) -> None:
        """A run the harness itself failed can never be release evidence."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            self._gpu_report_with(root, supervisor_exit=1)
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(
                any("FAILED by the harness itself" in item for item in failures),
                f"Got: {failures}",
            )

    def test_candidate_gpu_report_rejects_sibling_unconsumed_signals(self) -> None:
        """The siblings #692 exposed were unconsumed too -- pin every one.

        `zero_assertion_required_batches` was the field Codex caught, but the
        release gate ignored EVERY top-level verdict the supervisor writes. Each
        of these is a gate-failure reason in `run_gpu_harness.main()` and each
        must now be refused here. Table-driven so a newly added signal that is
        wired into the harness but not into this validator is a visible gap.
        """
        cases: list[tuple[str, Any, str]] = [
            ("empty_required_batches", ["CompositorHazard"], "CompositorHazard"),
            ("hollow_required_cases", ["RendererPipeline/Stage results report cull"], "RendererPipeline"),
            ("case_audit_missing_batches", ["Lifetime"], "Lifetime"),
            ("summary_parse_failures", ["Lifetime"], "Lifetime"),
            ("required_lifetime_failures", ["Lifetime/renderer_instance: skipped"], "Lifetime"),
            ("timed_out_batches", ["NodeSceneTree"], "NodeSceneTree"),
            ("max_batch_rc", 3221225477, "max_batch_rc"),
            ("parsed_failures", 2, "parsed_failures"),
            ("total_rid_leak_bytes", 4194304, "total_rid_leak_bytes"),
            ("totals_authoritative", False, "totals_authoritative"),
        ]
        for field, value, expected_text in cases:
            with self.subTest(field=field):
                with tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    manifest = _base_manifest(root)
                    evidence = _valid_candidate_evidence(root)
                    self._gpu_report_with(root, **{field: value})
                    failures = checker._validate_candidate_gpu_report(
                        root, manifest, evidence
                    )
                    self.assertTrue(
                        failures,
                        f"{field}={value!r} must be rejected as release evidence.",
                    )
                    self.assertTrue(
                        any(expected_text in item for item in failures),
                        f"{field} failure must name {expected_text!r}. Got: {failures}",
                    )

    # ---- #695: filter-miss counts vs. genuine runtime skips -----------------

    def _gpu_batch_with(self, root: Path, **batch_overrides: Any) -> list[str]:
        """Validate an otherwise-clean candidate report with one batch overridden."""
        manifest = _base_manifest(root)
        evidence = _valid_candidate_evidence(root)
        report = self._gpu_report_with(root)
        report["batches"][0].update(batch_overrides)
        _write(root / "gpu_report.json", json.dumps(report))
        return checker._validate_candidate_gpu_report(root, manifest, evidence)

    def test_candidate_gpu_batch_accepts_real_filter_skip_count(self) -> None:
        """#695 regression: doctest's `skipped` column must NOT reject a report.

        `test_cases.skipped` is `numTestCases - numTestCasesPassingFilters`
        (thirdparty/doctest/doctest.h:6307) -- the whole registered corpus minus
        the cases this batch's `--test-case=` filter selected. Measured on an
        RTX 3090 at b15c6ddda46, the three required batches report 2007 / 2004 /
        2004 out of a 2008-case corpus on a run with zero failures.

        Until #695 the gate rejected any non-zero value under
        `allow_skips: false`, which every required batch sets -- so candidate
        mode rejected EVERY real harness report on ALL THREE required batches,
        always. Its tests passed only because they set `skipped: 0`, which the
        producer cannot emit for a filtered batch.

        A four-digit filter-miss count on an otherwise clean batch is now
        accepted. The runtime-skip signal it used to be confused with is
        asserted separately below.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failures = self._gpu_batch_with(
                root, test_cases={"total": 1, "failed": 0, "skipped": 2007}
            )
            self.assertEqual(
                [],
                failures,
                "A clean batch whose filter simply did not select the other 2007 "
                "corpus cases must be accepted as release evidence.",
            )

    def test_candidate_gpu_batch_rejects_case_that_asserted_nothing(self) -> None:
        """The signal `allow_skips: false` is actually asking for.

        A case doctest STARTED, counted PASSED, and which evaluated zero
        assertions early-returned past every check. This is what all four
        RendererPipeline cases do on real hardware (#692/#694). It must be
        rejected even though the batch's rc, failed counts and summary are all
        clean -- nothing in the batch row is re-derivable into this verdict.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failures = self._gpu_batch_with(
                root, zero_assertion_cases=["Stage results report cull/sort skipped"]
            )
            self.assertTrue(failures, "A hollow case must be rejected.")
            self.assertTrue(
                any("ZERO assertions" in item for item in failures),
                f"Failure must name the hollow-case reason. Got: {failures}",
            )
            self.assertTrue(
                any("Stage results report cull/sort skipped" in item for item in failures),
                f"Failure must NAME the offending case. Got: {failures}",
            )

    def test_candidate_gpu_batch_rejects_partially_hollow_batch(self) -> None:
        """Per-case, not per-batch: #692's report-level check cannot see this.

        `zero_assertion_required_batches` fires only when the WHOLE batch
        evaluated zero assertions. A batch where three of four cases early-return
        and one asserts reports `asserts=8/8` with an empty top-level signal and
        sails through. The per-case audit is what closes that.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failures = self._gpu_batch_with(
                root,
                assertions={"failed": 0, "total": 8, "passed": 8},
                test_cases={"total": 4, "failed": 0, "skipped": 2004},
                cases_started=4,
                zero_assertion_cases=["case b", "case c", "case d"],
            )
            self.assertTrue(
                any("case b" in item for item in failures),
                f"A batch that asserts SOMETHING but has hollow cases must still be "
                f"rejected. Got: {failures}",
            )

    def test_candidate_gpu_batch_rejects_missing_case_audit_marker(self) -> None:
        """Fail closed when the producer never recorded that it looked.

        The hollow-case signal is carried by the ABSENCE of NO-ASSERTS lines, and
        absence is equally what a binary built without the listener produces. An
        empty `zero_assertion_cases` is only evidence when `case_assert_audit_ok`
        affirms the audit ran. Same laundering-by-deletion path that
        `supervisor_exit`'s required presence closes.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failures = self._gpu_batch_with(root, case_assert_audit_ok=False)
            self.assertTrue(
                any("per-case assertion audit" in item for item in failures),
                f"A report without the audit marker must be refused. Got: {failures}",
            )

    def test_candidate_gpu_batch_rejects_deleted_zero_assertion_field(self) -> None:
        """Deleting the field must not delete the gate."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            report = self._gpu_report_with(root)
            del report["batches"][0]["zero_assertion_cases"]
            _write(root / "gpu_report.json", json.dumps(report))
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(
                any("missing zero_assertion_cases" in item for item in failures),
                f"A report that omits the field must be refused. Got: {failures}",
            )

    def test_candidate_gpu_batch_zero_assertion_cases_must_be_a_list(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failures = self._gpu_batch_with(root, zero_assertion_cases="case b")
            self.assertTrue(
                any("zero_assertion_cases must be a list" in item for item in failures),
                f"Got: {failures}",
            )

    def test_candidate_gpu_batch_allow_skips_true_tolerates_hollow_cases(self) -> None:
        """Scope guard: the audit is enforced only where the manifest forbids skips.

        Without this the rejection tests above could pass vacuously -- e.g. if the
        new check ignored `allow_skips` and simply always fired.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["gpu_harness_policy"]["required_batches"][0]["allow_skips"] = True
            evidence = _valid_candidate_evidence(root)
            report = self._gpu_report_with(root)
            report["batches"][0]["zero_assertion_cases"] = ["case b"]
            report["batches"][0]["case_assert_audit_ok"] = False
            _write(root / "gpu_report.json", json.dumps(report))
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertEqual([], failures, f"allow_skips=true must tolerate this. Got: {failures}")

    def test_candidate_gpu_batch_still_rejects_non_numeric_skipped(self) -> None:
        """`skipped` is no longer gated on, but it is still structurally validated."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failures = self._gpu_batch_with(
                root, test_cases={"total": 1, "failed": 0, "skipped": "lots"}
            )
            self.assertTrue(
                any("test_cases.skipped must be numeric" in item for item in failures),
                f"Got: {failures}",
            )

    def test_candidate_missing_gpu_report_is_structured_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["gpu_harness_report"] = "missing_gpu_report.json"
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(any("gpu_harness_report unreadable" in item for item in failures))

    def test_candidate_evidence_must_be_json_object(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            failures = checker.validate_candidate(root, manifest, [])
            self.assertTrue(any("candidate evidence must be a JSON object" in item for item in failures))

    def test_candidate_gpu_report_rejects_non_object_batches(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(root / "gpu_report.json", json.dumps({"batches": [1]}))
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(any("batch at index 0 must be a JSON object" in item for item in failures))
            self.assertTrue(any("missing required batch" in item for item in failures))

    def test_candidate_gpu_batch_counters_must_be_objects(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": [],
                                "assertions": [],
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(any("test_cases must be a JSON object" in item for item in failures))
            self.assertTrue(any("assertions must be a JSON object" in item for item in failures))

    def test_candidate_gpu_batch_name_must_be_string(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": ["CompositorHazard"],
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(any("name must be a string" in item for item in failures))
            self.assertTrue(any("missing required batch: CompositorHazard" in item for item in failures))

    def test_candidate_gpu_batch_counters_must_be_numeric(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": "1", "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(any("test_cases.total must be numeric" in item for item in failures))

    def test_candidate_gpu_report_honors_allow_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["gpu_harness_policy"]["required_batches"][0]["allow_timeout"] = True
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": True,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertFalse(any("timed out" in item for item in failures))

    def test_candidate_gpu_report_rejects_lifetime_failed_scenario(self) -> None:
        # Codex PR #419 round-2 Finding 1: a candidate GPU report that recorded a
        # non-passing lifetime scenario (a fixture skip / mark_skipped / failure)
        # must FAIL --mode candidate. doctest counts a fixture skip as PASSED and a
        # non-byte RID leak adds zero rid_leak_bytes, so without inspecting
        # lifetime_failed_scenarios the report would pass every other batch gate.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                                "lifetime_failed_scenarios": [
                                    "tile_shader_recompile: skipped: tile binning pipeline not compiled"
                                ],
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(
                any(
                    "non-passing lifetime scenario" in item and "tile_shader_recompile" in item
                    for item in failures
                ),
                failures,
            )

    def test_candidate_gpu_report_accepts_clean_lifetime_field(self) -> None:
        # The mirror of the rejection test: an empty (or absent)
        # lifetime_failed_scenarios list must NOT introduce a failure, so a clean
        # harness report still passes the candidate gate.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                                "lifetime_failed_scenarios": [],
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertFalse(
                any("lifetime" in item for item in failures),
                failures,
            )

    def test_candidate_gpu_report_rejects_non_list_lifetime_field(self) -> None:
        # Defense-in-depth: a malformed lifetime_failed_scenarios (wrong type) must
        # surface as a structured failure rather than being silently ignored.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                                "lifetime_failed_scenarios": "tile_shader_recompile: skipped",
                            }
                        ]
                    }
                ),
            )
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(
                any("lifetime_failed_scenarios must be a list" in item for item in failures),
                failures,
            )

    def test_candidate_missing_benchmark_report_is_structured_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["benchmark_report"] = "missing_benchmark_report.json"
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("benchmark_report unreadable" in item for item in failures))

    def test_candidate_gpu_time_must_be_numeric(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "bench.json",
                json.dumps(
                    [
                        {
                            "lane_id": "static_baseline",
                            "commit_sha": "abc",
                            "godot_binary_commit": "abc",
                            "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                            "capture_count": 1,
                            "capture_reference_match_count": 1,
                            "capture_ssim_min": 0.99,
                            "capture_psnr_min": 40.0,
                            "gpu_timing_available": True,
                            "gpu_time_frame_ms": "1.25",
                        }
                    ]
                ),
            )
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("has invalid GPU time" in item for item in failures))

    def test_candidate_visual_capture_count_must_be_numeric(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "bench.json",
                json.dumps(
                    [
                        {
                            "lane_id": "static_baseline",
                            "commit_sha": "abc",
                            "godot_binary_commit": "abc",
                            "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                            "capture_count": "1",
                            "capture_reference_match_count": 1,
                            "capture_ssim_min": 0.99,
                            "capture_psnr_min": 40.0,
                            "gpu_timing_available": False,
                            "gpu_frame_time_source": "unavailable",
                        }
                    ]
                ),
            )
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("capture_count must be numeric" in item for item in failures))

    def test_candidate_benchmark_commit_must_match_candidate_commit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            _write(
                root / "bench.json",
                json.dumps(
                    [
                        {
                            "lane_id": "static_baseline",
                            "commit_sha": "old",
                            "godot_binary_commit": "old",
                            "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                            "capture_count": 1,
                            "capture_reference_match_count": 1,
                            "capture_threshold_pass_count": 1,
                            "capture_ssim_min": 0.99,
                            "capture_psnr_min": 40.0,
                            "gpu_timing_available": False,
                            "gpu_frame_time_source": "unavailable",
                        }
                    ]
                ),
            )
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("commit_sha does not match candidate commit" in item for item in failures))
            self.assertTrue(any("godot_binary_commit does not match candidate commit" in item for item in failures))

    def test_candidate_benchmark_exit_code_must_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            lane = json.loads((root / "bench.json").read_text(encoding="utf-8"))[0]
            lane["exit_code"] = 7
            lane["lane_valid"] = False
            _write(root / "bench.json", json.dumps([lane]))
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("exited with 7" in item for item in failures))
            self.assertTrue(any("lane_valid=false" in item for item in failures))

    def test_candidate_benchmark_visual_thresholds_must_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            lane = json.loads((root / "bench.json").read_text(encoding="utf-8"))[0]
            lane["capture_threshold_pass_count"] = 0
            lane["visual_reference_match"] = False
            _write(root / "bench.json", json.dumps([lane]))
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("did not pass all visual thresholds" in item for item in failures))
            self.assertTrue(any("failed visual reference match" in item for item in failures))

    def test_candidate_benchmark_rejects_cpu_sort_route_and_fallback_counters(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            lane = json.loads((root / "bench.json").read_text(encoding="utf-8"))[0]
            lane["cpu_sort_route_detected"] = True
            lane["cpu_sort_route_uid"] = "INSTANCE_SORT_CPU_FALLBACK"
            lane["sort_total_route_fallback_count"] = 2
            _write(root / "bench.json", json.dumps([lane]))
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(any("forbidden CPU sort route" in item for item in failures))
            self.assertTrue(any("unallowed sort fallback routes" in item for item in failures))

    def test_rows_from_report_reads_lane_results_key(self) -> None:
        # #351 E3: the benchmark suite runner writes rows under "lane_results"
        # (run_benchmark.py main()); the checker must read that key.
        report = {"lane_results": [{"lane_id": "static_baseline"}]}
        rows = checker._rows_from_report(report)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["lane_id"], "static_baseline")

    def test_gpu_timing_unavailability_accepts_both_source_keys(self) -> None:
        # The selected-route runner (run_benchmark.py) emits the timing source as
        # "gpu_frame_time_source"; the in-engine suite's overall block emits the same value
        # as "gpu_time_frame_source". When GPU timing is unavailable, an explicit
        # "unavailable" marker under EITHER key must satisfy the gate, and its absence under
        # both must fail closed (Codex #418).
        base = {"gpu_timing_available": False}
        self.assertEqual(
            checker._candidate_lane_gpu_timing_failures(
                "static_baseline", {**base, "gpu_frame_time_source": "unavailable"}),
            [],
        )
        self.assertEqual(
            checker._candidate_lane_gpu_timing_failures(
                "static_baseline", {**base, "gpu_time_frame_source": "unavailable"}),
            [],
        )
        failures = checker._candidate_lane_gpu_timing_failures("static_baseline", dict(base))
        self.assertTrue(
            any("lacks explicit GPU timing unavailability" in f for f in failures),
            failures,
        )

    def test_candidate_benchmark_lane_passes_with_351_fields(self) -> None:
        # #351 E3 positive proof: a lane row carrying route_uid / stage_statuses /
        # fallback_counters passes the candidate gate when the manifest's
        # required_fields_non_null includes all three (the REAL set).
        manifest = {
            "benchmark_acceptance": {
                "required_fields_non_null": [
                    "lane_id",
                    "route_uid",
                    "stage_statuses",
                    "fallback_counters",
                ],
            },
            "visual_acceptance": {"candidate_rules": {"capture_count_min": 1}},
        }
        row = {
            "lane_id": "static_baseline",
            "route_uid": "INSTANCE.RASTER.COMPUTE",
            "stage_statuses": {
                "cull": "success",
                "sort": "success",
                "raster": "success",
                "composite": "success",
            },
            "fallback_counters": {
                "sort_sync": 0,
                "sort_total_route": 0,
                "sort_cached": 0,
                "sort_identity": 0,
                "sort_cull_order": 0,
            },
            # other non-351 candidate-lane signals kept clean so this isolates the 3 fields
            "capture_count": 1,
            "capture_reference_match_count": 1,
            "capture_threshold_pass_count": 1,
            "capture_ssim_min": 0.99,
            "capture_psnr_min": 40.0,
            "gpu_timing_available": False,
            "gpu_frame_time_source": "unavailable",
            "exit_code": 0,
        }
        required = manifest["benchmark_acceptance"]["required_fields_non_null"]
        visual_rules = manifest["visual_acceptance"]["candidate_rules"]
        failures = checker._validate_candidate_benchmark_lane(
            "static_baseline", row, required, visual_rules, None
        )
        self.assertEqual(failures, [], failures)
        # The dedicated required-field check must also report no failures.
        field_failures = checker._candidate_lane_required_field_failures(
            "static_baseline", row, required
        )
        self.assertEqual(field_failures, [])

    def test_candidate_benchmark_lane_fails_on_missing_351_field(self) -> None:
        # #351 E3 negative proof: dropping any of the three #351 fields trips the
        # candidate gate with the "null/missing <field>" message.
        required = ["lane_id", "route_uid", "stage_statuses", "fallback_counters"]
        for missing in ("route_uid", "stage_statuses", "fallback_counters"):
            with self.subTest(missing=missing):
                row = {
                    "lane_id": "static_baseline",
                    "route_uid": "INSTANCE.RASTER.COMPUTE",
                    "stage_statuses": {"cull": "success"},
                    "fallback_counters": {"sort_sync": 0},
                }
                row[missing] = None
                field_failures = checker._candidate_lane_required_field_failures(
                    "static_baseline", row, required
                )
                self.assertTrue(
                    any(
                        f"candidate benchmark lane static_baseline has null/missing {missing}" == item
                        for item in field_failures
                    ),
                    field_failures,
                )

    def test_candidate_benchmark_report_under_lane_results_validates_351_fields(self) -> None:
        # #351 E3 end-to-end: a producer-shaped report (rows under "lane_results")
        # is now consumed, and a row with the three fields passes the gate.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["benchmark_acceptance"]["required_fields_non_null"] = [
                "lane_id",
                "commit_sha",
                "godot_binary_commit",
                "godot_binary_mtime_utc",
                "route_uid",
                "stage_statuses",
                "fallback_counters",
            ]
            evidence = _valid_candidate_evidence(root)
            lane = {
                "lane_id": "static_baseline",
                "commit_sha": "abc",
                "godot_binary_commit": "abc",
                "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                "route_uid": "INSTANCE.RASTER.COMPUTE",
                "stage_statuses": {
                    "cull": "success",
                    "sort": "success",
                    "raster": "success",
                    "composite": "success",
                },
                "fallback_counters": {
                    "sort_sync": 0,
                    "sort_total_route": 0,
                    "sort_cached": 0,
                    "sort_identity": 0,
                    "sort_cull_order": 0,
                },
                "capture_count": 1,
                "capture_reference_match_count": 1,
                "capture_threshold_pass_count": 1,
                "capture_ssim_min": 0.99,
                "capture_psnr_min": 40.0,
                "gpu_timing_available": False,
                "gpu_frame_time_source": "unavailable",
                "exit_code": 0,
                "report_valid": True,
                "visible_output_valid": True,
                "visual_reference_match": True,
                "proof_valid": True,
                "lane_valid": True,
            }
            # Producer-shaped: rows under the top-level "lane_results" key.
            _write(root / "bench.json", json.dumps({"lane_results": [lane]}))
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertEqual(failures, [], failures)

            # Drop one #351 field -> the gate must now fail with the precise message.
            broken = dict(lane)
            broken["route_uid"] = None
            _write(root / "bench.json", json.dumps({"lane_results": [broken]}))
            failures = checker._validate_candidate_benchmark_report(root, manifest, evidence)
            self.assertTrue(
                any(
                    "candidate benchmark lane static_baseline has null/missing route_uid" in item
                    for item in failures
                ),
                failures,
            )

    def test_candidate_json_numbers_must_be_finite(self) -> None:
        self.assertFalse(checker._is_json_number(float("nan")))
        self.assertFalse(checker._is_json_number(float("inf")))

    def test_candidate_fails_stale_and_incomplete_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            _write(
                root / "gpu_report.json",
                json.dumps(
                    {
                        "batches": [
                            {
                                "name": "CompositorHazard",
                                "timed_out": False,
                                "summary_parse_ok": True,
                                "rc": 0,
                                "test_cases": {"total": 1, "failed": 0, "skipped": 0},
                                "assertions": {"failed": 0},
                                "rid_leak_bytes": 0,
                            }
                        ]
                    }
                ),
            )
            _write(
                root / "bench.json",
                json.dumps(
                    [
                        {
                            "lane_id": "static_baseline",
                            "commit_sha": "abc",
                            "godot_binary_commit": "abc",
                            "godot_binary_mtime_utc": "2026-05-19T10:01:00Z",
                            "capture_count": 1,
                            "capture_reference_match_count": 1,
                            "capture_ssim_min": 0.99,
                            "capture_psnr_min": 40.0,
                            "gpu_timing_available": False,
                            "gpu_frame_time_source": "unavailable",
                        }
                    ]
                ),
            )
            evidence = {
                "commit": "abc",
                "commit_time_utc": "2026-05-19T10:00:00Z",
                "gpu_harness_report": "gpu_report.json",
                "benchmark_report": "bench.json",
                "artifacts": {
                    "linux_release_archive": {
                        "path": "x",
                        "sha256": "y",
                        "godot_binary_commit": "old",
                        "godot_binary_mtime_utc": "2026-05-19T09:00:00Z",
                    }
                },
            }
            failures = checker.validate_candidate(root, manifest, evidence)
            self.assertTrue(any("built from stale commit" in item for item in failures))
            self.assertTrue(any("binary predates commit" in item for item in failures))
            self.assertTrue(any("artifact group missing: known_limitations_page" in item for item in failures))

    def test_candidate_artifact_mtime_normalizes_timezone_awareness(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["commit_time_utc"] = "2026-05-19T10:00:00Z"
            evidence["artifacts"]["linux_release_archive"]["godot_binary_mtime_utc"] = "2026-05-19T09:00:00"
            failures = checker._validate_candidate_artifacts(root, manifest, evidence)
            self.assertTrue(any("binary predates commit" in item for item in failures))

    def test_candidate_artifact_paths_must_exist(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["artifacts"]["linux_release_archive"]["path"] = "artifacts/missing.tar.xz"
            failures = checker._validate_candidate_artifacts(root, manifest, evidence)
            self.assertTrue(any("artifact linux_release_archive path missing" in item for item in failures))

    def test_candidate_artifact_paths_must_be_repo_relative(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["artifacts"]["linux_release_archive"]["path"] = str((root.parent / "release.tar.xz").resolve())
            failures = checker._validate_candidate_artifacts(root, manifest, evidence)
            self.assertTrue(any("must be repo-relative" in item for item in failures))

    def test_candidate_artifact_paths_must_not_escape_repo(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["artifacts"]["linux_release_archive"]["path"] = "../release.tar.xz"
            failures = checker._validate_candidate_artifacts(root, manifest, evidence)
            self.assertTrue(any("must not escape the repository" in item for item in failures))

    def test_candidate_report_paths_must_not_escape_repo(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["gpu_harness_report"] = "../gpu_report.json"
            failures = checker._validate_candidate_gpu_report(root, manifest, evidence)
            self.assertTrue(any("must not escape the repository" in item for item in failures))

    def test_manifest_references_must_be_repo_relative(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["references"].append(str((root.parent / "not-a-repo-reference.md").resolve()))
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(any("reference path must be repo-relative" in item for item in failures))

    def test_candidate_artifact_sha256_must_match_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["artifacts"]["linux_release_archive"]["sha256"] = "0" * 64
            failures = checker._validate_candidate_artifacts(root, manifest, evidence)
            self.assertTrue(any("artifact linux_release_archive sha256 mismatch" in item for item in failures))

    def test_candidate_artifacts_must_be_object(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = _valid_candidate_evidence(root)
            evidence["artifacts"] = []
            failures = checker._validate_candidate_artifacts(root, manifest, evidence)
            self.assertTrue(any("candidate artifacts must be a JSON object" in item for item in failures))

    def test_accepted_limitation_requires_canonical_limitations_page(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 350,
                    "title": "Known limitation",
                    "status": "accepted_alpha_limitation",
                    "goal": "Document the public alpha limitation.",
                    "docs_path": "README.md",
                    "evidence_required": ["published known limitation entry"],
                }
            )
            _write(root / "README.md", "not the known limitations page\n")
            evidence = {"issue_classifications": {}}
            issues = _base_issue_snapshot(_issue(350, ["release blocker"]))
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("accepted limitation must use known_limitations_page" in item for item in failures))

    def test_accepted_limitation_accepts_canonical_limitations_page(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 350,
                    "title": "Known limitation",
                    "status": "accepted_alpha_limitation",
                    "goal": "Document the public alpha limitation.",
                    "docs_path": "docs/development/known-public-alpha-limitations.md",
                    "evidence_required": ["published known limitation entry"],
                }
            )
            evidence = {"issue_classifications": {}}
            issues = _base_issue_snapshot(_issue(350, ["release blocker"]))
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertEqual([], failures)

    def test_candidate_relevant_p1_requires_classification(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"issue_classifications": {}}
            issues = [{"number": 222, "state": "OPEN", "labels": [{"name": "priority:P1"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("issue #222 missing alpha classification" in item for item in failures))

    def test_candidate_uses_manifest_deferred_issue_classification(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"issue_classifications": {}}
            issues = [{"number": 369, "state": "OPEN", "labels": [{"name": "priority:P0"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertEqual([], failures)

    def test_candidate_uses_manifest_tracked_issue_even_without_relevant_label(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            evidence = {"issue_classifications": {}}
            issues = [{"number": 360, "state": "OPEN", "labels": [{"name": "priority:P3"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("issue #360 is still blocking" in item for item in failures))

    def test_candidate_cannot_downgrade_manifest_blocker_with_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            evidence = {
                "issue_classifications": {
                    "360": {
                        "status": "deferred",
                        "rationale": "claimed downstream follow-up",
                        "evidence_required": ["release owner approval"],
                    }
                }
            }
            issues = [{"number": 360, "state": "OPEN", "labels": [{"name": "priority:P0"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("issue #360 is still blocking" in item for item in failures))

    def test_deferred_candidate_classification_requires_evidence_requirements(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"issue_classifications": {"222": {"status": "deferred", "rationale": "post-alpha refactor"}}}
            issues = [{"number": 222, "state": "OPEN", "labels": [{"name": "priority:P1"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("classification must be tracked in public_alpha_issue_ledger" in item for item in failures))

    def test_candidate_rejects_untracked_accepted_limitation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {
                "issue_classifications": {
                    "222": {
                        "status": "accepted_alpha_limitation",
                        "docs_path": "docs/development/known-public-alpha-limitations.md",
                        "evidence_required": ["published known limitation entry"],
                    }
                }
            }
            issues = [{"number": 222, "state": "OPEN", "labels": [{"name": "priority:P1"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("classification must be tracked in public_alpha_issue_ledger" in item for item in failures))

    def test_candidate_closed_manifest_blocker_does_not_require_open_issue(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["public_alpha_issue_ledger"]["tracked_issues"].append(
                {
                    "number": 360,
                    "title": "Public alpha gate",
                    "status": "blocking",
                    "goal": "Keep public alpha evidence explicit.",
                    "evidence_required": ["candidate mode passes"],
                }
            )
            evidence = {"issue_classifications": {}}
            issues = _base_issue_snapshot(_issue(360, ["priority:P0"], state="CLOSED"))
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertFalse(any("manifest-tracked issue #360" in item for item in failures))
            self.assertFalse(any("issue #360 is still blocking" in item for item in failures))

    def test_candidate_rejects_legacy_post_alpha_classification(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {
                "issue_classifications": {
                    "222": {
                        "status": "post_alpha",
                        "rationale": "legacy release evidence should not bypass current public-alpha policy",
                    }
                }
            }
            issues = [{"number": 222, "state": "OPEN", "labels": [{"name": "priority:P1"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("invalid classification 'post_alpha'" in item for item in failures))

    def test_candidate_rejects_unlisted_blocker_alias(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {
                "issue_classifications": {
                    "222": {
                        "status": "blocker",
                        "evidence_required": ["legacy alias should not be accepted"],
                    }
                }
            }
            issues = [{"number": 222, "state": "OPEN", "labels": [{"name": "priority:P1"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("invalid classification 'blocker'" in item for item in failures))

    def test_candidate_issue_classification_must_be_object(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"issue_classifications": {"350": "blocking"}}
            issues = [{"number": 350, "state": "OPEN", "labels": [{"name": "release blocker"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("alpha classification must be a JSON object" in item for item in failures))

    def test_candidate_issue_classifications_must_be_object(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"issue_classifications": []}
            issues = [{"number": 350, "state": "OPEN", "labels": [{"name": "release blocker"}]}]
            failures = checker._validate_candidate_issues(root, manifest, evidence, issues)
            self.assertTrue(any("candidate issue_classifications must be a JSON object" in item for item in failures))

    def test_candidate_issue_classification_uses_live_labels(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            evidence = {"artifacts": {}, "issue_classifications": {"350": {"status": "blocking"}}}
            issues = [{"number": 350, "state": "OPEN", "labels": [{"name": "release blocker"}]}]
            failures = checker.validate_candidate(root, manifest, evidence, issues)
            self.assertTrue(any("issue #350 is still blocking" in item for item in failures))


def _lifetime_stdout_all_pass() -> str:
    return "\n".join(
        [
            '[GS-LIFETIME] {"scenario":"renderer_instance","passed":true,"rd_bytes_leaked":131072,'
            '"rdm_owned_leaked":0,"rdm_tracked_leaked":0,"teardown_sync":true,'
            '"threshold_bytes":4194304,"stringname_orphan_delta":-1,"fail_reason":""}',
            '[GS-LIFETIME] {"scenario":"failed_init","passed":true,"rd_bytes_leaked":0,'
            '"rdm_owned_leaked":0,"rdm_tracked_leaked":0,"teardown_sync":false,'
            '"threshold_bytes":4194304,"stringname_orphan_delta":-1,"fail_reason":""}',
            '[GS-LIFETIME] {"scenario":"scene_director_reload","passed":true,"rd_bytes_leaked":0,'
            '"rdm_owned_leaked":0,"rdm_tracked_leaked":0,"teardown_sync":true,'
            '"threshold_bytes":262144,"stringname_orphan_delta":-1,"fail_reason":""}',
            '[GS-LIFETIME] {"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":32768,'
            '"rdm_owned_leaked":0,"rdm_tracked_leaked":0,"teardown_sync":true,'
            '"threshold_bytes":65536,"stringname_orphan_delta":-1,"fail_reason":""}',
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"threshold_orphans":5,"fail_reason":""}',
        ]
    )


def _lifetime_manifest_section() -> dict[str, Any]:
    return {
        "stdout_marker": "[GS-LIFETIME] ",
        "required_scenarios": [
            "renderer_instance",
            "failed_init",
            "scene_director_reload",
            "asset_attach_detach",
            "stringname_orphans",
        ],
        "scenario_metric_fields": {
            "renderer_instance": "rd_bytes_leaked",
            "failed_init": "rd_bytes_leaked",
            "scene_director_reload": "rd_bytes_leaked",
            "asset_attach_detach": "rd_bytes_leaked",
        },
        "thresholds_bytes": {
            "renderer_instance": 4194304,
            "failed_init": 4194304,
            "scene_director_reload": 262144,
            "asset_attach_detach": 65536,
        },
        "thresholds_counts": {"stringname_orphans_max": 5},
        "advisory_fields": ["stringname_orphan_delta"],
        "advisory_sentinel_value": -1,
        "advisory_fields_strict_for": {
            "stringname_orphans": ["stringname_orphan_delta"],
        },
    }


class LifetimeAccountingProofTests(unittest.TestCase):
    def test_lifetime_accounting_proof_all_pass(self) -> None:
        section = _lifetime_manifest_section()
        passed, reasons = checker.validate_lifetime_accounting_proof(section, _lifetime_stdout_all_pass())
        self.assertTrue(passed, reasons)
        self.assertEqual([], reasons)

    def test_lifetime_accounting_proof_missing_scenario(self) -> None:
        section = _lifetime_manifest_section()
        stdout_lines = [
            line for line in _lifetime_stdout_all_pass().splitlines() if "stringname_orphans" not in line
        ]
        passed, reasons = checker.validate_lifetime_accounting_proof(section, "\n".join(stdout_lines))
        self.assertFalse(passed)
        self.assertTrue(any("stringname_orphans" in reason and "missing" in reason for reason in reasons))

    def test_lifetime_accounting_proof_scenario_failed(self) -> None:
        section = _lifetime_manifest_section()
        stdout = (
            '[GS-LIFETIME] {"scenario":"renderer_instance","passed":false,"rd_bytes_leaked":131072,'
            '"stringname_orphan_delta":-1,"fail_reason":"owned RD resources retained at teardown"}\n'
            '[GS-LIFETIME] {"scenario":"failed_init","passed":true,"rd_bytes_leaked":0,'
            '"stringname_orphan_delta":-1,"fail_reason":""}\n'
            '[GS-LIFETIME] {"scenario":"scene_director_reload","passed":true,"rd_bytes_leaked":0,'
            '"stringname_orphan_delta":-1,"fail_reason":""}\n'
            '[GS-LIFETIME] {"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":0,'
            '"stringname_orphan_delta":-1,"fail_reason":""}\n'
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"fail_reason":""}\n'
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(passed)
        self.assertTrue(
            any(
                "renderer_instance" in reason and "passed=false" in reason and "owned RD" in reason
                for reason in reasons
            )
        )

    def test_lifetime_accounting_proof_threshold_exceeded(self) -> None:
        section = _lifetime_manifest_section()
        stdout = _lifetime_stdout_all_pass().replace(
            '"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":32768',
            '"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":1048576',
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(passed)
        self.assertTrue(
            any(
                "asset_attach_detach" in reason and "exceeds threshold" in reason
                for reason in reasons
            )
        )

    def test_lifetime_accounting_proof_advisory_sentinel(self) -> None:
        section = _lifetime_manifest_section()
        passed, reasons = checker.validate_lifetime_accounting_proof(section, _lifetime_stdout_all_pass())
        self.assertTrue(passed, reasons)
        # The renderer_instance scenario reports stringname_orphan_delta=-1; it
        # must not be counted as an orphan-threshold violation.
        self.assertFalse(any("stringname_orphan_delta" in reason for reason in reasons))

    def test_lifetime_accounting_proof_advisory_real_value(self) -> None:
        section = _lifetime_manifest_section()
        stdout = _lifetime_stdout_all_pass().replace(
            '"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"threshold_orphans":5',
            '"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":99,"threshold_orphans":5',
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(passed)
        self.assertTrue(
            any(
                "stringname_orphans" in reason
                and "stringname_orphan_delta=99" in reason
                and "exceeds threshold 5" in reason
                for reason in reasons
            )
        )

    def test_lifetime_accounting_proof_missing_required_advisory_field(self) -> None:
        """A strict-required advisory field MUST be present in its scenario.

        Regression guard for PR #390 review: when stringname_orphans drops the
        stringname_orphan_delta field entirely, the validator previously
        passed silently and the count threshold was never enforced.
        """
        section = _lifetime_manifest_section()
        # Replace the stringname_orphans line with one that drops the
        # advisory field entirely (mirrors the bot-supplied bad-stdout).
        stdout = _lifetime_stdout_all_pass().replace(
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"threshold_orphans":5,"fail_reason":""}',
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,"fail_reason":""}',
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(passed, "missing strict-required advisory field must fail the gate")
        self.assertTrue(
            any(
                "stringname_orphans" in reason
                and "stringname_orphan_delta" in reason
                and "required advisory field" in reason
                for reason in reasons
            ),
            f"expected a clear missing-required-advisory failure, got: {reasons!r}",
        )

    def test_lifetime_accounting_proof_sentinel_in_non_strict_scenario_still_passes(self) -> None:
        """The renderer_instance scenario reports stringname_orphan_delta=-1
        as the sentinel ("not measured this run"). It is NOT listed in
        advisory_fields_strict_for, so the sentinel must continue to pass
        without regressing the existing tolerated-sentinel contract.
        """
        section = _lifetime_manifest_section()
        passed, reasons = checker.validate_lifetime_accounting_proof(section, _lifetime_stdout_all_pass())
        self.assertTrue(passed, reasons)
        self.assertFalse(
            any("renderer_instance" in reason and "stringname_orphan_delta" in reason for reason in reasons),
            f"sentinel in non-strict scenario must not trip the gate, got: {reasons!r}",
        )

    def test_lifetime_accounting_proof_strict_scenario_tolerates_sentinel(self) -> None:
        """For a strict-required scenario, the field MUST be present, but the
        sentinel value (-1, "not measured this run") is still tolerated as
        long as the field itself is reported. This preserves the ability of
        the lifetime fixture to emit the field even when the orphan probe is
        skipped, while closing the silent-skip hole.
        """
        section = _lifetime_manifest_section()
        stdout = _lifetime_stdout_all_pass().replace(
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"threshold_orphans":5,"fail_reason":""}',
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":-1,"threshold_orphans":5,"fail_reason":""}',
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertTrue(passed, reasons)

    def test_lifetime_accounting_proof_manifest_schema_valid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            failures = checker.validate_contract(root, manifest)
            self.assertEqual(
                [],
                [failure for failure in failures if "lifetime_accounting_proof" in failure],
            )

    def test_lifetime_accounting_proof_manifest_schema_missing_required_field(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            manifest["lifetime_accounting_proof"].pop("required_scenarios")
            failures = checker.validate_contract(root, manifest)
            self.assertTrue(
                any(
                    "lifetime_accounting_proof.required_scenarios" in failure
                    for failure in failures
                )
            )

    def test_lifetime_accounting_proof_reads_from_file_path(self) -> None:
        section = _lifetime_manifest_section()
        with tempfile.TemporaryDirectory() as tmp:
            stdout_path = Path(tmp) / "lifetime.log"
            stdout_path.write_text(_lifetime_stdout_all_pass() + "\n", encoding="utf-8")
            passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout_path)
            self.assertTrue(passed, reasons)

    def test_lifetime_accounting_proof_rejects_invalid_json_payload(self) -> None:
        section = _lifetime_manifest_section()
        stdout = _lifetime_stdout_all_pass() + "\n[GS-LIFETIME] {not valid json"
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(passed)
        self.assertTrue(any("is not valid JSON" in reason for reason in reasons))

    def test_lifetime_accounting_proof_missing_manifest_section(self) -> None:
        manifest = {"schema_version": 1}
        failures = checker._validate_lifetime_accounting_proof_schema(manifest)
        self.assertTrue(any("lifetime_accounting_proof section is missing" in failure for failure in failures))

    def test_lifetime_accounting_proof_advisory_fields_strict_for_schema(self) -> None:
        """Schema validation must reject malformed advisory_fields_strict_for."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            # Map value must be a non-empty list of strings.
            manifest["lifetime_accounting_proof"]["advisory_fields_strict_for"] = {
                "stringname_orphans": [],
            }
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "advisory_fields_strict_for" in failure
                    and "stringname_orphans" in failure
                    and "non-empty list" in failure
                    for failure in failures
                ),
                f"expected empty-list to be rejected, got: {failures!r}",
            )
            manifest["lifetime_accounting_proof"]["advisory_fields_strict_for"] = "not-an-object"
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "advisory_fields_strict_for must be a JSON object" in failure
                    for failure in failures
                ),
                f"expected non-object to be rejected, got: {failures!r}",
            )

    def test_lifetime_accounting_proof_strict_binding_required_when_advisory_listed(self) -> None:
        """Cross-field schema invariant (Codex P2 review on PR #390).

        When advisory_fields lists stringname_orphan_delta and
        thresholds_counts declares stringname_orphans_max, the schema MUST
        require advisory_fields_strict_for to map
        stringname_orphans -> [stringname_orphan_delta]. Otherwise the
        manifest can silently drop the field from the entry at runtime and
        the orphan-count threshold is never enforced (the runtime fix in
        commit c3ec582e14 only catches it when the map is actually present).

        Covers both failure modes:
          (a) advisory_fields_strict_for missing entirely.
          (b) advisory_fields_strict_for exists but does not list
              stringname_orphans -> [stringname_orphan_delta].
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            # (a) advisory_fields_strict_for missing entirely.
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            self.assertIn(
                "stringname_orphan_delta",
                section["advisory_fields"],
                "test precondition: base manifest lists the advisory field",
            )
            self.assertIn(
                "stringname_orphans_max",
                section["thresholds_counts"],
                "test precondition: base manifest declares the count threshold",
            )
            del section["advisory_fields_strict_for"]
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "advisory_fields_strict_for" in failure
                    and "stringname_orphan_delta" in failure
                    and "stringname_orphans" in failure
                    and "silently disablable" in failure
                    for failure in failures
                ),
                f"expected missing strict map to be rejected, got: {failures!r}",
            )

            # (b) advisory_fields_strict_for exists but does not list
            # stringname_orphans -> [stringname_orphan_delta]. Use a different
            # (placeholder) scenario so the map is structurally valid.
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            section["advisory_fields_strict_for"] = {
                "renderer_instance": ["stringname_orphan_delta"],
            }
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "advisory_fields_strict_for" in failure
                    and "stringname_orphan_delta" in failure
                    and "stringname_orphans" in failure
                    and "silently disablable" in failure
                    for failure in failures
                ),
                f"expected wrong-scenario strict map to be rejected, got: {failures!r}",
            )

            # (b') strict map lists the right scenario but the wrong field.
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            section["advisory_fields_strict_for"] = {
                "stringname_orphans": ["some_other_advisory_field"],
            }
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "advisory_fields_strict_for" in failure
                    and "stringname_orphan_delta" in failure
                    and "stringname_orphans" in failure
                    and "silently disablable" in failure
                    for failure in failures
                ),
                f"expected wrong-field strict map to be rejected, got: {failures!r}",
            )

            # Positive control: the base manifest (which DOES bind the field
            # correctly) must still pass cleanly.
            manifest = _base_manifest(root)
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertEqual(
                failures,
                [],
                f"base manifest must satisfy the new cross-field invariant, got: {failures!r}",
            )

    def test_lifetime_accounting_proof_rejects_duplicate_required_scenario(self) -> None:
        """Codex P1 review on PR #390: required scenarios must appear EXACTLY
        once. Two passing stringname_orphans entries -- as would result from
        concatenated or stale logs from a previous run -- must trip the gate.

        Without this check, an old successful run could contribute a
        stringname_orphans line even when the current run omitted the
        scenario, and required coverage would be silently satisfied.
        """
        section = _lifetime_manifest_section()
        duplicate_line = (
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"threshold_orphans":5,"fail_reason":""}'
        )
        stdout = _lifetime_stdout_all_pass() + "\n" + duplicate_line
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(
            passed,
            "duplicate required-scenario entries must fail the gate",
        )
        self.assertTrue(
            any(
                "stringname_orphans" in reason
                and "2 entries" in reason
                and "exactly one" in reason
                and ("duplicate" in reason or "stale" in reason)
                for reason in reasons
            ),
            f"expected duplicate-scenario error citing the count, got: {reasons!r}",
        )

    def test_lifetime_accounting_proof_rejects_duplicate_with_mixed_pass_fail(self) -> None:
        """Strictest interpretation of Codex P1: duplicates with any mix of
        passed=true/false are rejected. The bot's concern is mixed-run
        artifacts, so any duplicate is suspect even when one entry happens
        to be failing.
        """
        section = _lifetime_manifest_section()
        duplicate_failing_line = (
            '[GS-LIFETIME] {"scenario":"stringname_orphans","passed":false,'
            '"stringname_orphan_delta":0,"threshold_orphans":5,'
            '"fail_reason":"stale log from previous run"}'
        )
        stdout = _lifetime_stdout_all_pass() + "\n" + duplicate_failing_line
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(passed)
        self.assertTrue(
            any(
                "stringname_orphans" in reason
                and "2 entries" in reason
                and "exactly one" in reason
                for reason in reasons
            ),
            f"expected duplicate-scenario error even with mixed passed flags, got: {reasons!r}",
        )

    def test_lifetime_accounting_proof_schema_requires_orphan_threshold_when_advisory_listed(self) -> None:
        """Codex P2 review on PR #390 (schema side).

        When advisory_fields lists stringname_orphan_delta, the schema MUST
        require thresholds_counts to declare stringname_orphans_max.
        Otherwise a manifest typo or accidental key removal silently
        disables the orphan-count guard while still letting the runtime
        accept arbitrarily large deltas (passed=true with no enforcement).
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            # Precondition: base manifest lists the advisory field.
            self.assertIn("stringname_orphan_delta", section["advisory_fields"])
            # Remove the orphan-count threshold from thresholds_counts.
            section["thresholds_counts"] = {}
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "thresholds_counts" in failure
                    and "stringname_orphans_max" in failure
                    and "stringname_orphan_delta" in failure
                    and "silently disabled" in failure
                    for failure in failures
                ),
                f"expected missing-threshold-despite-advisory to be rejected, got: {failures!r}",
            )

    def test_lifetime_accounting_proof_runtime_rejects_delta_without_threshold(self) -> None:
        """Codex P2 review on PR #390 (runtime defense-in-depth side).

        Even if someone edits the schema check out, the runtime must refuse
        to silently skip orphan-delta enforcement when no threshold is
        declared. An entry that reports a real (non-sentinel) delta with no
        threshold in thresholds_counts must fail the lifetime gate instead
        of passing because count_threshold is None.
        """
        section = _lifetime_manifest_section()
        # Remove the threshold from the manifest section so the runtime
        # lookup returns None even though stringname_orphan_delta is in
        # the entry. The schema validator would normally reject this; we
        # are testing the runtime fallback behaviour.
        section["thresholds_counts"] = {}
        # Use a real (non-sentinel) value so the sentinel-skip branch
        # does not short-circuit the check.
        stdout = _lifetime_stdout_all_pass().replace(
            '"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":0,"threshold_orphans":5',
            '"scenario":"stringname_orphans","passed":true,'
            '"stringname_orphan_delta":42,"threshold_orphans":5',
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(
            passed,
            "runtime must refuse to silently skip enforcement when threshold is missing",
        )
        self.assertTrue(
            any(
                "stringname_orphans" in reason
                and "stringname_orphan_delta=42" in reason
                and "stringname_orphans_max" in reason
                and "not declared" in reason
                for reason in reasons
            ),
            f"expected runtime defense-in-depth error citing the missing threshold, got: {reasons!r}",
        )

    def test_main_lifetime_mode_validates_schema_before_runtime(self) -> None:
        """Codex P2 review on PR #390 (comment #3294976919).

        main()'s --mode lifetime branch must run schema validation BEFORE
        calling validate_lifetime_accounting_proof, so a manifest missing
        advisory_fields_strict_for cannot silently pass standalone lifetime
        runs. Pre-fix, the runtime treated stringname_orphan_delta as
        optional and returned passed=True for such manifests, disabling
        the orphan-count guard while still exiting 0.

        Strict interpretation: there is no opt-out flag -- any workflow
        that invokes --mode lifetime must satisfy the same schema
        invariants that --mode contract enforces.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # main() computes root = manifest_path.parents[2], so place
            # the manifest two directories deep so the resolved root points
            # at the tempdir. The lifetime path does not read other files
            # from root, so the tempdir itself can stay empty.
            manifest_path = root / "docs" / "reference" / "renderer_release_gate_manifest.json"
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            section = _lifetime_manifest_section()
            # Drop advisory_fields_strict_for entirely while leaving
            # advisory_fields listing stringname_orphan_delta. Pre-fix,
            # the schema check did not run in lifetime mode and the
            # runtime accepted the manifest because the missing strict
            # map silently disabled the bound enforcement.
            section.pop("advisory_fields_strict_for", None)
            manifest = {"lifetime_accounting_proof": section}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            stdout_path = root / "lifetime.log"
            stdout_path.write_text(_lifetime_stdout_all_pass() + "\n", encoding="utf-8")

            from contextlib import redirect_stdout
            import io

            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = checker.main(
                    [
                        "--mode",
                        "lifetime",
                        "--manifest",
                        str(manifest_path),
                        "--lifetime-stdout",
                        str(stdout_path),
                    ]
                )
            output = buf.getvalue()
            self.assertNotEqual(
                exit_code,
                0,
                f"lifetime mode must reject a manifest missing advisory_fields_strict_for; "
                f"got exit_code={exit_code} output={output!r}",
            )
            self.assertIn(
                "lifetime schema check failed",
                output,
                f"expected schema-failure header in lifetime mode output, got: {output!r}",
            )
            self.assertIn(
                "advisory_fields_strict_for",
                output,
                f"expected error to name the missing schema field, got: {output!r}",
            )
            self.assertIn(
                "stringname_orphan_delta",
                output,
                f"expected error to name the bound advisory field, got: {output!r}",
            )

    def test_schema_requires_strict_scenario_in_required_scenarios(self) -> None:
        """Codex P2 review on PR #390 (round 6, comment #3295080072).

        Rounds 3, 4, and 5 added schema invariants for the strict map and
        the count threshold, but none of them required the bound scenario
        itself to appear in required_scenarios. Removing
        stringname_orphans from required_scenarios silently disables
        orphan-threshold enforcement even when advisory_fields_strict_for
        still maps it and thresholds_counts still declares the bound
        threshold: the runtime only walks scenarios that actually appear
        in required_scenarios, so dropping it means the entry is never
        required to exist and the count threshold is never compared.

        Schema must reject at contract time, reusing the same
        ADVISORY_FIELD_STRICT_BINDINGS source of truth as the round-3/4/5
        invariants.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            # Preconditions: every other invariant is satisfied. The
            # exploit is specifically removing the scenario from
            # required_scenarios while keeping the rest intact.
            self.assertIn("stringname_orphan_delta", section["advisory_fields"])
            self.assertEqual(
                section["advisory_fields_strict_for"].get("stringname_orphans"),
                ["stringname_orphan_delta"],
                "test precondition: strict map binds the field to the scenario",
            )
            self.assertIn("stringname_orphans_max", section["thresholds_counts"])
            self.assertIn("stringname_orphans", section["required_scenarios"])
            # Drop only the scenario from required_scenarios.
            section["required_scenarios"] = [
                name for name in section["required_scenarios"] if name != "stringname_orphans"
            ]
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "required_scenarios" in failure
                    and "stringname_orphans" in failure
                    and "stringname_orphan_delta" in failure
                    and "bound scenario not required" in failure
                    for failure in failures
                ),
                f"expected missing-scenario-despite-advisory to be rejected, got: {failures!r}",
            )

    def test_lifetime_mode_rejects_missing_strict_scenario_in_required_scenarios(self) -> None:
        """Round-6 schema invariant must trip --mode lifetime too.

        Round 4 wired schema checks into the lifetime entrypoint so
        standalone lifetime runs cannot bypass invariants. The round-6
        invariant must therefore also fail lifetime mode when the bound
        scenario is missing from required_scenarios, with exit_code != 0
        before validate_lifetime_accounting_proof is even invoked.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = root / "docs" / "reference" / "renderer_release_gate_manifest.json"
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            section = _lifetime_manifest_section()
            # Drop stringname_orphans from required_scenarios while keeping
            # everything else intact (strict map + count threshold + advisory
            # field listed). Pre-fix, this would slip past the lifetime
            # gate because the runtime never walks the absent scenario.
            section["required_scenarios"] = [
                name for name in section["required_scenarios"] if name != "stringname_orphans"
            ]
            manifest = {"lifetime_accounting_proof": section}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            stdout_path = root / "lifetime.log"
            # Use a stdout artifact that also omits the scenario, matching
            # the manifest. Without the new invariant, the gate would
            # silently pass.
            stdout_lines = [
                line for line in _lifetime_stdout_all_pass().splitlines() if "stringname_orphans" not in line
            ]
            stdout_path.write_text("\n".join(stdout_lines) + "\n", encoding="utf-8")

            from contextlib import redirect_stdout
            import io

            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = checker.main(
                    [
                        "--mode",
                        "lifetime",
                        "--manifest",
                        str(manifest_path),
                        "--lifetime-stdout",
                        str(stdout_path),
                    ]
                )
            output = buf.getvalue()
            self.assertNotEqual(
                exit_code,
                0,
                f"--mode lifetime must reject a manifest whose required_scenarios "
                f"omits the bound strict scenario; got exit_code={exit_code} "
                f"output={output!r}",
            )
            self.assertIn(
                "lifetime schema check failed",
                output,
                f"expected schema-failure header in lifetime mode output, got: {output!r}",
            )
            self.assertIn(
                "required_scenarios",
                output,
                f"expected error to name the missing scenarios field, got: {output!r}",
            )
            self.assertIn(
                "stringname_orphans",
                output,
                f"expected error to name the missing bound scenario, got: {output!r}",
            )

    def test_schema_rejects_duplicate_required_scenarios(self) -> None:
        """Codex P2 review on PR #390 (comment #3295128412).

        The element-type loop in _validate_lifetime_accounting_proof_schema
        only checked that each required_scenarios entry was a non-empty
        string, so a typo replacing "failed_init" with a second
        "renderer_instance" was accepted silently. The runtime then walked
        scenarios via membership in required_scenarios, matched the
        duplicated entry twice while never requiring failed_init to appear
        in stdout, and validate_lifetime_accounting_proof returned success
        for an stdout artifact that never reported the dropped scenario --
        disabling coverage for failed_init while every other gate still
        returned success.

        Schema must reject duplicates at contract time and the failure
        message must name the duplicate so the operator can locate the
        typo immediately.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            # The bot's exploit recipe: replace failed_init with a second
            # copy of renderer_instance, leaving every other invariant
            # intact (strict map + thresholds + advisory fields).
            self.assertIn("renderer_instance", section["required_scenarios"])
            self.assertIn("failed_init", section["required_scenarios"])
            section["required_scenarios"] = [
                "renderer_instance" if name == "failed_init" else name
                for name in section["required_scenarios"]
            ]
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "required_scenarios" in failure
                    and "duplicate" in failure
                    and "renderer_instance" in failure
                    for failure in failures
                ),
                f"expected duplicate-required-scenarios to be rejected with the duplicate "
                f"value named, got: {failures!r}",
            )

    def test_lifetime_mode_rejects_duplicate_required_scenarios(self) -> None:
        """Round-4 wired the schema check into --mode lifetime so standalone
        lifetime runs cannot bypass invariants. The duplicate-rejection
        invariant added for Codex comment #3295128412 must therefore also
        fail lifetime mode, with exit_code != 0 before
        validate_lifetime_accounting_proof is invoked.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = root / "docs" / "reference" / "renderer_release_gate_manifest.json"
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            section = _lifetime_manifest_section()
            # Same exploit as the schema test: drop failed_init by
            # duplicating renderer_instance. Pre-fix, the lifetime gate
            # would accept this because the runtime never required
            # failed_init to appear in the stdout artifact.
            section["required_scenarios"] = [
                "renderer_instance" if name == "failed_init" else name
                for name in section["required_scenarios"]
            ]
            manifest = {"lifetime_accounting_proof": section}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            stdout_path = root / "lifetime.log"
            # Mirror the manifest: drop failed_init from stdout too.
            # Without the new invariant, the gate would silently pass.
            stdout_lines = [
                line for line in _lifetime_stdout_all_pass().splitlines()
                if "failed_init" not in line
            ]
            stdout_path.write_text("\n".join(stdout_lines) + "\n", encoding="utf-8")

            from contextlib import redirect_stdout
            import io

            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = checker.main(
                    [
                        "--mode",
                        "lifetime",
                        "--manifest",
                        str(manifest_path),
                        "--lifetime-stdout",
                        str(stdout_path),
                    ]
                )
            output = buf.getvalue()
            self.assertNotEqual(
                exit_code,
                0,
                f"--mode lifetime must reject a manifest whose required_scenarios "
                f"contains duplicates; got exit_code={exit_code} output={output!r}",
            )
            self.assertIn(
                "lifetime schema check failed",
                output,
                f"expected schema-failure header in lifetime mode output, got: {output!r}",
            )
            self.assertIn(
                "required_scenarios",
                output,
                f"expected error to name the required_scenarios field, got: {output!r}",
            )
            self.assertIn(
                "duplicate",
                output,
                f"expected error to mention duplicates, got: {output!r}",
            )
            self.assertIn(
                "renderer_instance",
                output,
                f"expected error to name the duplicated value, got: {output!r}",
            )

    def test_schema_requires_bytes_threshold_for_metric_scenario(self) -> None:
        """Codex P2 review on PR #390 (schema side, byte thresholds).

        The bot's exploit recipe: remove asset_attach_detach from
        thresholds_bytes while still listing it in required_scenarios and
        scenario_metric_fields, then feed a huge rd_bytes_leaked value --
        validate_lifetime_accounting_proof returned success because the
        runtime check at `if byte_threshold is not None` silently fell
        through, disabling the byte-leak guard for that scenario.

        The schema validator must reject such a manifest at contract time:
        every required scenario that declares a metric field MUST also
        declare a thresholds_bytes entry. Mirrors the orphan-count
        threshold enforcement added in 54fab045a8 but for byte thresholds.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = _base_manifest(root)
            section = manifest["lifetime_accounting_proof"]
            # Precondition: base manifest binds asset_attach_detach to
            # a metric field AND a byte threshold.
            self.assertIn("asset_attach_detach", section["required_scenarios"])
            self.assertIn("asset_attach_detach", section["scenario_metric_fields"])
            self.assertIn("asset_attach_detach", section["thresholds_bytes"])
            # The bot's exploit: drop the threshold while keeping the
            # metric-field binding and required-scenario membership.
            del section["thresholds_bytes"]["asset_attach_detach"]
            failures = checker._validate_lifetime_accounting_proof_schema(manifest)
            self.assertTrue(
                any(
                    "thresholds_bytes" in failure
                    and "asset_attach_detach" in failure
                    and "scenario_metric_fields" in failure
                    and "byte threshold not declared" in failure
                    for failure in failures
                ),
                f"expected missing-byte-threshold-despite-metric-field to be rejected, got: {failures!r}",
            )

    def test_lifetime_mode_rejects_metric_scenario_without_byte_threshold(self) -> None:
        """Codex P2 review on PR #390 (schema side, byte thresholds) via the
        --mode lifetime entrypoint added in f6932de597.

        Same exploit as test_schema_requires_bytes_threshold_for_metric_scenario,
        but routed through the lifetime-mode CLI. The schema-first guard
        added in f6932de597 means standalone lifetime runs must satisfy the
        same invariants -- removing a required byte threshold must fail
        with exit_code != 0 before validate_lifetime_accounting_proof is
        even invoked.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = root / "docs" / "reference" / "renderer_release_gate_manifest.json"
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            section = _lifetime_manifest_section()
            # Bot's exploit: drop asset_attach_detach from thresholds_bytes
            # while keeping it in required_scenarios + scenario_metric_fields.
            del section["thresholds_bytes"]["asset_attach_detach"]
            manifest = {"lifetime_accounting_proof": section}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            # Use a huge leak value, matching the bot's verification recipe.
            stdout_path = root / "lifetime.log"
            stdout_text = _lifetime_stdout_all_pass().replace(
                '"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":32768',
                '"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":999999999',
            )
            stdout_path.write_text(stdout_text + "\n", encoding="utf-8")

            from contextlib import redirect_stdout
            import io

            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = checker.main(
                    [
                        "--mode",
                        "lifetime",
                        "--manifest",
                        str(manifest_path),
                        "--lifetime-stdout",
                        str(stdout_path),
                    ]
                )
            output = buf.getvalue()
            self.assertNotEqual(
                exit_code,
                0,
                f"--mode lifetime must reject a manifest missing a required byte "
                f"threshold; got exit_code={exit_code} output={output!r}",
            )
            self.assertIn(
                "lifetime schema check failed",
                output,
                f"expected schema-failure header in lifetime mode output, got: {output!r}",
            )
            self.assertIn(
                "thresholds_bytes",
                output,
                f"expected error to name the missing threshold map, got: {output!r}",
            )
            self.assertIn(
                "asset_attach_detach",
                output,
                f"expected error to name the offending scenario, got: {output!r}",
            )

    def test_runtime_rejects_real_bytes_without_threshold(self) -> None:
        """Codex P2 review on PR #390 (runtime defense-in-depth, byte side).

        Even if someone edits the schema check out, the runtime must refuse
        to silently skip byte-threshold enforcement when no threshold is
        declared but the entry reports a real numeric rd_bytes_leaked
        value. Without this, the bot's exploit -- huge leak value with no
        threshold -- passes the gate.
        """
        section = _lifetime_manifest_section()
        # Bot's exploit: drop the threshold while keeping the metric-field
        # binding so the scenario still attempts byte enforcement.
        del section["thresholds_bytes"]["asset_attach_detach"]
        # Feed a huge non-sentinel leak value -- the exact recipe the bot
        # verified experimentally.
        stdout = _lifetime_stdout_all_pass().replace(
            '"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":32768',
            '"scenario":"asset_attach_detach","passed":true,"rd_bytes_leaked":999999999',
        )
        passed, reasons = checker.validate_lifetime_accounting_proof(section, stdout)
        self.assertFalse(
            passed,
            "runtime must refuse to silently skip byte enforcement when threshold is missing",
        )
        self.assertTrue(
            any(
                "asset_attach_detach" in reason
                and "rd_bytes_leaked=999999999" in reason
                and "thresholds_bytes.asset_attach_detach" in reason
                and "not declared" in reason
                for reason in reasons
            ),
            f"expected runtime defense-in-depth error citing the missing byte threshold, got: {reasons!r}",
        )

    def test_main_lifetime_mode_passes_on_valid_manifest(self) -> None:
        """Positive control for the new schema-first guard: a structurally
        valid manifest with a passing stdout artifact must still exit 0
        under --mode lifetime. Without this control, a regression that
        accidentally rejects all manifests would go undetected.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = root / "docs" / "reference" / "renderer_release_gate_manifest.json"
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            manifest = {"lifetime_accounting_proof": _lifetime_manifest_section()}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            stdout_path = root / "lifetime.log"
            stdout_path.write_text(_lifetime_stdout_all_pass() + "\n", encoding="utf-8")

            from contextlib import redirect_stdout
            import io

            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = checker.main(
                    [
                        "--mode",
                        "lifetime",
                        "--manifest",
                        str(manifest_path),
                        "--lifetime-stdout",
                        str(stdout_path),
                    ]
                )
            self.assertEqual(
                exit_code,
                0,
                f"valid lifetime manifest must still pass; got exit_code={exit_code} "
                f"output={buf.getvalue()!r}",
            )


def _load_run_gpu_harness():
    """Load the real run_gpu_harness.py supervisor module for runtime tests."""
    script = ROOT / "tests" / "ci" / "run_gpu_harness.py"
    harness_spec = importlib.util.spec_from_file_location("run_gpu_harness", script)
    assert harness_spec and harness_spec.loader
    module = importlib.util.module_from_spec(harness_spec)
    # Register before exec so dataclass annotation resolution (Optional[int])
    # can find the module in sys.modules.
    import sys

    sys.modules["run_gpu_harness"] = module
    harness_spec.loader.exec_module(module)
    return module


class GpuHarnessLifetimeSkipGateTests(unittest.TestCase):
    """Codex PR #419 Finding 2: a skipped/failed lifetime scenario in a REQUIRED
    batch must fail the GPU-harness gate even though doctest counts a skip as a
    PASSED test case."""

    def setUp(self) -> None:
        self.harness = _load_run_gpu_harness()

    def _parse(self, stdout: str):
        result = self.harness.BatchResult(name="Lifetime", filters=("x",))
        result.rc = 0
        self.harness._parse_summary(stdout, result)
        return result

    def test_lifetime_skip_marker_is_collected(self) -> None:
        stdout = "\n".join(
            [
                '[GS-LIFETIME] {"scenario":"renderer_instance","passed":true,"fail_reason":""}',
                '[GS-LIFETIME] {"scenario":"tile_shader_recompile","passed":false,'
                '"fail_reason":"skipped: tile binning pipeline not compiled in this harness"}',
                "[doctest] test cases:  4 |  4 passed |  0 failed |  0 skipped",
                "[doctest] assertions: 10 | 10 passed |  0 failed |",
                "[doctest] Status: SUCCESS!",
            ]
        )
        result = self._parse(stdout)
        self.assertEqual(
            result.lifetime_failed_scenarios,
            [
                "tile_shader_recompile: skipped: tile binning pipeline not "
                "compiled in this harness"
            ],
        )
        # doctest still reports the skip as a PASSED case — proving the
        # doctest-rc/failed-count gate alone would have greened this run.
        self.assertEqual(result.test_cases_failed, 0)
        self.assertTrue(result.summary_parse_ok)

    def test_lifetime_all_pass_collects_nothing(self) -> None:
        stdout = "\n".join(
            [
                '[GS-LIFETIME] {"scenario":"renderer_instance","passed":true,"fail_reason":""}',
                '[GS-LIFETIME] {"scenario":"tile_shader_recompile","passed":true,"fail_reason":""}',
            ]
        )
        self.assertEqual(self._parse(stdout).lifetime_failed_scenarios, [])

    def test_lifetime_real_failure_is_collected(self) -> None:
        stdout = (
            '[GS-LIFETIME] {"scenario":"renderer_instance","passed":false,'
            '"fail_reason":"rd_bytes_leaked=8388608 >= threshold=4194304"}'
        )
        self.assertEqual(
            self._parse(stdout).lifetime_failed_scenarios,
            ["renderer_instance: rd_bytes_leaked=8388608 >= threshold=4194304"],
        )

    def test_unparseable_lifetime_line_is_flagged(self) -> None:
        stdout = "[GS-LIFETIME] {not valid json"
        failures = self._parse(stdout).lifetime_failed_scenarios
        self.assertEqual(len(failures), 1)
        self.assertIn("unparseable", failures[0])

    def test_lifetime_field_serialized_in_report(self) -> None:
        result = self.harness.BatchResult(name="Lifetime", filters=("x",))
        result.lifetime_failed_scenarios = ["tile_shader_recompile: skipped: x"]
        self.assertEqual(
            result.to_dict()["lifetime_failed_scenarios"],
            ["tile_shader_recompile: skipped: x"],
        )

    def test_lifetime_failure_gate_only_for_required_batches(self) -> None:
        # Confirms the supervisor only escalates lifetime failures for batches
        # that are in REQUIRED_BATCHES — the gate decision logic in main().
        self.assertIn("Lifetime", self.harness.REQUIRED_BATCHES)


class CandidateSupplyChainBindingTests(unittest.TestCase):
    """The evidence bundle must describe the commit AND the bytes being shipped.

    Before this binding existed, `--mode candidate` only proved the bundle was
    internally consistent, so a stale bundle from an older validated commit could
    authorize publishing a newer, never-validated build (#593 review).
    """

    def _fixture(self) -> tuple[Path, dict[str, Any], dict[str, Any]]:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        manifest = _base_manifest(root)
        evidence = _valid_candidate_evidence(root)
        return root, manifest, evidence

    def test_expected_commit_accepts_matching_bundle(self) -> None:
        root, manifest, evidence = self._fixture()
        failures = checker._candidate_expected_commit_failures(evidence, "ABC")
        self.assertEqual([], failures, "commit comparison must be case-insensitive on hex")

    def test_stale_bundle_for_another_commit_fails_closed(self) -> None:
        root, manifest, evidence = self._fixture()
        failures = checker._candidate_expected_commit_failures(evidence, "deadbeef")
        self.assertTrue(any("does not match the commit being published" in f for f in failures))

    def test_missing_commit_cannot_satisfy_binding(self) -> None:
        root, manifest, evidence = self._fixture()
        evidence["commit"] = ""
        failures = checker._candidate_expected_commit_failures(evidence, "deadbeef")
        self.assertTrue(any("no commit to bind" in f for f in failures))

    def test_no_expected_commit_keeps_legacy_behaviour(self) -> None:
        root, manifest, evidence = self._fixture()
        self.assertEqual([], checker._candidate_expected_commit_failures(evidence, None))

    def test_artifact_digest_binding_rejects_different_bytes(self) -> None:
        root, manifest, evidence = self._fixture()
        recorded = evidence["artifacts"]["linux_release_archive"]["sha256"]
        self.assertEqual(
            [],
            checker._candidate_expected_artifact_sha_failures(
                evidence, {"linux_release_archive": recorded}
            ),
        )
        failures = checker._candidate_expected_artifact_sha_failures(
            evidence, {"linux_release_archive": "0" * 64}
        )
        self.assertTrue(any("not the bytes being published" in f for f in failures))

    def test_artifact_digest_binding_rejects_missing_group(self) -> None:
        root, manifest, evidence = self._fixture()
        failures = checker._candidate_expected_artifact_sha_failures(
            evidence, {"windows_release_archive": "1" * 64}
        )
        self.assertTrue(any("missing artifact group windows_release_archive" in f for f in failures))

    def test_validate_candidate_threads_bindings_through(self) -> None:
        root, manifest, evidence = self._fixture()
        failures = checker.validate_candidate(
            root,
            manifest,
            evidence,
            [],
            expected_commit="not-the-evidence-commit",
            expected_artifact_shas={"linux_release_archive": "2" * 64},
        )
        self.assertTrue(any("does not match the commit being published" in f for f in failures))
        self.assertTrue(any("not the bytes being published" in f for f in failures))

    def test_artifact_sha_cli_arg_rejects_malformed_values(self) -> None:
        with self.assertRaises(Exception):
            checker._parse_artifact_sha_arg("linux_release_archive")
        with self.assertRaises(Exception):
            checker._parse_artifact_sha_arg("linux_release_archive=nothex")
        self.assertEqual(
            ("linux_release_archive", "a" * 64),
            checker._parse_artifact_sha_arg("linux_release_archive=" + "A" * 64),
        )


class GpuHarnessCaseAssertAuditTests(unittest.TestCase):
    """#695: the supervisor must scrape the listener's per-case assertion audit.

    doctest's `skipped` column measures filter reach, not runtime skipping, so
    the harness cannot learn from its summary whether a case that reported PASSED
    actually asserted anything. The listener emits that per case; these tests pin
    the scrape and the required-batch escalation.
    """

    def setUp(self) -> None:
        self.harness = _load_run_gpu_harness()

    def _parse(self, stdout: str, name: str = "RendererPipeline"):
        result = self.harness.BatchResult(name=name, filters=("x",))
        result.rc = 0
        self.harness._parse_summary(stdout, result)
        return result

    def test_no_asserts_lines_are_collected_and_named(self) -> None:
        stdout = "\n".join(
            [
                "[GS-GPU][NO-ASSERTS] test=Stage results report cull/sort skipped",
                "[GS-GPU][NO-ASSERTS] test=Serial instancing failure injection",
                "[GS-GPU][CASE-ASSERT-AUDIT] started=4 zero_assert=2",
                # doctest's own view of the same run: nothing failed, and the
                # `skipped` column is four digits of pure filter arithmetic.
                "[doctest] test cases:  4 |  4 passed |  0 failed |  2004 skipped",
                "[doctest] assertions: 8 |  8 passed |  0 failed |",
                "[doctest] Status: SUCCESS!",
            ]
        )
        result = self._parse(stdout)
        self.assertEqual(
            result.zero_assertion_cases,
            [
                "Stage results report cull/sort skipped",
                "Serial instancing failure injection",
            ],
        )
        self.assertTrue(result.case_assert_audit_ok)
        self.assertEqual(result.cases_started, 4)
        # The signal is invisible in every counter doctest reports: the batch is
        # green, asserts non-zero, and `skipped` says 2004 whether or not any
        # case was hollow. That is precisely why the listener has to say it.
        self.assertEqual(result.test_cases_failed, 0)
        self.assertEqual(result.assertions_total, 8)
        self.assertEqual(result.test_cases_skipped, 2004)

    def test_clean_batch_records_audit_with_no_hollow_cases(self) -> None:
        stdout = "\n".join(
            [
                "[GS-GPU][CASE-ASSERT-AUDIT] started=1 zero_assert=0",
                "[doctest] test cases:  1 |  1 passed |  0 failed |  2007 skipped",
                "[doctest] assertions: 20 | 20 passed |  0 failed |",
                "[doctest] Status: SUCCESS!",
            ]
        )
        result = self._parse(stdout, name="CompositorHazard")
        self.assertEqual(result.zero_assertion_cases, [])
        self.assertTrue(result.case_assert_audit_ok)
        self.assertEqual(result.cases_started, 1)

    def test_audit_absent_when_binary_predates_the_listener(self) -> None:
        """No marker -> `case_assert_audit_ok` stays False, so the gate fails closed."""
        stdout = "\n".join(
            [
                "[doctest] test cases:  1 |  1 passed |  0 failed |  2007 skipped",
                "[doctest] assertions: 20 | 20 passed |  0 failed |",
                "[doctest] Status: SUCCESS!",
            ]
        )
        result = self._parse(stdout, name="CompositorHazard")
        self.assertFalse(result.case_assert_audit_ok)
        self.assertTrue(result.summary_parse_ok)

    def test_audit_fields_serialized_in_report(self) -> None:
        result = self.harness.BatchResult(name="RendererPipeline", filters=("x",))
        result.zero_assertion_cases = ["case b"]
        result.case_assert_audit_ok = True
        result.cases_started = 4
        row = result.to_dict()
        self.assertEqual(row["zero_assertion_cases"], ["case b"])
        self.assertIs(row["case_assert_audit_ok"], True)
        self.assertEqual(row["cases_started"], 4)


if __name__ == "__main__":
    unittest.main()
