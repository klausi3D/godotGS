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

    def test_postflight_finalizes_before_artifacts(self):
        text = (ROOT / ".github/workflows/gs_game_1.yml").read_text(encoding="utf-8")
        self.assertLess(text.index("id: contention_postflight"), text.index("--finalize-postflight"))
        self.assertLess(text.index("--finalize-postflight"), text.index("name: Upload synthetic report"))
        self.assertIn("steps.contention_postflight.outcome", text)


if __name__ == "__main__":
    unittest.main()
