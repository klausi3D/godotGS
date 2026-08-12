#!/usr/bin/env python3
"""Unit tests for tests/ci/check_sorter_build_signature_parity.py (#586 round-9).

The guard backs the round-9 fix: the deterministic sorter-creation latch is released when the
BUILD INPUTS change, and that release is only as good as the signature it compares. A build
input the signature does not capture is a setting the user can correct with no effect at all --
the sorter stays disabled and every translucent frame stays rejected until renderer teardown,
which is the defect round 9 exists to fix.

So what matters is that the guard FAILS on each shape of that regression, and does not simply
fail on everything:

  * `test_covered_source_passes`             -- not always-red.
  * `test_uncaptured_config_read_fails`      -- THE round-9 shape: a build input read but not
                                                captured.
  * `test_alias_read_is_seen`                -- the same, through the
                                                `const GPUSortingConfig &config = ...` alias
                                                the real probes use, which a naive scan for
                                                the global's name would miss entirely.
  * `test_unrelated_local_named_config_is_not_a_read`
                                             -- THE CONTROL for the alias rule. gpu_sorter.cpp
                                                really does have another local called `config`
                                                (the AUTO thresholds). Treating its fields as
                                                sorting-config reads would force unrelated
                                                values into the signature, and every edit to
                                                them would then cost a create_sorter().
  * `test_method_call_is_not_a_field_read`   -- accessor calls are not fields.
  * `test_unassigned_signature_member_fails` -- a member left at its default makes the
                                                comparison blind to that input.
  * `test_duplicate_assignment_fails`        -- two writes to one member discard an input.
  * `test_missing_capture_function_fails` /
    `test_member_parse_drift_fails`          -- a parser that finds nothing must FAIL.
  * `test_field_count_pin_enforced`          -- the guard cannot be hollowed out by deleting
                                                reads until it covers nothing.
  * `test_unrecorded_signature_fails` /
    `test_uncompared_signature_fails`        -- a signature nothing writes, or nothing
                                                compares, guards nothing.

Fixtures never touch the committed sources; each synthetic tree lives in a temp dir inside
ROOT and the module-level path constants and pins are patched per test.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_sorter_build_signature_parity.py"
spec = importlib.util.spec_from_file_location("check_sorter_build_signature_parity", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)

# A stand-in GPUSortingConfig with enough members to clear the parse-drift floor.
CONFIG_HEADER = """
struct GPUSortingConfig {
    float target_sort_time_ms = 2.0f;
    uint32_t max_sort_elements = 50000000;
    uint32_t max_overlap_records = 100000000;
    uint32_t max_overlap_records_adaptive_min = 100000;
    bool adaptive_overlap_budget_enabled = true;
    bool bounded_buffer_shrink_enabled = true;
    uint32_t max_raster_splats_per_tile = 65536;
    uint32_t radix_bits = 4;
    uint32_t workgroup_size = 256;
    uint32_t key_bits = 64;
    uint32_t tile_bits = 32;
    uint32_t depth_bits = 32;
    bool enable_tie_breaker = false;
    bool enable_performance_logging = false;
    uint8_t subgroup_prefix_mode = 0;
    uint32_t get_overlap_records_hard_cap() const { return max_overlap_records; }
};
"""

POLICY_HEADER = """
struct SorterBuildSignature {
    uint32_t radix_bits = 0;
    uint32_t workgroup_size = 0;
    uint8_t subgroup_prefix_mode = 0;
    uint32_t key_bits = 0;
    uint32_t tile_bits = 0;
    uint32_t depth_bits = 0;
    bool enable_tie_breaker = false;

    bool operator==(const SorterBuildSignature &p_other) const { return radix_bits == p_other.radix_bits; }
};
"""

CAPTURE = """
GaussianSplatting::SorterBuildSignature GPUSorterFactory::capture_radix_build_signature(const SortKeyConfig &p_key_config) {
    const GPUSortingConfig &config = g_gpu_sorting_config;
    GaussianSplatting::SorterBuildSignature signature;
    signature.radix_bits = config.radix_bits;
    signature.workgroup_size = config.workgroup_size;
    signature.subgroup_prefix_mode = config.subgroup_prefix_mode;
    signature.key_bits = p_key_config.key_bits;
    signature.tile_bits = p_key_config.tile_bits;
    signature.depth_bits = p_key_config.depth_bits;
    signature.enable_tie_breaker = p_key_config.enable_tie_breaker;
    return signature;
}
"""

# The reads the build path performs, in the two shapes the real file uses.
READS = """
static bool _subgroup_prefix_forced_off() {
    return g_gpu_sorting_config.subgroup_prefix_mode == GPUSortingConfig::SUBGROUP_PREFIX_FORCE_OFF;
}

SortKeyConfig SortKeyConfig::from_settings() {
    SortKeyConfig cfg;
    cfg.key_bits = (g_gpu_sorting_config.key_bits == 64) ? 64 : 32;
    cfg.tile_bits = g_gpu_sorting_config.tile_bits;
    cfg.depth_bits = g_gpu_sorting_config.depth_bits;
    cfg.enable_tie_breaker = g_gpu_sorting_config.enable_tie_breaker;
    return cfg;
}

bool RadixSort::is_supported(RenderingDevice *p_rd) {
    const GPUSortingConfig &config = g_gpu_sorting_config;
    uint32_t required = config.workgroup_size;
    return required > 0 && config.radix_bits == 4;
}
"""

RESOURCES = """
void TileGlobalSortResources::ensure_resources(uint32_t p_visible_count) {
    if (GPUSorterFactory::capture_radix_build_signature(cfg) != sorter_unavailable_build_signature) {
        sorter_unavailable_permanent = false;
    }
    sorter_unavailable_build_signature = GPUSorterFactory::capture_radix_build_signature(cfg);
}
"""

DEFAULT_FIELDS = 7


class SorterBuildSignatureParityTests(unittest.TestCase):
    def _run(
        self,
        *,
        reads: str = READS,
        capture: str = CAPTURE,
        policy: str = POLICY_HEADER,
        config: str = CONFIG_HEADER,
        resources: str = RESOURCES,
        field_pin: int | None = DEFAULT_FIELDS,
        member_pin: int | None = None,
    ) -> tuple[int, str]:
        with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
            tmp_path = Path(tmp)
            source_path = tmp_path / "gpu_sorter.cpp"
            source_path.write_text(reads + capture, encoding="utf-8")
            config_path = tmp_path / "gpu_sorting_config.h"
            config_path.write_text(config, encoding="utf-8")
            policy_path = tmp_path / "sort_fallback_policy.h"
            policy_path.write_text(policy, encoding="utf-8")
            resources_path = tmp_path / "tile_render_resources.cpp"
            resources_path.write_text(resources, encoding="utf-8")
            patches = [
                mock.patch.object(guard, "SORTER_SOURCE", source_path),
                mock.patch.object(guard, "CONFIG_HEADER", config_path),
                mock.patch.object(guard, "POLICY_HEADER", policy_path),
                mock.patch.object(guard, "RESOURCES_SOURCE", resources_path),
            ]
            if field_pin is not None:
                patches.append(mock.patch.object(guard, "EXPECTED_CONFIG_FIELDS", field_pin))
            if member_pin is not None:
                patches.append(mock.patch.object(guard, "EXPECTED_SIGNATURE_MEMBERS", member_pin))
            buffer = io.StringIO()
            with contextlib.ExitStack() as stack:
                for patch in patches:
                    stack.enter_context(patch)
                stack.enter_context(contextlib.redirect_stdout(buffer))
                code = guard.main()
            return code, buffer.getvalue()

    # --- not always-red ---------------------------------------------------------------

    def test_covered_source_passes(self):
        code, output = self._run()
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    # --- THE round-9 regression shape --------------------------------------------------

    def test_uncaptured_config_read_fails(self):
        """A build input read but not captured: the user corrects it, the signature does not
        move, the deterministic latch holds, and the correction can never take effect."""
        capture = CAPTURE.replace("    signature.radix_bits = config.radix_bits;\n", "")
        code, output = self._run(capture=capture, member_pin=7)
        self.assertEqual(code, 1, output)
        self.assertIn("does not capture radix_bits", output)

    def test_alias_read_is_seen(self):
        """The probes read through `const GPUSortingConfig &config = g_gpu_sorting_config;`.
        A guard that only scanned for the global's own name would call this covered."""
        reads = READS + """
bool RadixSort::extra_probe(RenderingDevice *p_rd) {
    const GPUSortingConfig &config = g_gpu_sorting_config;
    return config.max_raster_splats_per_tile > 0;
}
"""
        code, output = self._run(reads=reads, field_pin=8)
        self.assertEqual(code, 1, output)
        self.assertIn("max_raster_splats_per_tile", output)

    # --- THE CONTROL ------------------------------------------------------------------

    def test_unrelated_local_named_config_is_not_a_read(self):
        """gpu_sorter.cpp really does have a second local called `config` (the AUTO
        thresholds). Counting its fields would force unrelated values into the signature, and
        every edit to them would then cost a full create_sorter() and a re-latch."""
        reads = READS + """
GPUSorterFactory::AutoThresholds GPUSorterFactory::AutoThresholds::from_project_settings() {
    AutoThresholds config;
    config.bitonic_max_elements = 32768u;
    config.radix_max_elements = 1048576u;
    return config;
}
"""
        code, output = self._run(reads=reads)
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    def test_method_call_is_not_a_field_read(self):
        """An accessor is not a field, and demanding one be captured would be nonsense."""
        reads = READS + """
uint32_t some_budget() {
    return g_gpu_sorting_config.get_overlap_records_hard_cap();
}
"""
        code, output = self._run(reads=reads)
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    # --- fail-closed ------------------------------------------------------------------

    def test_unassigned_signature_member_fails(self):
        capture = CAPTURE.replace("    signature.tile_bits = p_key_config.tile_bits;\n", "")
        code, output = self._run(capture=capture)
        self.assertEqual(code, 1, output)
        self.assertIn("never assigned", output)
        self.assertIn("tile_bits", output)

    def test_duplicate_assignment_fails(self):
        capture = CAPTURE.replace(
            "    signature.tile_bits = p_key_config.tile_bits;\n",
            "    signature.tile_bits = p_key_config.tile_bits;\n"
            "    signature.tile_bits = p_key_config.depth_bits;\n",
        )
        code, output = self._run(capture=capture)
        self.assertEqual(code, 1, output)
        self.assertIn("assigned more than once", output)

    def test_missing_capture_function_fails(self):
        code, output = self._run(capture="")
        self.assertEqual(code, 1, output)
        self.assertIn("could not parse", output)

    def test_member_parse_drift_fails(self):
        code, output = self._run(config="struct GPUSortingConfig { uint32_t radix_bits = 4; };\n")
        self.assertEqual(code, 1, output)
        self.assertIn("member parse has drifted", output)

    def test_signature_member_pin_enforced(self):
        code, output = self._run(member_pin=99)
        self.assertEqual(code, 1, output)
        self.assertIn("pinned at 99", output)

    def test_field_count_pin_enforced(self):
        code, output = self._run(field_pin=99)
        self.assertEqual(code, 1, output)
        self.assertIn("covered config-field count changed", output)

    # --- the signature must still be wired --------------------------------------------

    def test_unrecorded_signature_fails(self):
        resources = RESOURCES.replace(
            "    sorter_unavailable_build_signature = GPUSorterFactory::capture_radix_build_signature(cfg);\n",
            "",
        )
        code, output = self._run(resources=resources)
        self.assertEqual(code, 1, output)
        self.assertIn("never assigns", output)

    def test_uncompared_signature_fails(self):
        resources = RESOURCES.replace(
            "GPUSorterFactory::capture_radix_build_signature(cfg) != sorter_unavailable_build_signature",
            "true",
        )
        code, output = self._run(resources=resources)
        self.assertEqual(code, 1, output)
        self.assertIn("never COMPARES", output)


if __name__ == "__main__":
    unittest.main()
