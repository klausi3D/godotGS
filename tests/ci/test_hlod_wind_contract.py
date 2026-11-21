"""W-S source/wiring contracts. These are not runtime or GPU evidence."""
import importlib.util
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules/gaussian_splatting"


def body(text, name):
    start = text.index(name)
    start = text.index("{", start)
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class HlodWindContract(unittest.TestCase):
    def test_wind_does_not_refuse_either_publish_or_frame_route(self):
        source = (MODULE / "renderer/resident_instance_contract_publisher.cpp").read_text()
        predicate = body(source, "GaussianSplatRenderer::hlod_streaming_unsupported_reason(")
        self.assertNotIn("get_wind_settings", predicate)
        self.assertNotIn("get_sphere_effector_count", predicate)
        self.assertIn("shadow_instance_filter", predicate)
        self.assertIn("painterly", predicate)
        frame = body(source, "GaussianSplatRenderer::prepare_hlod_frame(")
        self.assertNotRegex(frame, r'return fallback\([^;]*deformation_or_instance_domain')
        self.assertIn("instance.displacement_margin", frame)

    def test_current_server_frame_owns_shared_colour_and_caster_clock(self):
        source = (MODULE / "renderer/render_frame_context_manager.cpp").read_text()
        clock = body(source, "RenderFrameContextManager::sample_render_animation_time_seconds(")
        self.assertIn("get_frame_number", clock)
        self.assertIn("sampled_frame", clock)
        caster = body((MODULE / "renderer/gaussian_splat_renderer.cpp").read_text(),
                      "GaussianSplatRenderer::_render_hlod_shadow_casters(")
        self.assertIn("sample_render_animation_time_seconds", caster)
        self.assertIn("compute_wind_time_seconds", caster)

    def test_gpu_case_runs_inside_existing_ci_scenario(self):
        scenario = (ROOT / "tests/runtime/test_hlod_resident_main_view.gd").read_text()
        self.assertIn("await _run_streamed_wind_proof()", body_gd(scenario, "_run"))
        proof = body_gd(scenario, "_run_streamed_wind_proof")
        for signal in ("has_hlod_paged_source", "wind_enabled", "get_data()", "wind_off_identical",
                       "legacy_static", "wind_on_moved"):
            self.assertIn(signal, proof)
        runner = (ROOT / "tests/runtime/run_runtime_validation.py").read_text()
        self.assertIn('"Resident HLOD Main View": RUNTIME_DIR / "test_hlod_resident_main_view.gd"', runner)

    def test_filtered_empty_scene_uses_same_project_fallback_for_caster_and_bounds(self):
        # Registered effectors can all be outside this renderer's scope. The actual
        # colour producer returns zero when its filtered payload is empty, irrespective
        # of the raw count. Caster and bound admission must take that same legal route.
        colour = (MODULE / "renderer/render_pipeline_stages.cpp").read_text()
        producer = body(colour, "static uint32_t _populate_scene_effector_payload_for_renderer(")
        self.assertRegex(producer, r"if \(payload.is_empty\(\)\)\s*\{\s*return 0;")
        caster = (MODULE / "renderer/gs_deformation_frame.h").read_text()
        self.assertIn("if (!payload.is_empty())", caster)
        publisher = (MODULE / "renderer/resident_instance_contract_publisher.cpp").read_text()
        self.assertIn("legacy_effector = wind_effectors.is_empty()", publisher)

    def test_outer_scene_cull_receives_envelope_before_draw_for_world_and_instances(self):
        for name, cls in (("gaussian_splat_world_3d", "GaussianSplatWorld3D"),
                          ("gaussian_splat_node_3d", "GaussianSplatNode3D")):
            source = (MODULE / ("nodes/" + name + ".cpp")).read_text()
            with self.subTest(node=cls):
                update = body(source, cls + "::_update_deformation_visibility_margin(")
                self.assertIn("deformation_visibility_margin_for_renderer", update)
                self.assertIn("instance_set_extra_visibility_margin", update)
                self.assertRegex(source, r'connect\(SNAME\("frame_pre_draw"\), callback\)')
                self.assertRegex(source, r'disconnect\(SNAME\("frame_pre_draw"\), callback\)')

    def test_guard_lane_reaches_this_suite_and_fails_closed(self):
        spec = importlib.util.spec_from_file_location("ws_runner", ROOT / "tests/ci/run_module_tests.py")
        runner = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = runner
        spec.loader.exec_module(runner)
        with mock.patch.object(runner, "_run_message_guard", return_value=None) as invoke:
            runner._run_required_message_guards()
        self.assertIn(runner._run_hlod_wind_contract_tests, [c.args[0] for c in invoke.call_args_list])
        with mock.patch.object(runner, "_run_command", return_value=(0, "", "")):
            self.assertFalse(runner._run_hlod_wind_contract_tests()[0])


def body_gd(text, name):
    return text.split("func " + name + "(", 1)[1].split("\nfunc ", 1)[0]


if __name__ == "__main__":
    unittest.main()
