#pragma once

#include "gs_test_setting_guard.h"
#include "test_macros.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "../renderer/render_diagnostics_orchestrator.h"
#include "../renderer/rendering_diagnostics.h"
#include "../renderer/rendering_error.h"

namespace {

bool diagnostics_contract_has_key(const Array &p_contract, const String &p_key) {
    for (int i = 0; i < p_contract.size(); i++) {
        if (String(p_contract[i]) == p_key) {
            return true;
        }
    }
    return false;
}

void diagnostics_require_key(const Dictionary &p_dict, const char *p_key) {
    CHECK_MESSAGE(p_dict.has(p_key), vformat("Expected diagnostics dictionary key '%s'", p_key));
}

} // namespace

TEST_CASE("[Gaussian Diagnostics] Singleton initialization is idempotent") {
    GaussianRenderingDiagnostics::ensure_singleton();
    GaussianRenderingDiagnostics *first = GaussianRenderingDiagnostics::get_singleton();
    REQUIRE(first != nullptr);

    GaussianRenderingDiagnostics::ensure_singleton();
    GaussianRenderingDiagnostics *second = GaussianRenderingDiagnostics::get_singleton();
    CHECK(second == first);
}

TEST_CASE("[Gaussian Diagnostics] destroy_singleton releases the instance and is idempotent") {
    GaussianRenderingDiagnostics::ensure_singleton();
    REQUIRE(GaussianRenderingDiagnostics::get_singleton() != nullptr);

    GaussianRenderingDiagnostics::destroy_singleton();
    CHECK(GaussianRenderingDiagnostics::get_singleton() == nullptr);

    GaussianRenderingDiagnostics::destroy_singleton();
    CHECK(GaussianRenderingDiagnostics::get_singleton() == nullptr);

    // Re-arm so later tests/state in the same suite observe a live singleton again.
    GaussianRenderingDiagnostics::ensure_singleton();
    CHECK(GaussianRenderingDiagnostics::get_singleton() != nullptr);
}

TEST_CASE("[Gaussian Diagnostics] Null renderer notifications are safe no-ops") {
    GaussianRenderingDiagnostics::ensure_singleton();
    GaussianRenderingDiagnostics *diagnostics = GaussianRenderingDiagnostics::get_singleton();
    REQUIRE(diagnostics != nullptr);

    RenderingError error;
    diagnostics->register_renderer(nullptr);
    diagnostics->unregister_renderer(nullptr);
    diagnostics->notify_error(nullptr, error);
    diagnostics->notify_recovery(nullptr, error);
    diagnostics->notify_frame_completed(nullptr);
    diagnostics->request_runtime_report();

    CHECK(true);
}

TEST_CASE("[Gaussian Diagnostics] Production metrics contract exposes GPU timing capture fields without GPU") {
    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate();
    REQUIRE(renderer.is_valid());
    REQUIRE(renderer->debug_state_orchestrator != nullptr);

    RenderDiagnosticsOrchestrator::Dependencies dependencies;
    dependencies.renderer = renderer.ptr();
    dependencies.debug_state_orchestrator = renderer->debug_state_orchestrator.get();
    dependencies.build_device_capability_report = []() {
        Dictionary report;
        report["test_device_report"] = true;
        return report;
    };
    dependencies.runtime_ports.update_gpu_pass_metrics_from_tile_renderer =
            &GaussianSplatRenderer::clear_debug_overlay_dirty_flags;

    RenderDiagnosticsOrchestrator diagnostics(dependencies);

    Dictionary snapshot = diagnostics.get_runtime_diagnostic_snapshot();
    Array contract = snapshot.get("production_metrics_contract", Array());
    const char *expected_keys[] = {
        "gpu_frame_ms",
        "gpu_binning_ms",
        "gpu_prefix_ms",
        "gpu_raster_ms",
        "gpu_resolve_ms",
        "gpu_timing_frame_serial",
        "gpu_timing_frames_behind",
        "gpu_pass_breakdown_available",
        "raster_path_reason",
        "raster_compute_allowed",
        "raster_total_tiles",
        "raster_empty_tiles",
        "raster_overflow_tiles",
        "raster_max_splats_per_tile",
        "raster_avg_splats_per_tile",
        "raster_occupancy_ratio",
        "raster_dense_ratio",
        "raster_overlap_records",
        "raster_overlap_record_budget_effective",
        "raster_overlap_thinning_keep_ratio",
        "raster_feature_global_sort",
        "raster_feature_packed_stage_data",
        "raster_feature_tighter_bounds",
        "raster_feature_sh_amortization",
        "raster_feature_quantized_storage",
        "raster_feature_debug_counters",
        "raster_tile_splat_capacity",
        "raster_max_raster_splats_per_tile",
        "raster_shader_defines_hash",
        "route_uid",
        "sort_route_uid",
        "cull_route_uid",
        "cull_route_reason",
    };

    for (const char *key : expected_keys) {
        CHECK_MESSAGE(diagnostics_contract_has_key(contract, key),
                vformat("Expected production metrics contract to include '%s'", key));
    }
}

TEST_CASE("[Gaussian Diagnostics] Production metrics preserve GPU timing capture semantics without GPU") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (project_settings == nullptr) {
        MESSAGE("Skipping test - ProjectSettings unavailable");
        return;
    }
    OS *os = OS::get_singleton();
    if (os == nullptr) {
        MESSAGE("Skipping test - OS unavailable");
        return;
    }

    const String validate_setting = "rendering/gaussian_splatting/diagnostics/validate_production_metrics";
    const String summary_interval_setting = "rendering/gaussian_splatting/diagnostics/summary_interval_frames";
    const String summary_history_setting = "rendering/gaussian_splatting/diagnostics/summary_history_size";
    const String gate_enabled_setting = "rendering/gaussian_splatting/diagnostics/perf_gate_enabled";
    ProjectSettingGuard validate_guard(project_settings, validate_setting);
    ProjectSettingGuard summary_interval_guard(project_settings, summary_interval_setting);
    ProjectSettingGuard summary_history_guard(project_settings, summary_history_setting);
    ProjectSettingGuard gate_enabled_guard(project_settings, gate_enabled_setting);
    project_settings->set_setting(validate_setting, true);
    project_settings->set_setting(summary_interval_setting, 1);
    project_settings->set_setting(summary_history_setting, 2);
    project_settings->set_setting(gate_enabled_setting, false);

    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate();
    REQUIRE(renderer.is_valid());
    REQUIRE(renderer->debug_state_orchestrator != nullptr);

    Ref<GaussianData> data;
    data.instantiate();
    data->resize(4096);
    renderer->get_scene_state().gaussian_data = data;

    GaussianSplatRenderer::FrameState &frame_state = renderer->get_frame_state();
    frame_state.frame_counter = 42;
    frame_state.render_time_ms = 1.75f;
    frame_state.sort_time_ms = 0.50f;
    frame_state.visible_splat_count.store(2048, std::memory_order_release);

    GaussianSplatRenderer::PerformanceMetrics &perf = renderer->get_performance_state().metrics;
    perf.data_source = "diagnostics_test";
    perf.raster_path = "tile";
    perf.raster_path_reason = "Compute raster disabled by pipeline settings";
    perf.raster_compute_allowed = false;
    perf.raster_total_tiles = 100;
    perf.raster_empty_tiles = 20;
    perf.raster_overflow_tiles = 3;
    perf.raster_max_splats_per_tile = 4096;
    perf.raster_avg_splats_per_tile = 128.0f;
    perf.raster_occupancy_ratio = 0.80f;
    perf.raster_dense_ratio = 0.25f;
    perf.raster_overlap_records = 300000;
    perf.raster_overlap_record_budget = 400000;
    perf.raster_overlap_record_budget_effective = 350000;
    perf.raster_overlap_record_budget_configured = 400000;
    perf.raster_overlap_thinning_keep_ratio = 0.875f;
    perf.raster_feature_global_sort = true;
    perf.raster_feature_packed_stage_data = false;
    perf.raster_feature_tighter_bounds = true;
    perf.raster_feature_sh_amortization = false;
    perf.raster_sh_amortization_divisor = 1;
    perf.raster_feature_quantized_storage = true;
    perf.raster_feature_debug_counters = false;
    perf.raster_tile_splat_capacity = 1024;
    perf.raster_max_raster_splats_per_tile = 8192;
    perf.raster_shader_defines_hash = 12345;
    perf.cull_route_uid = RenderRouteUID::INSTANCE_CULL_GPU;
    perf.cull_route_reason = "gpu_culler";
    perf.gpu_frame_time_ms = 3.50f;
    perf.gpu_tile_binning_time_ms = 0.40f;
    perf.gpu_tile_prefix_time_ms = 0.30f;
    perf.gpu_tile_raster_time_ms = 2.10f;
    perf.gpu_tile_resolve_time_ms = 0.20f;
    perf.gpu_timing_frame_serial = 40;
    perf.gpu_timing_frames_behind = 2;

    GaussianSplatRenderer::DebugState &debug_state = renderer->get_debug_state();
    debug_state.route_uid = RenderRouteUID::INSTANCE_RASTER_COMPUTE;
    debug_state.sort_route_uid = RenderRouteUID::INSTANCE_SORT_GPU;
    debug_state.last_stage_metrics_valid = true;
    GaussianSplatRenderer::StageMetrics &stage_metrics = debug_state.last_stage_metrics;
    stage_metrics.route_uid = RenderRouteUID::INSTANCE_STREAMING;
    stage_metrics.selected_route_backend = "streaming";
    stage_metrics.cull.has_visible = true;
    stage_metrics.cull.visible_count = 2048;
    stage_metrics.cull.candidate_count = 4096;
    stage_metrics.cull.cull_time_ms = 0.25f;
    stage_metrics.cull.visible_domain = GaussianRenderState::IndexDomain::GAUSSIAN_GLOBAL;
    stage_metrics.sort.did_sort = true;
    stage_metrics.sort.input_count = 2048;
    stage_metrics.sort.sorted_count = 2048;
    stage_metrics.sort.sort_time_ms = 0.50f;
    stage_metrics.sort.input_domain = GaussianRenderState::IndexDomain::GAUSSIAN_GLOBAL;
    stage_metrics.sort.output_domain = GaussianRenderState::IndexDomain::GAUSSIAN_GLOBAL;
    stage_metrics.raster.render_time_ms = 1.00f;
    stage_metrics.raster.raster_path = "tile";
    stage_metrics.composite_time_ms = 0.10f;
    stage_metrics.composite_executed = true;

    RenderDiagnosticsOrchestrator::Dependencies dependencies;
    dependencies.renderer = renderer.ptr();
    dependencies.debug_state_orchestrator = renderer->debug_state_orchestrator.get();
    dependencies.build_device_capability_report = []() {
        Dictionary report;
        report["test_device_report"] = true;
        return report;
    };
    dependencies.runtime_ports.update_gpu_pass_metrics_from_tile_renderer =
            &GaussianSplatRenderer::clear_debug_overlay_dirty_flags;

    RenderDiagnosticsOrchestrator diagnostics(dependencies);
    const uint64_t frame_start_usec = os->get_ticks_usec() > 1000 ? os->get_ticks_usec() - 1000 : 0;
    diagnostics.finalize_frame_metrics(frame_start_usec);

    Dictionary snapshot = diagnostics.get_runtime_diagnostic_snapshot();
    Dictionary production_metrics = snapshot.get("production_metrics", Dictionary());
    Dictionary validation = snapshot.get("production_metrics_validation", Dictionary());
    Dictionary telemetry = snapshot.get("telemetry", Dictionary());

    diagnostics_require_key(production_metrics, "gpu_frame_ms");
    diagnostics_require_key(production_metrics, "gpu_binning_ms");
    diagnostics_require_key(production_metrics, "gpu_prefix_ms");
    diagnostics_require_key(production_metrics, "gpu_raster_ms");
    diagnostics_require_key(production_metrics, "gpu_resolve_ms");
    diagnostics_require_key(production_metrics, "gpu_timing_frame_serial");
    diagnostics_require_key(production_metrics, "gpu_timing_frames_behind");
    diagnostics_require_key(production_metrics, "gpu_pass_breakdown_available");
    diagnostics_require_key(production_metrics, "raster_path_reason");
    diagnostics_require_key(production_metrics, "raster_max_splats_per_tile");
    diagnostics_require_key(production_metrics, "raster_overlap_thinning_keep_ratio");
    diagnostics_require_key(production_metrics, "raster_feature_tighter_bounds");
    diagnostics_require_key(production_metrics, "raster_shader_defines_hash");
    diagnostics_require_key(production_metrics, "selected_route_uid");
    diagnostics_require_key(production_metrics, "selected_route_backend");

    CHECK(float(production_metrics.get("gpu_frame_ms", 0.0f)) == doctest::Approx(3.50f));
    CHECK(float(production_metrics.get("gpu_binning_ms", 0.0f)) == doctest::Approx(0.40f));
    CHECK(float(production_metrics.get("gpu_prefix_ms", 0.0f)) == doctest::Approx(0.30f));
    CHECK(float(production_metrics.get("gpu_raster_ms", 0.0f)) == doctest::Approx(2.10f));
    CHECK(float(production_metrics.get("gpu_resolve_ms", 0.0f)) == doctest::Approx(0.20f));
    CHECK(int64_t(production_metrics.get("gpu_timing_frame_serial", int64_t(-1))) == 40);
    CHECK(int64_t(production_metrics.get("gpu_timing_frames_behind", int64_t(-1))) == 2);
    CHECK(bool(production_metrics.get("gpu_pass_breakdown_available", false)));
    CHECK(String(production_metrics.get("raster_path_reason", String())) == "Compute raster disabled by pipeline settings");
    CHECK(int64_t(production_metrics.get("raster_max_splats_per_tile", int64_t(0))) == 4096);
    CHECK(float(production_metrics.get("raster_overlap_thinning_keep_ratio", 0.0f)) == doctest::Approx(0.875f));
    CHECK(bool(production_metrics.get("raster_feature_tighter_bounds", false)));
    CHECK(String(production_metrics.get("raster_shader_defines_hash", String())) == "12345");
    CHECK(String(production_metrics.get("selected_route_uid", String())) == String(RenderRouteUID::INSTANCE_STREAMING));
    CHECK(String(production_metrics.get("selected_route_backend", String())) == String("streaming"));
    CHECK(bool(validation.get("valid", false)));

    CHECK(float(telemetry.get("gpu_frame_time_ms", 0.0f)) == doctest::Approx(3.50f));
    CHECK(float(telemetry.get("gpu_tile_binning_time_ms", 0.0f)) == doctest::Approx(0.40f));
    CHECK(float(telemetry.get("gpu_tile_prefix_time_ms", 0.0f)) == doctest::Approx(0.30f));
    CHECK(float(telemetry.get("gpu_tile_raster_time_ms", 0.0f)) == doctest::Approx(2.10f));
    CHECK(float(telemetry.get("gpu_tile_resolve_time_ms", 0.0f)) == doctest::Approx(0.20f));
    CHECK(int64_t(telemetry.get("gpu_timing_frame_serial", int64_t(-1))) == 40);
    CHECK(int64_t(telemetry.get("gpu_timing_frames_behind", int64_t(-1))) == 2);
    CHECK(int64_t(telemetry.get("raster_overlap_records", int64_t(0))) == 300000);
    CHECK(bool(telemetry.get("raster_feature_quantized_storage", false)));
    CHECK(String(telemetry.get("selected_route_uid", String())) == String(RenderRouteUID::INSTANCE_STREAMING));
    CHECK(String(telemetry.get("selected_route_backend", String())) == String("streaming"));

    Array summaries = snapshot.get("production_metrics_summaries", Array());
    REQUIRE(summaries.size() == 1);
    Dictionary summary = summaries[0];
    CHECK(summary.has("avg_stage_total_ms"));
    CHECK(int64_t(summary.get("frame_count", int64_t(0))) == 1);
}

// #528: the per-frame telemetry reset is now single-sourced in the
// PerformanceMetrics::reset_*() helpers (see render_performance_types.h), replacing
// hand-maintained field-by-field lists duplicated across render-path sites. These
// tests pin the two properties the refactor must preserve:
//  (1) each reset group returns exactly its fields to their struct defaults
//      (fresh-vs-reset equality), and
//  (2) the resets never touch cumulative counters or per-stage outputs
//      (a blanket `*this = {}` reset was rejected for this reason).
// The static coverage guard tests/ci/check_metric_reset_parity.py enforces that
// every field carries a reset disposition; these doctests pin the runtime values.
TEST_CASE("[Gaussian Diagnostics][PerformanceMetrics][Reset] reset helpers restore struct defaults") {
    using PerformanceMetrics = GaussianSplatRenderer::PerformanceMetrics;
    const PerformanceMetrics fresh;
    PerformanceMetrics m;

    // Mutate every field the reset helpers cover to a non-default value.
    // reset_raster_frame_stats group:
    m.raster_path_reason = "stale";
    m.raster_compute_allowed = true;
    m.raster_total_tiles = 11;
    m.raster_empty_tiles = 12;
    m.raster_overflow_tiles = 13;
    m.raster_max_splats_per_tile = 14;
    m.raster_avg_splats_per_tile = 15.0f;
    m.raster_occupancy_ratio = 0.16f;
    m.raster_dense_ratio = 0.17f;
    m.raster_overflow_ratio = 0.18f;
    m.raster_overlap_records = 19;
    m.raster_overlap_record_budget = 20;
    m.raster_overlap_record_budget_effective = 21;
    m.raster_overlap_record_budget_configured = 22;
    m.raster_overlap_thinning_keep_ratio = 0.23f; // default is 1.0f
    m.raster_feature_global_sort = true;
    m.raster_feature_packed_stage_data = true;
    m.raster_feature_tighter_bounds = true;
    m.raster_feature_sh_amortization = true;
    m.raster_sh_amortization_divisor = 24; // default is 1
    m.raster_feature_quantized_storage = true;
    m.raster_feature_debug_counters = true;
    m.raster_tile_splat_capacity = 25;
    m.raster_max_raster_splats_per_tile = 26;
    m.raster_shader_defines_hash = 27;
    // reset_gpu_core_pass_timings group:
    m.gpu_frame_time_ms = 30.0f;
    m.gpu_frame_time_valid = true;
    m.gpu_tile_overlap_count_time_ms = 31.0f;
    m.gpu_tile_overlap_count_time_valid = true;
    m.gpu_tile_binning_time_ms = 32.0f;
    m.gpu_tile_overlap_emit_time_ms = 33.0f;
    m.gpu_tile_overlap_emit_time_valid = true;
    m.gpu_tile_overlap_sort_time_ms = 34.0f;
    m.gpu_tile_overlap_sort_time_valid = true;
    m.tile_overlap_sort_cpu_dispatch_ms = 35.0f;
    m.tile_overlap_sort_cpu_dispatch_valid = true;
    m.gpu_tile_raster_time_ms = 36.0f;
    m.gpu_tile_raster_time_valid = true;
    // reset_gpu_extended_pass_timings group:
    m.gpu_tile_prefix_time_ms = 40.0f;
    m.gpu_tile_prefix_time_valid = true;
    m.tile_prefix_cpu_sync_fallback_ms = 41.0f;
    m.tile_prefix_cpu_sync_fallback_valid = true;
    m.gpu_tile_resolve_time_ms = 42.0f;
    m.gpu_tile_resolve_time_valid = true;
    // reset_gpu_timeline_metrics group:
    m.gpu_timeline_inflight_frames = 50;
    m.gpu_timeline_completed_frames = 51;
    m.gpu_timeline_stall_count = 52;
    m.gpu_timeline_stall_ms = 53.0f;
    m.gpu_timeline_last_value = 54;
    // reset_gpu_readback_state group:
    m.gpu_utilization = 0.60f;
    m.gpu_timing_frame_serial = 61;
    m.gpu_timing_frames_behind = 62;
    m.tile_sort_sync_fallback_count = 63;

    m.reset_raster_frame_stats();
    m.reset_gpu_core_pass_timings();
    m.reset_gpu_extended_pass_timings();
    m.reset_gpu_timeline_metrics();
    m.reset_gpu_readback_state();

    // Every covered field is back to its struct default (fresh == reset).
    CHECK(m.raster_path_reason == fresh.raster_path_reason);
    CHECK(m.raster_compute_allowed == fresh.raster_compute_allowed);
    CHECK(m.raster_total_tiles == fresh.raster_total_tiles);
    CHECK(m.raster_empty_tiles == fresh.raster_empty_tiles);
    CHECK(m.raster_overflow_tiles == fresh.raster_overflow_tiles);
    CHECK(m.raster_max_splats_per_tile == fresh.raster_max_splats_per_tile);
    CHECK(m.raster_avg_splats_per_tile == doctest::Approx(fresh.raster_avg_splats_per_tile));
    CHECK(m.raster_occupancy_ratio == doctest::Approx(fresh.raster_occupancy_ratio));
    CHECK(m.raster_dense_ratio == doctest::Approx(fresh.raster_dense_ratio));
    CHECK(m.raster_overflow_ratio == doctest::Approx(fresh.raster_overflow_ratio));
    CHECK(m.raster_overlap_records == fresh.raster_overlap_records);
    CHECK(m.raster_overlap_record_budget == fresh.raster_overlap_record_budget);
    CHECK(m.raster_overlap_record_budget_effective == fresh.raster_overlap_record_budget_effective);
    CHECK(m.raster_overlap_record_budget_configured == fresh.raster_overlap_record_budget_configured);
    CHECK(m.raster_overlap_thinning_keep_ratio == doctest::Approx(fresh.raster_overlap_thinning_keep_ratio));
    CHECK(m.raster_feature_global_sort == fresh.raster_feature_global_sort);
    CHECK(m.raster_feature_packed_stage_data == fresh.raster_feature_packed_stage_data);
    CHECK(m.raster_feature_tighter_bounds == fresh.raster_feature_tighter_bounds);
    CHECK(m.raster_feature_sh_amortization == fresh.raster_feature_sh_amortization);
    CHECK(m.raster_sh_amortization_divisor == fresh.raster_sh_amortization_divisor);
    CHECK(m.raster_feature_quantized_storage == fresh.raster_feature_quantized_storage);
    CHECK(m.raster_feature_debug_counters == fresh.raster_feature_debug_counters);
    CHECK(m.raster_tile_splat_capacity == fresh.raster_tile_splat_capacity);
    CHECK(m.raster_max_raster_splats_per_tile == fresh.raster_max_raster_splats_per_tile);
    CHECK(m.raster_shader_defines_hash == fresh.raster_shader_defines_hash);
    CHECK(m.gpu_frame_time_ms == doctest::Approx(fresh.gpu_frame_time_ms));
    CHECK(m.gpu_frame_time_valid == fresh.gpu_frame_time_valid);
    CHECK(m.gpu_tile_overlap_count_time_ms == doctest::Approx(fresh.gpu_tile_overlap_count_time_ms));
    CHECK(m.gpu_tile_overlap_count_time_valid == fresh.gpu_tile_overlap_count_time_valid);
    CHECK(m.gpu_tile_binning_time_ms == doctest::Approx(fresh.gpu_tile_binning_time_ms));
    CHECK(m.gpu_tile_overlap_emit_time_ms == doctest::Approx(fresh.gpu_tile_overlap_emit_time_ms));
    CHECK(m.gpu_tile_overlap_emit_time_valid == fresh.gpu_tile_overlap_emit_time_valid);
    CHECK(m.gpu_tile_overlap_sort_time_ms == doctest::Approx(fresh.gpu_tile_overlap_sort_time_ms));
    CHECK(m.gpu_tile_overlap_sort_time_valid == fresh.gpu_tile_overlap_sort_time_valid);
    CHECK(m.tile_overlap_sort_cpu_dispatch_ms == doctest::Approx(fresh.tile_overlap_sort_cpu_dispatch_ms));
    CHECK(m.tile_overlap_sort_cpu_dispatch_valid == fresh.tile_overlap_sort_cpu_dispatch_valid);
    CHECK(m.gpu_tile_raster_time_ms == doctest::Approx(fresh.gpu_tile_raster_time_ms));
    CHECK(m.gpu_tile_raster_time_valid == fresh.gpu_tile_raster_time_valid);
    CHECK(m.gpu_tile_prefix_time_ms == doctest::Approx(fresh.gpu_tile_prefix_time_ms));
    CHECK(m.gpu_tile_prefix_time_valid == fresh.gpu_tile_prefix_time_valid);
    CHECK(m.tile_prefix_cpu_sync_fallback_ms == doctest::Approx(fresh.tile_prefix_cpu_sync_fallback_ms));
    CHECK(m.tile_prefix_cpu_sync_fallback_valid == fresh.tile_prefix_cpu_sync_fallback_valid);
    CHECK(m.gpu_tile_resolve_time_ms == doctest::Approx(fresh.gpu_tile_resolve_time_ms));
    CHECK(m.gpu_tile_resolve_time_valid == fresh.gpu_tile_resolve_time_valid);
    CHECK(m.gpu_timeline_inflight_frames == fresh.gpu_timeline_inflight_frames);
    CHECK(m.gpu_timeline_completed_frames == fresh.gpu_timeline_completed_frames);
    CHECK(m.gpu_timeline_stall_count == fresh.gpu_timeline_stall_count);
    CHECK(m.gpu_timeline_stall_ms == doctest::Approx(fresh.gpu_timeline_stall_ms));
    CHECK(m.gpu_timeline_last_value == fresh.gpu_timeline_last_value);
    CHECK(m.gpu_utilization == doctest::Approx(fresh.gpu_utilization));
    CHECK(m.gpu_timing_frame_serial == fresh.gpu_timing_frame_serial);
    CHECK(m.gpu_timing_frames_behind == fresh.gpu_timing_frames_behind);
    CHECK(m.tile_sort_sync_fallback_count == fresh.tile_sort_sync_fallback_count);
}

TEST_CASE("[Gaussian Diagnostics][PerformanceMetrics][Reset] per-frame resets never touch cumulative/per-stage fields") {
    using PerformanceMetrics = GaussianSplatRenderer::PerformanceMetrics;
    PerformanceMetrics m;

    // Representative cumulative counters, rolling aggregates, and per-stage outputs
    // that the reset helpers must leave untouched (would be corrupted by a blanket
    // `*this = {}`). Seed them with sentinels that differ from the struct defaults.
    m.total_frames_rendered = 900;
    m.raster_pipeline_reformats = 7;               // deliberately monotonic
    m.sort_cache_hits = 901;
    m.cull_projection_contract_mismatch_count = 902;
    m.avg_frame_time_ms = 9.03f;
    m.peak_frame_time_ms = 9.04f;
    m.last_frame_start_usec = 905;
    m.uploaded_splat_count = 906;
    m.rendered_splat_count = 907;
    m.buffer_upload_time_ms = 9.08f;
    m.culling_time_ms = 9.09f;
    m.data_source = "sentinel_source";
    m.raster_path = "sentinel_path";
    m.cull_route_uid = "sentinel_route";
    m.visible_after_culling = 910;

    // Apply the full set of reset helpers (superset of any single site's composition).
    m.reset_raster_frame_stats();
    m.reset_gpu_core_pass_timings();
    m.reset_gpu_extended_pass_timings();
    m.reset_gpu_timeline_metrics();
    m.reset_gpu_readback_state();

    CHECK(m.total_frames_rendered == 900);
    CHECK(m.raster_pipeline_reformats == 7);
    CHECK(m.sort_cache_hits == 901);
    CHECK(m.cull_projection_contract_mismatch_count == 902);
    CHECK(m.avg_frame_time_ms == doctest::Approx(9.03f));
    CHECK(m.peak_frame_time_ms == doctest::Approx(9.04f));
    CHECK(m.last_frame_start_usec == 905);
    CHECK(m.uploaded_splat_count == 906);
    CHECK(m.rendered_splat_count == 907);
    CHECK(m.buffer_upload_time_ms == doctest::Approx(9.08f));
    CHECK(m.culling_time_ms == doctest::Approx(9.09f));
    CHECK(m.data_source == String("sentinel_source"));
    CHECK(m.raster_path == String("sentinel_path"));
    CHECK(m.cull_route_uid == String("sentinel_route"));
    CHECK(m.visible_after_culling == 910);
}
