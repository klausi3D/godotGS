"""Lane wiring contracts; constructed tests, never GPU evidence."""
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
import contextlib
import importlib.util
import math
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

    def test_incomplete_atlas_reads_make_their_rows_not_run(self):
        # PR #1532 review: _atlas_pages() drops a failed or short page read and
        # returns the rest. W2 then built its merge oracle from a truncated child
        # population and could PASS. Every row that reads atlas pages checks that
        # each requested page arrived and is NOT_RUN, naming the pages, otherwise.
        missing = self._harness_function("_atlas_missing")
        self.assertTrue(missing, "no completeness check for atlas page reads")
        self.assertIn('node["pages"].size() < need', missing)  # Unallocated pages count as missing.
        self.assertIn("not pages.has(page)", missing)
        reason = 'atlas pages not read completely: %s'
        for name, rows in (("_identity", ("W1",)), ("_wind_lod", ("W2",)),
                           ("_deformation_rows", ("W3", "W4")), ("_regions_rows", ("R2", "R3"))):
            body = self._harness_function(name)
            reads = len(re.findall(r"await _atlas_pages\(", body))
            self.assertGreater(reads, 0, name)
            self.assertEqual(len(re.findall(r"_atlas_missing\(", body)), reads, name)
            self.assertIn(reason, body, name)
            for row in rows:
                self.assertRegex(body, r'\["' + row + r'"\][^\n]*not_run_reason|for row in \[[^\]]*"' + row + '"', name + " " + row)
        # W2: the check precedes any use of the pages for the oracle.
        w2 = self._harness_function("_wind_lod")
        self.assertLess(w2.index("_atlas_missing("), w2.index("var center = Vector3("))

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

    def test_w2_probe_is_non_vacuous_and_its_control_breaks_only_the_parent(self):
        # Run 45231549548: the W2 parent was the first resident one in node order,
        # ~270 m from pose A where the distance fade is 0; 0 px matched 0 px. The
        # old control (interiors at full weight) moved parent and children alike.
        parents = self._harness_function("_w2_parents")
        self.assertIn('int(parent["level"]) != 1', parents)  # Leaf children only.
        self.assertIn('parents.sort_custom(func(a, b): return a["distance"] < b["distance"])', parents)
        w2 = self._harness_function("_wind_lod")
        self.assertIn("for parent in _w2_parents(fine):", w2)
        self.assertIn("if children_px < W2_MIN_CHILD_DISPLACEMENT_PX:", w2)
        self.assertIn('"children_displacement_px": children_px', w2)
        self.assertIn('"not_run_reason": "W2 no non-vacuous probe', w2)
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        verdict_text = (ROOT / "tests/runtime/gs_game_1_verdict.py").read_text(encoding="utf-8")
        floor = re.search(r"const W2_MIN_CHILD_DISPLACEMENT_PX = ([0-9.]+)", script)
        self.assertIsNotNone(floor)
        self.assertIn(f"W2_MIN_CHILD_DISPLACEMENT_PX = {floor.group(1)}", verdict_text)
        shader = (ROOT / "modules/gaussian_splatting/shaders/includes/gs_deformation.glsl").read_text(encoding="utf-8")
        control = shader.split("#ifdef GS_TEST_COARSE_FADE_ZERO", 1)[1].split("#endif", 1)[0]
        self.assertIn("if (level > 0u) { return 0.0; }", control)
        self.assertNotIn("GS_TEST_COARSE_FADE_OFF", shader)
        header = (ROOT / "modules/gaussian_splatting/renderer/splat_identity_test_control.h").read_text(encoding="utf-8")
        self.assertIn('if (force_coarse_fade_zero.load()) { result += "#define GS_TEST_COARSE_FADE_ZERO 1\\n"; }', header)

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
        # The churn stands behind the pose-A camera and looks away from what pose A
        # draws: poses looking through the centre redrew pose A's nodes, so the LRU
        # (atlas full, no leases) kept them.
        self.assertIn("for pose in _churn_poses():", churn)
        self.assertIn("Vector3(pose.x, 1, pose.z) + outward * 100", churn)
        poses = self._harness_function("_churn_poses")
        self.assertIn("for radius in [230.0, 180.0, 120.0, 60.0]:", poses)
        self.assertIn("var angle = deg_to_rad(15.0 * step)", poses)  # Within 60 degrees of +z.
        self.assertIn("for step in range(-4, 5):", poses)
        self.assertIn("if radius < 130 and absi(step) > 3:", poses)
        self.assertNotIn("Vector3(-x * 0.7, 1, -z * 0.7)", churn)
        # The scene's atlas budget is unchanged (P1/M1 keep it): the churn sets nothing.
        self.assertNotIn("_setting(", churn)
        self.assertNotIn("atlas_vram_budget", churn)
        # Per-pose residency counters tell an LRU from pinned or leaked nodes.
        self.assertIn('"residency": _churn_counters(obs, ids)', churn)
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        for key in ("hlod_stream_pages_free", "hlod_stream_evicted_total", "hlod_stream_allocation_refused_total",
                    "hlod_stream_allocation_protected_capacity_total"):
            self.assertIn(f'"{key}"', script)
        counters = self._harness_function("_churn_counters")
        for key in ("leases", "edit_hold", "drawn_this_frame", "last_drawn_frame"):
            self.assertIn(f'"{key}": hold.get("{key}")', counters)
        bridge = (ROOT / "modules/gaussian_splatting/renderer/gaussian_splat_renderer_bindings.cpp").read_text(encoding="utf-8")
        self.assertIn("residency.test_hold(node, hold)", bridge)
        # Eviction is forced only through the residency's own evict() (refuses protected
        # nodes), after page pressure, for pose-A nodes still resident; recorded per pose.
        self.assertIn('forced = await _native("_test_gs_game_1_evict", [still])', churn)
        self.assertIn('"absent_before_forcing": lru_evicted, "forced": forced', churn)
        evict = bridge.split("Dictionary GaussianSplatRenderer::_test_gs_game_1_evict(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("state.hlod_streamer->_test_evict_node(node) == OK", evict)
        streamer = (ROOT / "modules/gaussian_splatting/renderer/hlod_node_streamer.cpp").read_text(encoding="utf-8")
        self.assertIn("return residency.evict(p_node) ? OK : ERR_BUSY;", streamer)
        self.assertIn('ClassDB::bind_method(D_METHOD("_test_gs_game_1_evict", "nodes")', bridge)

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

    def test_l2_spot_lights_what_pose_a_sees(self):
        # L2 measures spot shadows at pose A. A spot aimed at (0, 0, -10) lit nothing
        # visible from there (a splat wall at about z = 13 hides that ground), so the
        # row read 0 shadow pixels however the shadow path behaved. Like omni[0], the
        # spot must light the splats facing the pose-A camera: its cone must reach the eye.
        script = (ROOT / "tests/runtime/gs_game_1_harness.gd").read_text(encoding="utf-8")
        number = r"(-?\d+(?:\.\d+)?)"
        vector = rf"Vector3\({number}, {number}, {number}\)"

        def vec(match, at=0):
            return [float(match.group(at + k)) for k in (1, 2, 3)]

        pose = script.split("func _pose_a() -> void:", 1)[1].split("\nfunc ", 1)[0]
        eye = vec(re.search(rf"_camera\.look_at_from_position\({vector}", pose))
        position = vec(re.search(rf"\n\t_spot\.position = {vector}\n", script))
        target = vec(re.search(rf"\n\t_spot\.look_at\({vector}, Vector3\.UP\)\n", script))
        spot_range = float(re.search(rf"\n\t_spot\.spot_range = {number}\n", script).group(1))
        self.assertNotIn("_spot.spot_angle", script, "the cone check assumes Godot's default 45 degree spot angle")
        axis = [t - p for t, p in zip(target, position)]
        to_eye = [e - p for e, p in zip(eye, position)]
        length = lambda v: sum(c * c for c in v) ** 0.5
        cosine = sum(a * b for a, b in zip(axis, to_eye)) / (length(axis) * length(to_eye))
        self.assertGreater(cosine, math.cos(math.radians(45.0)), "pose-A eye is outside the spot cone")
        self.assertLess(length(to_eye), spot_range, "pose-A eye is beyond the spot range")

    def test_postflight_finalizes_before_artifacts(self):
        text = (ROOT / ".github/workflows/gs_game_1.yml").read_text(encoding="utf-8")
        self.assertLess(text.index("id: contention_postflight"), text.index("--finalize-postflight"))
        self.assertLess(text.index("--finalize-postflight"), text.index("name: Upload synthetic report"))
        self.assertIn("steps.contention_postflight.outcome", text)


if __name__ == "__main__":
    unittest.main()
