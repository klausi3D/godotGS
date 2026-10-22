#!/usr/bin/env python3
"""Wiring tests for tests/ci/check_gaussian_layout_sync.py (#54).

The IndirectDispatch ABI pin is one function, `_check_indirect_dispatch_abi`, invoked by one
line in `main()`. Deleting that line used to leave the script exiting 0 with every ABI and
binding check unreachable -- and its coverage summary was printed from the pin lists, not from
anything the check did, so the output looked identical (Codex review on PR #1041). These tests
drive the REAL `main()` against the committed tree and assert the check was reached and
examined something; `run_module_tests.py` runs this file before the guard itself.

The HLOD oracle coverage tests also mutate copies of the real declarations/comparator and
exercise the existing runner wiring. No invented declaration inventory stands in for the source.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_gaussian_layout_sync.py"
spec = importlib.util.spec_from_file_location("check_gaussian_layout_sync", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)


class IndirectDispatchAbiWiringTests(unittest.TestCase):
    def _run_main_recording_abi_calls(self) -> tuple[list[dict[str, int]], str]:
        real = guard._check_indirect_dispatch_abi
        results: list[dict[str, int]] = []

        def recording(failures: list[str]) -> dict[str, int]:
            coverage = real(failures)
            results.append(coverage)
            return coverage

        stdout = io.StringIO()
        with mock.patch.object(guard, "_check_indirect_dispatch_abi", recording):
            with contextlib.redirect_stdout(stdout):
                guard.main()
        return results, stdout.getvalue()

    def test_main_reaches_the_indirect_dispatch_abi_check(self) -> None:
        results, _ = self._run_main_recording_abi_calls()
        self.assertEqual(
            len(results),
            1,
            "main() must call _check_indirect_dispatch_abi exactly once; without that call every "
            "IndirectDispatch ABI and binding pin is unreachable while the guard still exits 0",
        )

    def test_the_check_examined_every_kind_of_site(self) -> None:
        """Reached is not enough: a check that examined nothing is equally decorative."""
        results, _ = self._run_main_recording_abi_calls()
        if len(results) != 1:
            self.fail(f"expected one _check_indirect_dispatch_abi call, got {len(results)}")
            return
        coverage = results[0]
        for key in ("glsl_declarations", "host_binding_sites", "host_ctor_binding_sites", "host_struct_mirrors"):
            with self.subTest(key=key):
                self.assertGreater(coverage.get(key, 0), 0, f"_check_indirect_dispatch_abi examined no {key}")

    def test_summary_reports_what_the_check_examined(self) -> None:
        """The PASSED summary must be derived from the check's own result, not the pin lists,
        so it cannot claim coverage for a check that did not run."""
        results, output = self._run_main_recording_abi_calls()
        if len(results) != 1:
            self.fail(f"expected one _check_indirect_dispatch_abi call, got {len(results)}")
            return
        coverage = results[0]
        match = re.search(
            r"IndirectDispatchLayout is pinned across (\d+) GLSL declaration\(s\) .*, (\d+) host append_id "
            r"binding site\(s\), (\d+) host constructor binding site\(s\), and (\d+) host struct mirror\(s\)",
            output,
        )
        if match is None:
            self.fail(f"no IndirectDispatch coverage line in guard output:\n{output}")
            return
        self.assertEqual(
            tuple(int(group) for group in match.groups()),
            (
                coverage["glsl_declarations"],
                coverage["host_binding_sites"],
                coverage["host_ctor_binding_sites"],
                coverage["host_struct_mirrors"],
            ),
        )


class QuantizedStorageWiringTests(unittest.TestCase):
    def test_main_builds_the_quantized_contract_from_host_storage(self) -> None:
        with mock.patch.object(guard, "_build_quantized_expected_layout", wraps=guard._build_quantized_expected_layout) as build:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(guard.main(), 0)
        build.assert_called_once()

    def test_main_compares_the_actual_quantized_shader_storage(self) -> None:
        real = guard._compare_layouts
        compared = []

        def recording(expected, actual, source_path, source_struct_name, expected_label, failures):
            if source_struct_name == "GaussianQuantized":
                compared.append((expected.size, actual.size, tuple(actual.fields)))
            return real(expected, actual, source_path, source_struct_name, expected_label, failures)

        with mock.patch.object(guard, "_compare_layouts", recording):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(guard.main(), 0)
        self.assertGreater(len(compared), 0, "the quantized storage contract must be compared with a real shader")
        for expected_size, actual_size, fields in compared:
            self.assertEqual(expected_size, 160)
            self.assertEqual(actual_size, 160)
            self.assertIn(guard.FlatField("sh_encoded", "uint", 24), fields)
            self.assertIn(guard.FlatField("_tail_padding", "uint", 2), fields)


class HlodCutSemanticCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.declaration = guard.GAUSSIAN_DATA_H.read_text(encoding="utf-8")
        cls.oracle = guard.HLOD_TEST_H.read_text(encoding="utf-8")

    def _run_sources(self, declaration: str, oracle: str, *, through_main: bool = False) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as directory:
            data_h = Path(directory) / "gaussian_data.h"
            test_h = Path(directory) / "test_gaussian_splat_hlod.h"
            data_h.write_text(declaration, encoding="utf-8")
            test_h.write_text(oracle, encoding="utf-8")
            with mock.patch.object(guard, "GAUSSIAN_DATA_H", data_h), mock.patch.object(guard, "HLOD_TEST_H", test_h):
                if not through_main:
                    failures: list[str] = []
                    guard._check_hlod_cut_semantic_coverage(failures)
                    return int(bool(failures)), "\n".join(failures)
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = guard.main()
                return result, output.getvalue()

    def test_main_reaches_oracle_check_and_reports_actual_coverage(self) -> None:
        counts = []
        real = guard._check_hlod_cut_semantic_coverage

        def recording(failures):
            count = real(failures)
            counts.append(count)
            return count

        with mock.patch.object(guard, "_check_hlod_cut_semantic_coverage", recording):
            result, output = self._run_sources(self.declaration, self.oracle, through_main=True)
        self.assertEqual(result, 0, output)
        self.assertEqual(len(counts), 1, "main must invoke the oracle coverage check exactly once")
        self.assertGreater(counts[0], 0)
        self.assertIn(f"HLOD cut oracle compares all {counts[0]} semantic Gaussian members", output)

    def test_main_propagates_missing_semantic_comparison(self) -> None:
        oracle = self.oracle.replace("hlod_cut_bits(p_a.area, p_b.area) &&", "")
        result, output = self._run_sources(self.declaration, oracle, through_main=True)
        self.assertEqual(result, 1, output)
        self.assertIn("Gaussian.area is not compared", output)

    def test_main_rejects_initialized_multiple_declarators(self) -> None:
        for original, replacement in (
            ("float area = 0.0f;", "float area = 0.0f, semantic_value = 0.0f;"),
            ("uint32_t render_meta = 0;", "uint16_t render_meta = 0, semantic_value = 0;"),
        ):
            with self.subTest(replacement=replacement):
                self.assertEqual(self.declaration.count(original), 1)
                declaration = self.declaration.replace(original, replacement)
                result, output = self._run_sources(declaration, self.oracle, through_main=True)
                self.assertEqual(result, 1, output)
                self.assertIn("HLOD cut oracle: unsupported Gaussian declaration:", output)

    def test_same_size_padding_repurpose_requires_comparison(self) -> None:
        for name in ("semantic_value", "_padding_future"):
            with self.subTest(name=name):
                declaration = self.declaration.replace("float _padding = 0.0f;", f"float {name} = 0.0f;")
                result, output = self._run_sources(declaration, self.oracle)
                self.assertEqual(result, 1, output)
                self.assertIn(f"Gaussian.{name} is not compared", output)

    def test_same_size_new_field_in_tail_padding_requires_comparison(self) -> None:
        declaration = self.declaration.replace(
            "float _padding2[2] = { 0.0f, 0.0f };",
            "float semantic_value = 0.0f;\n    float _padding2[1] = { 0.0f };",
        )
        result, output = self._run_sources(declaration, self.oracle)
        self.assertEqual(result, 1, output)
        self.assertIn("Gaussian.semantic_value is not compared", output)
        self.assertIn("exact padding declaration changed: _padding2", output)

    def test_new_field_with_comparison_passes_coverage_without_inventory_update(self) -> None:
        # Only the coverage property is exercised here; size/version gates are independent.
        declaration = self.declaration.replace("struct Gaussian {", "struct Gaussian {\n    float semantic_value = 0.0f;")
        oracle = self.oracle.replace(
            "return hlod_cut_bits(p_a.position, p_b.position)",
            "return hlod_cut_bits(p_a.semantic_value, p_b.semantic_value) && hlod_cut_bits(p_a.position, p_b.position)",
        )
        result, output = self._run_sources(declaration, oracle)
        self.assertEqual(result, 0, output)

    def test_removed_comparisons_are_not_covered_by_comments_or_other_uses(self) -> None:
        for expression, name in (
            ("hlod_cut_bits(p_a.area, p_b.area) &&", "area"),
            ("p_a.painterly_meta == p_b.painterly_meta &&", "painterly_meta"),
        ):
            with self.subTest(name=name):
                oracle = self.oracle.replace(expression, f"/* {expression} */")
                result, output = self._run_sources(self.declaration, oracle)
                self.assertEqual(result, 1, output)
                self.assertIn(f"Gaussian.{name} is not compared", output)

    def test_unsupported_comparison_and_declaration_fail_closed(self) -> None:
        for declaration, oracle in (
            (self.declaration, self.oracle.replace("p_a.area, p_b.area", "p_a.area, p_a.area")),
            (self.declaration, self.oracle.replace("p_a.area, p_b.area)", "p_a.area, p_b.area) || true")),
            (self.declaration.replace("float _padding =", "uint32_t _padding ="), self.oracle),
            (self.declaration.replace("Vector3 position;", "Vector3 position, added_position;"), self.oracle),
            (self.declaration, self.oracle.replace("inline bool hlod_cut_semantics(", "inline bool removed_comparator(")),
        ):
            with self.subTest(declaration_changed=declaration != self.declaration, oracle_changed=oracle != self.oracle):
                result, output = self._run_sources(declaration, oracle)
                self.assertEqual(result, 1, output)
                self.assertIn("HLOD cut oracle:", output)

    def test_existing_guard_lane_invokes_both_layout_scripts(self) -> None:
        runner_spec = importlib.util.spec_from_file_location("hlod_layout_runner", ROOT / "tests/ci/run_module_tests.py")
        assert runner_spec and runner_spec.loader
        runner = importlib.util.module_from_spec(runner_spec)
        sys.modules[runner_spec.name] = runner
        self.addCleanup(sys.modules.pop, runner_spec.name)
        runner_spec.loader.exec_module(runner)
        commands = []

        def command(args):
            commands.append(args)
            return 0, "", ""

        def message_guard(callback, *args, **kwargs):
            if callback is runner._run_gaussian_layout_guard:
                self.assertTrue(callback()[0])
            return None

        # Traverse the real enabled-runner list and script loop, without spawning
        # this test recursively or executing unrelated guards.
        with mock.patch.object(runner, "_run_command", command), mock.patch.object(runner, "_run_message_guard", message_guard):
            self.assertIsNone(runner._run_optional_message_guards(SimpleNamespace(skip_build_metadata_guard=False)))
        self.assertEqual(commands, [[sys.executable, str(Path(__file__).resolve())], [sys.executable, str(SCRIPT)]])


class SHSelectionWiringTests(unittest.TestCase):
    def test_current_chunk_limit_reaches_both_binning_record_paths(self) -> None:
        module = ROOT / "modules" / "gaussian_splatting"
        stage_b = (module / "compute/depth_compute.glsl").read_text(encoding="utf-8")
        binning = (module / "shaders/tile_binning.glsl").read_text(encoding="utf-8")
        # Shared registered chunks: the per-instance LOD policy is capped by the chunk limit.
        self.assertRegex(stage_b, r"instance_sh_limit\s*=\s*gs_instance_sh_limit\(uint\(params\.cull_frustum_radius\.w\s*\+\s*0\.5\),\s*instance\.lod\.x\)")
        self.assertRegex(stage_b, r"\.atlas_index\s*=\s*gs_pack_splat_ref_atlas_index\(atlas_index,\s*min\(chunk\.sh_limit,\s*instance_sh_limit\)\)")
        start = binning.index("SplatRefGPU splat_ref =")
        end = binning.index("#if defined(USE_QUANTIZED_GAUSSIANS)", start)
        common = binning[start:end]
        self.assertRegex(common, r"gaussian_idx\s*=\s*gs_splat_ref_atlas_index\(splat_ref\.atlas_index\)")
        self.assertRegex(common, r"chunk_sh_limit\s*=\s*gs_splat_ref_sh_limit\(splat_ref\.atlas_index\)")
        self.assertRegex(binning, r"sh_band_level\s*=\s*min\(sh_band_level,\s*chunk_sh_limit\)")
        # The per-Gaussian amortized SH cache is shared by every instance: reduced-limit
        # references must bypass it (PR #1205 review).
        self.assertRegex(binning, r"bool use_cache\s*=\s*sh_divisor\s*>\s*1u\s*&&\s*gs_sh_cache_allowed\(chunk_sh_limit,\s*gs_get_sh_band_level\(\)\);")

    def test_resident_raster_params_reload_runtime_sh_settings(self) -> None:
        source = (ROOT / "modules/gaussian_splatting/renderer/tile_render_stages.cpp").read_text(encoding="utf-8")
        reload = source.index("sh_config_reload_if_settings_changed();")
        band = source.index("params.sh_config[0] = static_cast<float>(g_sh_config.sh_bands);")
        self.assertLess(reload, band, "the raster params must reload SH settings before reading them")
        # A changed degree must refresh the amortized SH colour cache (PR #1205 review).
        between = source[reload:band]
        self.assertRegex(between, r"sh_config_note_band_level\(owner\.sh_cache_band_level,\s*uint32_t\(g_sh_config\.sh_bands\)\)")
        self.assertRegex(between, r"owner\.sh_cache_needs_full_update\s*=\s*true;")
        # Hidden entries are not reached by the forced pass: the whole cache is reset (PR #1205 review).
        self.assertRegex(between, r"owner\.sh_cache_buffers\.clear_color_cache\(p_resource_device\);")
        binning = (ROOT / "modules/gaussian_splatting/shaders/tile_binning.glsl").read_text(encoding="utf-8")
        self.assertRegex(binning, r"update_sh\s*=\s*phase_match\s*\|\|\s*cached_color\s*==\s*0u;")
        resources = (ROOT / "modules/gaussian_splatting/renderer/tile_render_resources.cpp").read_text(encoding="utf-8")
        clear_body = resources[resources.index("bool TileSHCacheBuffers::clear_color_cache("):]
        clear_body = clear_body[:clear_body.index("\n}\n")]
        self.assertRegex(clear_body, r"buffer_clear\(sh_color_cache,\s*0,\s*sh_color_cache_size\)")

    def test_effective_quality_is_synchronized_at_production_streaming_entry(self) -> None:
        source = (ROOT / "modules/gaussian_splatting/core/gaussian_streaming.cpp").read_text(encoding="utf-8")
        start = source.index("void GaussianStreamingSystem::update_streaming(")
        prefix = source[start:source.index("\n    if (", start)]
        self.assertRegex(prefix, r"_reload_sh_config_if_dirty\(\);\s*set_global_sh_band_level\(int\(g_sh_config\.sh_bands\)\)")


if __name__ == "__main__":
    unittest.main()
