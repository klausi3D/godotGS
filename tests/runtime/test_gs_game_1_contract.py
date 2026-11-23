"""Lane wiring contracts; constructed tests, never GPU evidence."""
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
import contextlib
import importlib.util
import sys
import re
import tempfile
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class ContractTests(unittest.TestCase):
    def test_dispatch_report_only_and_no_successful_skip(self):
        text = (ROOT / ".github/workflows/gs_game_1.yml").read_text(encoding="utf-8")
        workflow = yaml.safe_load(text)
        self.assertEqual(set(workflow.get("on", workflow.get(True))), {"workflow_dispatch"})
        self.assertEqual(workflow["permissions"], {"contents": "read"})
        self.assertFalse(workflow["concurrency"]["cancel-in-progress"])
        self.assertIn("report-only", text)
        self.assertIn("tests=yes", text)
        self.assertIn("gs_game_1_lane.py", text)
        terminal = workflow["jobs"]["report"]
        self.assertEqual(terminal["runs-on"], "ubuntu-latest")
        self.assertIn("always()", terminal["if"])
        self.assertIn("success", str(terminal["steps"]))
        self.assertIn("exit 1", str(terminal["steps"]))
        artifacts = [s["with"]["path"] for j in workflow["jobs"].values()
                     for s in j["steps"] if "upload-artifact" in s.get("uses", "")]
        self.assertTrue(any("verdict.md" in p and "sha256" in p and "raw.json" in p for p in artifacts))
        self.assertNotIn("/srv/gs-testdaten", text)

    def test_guard_wiring_is_reachable(self):
        text = (ROOT / "tests/ci/run_module_tests.py").read_text()
        for name in ("test_gs_game_1_verdict.py", "test_gs_game_1_contract.py"):
            self.assertTrue(name in text, "unwired suite: " + name)
        self.assertGreater(text.count("_run_gs_game_1_tests"), 1)
        gate = (ROOT / ".github/workflows/agentic_pr_gate.yml").read_text()
        self.assertIn("--guard-only", gate)
        anchors = (ROOT / "tests/ci/test_preflight_runner_gpu_environment.py").read_text()
        self.assertIn('"gs_game_1.yml"', anchors)

    def test_controls_have_test_only_observable_bridge(self):
        text = (ROOT / "modules/gaussian_splatting/renderer/gaussian_splat_renderer_bindings.cpp").read_text()
        self.assertTrue("_test_gs_game_1_control" in text, "native bridge missing")
        self.assertTrue("_test_gs_game_1_observe" in text, "native observation missing")
        identity = (ROOT / "modules/gaussian_splatting/renderer/splat_identity_test_control.h").read_text()
        self.assertIn("GS_TEST_ADDRESS_WIND_PHASE", identity)

    def test_capture_stride_matches_the_host_layout_contract(self):
        header = (ROOT / "modules/gaussian_splatting/renderer/splat_identity_test_control.h").read_text(encoding="utf-8")
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        host = re.search(r"sizeof\(GSGame1Sample\) == ([0-9]+)", header)
        harness = re.search(r"const CAPTURE_STRIDE = ([0-9]+)", script)
        self.assertIsNotNone(host)
        self.assertIsNotNone(harness)
        self.assertEqual(host.group(1), harness.group(1))
        layout = (ROOT / "tests/ci/check_gaussian_layout_sync.py").read_text(encoding="utf-8")
        self.assertIn('"GSGame1Sample", "GSGame1Sample"', layout)

    def test_image_deadline_binds_readback_acceptance_after_await(self):
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        wait = script.split("func _wait_image() -> Image:", 1)[1].split("\nfunc ", 1)[0]
        poll = wait.index('await _native("_test_gs_game_1_image_poll")')
        accept = wait.index('if result.get("complete") == true:')
        # A readback can complete during an await that ends past the bound.
        # The loop-entry check alone cannot reject that late success.
        self.assertRegex(wait[poll:accept],
                         r"\n\t\tif Time\.get_ticks_msec\(\) > deadline:\s*\n\t\t\treturn null")
        self.assertLess(accept, wait.index("_last_image_proof = result"))

    def test_constructed_arm_envelopes_reject_partial_or_stale_evidence(self):
        # These are constructed interface inputs, not claimed producer captures.
        here = ROOT / "tests/runtime"
        sys.path.insert(0, str(here))
        try:
            spec = importlib.util.spec_from_file_location("game_lane_test", here / "gs_game_1_lane.py")
            lane = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(lane)
        finally:
            sys.path.remove(str(here))
        plan = {"job_id": "job", "control": "W1", "dynamics": True}
        valid = plan | {"run": "control-W1", "completed": True, "tests_enabled": True, "ledger_cap_verified": True}
        self.assertEqual(lane.producer_error(valid, plan, "control-W1", 0), "")
        self.assertIn("NOT_RUN", lane.producer_error(valid, plan, "control-W1", 1))
        for key, value in (("completed", False), ("job_id", "other"),
                           ("run", "stale"), ("control", "none"),
                           ("tests_enabled", False), ("ledger_cap_verified", False), ("dynamics", False)):
            with self.subTest(key=key):
                self.assertTrue(lane.producer_error(valid | {key: value}, plan, "control-W1", 0))
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            lane.save(out / "raw.json", {"rows": {}})
            self.assertEqual(lane.finalize(out, "failure"), 2)

    def test_t0_world_is_generated_in_the_auto_tree_producer_input_encoding(self):
        # Run 45026893667: the lane generated plain_v3_uncompressed and the native
        # auto-tree source refused it (input_requires_plain_v3_gsq). The encoding the
        # builder accepts is the one dispatch_lane's own auto-tree T0 producer feeds it,
        # so both producers' generator commands are captured, not hand-copied.
        here = ROOT / "tests/runtime"
        sys.path.insert(0, str(here))
        try:
            spec = importlib.util.spec_from_file_location("game_lane_encoding", here / "gs_game_1_lane.py")
            lane = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(lane)
        finally:
            sys.path.remove(str(here))
        dispatch = lane.dispatch
        calls = []

        def record(cmd, log, runner, timeout):
            calls.append(list(cmd))
            return 1  # Stop each producer after its first (generator) command.

        def generated(cmd):
            self.assertEqual(Path(cmd[1]).name, "generate_world.py")
            self.assertEqual(cmd[cmd.index("--tier") + 1], "T0")
            return cmd[cmd.index("--encoding") + 1]

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(dispatch, "_run_logged", record), \
                mock.patch.object(lane.cache, "cache_lock", lambda root, exclusive: contextlib.nullcontext()), \
                mock.patch.object(lane.shutil, "disk_usage", return_value=SimpleNamespace(free=1 << 62)):
            scratch = Path(temporary)
            with self.assertRaises(dispatch.LaneError):
                lane.prepare_world(Path("godot"), scratch / "cache", scratch / "out", True)
            with self.assertRaises(dispatch.LaneError):
                dispatch.prepare_auto_tree_t0(ROOT, Path("godot"), scratch / "auto", scratch / "auto-out")
        self.assertEqual(len(calls), 2)
        lane_encoding, producer_encoding = generated(calls[0]), generated(calls[1])
        self.assertEqual(producer_encoding, dispatch.ENCODING_GSQ)
        self.assertEqual(lane_encoding, producer_encoding)

    def test_unloadable_world_ends_the_arm_before_any_deadline_wait(self):
        # Run 45046955733: ResourceLoader.load refused the tree, _build carried on
        # with a null world, and every arm burned the 240 s settle deadline before
        # reporting "did not settle". The load result must end the arm first.
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        build = script.split("func _build() -> String:", 1)
        self.assertEqual(len(build), 2, "_build must return the world load result")
        build = build[1].split("\nfunc ", 1)[0]
        load = build.index('ResourceLoader.load(str(_plan["world"]))')
        check = re.search(r"\n\tif not loaded is GaussianSplatWorld:\n\t\treturn \"NOT_RUN: world failed to load", build[load:])
        self.assertIsNotNone(check, "a null or foreign world load is not checked")
        # Nothing that consumes the world (apply, regions) runs before the check.
        self.assertLess(load + check.start(), build.index("_world.world = loaded"))
        self.assertLess(build.index("_world.world = loaded"), build.index("_world.apply_world()"))
        run = script.split("func _run() -> void:", 1)[1].split("\nfunc ", 1)[0]
        refused = re.search(r"var refused = _build\(\)\n\tif refused != \"\":\n\t\t_raw\[\"reason\"\] = refused\n"
                            r"\t\t_save\(\)\n\t\tquit\(3\)\n\t\treturn", run)
        self.assertIsNotNone(refused, "_run does not end the arm on a refused build")
        # Ends before the first deadline-bounded wait, whose length stays unchanged.
        self.assertLess(refused.start(), run.index("var deadline = Time.get_ticks_msec() + DEADLINE_MS"))
        self.assertLess(refused.start(), run.index("await _settle()"))
        self.assertIn("const DEADLINE_MS = 240000", script)

    @staticmethod
    def _harness_function(name):
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        parts = re.split(r"\nfunc " + re.escape(name) + r"\(", script, maxsplit=1)
        if len(parts) != 2:
            return ""
        return parts[1].split("\nfunc ", 1)[0]

    def test_stall_push_constant_is_the_16_byte_block_the_pipeline_requires(self):
        # Run 45073045558: "This compute pipeline requires (16) bytes of push
        # constant data, supplied: (4)"; the stall never ran, so every calibration
        # attempt measured the same 6.4e-5 ms and P1's control never stalled.
        stall = self._harness_function("_stall_on_rd")
        self.assertRegex(stall, r"push\.resize\(16\)")
        self.assertRegex(stall, r"compute_list_set_push_constant\(list, push, 16\)")
        self.assertNotRegex(stall, r"compute_list_set_push_constant\(list, push, 4\)")

    def test_atlas_payload_is_read_by_page_not_as_one_whole_buffer(self):
        # A whole-buffer async readback of the ~320 MB atlas returned zero-filled
        # bytes (every decoded semantic was 0) while a ranged read of the same page
        # returned the record the GPU capture saw (0x80ff0003). W2, W4, R1 and R3
        # selected nothing from the zeros.
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        self.assertNotRegex(script, r'_buffer_bytes\([^)]*\["atlas_buffer"\]\)')
        pages = self._harness_function("_atlas_pages")
        self.assertIn("_buffer_bytes(buffer, page * PAGE_SPLATS * RAW_STRIDE, PAGE_SPLATS * RAW_STRIDE)", pages)
        self.assertIn("func _raw_splat(pages: Dictionary, node: Dictionary, local_index: int)", script)
        read = self._harness_function("_buffer_bytes")
        self.assertIn("buffer_get_data_async(buffer,", read)
        self.assertIn("p_offset, p_size)", read)

    def test_wind_probes_are_leaf_splats_inside_the_wind_fade(self):
        # The W1/W3 probes were local splat 0 of the first drawn node ids: height-1
        # nodes 300 m away, where gs_wind_distance_level_fade is 0. Frozen wind is
        # non-zero by the ADR (§7 W1 step 1) only where the fade lets it act.
        keys = self._harness_function("_wind_keys")
        self.assertIn("int(node[\"level\"]) != 0", keys)
        self.assertIn("< 64.0", keys)
        self.assertIn("wind > 0", keys)
        self.assertIn("_wind_keys(", self._harness_function("_identity"))
        self.assertIn("_wind_keys(", self._harness_function("_deformation_rows"))
        self.assertNotIn("_world_keys(before)", self._harness_function("_identity"))

    def test_rows_without_full_churn_eviction_name_why(self):
        # Churn never evicts the whole pose-A set (44/63 at best locally), so W1,
        # C1 and R2 have no ADR §7 oracle; they must say so, not just be absent.
        churn = self._harness_function("_churn")
        self.assertIn('"not_run_reason": _churn_reason(ids, evidence, last)', churn)
        reason = self._harness_function("_churn_reason")
        self.assertIn("churn cannot reach full eviction: %d/%d; %d non-drawn non-ancestor nodes stay resident", reason)
        identity = self._harness_function("_identity")
        self.assertIn('_raw["rows"]["W1"]["not_run_reason"] = reason', identity)
        self.assertIn('_raw["rows"]["C1"] = {"not_run_reason": reason}', identity)
        self.assertIn('_raw["rows"]["R2"] = {"not_run_reason":', self._harness_function("_regions_rows"))
        # The scene's atlas budget and the churn poses are unchanged (P1/M1 keep them).
        self.assertIn("Vector3(-x * 0.7, 1, -z * 0.7)", churn)

    def test_w4_probes_are_in_view_and_a_missing_row_names_why(self):
        # Locally all 64 W4 probes came from the first drawn node, outside the
        # view: 0/64 colour-valid, so W4 never measured. One in-frustum probe per
        # node gave 21/27 colour-valid.
        rows = self._harness_function("_deformation_rows")
        rock = rows.split("# Rock region:", 1)[1]
        self.assertIn('_camera.is_position_in_frustum(splat["position"])', rock)
        self.assertIn('"not_run_reason": "W4 found no in-frustum zero-weight rock-region probe"', rock)
        self.assertIn('"not_run_reason": "W4 probes not colour-valid: 0/%d" % keys.size()', rock)

    def test_null_drawn_ids_name_why_w3_o2_w4_cannot_run(self):
        # Local O2 control arm: hlod_drawn_node_ids was null at pose A, every
        # helper iterated it ("Unable to iterate on object of type 'Nil'") and
        # W3, O2 and W4 silently disappeared.
        self.assertIn('if ids is Array or ids is PackedInt32Array or ids is PackedInt64Array:', self._harness_function("_drawn_ids"))
        rows = self._harness_function("_deformation_rows")
        self.assertIn('if _drawn_ids(obs).is_empty():', rows)
        self.assertIn('for row in ["W3", "O2", "W4"]:', rows)
        self.assertNotIn('obs.get("stats", {}).get("hlod_drawn_node_ids", [])', rows)

    def test_r1_expected_levels_come_from_the_tree_never_from_the_sweep(self):
        # PR #1532 review: expected_lods built from what the tau sweep drew let a
        # level the sweep never reached (5 and 7 locally) drop out of the oracle, so
        # the remaining levels could PASS. ADR §7 R1 is "at every LOD level".
        regions = self._harness_function("_regions_rows")
        sweep = regions.split("for tau in [", 1)
        self.assertEqual(len(sweep), 2)
        before, inside = sweep[0], sweep[1].split('_raw["rows"]["R1"]', 1)[0]
        # Derived once, before any tau, from the tree's nodes and the asset's region volume.
        self.assertRegex(before, r"var tree = _region_levels\(await _native\(\"_test_gs_game_1_observe\"\), \"mid\"\)\n")
        self.assertIn('var expected_lods: Array = tree.get("levels", [])', before)
        self.assertNotRegex(inside, r"expected_lods\s*(\.append|=|\+=)")
        self.assertNotIn("_has_region(", inside)
        levels = self._harness_function("_region_levels")
        self.assertIn('.get_basename() + ".manifest.json"', levels)
        self.assertIn('obs.get("nodes", [])', levels)
        self.assertNotIn("hlod_drawn_node_ids", levels)
        self.assertNotIn("_drawn_", levels)
        # An unreached level is named and makes the row NOT_RUN, never PASS.
        tail = regions.split('_raw["rows"]["R1"]', 1)[1]
        self.assertIn('"unreached_lods": unreached', tail)
        self.assertIn('"not_run_reason"', tail)

    def test_p1_reports_viewport_gpu_time_beside_the_gs_pass_time(self):
        sample = self._harness_function("_sample_frames")
        self.assertIn("viewport_set_measure_render_time(root.get_viewport_rid(), true)", sample)
        self.assertIn('rec["viewport_gpu_ms"] = RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid())', sample)
        self.assertIn('target["gpu_frame_ms"] = timing.get("gpu_coherent_total_ms")', sample)
        # The verdict logic is unchanged: it never reads the added field.
        verdict_text = (ROOT / "tests/runtime/gs_game_1_verdict.py").read_text(encoding="utf-8")
        self.assertNotIn("viewport_gpu_ms", verdict_text)

    def test_postflight_finalizes_before_artifacts(self):
        text = (ROOT / ".github/workflows/gs_game_1.yml").read_text(encoding="utf-8")
        self.assertLess(text.index("id: contention_postflight"), text.index("--finalize-postflight"))
        self.assertLess(text.index("--finalize-postflight"), text.index("name: Upload synthetic report"))
        self.assertIn("steps.contention_postflight.outcome", text)


if __name__ == "__main__":
    unittest.main()
