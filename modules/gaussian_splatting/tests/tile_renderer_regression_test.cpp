#include "core/config/project_settings.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/object/ref_counted.h"
#include "core/templates/vector.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <utility>

#include "../renderer/tile_renderer.h"
#include "../renderer/gpu_sorting_config.h"
#include "../renderer/gpu_sorting_constants.h"
#include "../renderer/sort_fallback_policy.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/pipeline_io_contracts.h"
#include "../core/gaussian_data.h"
#include "gs_test_setting_guard.h"

namespace {

static TileRenderer::RenderParams make_render_params(RID p_gaussian_buffer, RID p_sorted_indices, uint32_t p_splat_count,
        int p_viewport_width, int p_viewport_height, int p_tile_size) {
    TileRenderer::RenderParams params;
    params.gaussian_buffer = p_gaussian_buffer;
    params.sorted_indices = p_sorted_indices;
    params.splat_count = p_splat_count;
    params.total_gaussians = p_splat_count;
    params.viewport_size = Vector2i(p_viewport_width, p_viewport_height);
    params.world_to_camera_transform = Transform3D();
    const int safe_height = (p_viewport_height > 0) ? p_viewport_height : 1;
    params.projection.set_perspective(60.0f, float(p_viewport_width) / float(safe_height), 0.1f, 100.0f);
    params.render_projection = params.projection;
    params.tile_size = p_tile_size;
    params.debug_show_performance_hud = true; // Enables async tile stats collection in test mode.
    return params;
}

// Instance-pipeline input buffers that the post-instance-routing TileRenderer::render()
// requires. On current master, render() routes unconditionally through the instance
// pipeline: _validate_and_configure_settings() runs first_tile_runtime_violation(), which
// hard-fails (reason=tile_instance_buffer_missing / _grading_ / _splat_ref_ / _indirect_*)
// unless instance_buffer / instance_grading_buffer / splat_ref_buffer /
// instance_indirect_count_buffer / instance_indirect_dispatch_buffer are all valid. The
// tile_binning shader then binds splat_ref/instance/grading at set=0 bindings 12/13/20 and
// derives the visible count from the indirect buffers' element_count, so the pre-instance
// direct path (gaussian_buffer + sorted_indices only) can no longer drive real binning. A
// GPU test that provokes a genuine binning overlap-record drop must therefore build these.
//
// Layout: a single identity instance (no rotation, unit uniform scale, no translation, full
// opacity) so each splat's world position equals its gaussian's local position; splat_refs
// map visible splat i -> atlas gaussian i on that instance; both indirect buffers carry the
// shared IndirectDispatchLayout with element_count == p_splat_count and dispatch_xyz sized to
// cover all p_splat_count threads in BINNING_GROUP_SIZE-wide workgroups.
struct InstancePipelineTestInputs {
    RID splat_ref_buffer;
    RID instance_buffer;
    RID instance_grading_buffer;
    RID indirect_count_buffer;
    RID indirect_dispatch_buffer;

    bool is_valid() const {
        return splat_ref_buffer.is_valid() && instance_buffer.is_valid() &&
                instance_grading_buffer.is_valid() && indirect_count_buffer.is_valid() &&
                indirect_dispatch_buffer.is_valid();
    }

    void free(RenderingDevice *p_rd) {
        if (p_rd == nullptr) {
            return;
        }
        RID *rids[] = { &splat_ref_buffer, &instance_buffer, &instance_grading_buffer,
                &indirect_count_buffer, &indirect_dispatch_buffer };
        for (RID *rid : rids) {
            if (rid->is_valid()) {
                p_rd->free(*rid);
                *rid = RID();
            }
        }
    }
};

static InstancePipelineTestInputs create_instance_pipeline_test_inputs(RenderingDevice *p_rd, uint32_t p_splat_count) {
    InstancePipelineTestInputs inputs;
    if (p_rd == nullptr || p_splat_count == 0) {
        return inputs;
    }

    // splat_ref_buffer (binding 12): one row per visible splat -> atlas gaussian i, instance 0.
    Vector<uint8_t> splat_ref_data;
    splat_ref_data.resize(int64_t(p_splat_count) * int64_t(sizeof(SplatRefGPU)));
    {
        SplatRefGPU *refs = reinterpret_cast<SplatRefGPU *>(splat_ref_data.ptrw());
        for (uint32_t i = 0; i < p_splat_count; i++) {
            refs[i].instance_id = 0u;
            refs[i].atlas_index = i;
        }
    }
    inputs.splat_ref_buffer = p_rd->storage_buffer_create(splat_ref_data.size(), splat_ref_data);

    // instance_buffer (binding 13): a single identity instance.
    InstanceDataGPU instance = {};
    instance.rotation[3] = 1.0f;           // identity quaternion (0,0,0,1)
    instance.inv_rotation[3] = 1.0f;
    instance.translation_scale[3] = 1.0f;  // w = uniform_scale = 1
    instance.params[0] = 1.0f;             // x = opacity
    Vector<uint8_t> instance_data;
    instance_data.resize(sizeof(InstanceDataGPU));
    memcpy(instance_data.ptrw(), &instance, sizeof(InstanceDataGPU));
    inputs.instance_buffer = p_rd->storage_buffer_create(instance_data.size(), instance_data);

    // instance_grading_buffer (binding 20): neutral grading (primary.x == 0 -> disabled).
    InstanceGradingGPU grading = {};
    Vector<uint8_t> grading_data;
    grading_data.resize(sizeof(InstanceGradingGPU));
    memcpy(grading_data.ptrw(), &grading, sizeof(InstanceGradingGPU));
    inputs.instance_grading_buffer = p_rd->storage_buffer_create(grading_data.size(), grading_data);

    // Indirect buffers: element_count feeds gs_get_visible_gaussian_count() (binding 15, from
    // indirect_count_buffer) and dispatch_xyz feeds compute_list_dispatch_indirect (from
    // indirect_dispatch_buffer). Both carry the full IndirectDispatchLayout so either read is
    // satisfied; the dispatch buffer additionally needs the DISPATCH_INDIRECT usage bit.
    GaussianSplatting::IndirectDispatchLayout dispatch = {};
    dispatch.dispatch_x = (p_splat_count + TileRenderer::BINNING_GROUP_SIZE - 1u) / TileRenderer::BINNING_GROUP_SIZE;
    dispatch.dispatch_y = 1u;
    dispatch.dispatch_z = 1u;
    dispatch.element_count = p_splat_count;
    Vector<uint8_t> dispatch_data;
    dispatch_data.resize(sizeof(GaussianSplatting::IndirectDispatchLayout));
    memcpy(dispatch_data.ptrw(), &dispatch, sizeof(GaussianSplatting::IndirectDispatchLayout));
    inputs.indirect_count_buffer = p_rd->storage_buffer_create(dispatch_data.size(), dispatch_data,
            RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
    inputs.indirect_dispatch_buffer = p_rd->storage_buffer_create(dispatch_data.size(), dispatch_data,
            RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);

    return inputs;
}

static void bind_instance_pipeline_inputs(TileRenderer::RenderParams &p_params,
        const InstancePipelineTestInputs &p_inputs, uint32_t p_splat_count) {
    p_params.instance_buffer = p_inputs.instance_buffer;
    p_params.instance_grading_buffer = p_inputs.instance_grading_buffer;
    p_params.splat_ref_buffer = p_inputs.splat_ref_buffer;
    p_params.instance_indirect_count_buffer = p_inputs.indirect_count_buffer;
    p_params.instance_indirect_dispatch_buffer = p_inputs.indirect_dispatch_buffer;
    // GPU-indirect visible count can exceed the (stale) CPU splat_count; size the per-splat
    // projection/visibility buffers for the full instance-pipeline count.
    p_params.max_visible_splats = p_splat_count;
}

static bool read_texture_pixels(RenderingDevice *p_rd, RID p_texture, Vector<uint8_t> &r_pixels) {
    r_pixels.clear();
    if (p_rd == nullptr || !p_texture.is_valid()) {
        return false;
    }
    r_pixels = p_rd->texture_get_data(p_texture, 0);
    return !r_pixels.is_empty() && (r_pixels.size() % 4) == 0;
}

struct TextureMetrics {
    float average_luma = 0.0f;
    float average_alpha = 0.0f;
    uint32_t non_zero_pixels = 0;
};

static TextureMetrics compute_texture_metrics(const Vector<uint8_t> &p_pixels) {
    TextureMetrics metrics;
    if (p_pixels.is_empty() || (p_pixels.size() % 4) != 0) {
        return metrics;
    }

    const uint8_t *read = p_pixels.ptr();
    const int pixel_count = p_pixels.size() / 4;
    double luma_sum = 0.0;
    double alpha_sum = 0.0;

    for (int i = 0; i < pixel_count; i++) {
        const uint8_t r = read[i * 4 + 0];
        const uint8_t g = read[i * 4 + 1];
        const uint8_t b = read[i * 4 + 2];
        const uint8_t a = read[i * 4 + 3];
        const uint8_t intensity = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
        if (intensity > 0 || a > 0) {
            metrics.non_zero_pixels++;
        }

        luma_sum += (0.2126 * double(r) + 0.7152 * double(g) + 0.0722 * double(b)) / 255.0;
        alpha_sum += double(a) / 255.0;
    }

    metrics.average_luma = float(luma_sum / double(pixel_count));
    metrics.average_alpha = float(alpha_sum / double(pixel_count));
    return metrics;
}

} // namespace

#ifndef TILE_RENDERER_REGRESSION_TEST_H
#define TILE_RENDERER_REGRESSION_TEST_H

/**
 * Tile Renderer Regression Test Suite
 *
 * Tests for Issue #127 - Tile Rasterization Fortification
 * Validates overflow protection, error detection, and dense scene rendering
 */
class TileRendererRegressionTest : public RefCounted {
    GDCLASS(TileRendererRegressionTest, RefCounted);

public:
    struct TestResult {
        bool passed = false;
        String error_message;
        float execution_time_ms = 0.0f;
        TileRenderer::RenderStats stats;
    };

    TileRendererRegressionTest();
    ~TileRendererRegressionTest();

    // Main test suite entry point
    bool run_all_tests(RenderingDevice *p_rd);

    // Individual test cases
    TestResult test_overflow_protection(RenderingDevice *p_rd);
    TestResult test_dense_scene_rendering(RenderingDevice *p_rd);
    TestResult test_validation_and_error_detection(RenderingDevice *p_rd);
    TestResult test_compute_fragment_clamp_parity(RenderingDevice *p_rd);
    TestResult test_distance_cull_sort_order_stability(RenderingDevice *p_rd);
    TestResult test_compute_format_fallback(RenderingDevice *p_rd);
    TestResult test_alpha_compositing_accuracy(RenderingDevice *p_rd);
    TestResult test_performance_regression(RenderingDevice *p_rd);
    TestResult test_renderer_lifecycle_leak_detection(RenderingDevice *p_rd);
    TestResult test_zero_work_frame_resets_raster_timing(RenderingDevice *p_rd);
    // C4b (G4), Channel A: provoke a binning overlap-record drop and assert the always-on
    // resident-signal telemetry (overflow_drop_events) goes non-zero. Self-initializes the
    // tile renderer, so it can be driven standalone from a dedicated [RequiresGPU] TEST_CASE.
    TestResult test_overflow_drop_telemetry(RenderingDevice *p_rd);
    // #586: with the global-composite sorter unavailable and translucent work present, the
    // frame must be REJECTED (nothing published) instead of rasterized in the wrong alpha
    // order -- and a healthy sorter must still publish. Self-initializes the tile renderer.
    TestResult test_sorter_unavailable_rejects_frame(RenderingDevice *p_rd);
    // #586 PR 2: after a real creation failure the tile sorter must be rebuilt by the
    // production code on the shared GPU-003 backoff -- not before the window, not never.
    TestResult test_sorter_unavailable_retries_and_recovers(RenderingDevice *p_rd);

    // Test utilities
    Vector<Gaussian> generate_test_gaussians(uint32_t count, bool valid = true);
    RID create_test_gaussian_buffer(RenderingDevice *p_rd, const Vector<Gaussian> &gaussians);
    RID create_test_sorted_indices(RenderingDevice *p_rd, uint32_t count);
    bool compare_render_output(RID texture1, RID texture2, float tolerance = 0.01f);

    // Reference data generation
    bool generate_reference_captures(RenderingDevice *p_rd);
    bool validate_against_reference(RID output_texture, const String &reference_name);

private:
    Ref<TileRenderer> tile_renderer;
    Vector<TestResult> test_results;

    // Test configuration
    static constexpr int TEST_VIEWPORT_WIDTH = 512;
    static constexpr int TEST_VIEWPORT_HEIGHT = 512;
    static constexpr int TEST_TILE_SIZE = 16;
    static constexpr uint32_t DENSE_SCENE_SPLAT_COUNT = 50000;
    static constexpr uint32_t OVERFLOW_TEST_SPLAT_COUNT = 100000;

    void _log_test_result(const String &test_name, const TestResult &result);
    Gaussian _create_test_gaussian(const Vector3 &position, const Vector3 &scale = Vector3(1, 1, 1), float opacity = 1.0f);
    bool _validate_tile_overflow_handling(const TileRenderer::RenderStats &stats);
};

#endif // TILE_RENDERER_REGRESSION_TEST_H

// Implementation

TileRendererRegressionTest::TileRendererRegressionTest() {
    tile_renderer.instantiate();
}

TileRendererRegressionTest::~TileRendererRegressionTest() {
    if (tile_renderer.is_valid()) {
        tile_renderer->cleanup();
    }
}

bool TileRendererRegressionTest::run_all_tests(RenderingDevice *p_rd) {
    ERR_FAIL_NULL_V(p_rd, false);

    print_line("[TileRendererRegressionTest] Starting tile rasterization regression tests...");

    test_results.clear();
    bool all_passed = true;

    // Initialize tile renderer
    Error err = tile_renderer->initialize(p_rd, Vector2i(TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT), TEST_TILE_SIZE);
    if (err != OK) {
        ERR_PRINT("[TileRendererRegressionTest] Failed to initialize tile renderer");
        return false;
    }

    // Run individual test cases
    Vector<std::pair<String, std::function<TestResult()>>> tests = {
        {"overflow_protection", [this, p_rd]() { return test_overflow_protection(p_rd); }},
        {"dense_scene_rendering", [this, p_rd]() { return test_dense_scene_rendering(p_rd); }},
        {"validation_and_error_detection", [this, p_rd]() { return test_validation_and_error_detection(p_rd); }},
        {"compute_fragment_clamp_parity", [this, p_rd]() { return test_compute_fragment_clamp_parity(p_rd); }},
        {"distance_cull_sort_order_stability", [this, p_rd]() { return test_distance_cull_sort_order_stability(p_rd); }},
        {"compute_format_fallback", [this, p_rd]() { return test_compute_format_fallback(p_rd); }},
        {"alpha_compositing_accuracy", [this, p_rd]() { return test_alpha_compositing_accuracy(p_rd); }},
        {"performance_regression", [this, p_rd]() { return test_performance_regression(p_rd); }},
        {"renderer_lifecycle_leak_detection", [this, p_rd]() { return test_renderer_lifecycle_leak_detection(p_rd); }},
        {"zero_work_frame_resets_raster_timing", [this, p_rd]() { return test_zero_work_frame_resets_raster_timing(p_rd); }}
    };

    for (const auto &test : tests) {
        uint64_t start_time = OS::get_singleton()->get_ticks_usec();
        TestResult result = test.second();
        result.execution_time_ms = (OS::get_singleton()->get_ticks_usec() - start_time) / 1000.0f;

        test_results.push_back(result);
        _log_test_result(test.first, result);

        if (!result.passed) {
            all_passed = false;
        }
    }

    print_line(vformat("[TileRendererRegressionTest] Test suite completed. %s",
               all_passed ? "ALL TESTS PASSED" : "SOME TESTS FAILED"));

    return all_passed;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_overflow_protection(RenderingDevice *p_rd) {
    TestResult result;

    tile_renderer->set_debug_binning_counters_enabled(true);

    Vector<Gaussian> gaussians = generate_test_gaussians(OVERFLOW_TEST_SPLAT_COUNT);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, OVERFLOW_TEST_SPLAT_COUNT);
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create overflow test buffers";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, OVERFLOW_TEST_SPLAT_COUNT,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);

    TileRenderer::RenderParams baseline_params = params;
    baseline_params.splat_count = MIN<uint32_t>(2048u, OVERFLOW_TEST_SPLAT_COUNT);
    uint32_t baseline_overlap_records = 0;
    for (int frame = 0; frame < 6; frame++) {
        RID baseline_output = tile_renderer->render(p_rd, baseline_params);
        if (!baseline_output.is_valid()) {
            cleanup();
            result.error_message = "Baseline render failed before overflow workload spike";
            return result;
        }
        baseline_overlap_records = MAX<uint32_t>(baseline_overlap_records, tile_renderer->get_last_render_stats().overlap_records);
    }

    TileRenderer::OverflowStatsSnapshot overflow_stats;
    bool counters_ready = false;
    uint32_t peak_overlap_records = 0;
    for (int frame = 0; frame < 10; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Render failed under overflow workload";
            return result;
        }

        peak_overlap_records = MAX<uint32_t>(peak_overlap_records, tile_renderer->get_last_render_stats().overlap_records);
        overflow_stats = tile_renderer->get_overflow_stats();
        if (overflow_stats.raster_splats_iterated > 0) {
            counters_ready = true;
        }
    }

    result.stats = tile_renderer->get_last_render_stats();

    if (!counters_ready) {
        cleanup();
        result.error_message = "Overflow counters did not become available";
        return result;
    }

    const bool overflow_detected = overflow_stats.overflow_tile_count > 0 ||
            overflow_stats.overflow_splats_clamped > 0 ||
            overflow_stats.overflow_splats_aggregated > 0;
    if (!overflow_detected) {
        cleanup();
        result.error_message = "Expected overflow workload to trigger overflow counters";
        return result;
    }
    if (peak_overlap_records == 0) {
        cleanup();
        result.error_message = "Expected non-zero overlap records after overflow workload spike";
        return result;
    }
    if (peak_overlap_records < baseline_overlap_records) {
        cleanup();
        result.error_message = vformat("Overlap records did not increase after workload spike (baseline=%u peak=%u)",
                baseline_overlap_records, peak_overlap_records);
        return result;
    }

    if (overflow_stats.raster_reject_gaussian_idx_oob != 0 || overflow_stats.raster_reject_sorted_idx_oob != 0) {
        cleanup();
        result.error_message = vformat("Valid overflow workload should not hit OOB indices (gaussian=%d sorted=%d)",
                overflow_stats.raster_reject_gaussian_idx_oob, overflow_stats.raster_reject_sorted_idx_oob);
        return result;
    }

    cleanup();
    result.passed = true;

    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_dense_scene_rendering(RenderingDevice *p_rd) {
    TestResult result;

    tile_renderer->set_debug_binning_counters_enabled(true);

    Vector<Gaussian> gaussians = generate_test_gaussians(DENSE_SCENE_SPLAT_COUNT);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, DENSE_SCENE_SPLAT_COUNT);
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create dense-scene test buffers";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, DENSE_SCENE_SPLAT_COUNT,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);

    bool counters_ready = false;
    float worst_total_ms = 0.0f;
    for (int frame = 0; frame < 6; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Failed to render dense scene workload";
            return result;
        }

        const TileRenderer::DebugCounterSnapshot counters = tile_renderer->get_debug_counters();
        if (counters.success_count > 0) {
            counters_ready = true;
        }

        const float total_ms = tile_renderer->get_tile_assignment_time() + tile_renderer->get_rasterization_time();
        if (!std::isfinite(total_ms) || total_ms < 0.0f) {
            cleanup();
            result.error_message = vformat("Invalid timing metric for dense scene: %.3f ms", total_ms);
            return result;
        }
        if (total_ms > worst_total_ms) {
            worst_total_ms = total_ms;
        }
    }

    const TileRenderer::OverflowStatsSnapshot overflow_stats = tile_renderer->get_overflow_stats();
    if (overflow_stats.raster_reject_gaussian_idx_oob != 0 || overflow_stats.raster_reject_sorted_idx_oob != 0) {
        cleanup();
        result.error_message = vformat("Dense scene hit OOB rejects (gaussian=%d sorted=%d)",
                overflow_stats.raster_reject_gaussian_idx_oob, overflow_stats.raster_reject_sorted_idx_oob);
        return result;
    }
    if (!counters_ready) {
        cleanup();
        result.error_message = "Dense scene did not report any visible splats";
        return result;
    }
    if (worst_total_ms > 5000.0f) {
        cleanup();
        result.error_message = vformat("Dense scene render exceeded sanity budget: %.3f ms", worst_total_ms);
        return result;
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;

    return result;
}

Vector<Gaussian> TileRendererRegressionTest::generate_test_gaussians(uint32_t count, bool valid) {
    Vector<Gaussian> gaussians;
    gaussians.resize(count);

    for (uint32_t i = 0; i < count; i++) {
        Vector3 position(0.0f, 0.0f, -5.0f);
        Vector3 scale(1.0f, 1.0f, 1.0f);
        float opacity = 0.8f;

        if (valid) {
            // Generate valid Gaussians distributed across the viewport
            position = Vector3(
                (float(i % 32) / 32.0f - 0.5f) * 10.0f,
                (float((i / 32) % 32) / 32.0f - 0.5f) * 10.0f,
                -5.0f - float(i / 1024) * 0.1f // Depth variation
            );
            scale = Vector3(0.1f, 0.1f, 0.1f);
            opacity = 0.8f;
        } else {
            // Generate invalid Gaussians for error testing
            if (i % 4 == 0) {
                position = Vector3(NAN, 0, 0); // NaN position
            } else if (i % 4 == 1) {
                position = Vector3(0, 0, 1000000); // Extreme position
            } else if (i % 4 == 2) {
                scale = Vector3(-1, 1, 1); // Invalid scale
            } else {
                opacity = -1.0f; // Invalid opacity
            }
        }

        gaussians.write[i] = _create_test_gaussian(position, scale, opacity);
    }

    return gaussians;
}

Gaussian TileRendererRegressionTest::_create_test_gaussian(const Vector3 &position, const Vector3 &scale, float opacity) {
    Gaussian gaussian = {};  // Zero-initialize all fields

    gaussian.position = position;
    gaussian.scale = scale;
    gaussian.opacity = opacity;

    // Identity rotation (quaternion)
    gaussian.rotation = Quaternion(0, 0, 0, 1);

    // Simple color (red for testing)
    gaussian.sh_dc = Color(1.0f, 0.5f, 0.2f, 1.0f);

    // First-order SH coefficients (zeroed by default initialization above)
    // gaussian.sh_1[0..2] are already zero

    // Initialize other fields
    gaussian.normal = Vector3(0, 0, 1);
    gaussian.area = scale.x * scale.y;
    gaussian.brush_axes = Vector2(1, 1);
    gaussian.stroke_age = 0.0f;
    gaussian.painterly_meta = 0;

    return gaussian;
}

bool TileRendererRegressionTest::_validate_tile_overflow_handling(const TileRenderer::RenderStats &stats) {
    // Check that overflow was detected and handled
    if (stats.tiles_with_overflow == 0) {
        return false; // Should have detected overflow with this many splats
    }

    // Check that max splats per tile doesn't exceed the hard limit by too much
    const float tile_capacity = tile_renderer->get_tile_splat_capacity();
    if (stats.max_splats_in_tile > tile_capacity * 1.1f) {
        return false; // Overflow protection failed
    }

    // Check that error flag is set appropriately
    if (stats.tiles_with_overflow > 0 && !stats.has_rendering_errors) {
        return false; // Should have flagged errors
    }

    return true;
}

void TileRendererRegressionTest::_log_test_result(const String &test_name, const TestResult &result) {
    String status = result.passed ? "PASS" : "FAIL";
    String message = result.passed ? "" : vformat(" - %s", result.error_message);

    print_line(vformat("[%s] %s (%.2f ms)%s", status, test_name, result.execution_time_ms, message));

    if (result.passed && result.stats.total_tiles > 0) {
        print_verbose(vformat("  Stats: %d tiles, %d overflow, %.1f avg splats/tile",
                              result.stats.total_tiles, result.stats.tiles_with_overflow, result.stats.average_splats_per_tile));
    }
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_validation_and_error_detection(RenderingDevice *p_rd) {
    TestResult result;

    tile_renderer->set_debug_binning_counters_enabled(true);

    const uint32_t splat_count = 512;
    Vector<Gaussian> gaussians = generate_test_gaussians(splat_count);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, splat_count);
    RID invalid_sorted_indices;
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
        if (invalid_sorted_indices.is_valid()) {
            p_rd->free(invalid_sorted_indices);
            invalid_sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create validation buffers";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);

    RID valid_output = tile_renderer->render(p_rd, params);
    if (!valid_output.is_valid()) {
        cleanup();
        result.error_message = "Control render failed before validation checks";
        return result;
    }

    TileRenderer::RenderParams missing_sorted = params;
    missing_sorted.sorted_indices = RID();
    RID missing_sorted_output = tile_renderer->render(p_rd, missing_sorted);
    if (missing_sorted_output.is_valid()) {
        cleanup();
        result.error_message = "Render should fail with missing sorted index buffer";
        return result;
    }

    TileRenderer::RenderParams missing_gaussian = params;
    missing_gaussian.gaussian_buffer = RID();
    RID missing_gaussian_output = tile_renderer->render(p_rd, missing_gaussian);
    if (missing_gaussian_output.is_valid()) {
        cleanup();
        result.error_message = "Render should fail with missing gaussian buffer";
        return result;
    }

    Vector<uint32_t> bad_indices;
    bad_indices.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        bad_indices.write[i] = (i % 3 == 0) ? (splat_count + i + 17) : i;
    }
    Vector<uint8_t> bad_index_bytes;
    bad_index_bytes.resize(bad_indices.size() * sizeof(uint32_t));
    memcpy(bad_index_bytes.ptrw(), bad_indices.ptr(), bad_index_bytes.size());
    invalid_sorted_indices = p_rd->storage_buffer_create(bad_index_bytes.size(), bad_index_bytes);
    if (!invalid_sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create invalid sorted index buffer";
        return result;
    }
    p_rd->set_resource_name(invalid_sorted_indices, "GS_Test_Regression_InvalidSortedIndices");

    TileRenderer::RenderParams bad_index_params = params;
    bad_index_params.sorted_indices = invalid_sorted_indices;

    bool saw_oob_reject = false;
    for (int frame = 0; frame < 10; frame++) {
        RID output = tile_renderer->render(p_rd, bad_index_params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Render failed unexpectedly with invalid sorted indices workload";
            return result;
        }

        const TileRenderer::OverflowStatsSnapshot overflow_stats = tile_renderer->get_overflow_stats();
        if (overflow_stats.raster_reject_gaussian_idx_oob > 0 || overflow_stats.raster_reject_sorted_idx_oob > 0) {
            saw_oob_reject = true;
            break;
        }
    }
    if (!saw_oob_reject) {
        cleanup();
        result.error_message = "Expected OOB rejection counters for invalid sorted indices";
        return result;
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_compute_fragment_clamp_parity(RenderingDevice *p_rd) {
    TestResult result;

    tile_renderer->set_debug_binning_counters_enabled(true);

    const uint32_t splat_count = 4096;
    Vector<Gaussian> gaussians;
    gaussians.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        gaussians.write[i] = _create_test_gaussian(Vector3(0.0f, 0.0f, -3.0f), Vector3(2.5f, 2.5f, 2.5f), 0.9f);
    }

    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, splat_count);
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create clamp parity buffers";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);

    TileRenderer::OverflowStatsSnapshot fragment_overflow;
    TileRenderer::OverflowStatsSnapshot compute_overflow;
    TextureMetrics fragment_metrics;
    TextureMetrics compute_metrics;
    bool fragment_ready = false;
    bool compute_ready = false;
    bool compute_path_observed = false;
    bool fragment_metrics_ready = false;
    bool compute_metrics_ready = false;

    params.compute_raster_policy = GaussianSplatting::ComputeRasterPolicy::ForceOff;
    for (int frame = 0; frame < 12; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Fragment reference render failed in clamp parity test";
            return result;
        }
        const TileRenderer::RenderStats stats = tile_renderer->get_last_render_stats();
        if (stats.last_raster_used_compute) {
            cleanup();
            result.error_message = "ForceOff compute policy unexpectedly used compute raster";
            return result;
        }
        fragment_overflow = tile_renderer->get_overflow_stats();
        if (fragment_overflow.raster_sample_count > 0) {
            fragment_ready = true;
        }
    }

    if (!fragment_ready) {
        cleanup();
        result.error_message = "Fragment clamp parity phase did not produce readable overflow stats";
        return result;
    }
    if (fragment_overflow.overflow_splats_clamped == 0) {
        cleanup();
        result.error_message = "Clamp parity workload did not trigger any clamped splats in fragment mode";
        return result;
    }
    {
        RID fragment_output = tile_renderer->render(p_rd, params);
        Vector<uint8_t> fragment_pixels;
        if (fragment_output.is_valid() && read_texture_pixels(p_rd, fragment_output, fragment_pixels)) {
            fragment_metrics = compute_texture_metrics(fragment_pixels);
            fragment_metrics_ready = fragment_metrics.non_zero_pixels > 0;
        }
    }

    params.compute_raster_policy = GaussianSplatting::ComputeRasterPolicy::ForceOn;
    for (int frame = 0; frame < 12; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Compute phase render failed in clamp parity test";
            return result;
        }
        const TileRenderer::RenderStats stats = tile_renderer->get_last_render_stats();
        compute_path_observed = compute_path_observed || stats.last_raster_used_compute;
        compute_overflow = tile_renderer->get_overflow_stats();
        if (compute_overflow.raster_sample_count > 0) {
            compute_ready = true;
        }
    }

    if (!compute_ready) {
        cleanup();
        result.error_message = "Compute clamp parity phase did not produce readable overflow stats";
        return result;
    }
    {
        RID compute_output = tile_renderer->render(p_rd, params);
        Vector<uint8_t> compute_pixels;
        if (compute_output.is_valid() && read_texture_pixels(p_rd, compute_output, compute_pixels)) {
            compute_metrics = compute_texture_metrics(compute_pixels);
            compute_metrics_ready = compute_metrics.non_zero_pixels > 0;
        }
    }

    if (compute_path_observed) {
        if (compute_overflow.overflow_splats_clamped != fragment_overflow.overflow_splats_clamped) {
            cleanup();
            result.error_message = vformat("Clamp parity mismatch between fragment (%u) and compute (%u)",
                    fragment_overflow.overflow_splats_clamped, compute_overflow.overflow_splats_clamped);
            return result;
        }
        if (!fragment_metrics_ready || !compute_metrics_ready) {
            cleanup();
            result.error_message = "Failed to collect render metrics for compute/fragment parity comparison";
            return result;
        }
        const float luma_delta = std::abs(fragment_metrics.average_luma - compute_metrics.average_luma);
        const float alpha_delta = std::abs(fragment_metrics.average_alpha - compute_metrics.average_alpha);
        if (luma_delta > 0.03f || alpha_delta > 0.03f) {
            cleanup();
            result.error_message = vformat("Compute/fragment visual parity drift exceeds tolerance (luma=%.4f alpha=%.4f)",
                    luma_delta, alpha_delta);
            return result;
        }
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_distance_cull_sort_order_stability(RenderingDevice *p_rd) {
    TestResult result;

    tile_renderer->set_debug_binning_counters_enabled(true);

    const uint32_t grid_x = 48;
    const uint32_t grid_y = 32;
    const uint32_t splat_count = grid_x * grid_y;
    Vector<Gaussian> gaussians;
    gaussians.resize(splat_count);
    for (uint32_t y = 0; y < grid_y; y++) {
        for (uint32_t x = 0; x < grid_x; x++) {
            const uint32_t idx = y * grid_x + x;
            const float fx = (float(x) + 0.5f) / float(grid_x);
            const float fy = (float(y) + 0.5f) / float(grid_y);
            const float wx = (fx - 0.5f) * 8.5f;
            const float wy = (fy - 0.5f) * 5.5f;
            const float wz = -8.0f - 0.002f * float(idx % 31);

            Gaussian gaussian = _create_test_gaussian(Vector3(wx, wy, wz), Vector3(0.09f, 0.09f, 0.09f), 0.92f);
            const float color_a = float((idx * 17u) % 251u) / 250.0f;
            const float color_b = float((idx * 29u) % 241u) / 240.0f;
            const float color_c = float((idx * 43u) % 239u) / 238.0f;
            gaussian.sh_dc = Color(0.15f + 0.85f * color_a, 0.10f + 0.90f * color_b, 0.12f + 0.88f * color_c, 1.0f);
            gaussians.write[idx] = gaussian;
        }
    }

    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices_identity = create_test_sorted_indices(p_rd, splat_count);
    RID sorted_indices_reversed;
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices_identity.is_valid()) {
            p_rd->free(sorted_indices_identity);
            sorted_indices_identity = RID();
        }
        if (sorted_indices_reversed.is_valid()) {
            p_rd->free(sorted_indices_reversed);
            sorted_indices_reversed = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices_identity.is_valid()) {
        cleanup();
        result.error_message = "Failed to create distance-cull sort-order stability buffers";
        return result;
    }

    Vector<uint32_t> reversed_indices;
    reversed_indices.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        reversed_indices.write[i] = splat_count - 1u - i;
    }
    Vector<uint8_t> reversed_bytes;
    reversed_bytes.resize(reversed_indices.size() * sizeof(uint32_t));
    memcpy(reversed_bytes.ptrw(), reversed_indices.ptr(), reversed_bytes.size());
    sorted_indices_reversed = p_rd->storage_buffer_create(reversed_bytes.size(), reversed_bytes);
    if (!sorted_indices_reversed.is_valid()) {
        cleanup();
        result.error_message = "Failed to create reversed sorted index buffer";
        return result;
    }
    p_rd->set_resource_name(sorted_indices_reversed, "GS_Test_Regression_SortedIndicesReversed");

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices_identity, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
    params.compute_raster_policy = GaussianSplatting::ComputeRasterPolicy::ForceOff;
    params.distance_cull_enabled = true;
    params.distance_cull_start = 1.0f;
    params.distance_cull_max_rate = 0.8f;
    params.opacity_aware_culling = false;
    params.tiny_splat_screen_radius = 0.0f;
    params.lod_blend_enabled = false;
    params.enable_direct_lighting = false;

    RID identity_output;
    RID reversed_output;
    Vector<uint8_t> identity_pixels;
    Vector<uint8_t> reversed_pixels;
    bool saw_identity_cull = false;
    bool saw_reversed_cull = false;
    for (int frame = 0; frame < 6; frame++) {
        params.sorted_indices = sorted_indices_identity;
        identity_output = tile_renderer->render(p_rd, params);
        if (!identity_output.is_valid()) {
            cleanup();
            result.error_message = "Identity-order render failed in distance-cull stability test";
            return result;
        }
        const TileRenderer::DebugCounterSnapshot counters = tile_renderer->get_debug_counters();
        saw_identity_cull = saw_identity_cull || counters.distance_cull_reject > 0;
    }
    if (!read_texture_pixels(p_rd, identity_output, identity_pixels)) {
        cleanup();
        result.error_message = "Failed to read identity-order output in distance-cull stability test";
        return result;
    }

    for (int frame = 0; frame < 6; frame++) {
        params.sorted_indices = sorted_indices_reversed;
        reversed_output = tile_renderer->render(p_rd, params);
        if (!reversed_output.is_valid()) {
            cleanup();
            result.error_message = "Reversed-order render failed in distance-cull stability test";
            return result;
        }
        const TileRenderer::DebugCounterSnapshot counters = tile_renderer->get_debug_counters();
        saw_reversed_cull = saw_reversed_cull || counters.distance_cull_reject > 0;
    }
    if (!read_texture_pixels(p_rd, reversed_output, reversed_pixels)) {
        cleanup();
        result.error_message = "Failed to read reversed-order output in distance-cull stability test";
        return result;
    }

    if (!saw_identity_cull || !saw_reversed_cull) {
        cleanup();
        result.error_message = "Distance-cull stability workload did not trigger distance_cull_reject counters";
        return result;
    }
    if (identity_pixels.size() != reversed_pixels.size() || identity_pixels.is_empty()) {
        cleanup();
        result.error_message = "Distance-cull stability output size mismatch";
        return result;
    }

    const TextureMetrics identity_metrics = compute_texture_metrics(identity_pixels);
    const TextureMetrics reversed_metrics = compute_texture_metrics(reversed_pixels);
    if (identity_metrics.non_zero_pixels == 0 || reversed_metrics.non_zero_pixels == 0) {
        cleanup();
        result.error_message = "Distance-cull stability outputs are unexpectedly empty";
        return result;
    }

    double normalized_error = 0.0;
    const uint8_t *identity_read = identity_pixels.ptr();
    const uint8_t *reversed_read = reversed_pixels.ptr();
    for (int i = 0; i < identity_pixels.size(); i++) {
        int diff = int(identity_read[i]) - int(reversed_read[i]);
        if (diff < 0) {
            diff = -diff;
        }
        normalized_error += double(diff) / 255.0;
    }
    normalized_error /= double(identity_pixels.size());

    const float luma_delta = std::abs(identity_metrics.average_luma - reversed_metrics.average_luma);
    const float alpha_delta = std::abs(identity_metrics.average_alpha - reversed_metrics.average_alpha);
    if (normalized_error > 0.015 || luma_delta > 0.015f || alpha_delta > 0.015f) {
        cleanup();
        result.error_message = vformat(
                "Distance-cull output drift under sorted-order churn exceeds tolerance (error=%.4f luma=%.4f alpha=%.4f)",
                normalized_error, luma_delta, alpha_delta);
        return result;
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_compute_format_fallback(RenderingDevice *p_rd) {
    TestResult result;

    const uint32_t splat_count = 2048;
    Vector<Gaussian> gaussians;
    gaussians.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        const float x = (float(i % 64) / 63.0f - 0.5f) * 0.6f;
        const float y = (float((i / 64) % 32) / 31.0f - 0.5f) * 0.6f;
        gaussians.write[i] = _create_test_gaussian(Vector3(x, y, -3.0f), Vector3(0.6f, 0.6f, 0.6f), 0.85f);
    }

    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, splat_count);
    auto cleanup = [&]() {
        tile_renderer->set_output_format(RD::DATA_FORMAT_R8G8B8A8_UNORM);
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create compute format fallback buffers";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
    params.compute_raster_policy = GaussianSplatting::ComputeRasterPolicy::ForceOn;

    tile_renderer->set_output_format(RD::DATA_FORMAT_R8G8B8A8_UNORM);
    bool baseline_compute_supported = false;
    for (int frame = 0; frame < 4; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Baseline render failed in compute format fallback test";
            return result;
        }
        baseline_compute_supported = baseline_compute_supported || tile_renderer->get_last_render_stats().last_raster_used_compute;
    }

    tile_renderer->set_output_format(RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
    if (tile_renderer->get_output_format() != RD::DATA_FORMAT_R16G16B16A16_SFLOAT) {
        cleanup();
        result.stats = tile_renderer->get_last_render_stats();
        result.passed = true;
        return result;
    }

    bool saw_fragment_fallback = false;
    bool saw_compute = false;
    bool saw_valid_output = false;
    for (int frame = 0; frame < 8; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Render failed in compute format fallback test";
            return result;
        }
        saw_valid_output = true;
        const TileRenderer::RenderStats stats = tile_renderer->get_last_render_stats();
        saw_compute = saw_compute || stats.last_raster_used_compute;
        saw_fragment_fallback = saw_fragment_fallback || !stats.last_raster_used_compute;
    }

    if (!saw_valid_output) {
        cleanup();
        result.error_message = "Compute format fallback test did not produce output";
        return result;
    }
    if (baseline_compute_supported && (saw_compute || !saw_fragment_fallback)) {
        cleanup();
        result.error_message = "Expected non-RGBA8 output format to force compute->fragment fallback";
        return result;
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_alpha_compositing_accuracy(RenderingDevice *p_rd) {
    TestResult result;

    Vector<Gaussian> low_opacity_gaussians;
    Vector<Gaussian> high_opacity_gaussians;
    low_opacity_gaussians.resize(4);
    high_opacity_gaussians.resize(4);

    const Vector3 positions[4] = {
        Vector3(-0.15f, 0.0f, -3.0f),
        Vector3(0.15f, 0.0f, -3.0f),
        Vector3(0.0f, -0.15f, -3.0f),
        Vector3(0.0f, 0.15f, -3.0f),
    };
    for (int i = 0; i < 4; i++) {
        low_opacity_gaussians.write[i] = _create_test_gaussian(positions[i], Vector3(0.45f, 0.45f, 0.45f), 0.2f);
        high_opacity_gaussians.write[i] = _create_test_gaussian(positions[i], Vector3(0.45f, 0.45f, 0.45f), 0.9f);
    }

    RID low_gaussian_buffer = create_test_gaussian_buffer(p_rd, low_opacity_gaussians);
    RID high_gaussian_buffer = create_test_gaussian_buffer(p_rd, high_opacity_gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, 4);
    auto cleanup = [&]() {
        if (low_gaussian_buffer.is_valid()) {
            p_rd->free(low_gaussian_buffer);
            low_gaussian_buffer = RID();
        }
        if (high_gaussian_buffer.is_valid()) {
            p_rd->free(high_gaussian_buffer);
            high_gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!low_gaussian_buffer.is_valid() || !high_gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create alpha compositing test buffers";
        return result;
    }

    TileRenderer::RenderParams low_params = make_render_params(low_gaussian_buffer, sorted_indices, 4,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
    TileRenderer::RenderParams high_params = make_render_params(high_gaussian_buffer, sorted_indices, 4,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);

    RID low_output = tile_renderer->render(p_rd, low_params);
    low_output = tile_renderer->render(p_rd, low_params); // Use second frame after pipeline warmup.
    RID high_output = tile_renderer->render(p_rd, high_params);
    high_output = tile_renderer->render(p_rd, high_params);

    if (!low_output.is_valid() || !high_output.is_valid()) {
        cleanup();
        result.error_message = "Render failed during alpha compositing comparison";
        return result;
    }

    Vector<uint8_t> low_pixels;
    Vector<uint8_t> high_pixels;
    if (!read_texture_pixels(p_rd, low_output, low_pixels) || !read_texture_pixels(p_rd, high_output, high_pixels)) {
        cleanup();
        result.error_message = "Failed to read back output textures for alpha comparison";
        return result;
    }

    if (low_pixels.size() != high_pixels.size()) {
        cleanup();
        result.error_message = "Alpha comparison texture size mismatch";
        return result;
    }

    const TextureMetrics low_metrics = compute_texture_metrics(low_pixels);
    const TextureMetrics high_metrics = compute_texture_metrics(high_pixels);
    if (low_metrics.non_zero_pixels == 0 || high_metrics.non_zero_pixels == 0) {
        cleanup();
        result.error_message = "Expected non-zero rendered pixels for alpha compositing validation";
        return result;
    }

    const int center_x = TEST_VIEWPORT_WIDTH / 2;
    const int center_y = TEST_VIEWPORT_HEIGHT / 2;
    const int center_idx = (center_y * TEST_VIEWPORT_WIDTH + center_x) * 4;
    const int low_center_rgb = int(low_pixels[center_idx + 0]) + int(low_pixels[center_idx + 1]) + int(low_pixels[center_idx + 2]);
    const int high_center_rgb = int(high_pixels[center_idx + 0]) + int(high_pixels[center_idx + 1]) + int(high_pixels[center_idx + 2]);

    if (high_center_rgb <= low_center_rgb) {
        cleanup();
        result.error_message = vformat("Expected higher opacity to increase center intensity (low=%d high=%d)",
                low_center_rgb, high_center_rgb);
        return result;
    }
    if (high_metrics.average_luma <= low_metrics.average_luma + 0.001f) {
        cleanup();
        result.error_message = vformat("Expected higher opacity luma (low=%.4f high=%.4f)",
                low_metrics.average_luma, high_metrics.average_luma);
        return result;
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_performance_regression(RenderingDevice *p_rd) {
    TestResult result;

    const uint32_t splat_count = 8192;
    Vector<Gaussian> gaussians = generate_test_gaussians(splat_count);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, splat_count);
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create performance regression buffers";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);

    float worst_ms = 0.0f;
    bool saw_non_zero_timing = false;
    for (int frame = 0; frame < 6; frame++) {
        RID output = tile_renderer->render(p_rd, params);
        if (!output.is_valid()) {
            cleanup();
            result.error_message = "Performance regression workload render failed";
            return result;
        }

        const float total_ms = tile_renderer->get_tile_assignment_time() + tile_renderer->get_rasterization_time();
        if (!std::isfinite(total_ms) || total_ms < 0.0f) {
            cleanup();
            result.error_message = vformat("Invalid performance timing value: %.4f ms", total_ms);
            return result;
        }
        saw_non_zero_timing = saw_non_zero_timing || (total_ms > 0.0f);
        if (total_ms > worst_ms) {
            worst_ms = total_ms;
        }
    }

    if (!saw_non_zero_timing) {
        cleanup();
        result.error_message = "Performance timing metrics stayed at zero for all frames";
        return result;
    }
    if (worst_ms > 5000.0f) {
        cleanup();
        result.error_message = vformat("Performance sanity budget exceeded: %.3f ms", worst_ms);
        return result;
    }

    result.stats = tile_renderer->get_last_render_stats();
    cleanup();
    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_renderer_lifecycle_leak_detection(RenderingDevice *p_rd) {
    TestResult result;

    static constexpr int LIFECYCLE_ITERATIONS = 3;
    static constexpr uint32_t TEST_SPLAT_COUNT = 2048;
    static constexpr uint64_t ALLOCATION_SLACK = 32;
    static constexpr uint64_t MEMORY_SLACK_BYTES = 16ull * 1024ull * 1024ull;

    uint64_t cleanup_baseline_allocations = 0;
    uint64_t cleanup_baseline_memory = 0;
    bool cleanup_baseline_set = false;

    for (int iteration = 0; iteration < LIFECYCLE_ITERATIONS; iteration++) {
        Vector<Gaussian> gaussians = generate_test_gaussians(TEST_SPLAT_COUNT);
        RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
        RID sorted_indices = create_test_sorted_indices(p_rd, TEST_SPLAT_COUNT);
        auto free_cycle_buffers = [&]() {
            if (gaussian_buffer.is_valid()) {
                p_rd->free(gaussian_buffer);
                gaussian_buffer = RID();
            }
            if (sorted_indices.is_valid()) {
                p_rd->free(sorted_indices);
                sorted_indices = RID();
            }
        };

        if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
            free_cycle_buffers();
            result.error_message = "Failed to create lifecycle leak detection buffers";
            return result;
        }

        TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, TEST_SPLAT_COUNT,
                TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
        for (int frame = 0; frame < 3; frame++) {
            RID output = tile_renderer->render(p_rd, params);
            if (!output.is_valid()) {
                free_cycle_buffers();
                result.error_message = vformat("Renderer lifecycle iteration %d failed to render", iteration);
                return result;
            }
            result.stats = tile_renderer->get_last_render_stats();
        }

        free_cycle_buffers();
        tile_renderer->cleanup();

        // Drain queued GPU work so allocation counters reflect post-cleanup state.
        p_rd->submit();
        p_rd->sync();

        const uint64_t allocations_after_cleanup = p_rd->get_device_allocation_count();
        const uint64_t memory_after_cleanup = p_rd->get_device_total_memory();
        if (!cleanup_baseline_set) {
            cleanup_baseline_set = true;
            cleanup_baseline_allocations = allocations_after_cleanup;
            cleanup_baseline_memory = memory_after_cleanup;
        } else {
            if (allocations_after_cleanup > cleanup_baseline_allocations + ALLOCATION_SLACK) {
                result.error_message = vformat(
                        "Allocation count grew across renderer lifecycle cleanup (baseline=%s current=%s slack=%s)",
                        String::num_uint64(cleanup_baseline_allocations),
                        String::num_uint64(allocations_after_cleanup),
                        String::num_uint64(ALLOCATION_SLACK));
                return result;
            }
            if (memory_after_cleanup > cleanup_baseline_memory + MEMORY_SLACK_BYTES) {
                result.error_message = vformat(
                        "Device memory grew across renderer lifecycle cleanup (baseline=%s current=%s slack=%s bytes)",
                        String::num_uint64(cleanup_baseline_memory),
                        String::num_uint64(memory_after_cleanup),
                        String::num_uint64(MEMORY_SLACK_BYTES));
                return result;
            }
        }

        if (iteration + 1 < LIFECYCLE_ITERATIONS) {
            Error err = tile_renderer->initialize(p_rd, Vector2i(TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT), TEST_TILE_SIZE);
            if (err != OK) {
                result.error_message = vformat("Failed to reinitialize tile renderer in lifecycle iteration %d", iteration);
                return result;
            }
        }
    }

    // Re-initialize the shared renderer after the final cleanup so subsequent
    // tests find it in a usable state.
    {
        Error err = tile_renderer->initialize(p_rd, Vector2i(TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT), TEST_TILE_SIZE);
        if (err != OK) {
            result.error_message = "Failed to reinitialize tile renderer after lifecycle leak test";
            return result;
        }
    }

    result.passed = true;
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_zero_work_frame_resets_raster_timing(RenderingDevice *p_rd) {
    TestResult result;

    static constexpr uint32_t TEST_SPLAT_COUNT = 2048;

    Vector<Gaussian> gaussians = generate_test_gaussians(TEST_SPLAT_COUNT);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, TEST_SPLAT_COUNT);
    auto cleanup = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
    };

    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid()) {
        cleanup();
        result.error_message = "Failed to create buffers for zero-work raster timing test";
        return result;
    }

    TileRenderer::RenderParams active_params = make_render_params(gaussian_buffer, sorted_indices, TEST_SPLAT_COUNT,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
    RID active_output = tile_renderer->render(p_rd, active_params);
    if (!active_output.is_valid()) {
        cleanup();
        result.error_message = "Failed to render active frame for zero-work timing test";
        return result;
    }

    TileRenderer::RenderParams idle_params = active_params;
    idle_params.splat_count = 0;
    idle_params.total_gaussians = 0;
    RID idle_output = tile_renderer->render(p_rd, idle_params);
    if (!idle_output.is_valid()) {
        cleanup();
        result.error_message = "Failed to render zero-work frame for timing reset test";
        return result;
    }

    const float idle_raster_ms = tile_renderer->get_rasterization_time();
    if (idle_raster_ms != 0.0f) {
        cleanup();
        result.error_message = vformat("Expected rasterization_ms to reset to 0 on zero-work frame (got %.6f)", idle_raster_ms);
        return result;
    }

    cleanup();
    result.passed = true;
    return result;
}

RID TileRendererRegressionTest::create_test_gaussian_buffer(RenderingDevice *p_rd, const Vector<Gaussian> &gaussians) {
    if (gaussians.size() == 0) {
        return RID();
    }

    // Pack into the GPU layout rather than memcpy'ing the CPU struct.
    //
    // This used to upload `Vector<Gaussian>` verbatim, which only ever worked
    // because `sizeof(Gaussian)` and `sizeof(PackedGaussian)` both happened to
    // be 144 -- and even then only the SIZE matched. The two layouts differ in
    // content regardless: the CPU struct carries `Vector3 sh_1[3]` (36 bytes)
    // where the GPU layout carries `sh_encoded[12]` (48), so the shader was
    // already reading SH from the wrong offsets. Shrinking PackedGaussian to
    // 128 turned that latent content mismatch into a stride mismatch too.
    //
    // Packing through the real `pack_gaussian()` fixes both: the buffer now
    // holds exactly what the shader's `Gaussian[]` declares, at its stride,
    // and it cannot silently desynchronise again the next time either struct
    // changes size.
    Vector<PackedGaussian> packed;
    packed.resize(gaussians.size());
    SHCompressionMetrics metrics;
    for (int i = 0; i < gaussians.size(); i++) {
        pack_gaussian(gaussians[i], packed.write[i], metrics);
    }

    Vector<uint8_t> buffer_data;
    buffer_data.resize(packed.size() * sizeof(PackedGaussian));
    memcpy(buffer_data.ptrw(), packed.ptr(), buffer_data.size());

    RID buffer = p_rd->storage_buffer_create(buffer_data.size(), buffer_data);
    p_rd->set_resource_name(buffer, "GS_Test_Regression_GaussianBuffer");
    return buffer;
}

RID TileRendererRegressionTest::create_test_sorted_indices(RenderingDevice *p_rd, uint32_t count) {
    Vector<uint32_t> indices;
    indices.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        indices.write[i] = i;
    }

    Vector<uint8_t> buffer_data;
    buffer_data.resize(indices.size() * sizeof(uint32_t));
    memcpy(buffer_data.ptrw(), indices.ptr(), buffer_data.size());

    RID buffer = p_rd->storage_buffer_create(buffer_data.size(), buffer_data);
    p_rd->set_resource_name(buffer, "GS_Test_Regression_SortedIndices");
    return buffer;
}

bool TileRendererRegressionTest::compare_render_output(RID texture1, RID texture2, float tolerance) {
    if (!texture1.is_valid() || !texture2.is_valid()) {
        return false;
    }

    RenderingDevice *rd = tile_renderer->get_output_texture_owner();
    if (!rd) {
        return false;
    }

    Vector<uint8_t> pixels_a;
    Vector<uint8_t> pixels_b;
    if (!read_texture_pixels(rd, texture1, pixels_a) || !read_texture_pixels(rd, texture2, pixels_b)) {
        return false;
    }
    if (pixels_a.size() != pixels_b.size()) {
        return false;
    }

    const uint8_t *a = pixels_a.ptr();
    const uint8_t *b = pixels_b.ptr();
    double normalized_error = 0.0;
    for (int i = 0; i < pixels_a.size(); i++) {
        const int diff = int(a[i]) - int(b[i]);
        const int abs_diff = (diff >= 0) ? diff : -diff;
        normalized_error += double(abs_diff) / 255.0;
    }
    normalized_error /= double(pixels_a.size());
    return normalized_error <= double(tolerance);
}

bool TileRendererRegressionTest::generate_reference_captures(RenderingDevice *p_rd) {
    // Stub implementation
    return true;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_overflow_drop_telemetry(RenderingDevice *p_rd) {
    // C4b / exit criterion G4 ("no silent degradation"), Channel A: on-GPU proof that a real
    // binning overlap-record drop makes the always-on resident-signal telemetry fire, and that
    // it does NOT fire without one. This drives TileRenderer::render() through the actual
    // instance pipeline (build_instance_pipeline_test_inputs supplies the splat_ref / instance /
    // grading / indirect buffers the post-instance-routing render() now requires), so the
    // tile_binning EMIT pass runs for real.
    //
    // Two phases, control first so the sticky drop signal cannot leak across them:
    //   1. Control: a small 512-splat scene whose overlap records stay far under the forced-low
    //      budget. It renders cleanly and overflow_drop_events MUST stay put -- this is what
    //      makes the test discriminate (a drop counter that ticked here would be broken).
    //   2. Overflow: force max_overlap_records to its minimum valid value (100000) so a dense
    //      100K-splat cloud at 512x512 (each splat covering several tiles) exhausts the global
    //      overlap-record budget -> the EMIT-pass drop sites set overflow_drop_signal, which the
    //      always-on async readback surfaces as a non-zero get_overflow_drop_events(). The signal
    //      is sticky (not cleared per frame), so the drop cannot be lost to async-readback timing.
    //
    // Runs on the self-hosted GPU harness lane (needs a real device); it cannot execute on the
    // agent's rasterless environment (REQUIRE_LOCAL_GPU_DEVICE skips there).
    TestResult result;

    ProjectSettings *ps = ProjectSettings::get_singleton();
    {
        // Scope the guard so it restores the setting BEFORE we re-sync the global config below.
        ProjectSettingGuard overlap_guard(ps, GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH);
        if (ps) {
            ps->set_setting(GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH, 100000); // MIN_OVERLAP_RECORDS
        }
        g_gpu_sorting_config.load_from_project_settings();

        result = [&]() -> TestResult {
            TestResult r;

            Error err = tile_renderer->initialize(p_rd, Vector2i(TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT), TEST_TILE_SIZE);
            if (err != OK) {
                r.error_message = "Failed to initialize tile renderer for overflow-drop telemetry test";
                return r;
            }
            tile_renderer->set_debug_binning_counters_enabled(true);

            // Build the full instance-pipeline input set for a given splat count, wire it into
            // params, and render it p_frames times. On failure r_error is set and false returned.
            auto render_scene = [&](uint32_t p_splat_count, int p_frames, uint32_t &r_max_clamped,
                    String &r_error) -> bool {
                r_max_clamped = 0;
                Vector<Gaussian> gaussians = generate_test_gaussians(p_splat_count);
                RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
                RID sorted_indices = create_test_sorted_indices(p_rd, p_splat_count);
                InstancePipelineTestInputs instance_inputs = create_instance_pipeline_test_inputs(p_rd, p_splat_count);
                auto free_scene = [&]() {
                    if (gaussian_buffer.is_valid()) {
                        p_rd->free(gaussian_buffer);
                    }
                    if (sorted_indices.is_valid()) {
                        p_rd->free(sorted_indices);
                    }
                    instance_inputs.free(p_rd);
                };
                if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid() || !instance_inputs.is_valid()) {
                    free_scene();
                    r_error = "Failed to create overflow-drop telemetry scene buffers";
                    return false;
                }

                TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices,
                        p_splat_count, TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
                bind_instance_pipeline_inputs(params, instance_inputs, p_splat_count);

                const uint32_t drop_events_before = tile_renderer->get_overflow_drop_events();
                for (int frame = 0; frame < p_frames; frame++) {
                    RID output = tile_renderer->render(p_rd, params);
                    if (!output.is_valid()) {
                        free_scene();
                        r_error = "Render failed under overflow-drop telemetry workload";
                        return false;
                    }
                    r_max_clamped = MAX<uint32_t>(r_max_clamped, tile_renderer->get_overflow_stats().overflow_splats_clamped);
                    // Early out once the drop counter has moved (overflow phase only benefits).
                    if (tile_renderer->get_overflow_drop_events() > drop_events_before) {
                        break;
                    }
                }
                free_scene();
                return true;
            };

            // --- Phase 1: control. A small, non-overflowing scene must leave the counter at 0. ---
            const uint32_t control_baseline = tile_renderer->get_overflow_drop_events();
            uint32_t control_clamped = 0;
            String control_error;
            if (!render_scene(512u, 8, control_clamped, control_error)) {
                r.error_message = control_error;
                return r;
            }
            const uint32_t control_after = tile_renderer->get_overflow_drop_events();
            if (control_after != control_baseline) {
                r.error_message = vformat(
                        "Control (no-overflow) scene raised overflow_drop_events %d->%d (overflow_splats_clamped "
                        "observed=%d). The Channel A drop counter fired without any binning overlap-record drop; "
                        "the telemetry does not discriminate.",
                        control_baseline, control_after, control_clamped);
                return r;
            }

            // --- Phase 2: overflow. The dense workload must drive the counter above baseline. ---
            const uint32_t baseline_drop_events = tile_renderer->get_overflow_drop_events();
            uint32_t clamped_seen = 0;
            String overflow_error;
            if (!render_scene(OVERFLOW_TEST_SPLAT_COUNT, 24, clamped_seen, overflow_error)) {
                r.error_message = overflow_error;
                return r;
            }
            const uint32_t drop_events = tile_renderer->get_overflow_drop_events();
            if (drop_events <= baseline_drop_events) {
                r.error_message = vformat(
                        "overflow_drop_events did not increment after 24 frames of a dense overflow workload "
                        "(baseline=%d final=%d; overflow_splats_clamped observed=%d). clamped==0 => the workload "
                        "did not provoke a binning overlap-record drop; clamped>0 => the Channel A resident-signal "
                        "telemetry did not fire.",
                        baseline_drop_events, drop_events, clamped_seen);
                return r;
            }

            r.passed = true;
            return r;
        }();
    }
    // Guard has restored the project setting; re-sync the global config so later tests see the
    // original max_overlap_records rather than the forced-low value.
    g_gpu_sorting_config.load_from_project_settings();

    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_sorter_unavailable_rejects_frame(RenderingDevice *p_rd) {
    // #586 on-GPU proof, driving the REAL failure path rather than an injected end state.
    //
    // The defect: when TileGlobalSortResources::sorter is invalid and there is translucent
    // work, TileRenderer::render() fell straight through binning/emit/raster and PRESENTED
    // tiles in atomic-append order -- mathematically wrong alpha compositing dressed up as a
    // normal render (unsorted_composite_frames counted it; nothing stopped it). The fix turns
    // that into a publish reject: render() returns an invalid RID, the caller presents
    // nothing, and global_composite_rejected_frames counts it.
    //
    // How the sorter is made unavailable -- the production path, not a test hook:
    // radix_bits=8 x workgroup_size=64 x key_bits=64 makes RadixSort's histogram buffer
    // 128 bytes per element (workgroups * 256 bins * 8 passes * 4 bytes), so a capacity
    // request of REFUSED_CAPACITY (34,000,000) records needs 4,352,000,000 bytes for that
    // one buffer, above the RenderingDevice uint32 size limit. ensure_resources()'s
    // allocation-free size preflight (#586 PR 2; before it, RadixSort::initialize()'s own
    // guard after a full shader compile) refuses the request, and
    // TileGlobalSortResources::ensure_resources() runs its disable_sorter() lambda for real:
    // sorter shut down and unref'd, sorter_available cleared. The key/value buffers
    // (34M * 12 B = 408 MB) are then allocated normally, which is exactly the state the choke
    // point sees in the field. The same recipe reproduces the defect in the QA project from
    // project settings alone (max_overlap_records_adaptive_min), so this is the live trigger,
    // not a simulation of one. NOT covered here: the capability-probe-false cause (no
    // supported desktop GPU fails the probe) and the VRAM-pressure allocation-failure cause
    // (not deterministically reproducible on NVIDIA, which spills to host memory).
    //
    // Phases; the discriminating assertion is the OBSERVABLE CONSEQUENCE (was a frame
    // published?), never a log line:
    //   1. HEALTHY CONTROL -- default sorter config, same scene: render() must publish and
    //      neither counter may move. Without this the fix could "pass" by rejecting everything.
    //   2. SORTER UNAVAILABLE -- the real trigger above, same scene, splat_count > 0: render()
    //      must return an INVALID RID, global_composite_rejected_frames must go +1 with reason
    //      SORTER_UNAVAILABLE, unsorted_composite_frames must NOT move, the assignment stage
    //      (which DID run: count, prefix, emit) must still report its measured CPU cost, and
    //      rasterization (which did not run) must report none. Pre-fix this phase publishes a
    //      valid RID and increments unsorted_composite_frames: RED on base by design.
    //   3. RECOVERY CONTROL -- config restored and the unavailable flag cleared through the
    //      test accessor: the renderer must rebuild a sorter and publish again, so the reject
    //      is a per-frame decision and not a one-way kill switch. Clearing the flag here is a
    //      TEST action so this case stays about the reject; the production retry (on the
    //      shared GPU-003 backoff) is proven by the retries-and-recovers case below.
    //
    // Premise assertions guard every link, so a RED run shows the branch was actually reached
    // rather than the test having quietly missed it. Missing preconditions FAIL, never skip.
    TestResult result;

    Error err = tile_renderer->initialize(p_rd, Vector2i(TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT), TEST_TILE_SIZE);
    if (err != OK) {
        result.error_message = "Failed to initialize tile renderer for the #586 sorter-unavailable reject test";
        return result;
    }

    const uint32_t splat_count = 4096u;
    // Above the uint32 buffer limit for radix_bits=8 x workgroup_size=64 x key_bits=64 (128 B/record),
    // and below max_overlap_records' validation ceiling (200M) so ensure_resources does not clamp it away.
    const uint32_t REFUSED_CAPACITY = 34000000u;

    Vector<Gaussian> gaussians = generate_test_gaussians(splat_count);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, splat_count);
    InstancePipelineTestInputs instance_inputs = create_instance_pipeline_test_inputs(p_rd, splat_count);
    auto free_scene = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
        instance_inputs.free(p_rd);
    };
    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid() || !instance_inputs.is_valid()) {
        free_scene();
        result.error_message = "Failed to create scene buffers for the #586 sorter-unavailable reject test";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
    bind_instance_pipeline_inputs(params, instance_inputs, splat_count);

    auto &sort_resources = tile_renderer->_test_global_sort_resources();
    ProjectSettings *ps = ProjectSettings::get_singleton();

    result = [&]() -> TestResult {
        TestResult r;

        // ---- Phase 1: healthy control. A working sorter must still publish the frame. ----
        const uint64_t rejected_before_control = tile_renderer->get_global_composite_rejected_frames();
        const uint64_t unsorted_before_control = tile_renderer->get_unsorted_composite_frames();
        RID healthy_output = tile_renderer->render(p_rd, params);
        if (!healthy_output.is_valid()) {
            r.error_message = "Healthy control frame did not publish: render() returned an invalid RID with a "
                              "working sorter. The scene setup, not the #586 reject, is at fault.";
            return r;
        }
        if (!sort_resources.sorter.is_valid() || !sort_resources.sorter_available) {
            r.error_message = vformat(
                    "Premise failed: after the healthy control frame the global-composite sorter is not live "
                    "(sorter_valid=%s sorter_available=%s). This device never built one, so the control does not "
                    "control for anything and phase 2 would prove nothing.",
                    sort_resources.sorter.is_valid() ? "true" : "false",
                    sort_resources.sorter_available ? "true" : "false");
            return r;
        }
        if (tile_renderer->get_global_composite_rejected_frames() != rejected_before_control) {
            r.error_message = "Healthy control frame was counted as REJECTED. The fix must not reject frames on a "
                              "capable GPU with a working sorter.";
            return r;
        }
        if (tile_renderer->get_unsorted_composite_frames() != unsorted_before_control) {
            r.error_message = "Healthy control frame was counted as UNSORTED with a working sorter; the scene does "
                              "not control for the defect.";
            return r;
        }

        // ---- Phase 2: make the sorter unavailable through the production path, then render. ----
        const uint64_t rejected_before = tile_renderer->get_global_composite_rejected_frames();
        const uint64_t unsorted_before = tile_renderer->get_unsorted_composite_frames();
        {
            // Scoped so the settings are restored BEFORE the recovery phase re-syncs the config.
            // A named gpu_preset would override the manual radix/workgroup values below
            // (load_from_project_settings applies the preset first), so pin "custom".
            ProjectSettingGuard preset_guard(ps, GPUSortingConfig::GPU_PRESET_PATH);
            ProjectSettingGuard radix_guard(ps, GPUSortingConfig::RADIX_BITS_PATH);
            ProjectSettingGuard workgroup_guard(ps, GPUSortingConfig::WORKGROUP_SIZE_PATH);
            ProjectSettingGuard key_bits_guard(ps, GPUSortingConfig::KEY_BITS_PATH);
            ProjectSettingGuard overlap_guard(ps, GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH);
            if (ps) {
                ps->set_setting(GPUSortingConfig::GPU_PRESET_PATH, "custom");
                ps->set_setting(GPUSortingConfig::RADIX_BITS_PATH, 8);
                ps->set_setting(GPUSortingConfig::WORKGROUP_SIZE_PATH, 64);
                ps->set_setting(GPUSortingConfig::KEY_BITS_PATH, 64);
                ps->set_setting(GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH, 200000000);
            }
            g_gpu_sorting_config.load_from_project_settings();

            // Premise: with the config as loaded, the request really is above the device limit,
            // so create_sorter() MUST refuse. If it is not, no latch would be provoked and the
            // case would be testing nothing.
            const uint64_t largest_bytes = GPUSortingConstants::sort_path_max_buffer_bytes(
                    uint64_t(REFUSED_CAPACITY), g_gpu_sorting_config.radix_bits, g_gpu_sorting_config.workgroup_size,
                    g_gpu_sorting_config.key_bits);
            if (largest_bytes <= uint64_t(UINT32_MAX)) {
                r.error_message = vformat(
                        "Premise failed: %d records at radix_bits=%d workgroup_size=%d key_bits=%d need %s bytes for the "
                        "largest sort buffer, which does NOT exceed the uint32 limit, so create_sorter() would succeed and "
                        "the production latch could not be provoked (ProjectSettings present=%s).",
                        int(REFUSED_CAPACITY), int(g_gpu_sorting_config.radix_bits), int(g_gpu_sorting_config.workgroup_size),
                        int(g_gpu_sorting_config.key_bits), String::num_uint64(largest_bytes), ps ? "true" : "false");
                return r;
            }

            // Grow the global sort capacity through the production function: the sorter is shut
            // down for the grow, create_sorter() refuses, and disable_sorter() latches.
            tile_renderer->_test_ensure_global_sort_resources(REFUSED_CAPACITY);
        }
        // The guards restored the project settings; re-sync so the recovery phase and every
        // later case in this process see the original sorter configuration.
        g_gpu_sorting_config.load_from_project_settings();

        if (sort_resources.sorter.is_valid() || sort_resources.sorter_available) {
            r.error_message = vformat(
                    "Premise failed: the refused capacity request did not disable the global-composite sorter "
                    "(sorter_valid=%s sorter_available=%s capacity=%d). The production latch was not provoked, so the "
                    "frame below would not reach the sorter-unavailable branch.",
                    sort_resources.sorter.is_valid() ? "true" : "false",
                    sort_resources.sorter_available ? "true" : "false", int(sort_resources.capacity));
            return r;
        }
        if (!sort_resources.keys_buffer.is_valid() || !sort_resources.values_buffer.is_valid()) {
            r.error_message = "Premise failed: the key/value sort buffers were not allocated after the sorter was "
                              "disabled, so the frame would exit at the pre-binning resource check instead of reaching "
                              "the choke point this case is about (out of VRAM?).";
            return r;
        }

        RID degraded_output = tile_renderer->render(p_rd, params);

        // Premise: the latch held across ensure_resources -- no sorter was rebuilt, so the frame
        // really reached the draw path with no sorter.
        if (sort_resources.sorter.is_valid()) {
            r.error_message = "Premise failed: a global-composite sorter was rebuilt during the degraded frame, so the "
                              "sorter-unavailable branch was never reached and this case proves nothing.";
            return r;
        }
        if (params.splat_count == 0) {
            r.error_message = "Premise failed: the degraded frame carried splat_count == 0, so there was nothing to "
                              "composite and no wrong output was possible.";
            return r;
        }

        // DISCRIMINATING ASSERTION: the observable consequence. Nothing was published.
        if (degraded_output.is_valid()) {
            r.error_message = vformat(
                    "#586 REGRESSION: with the global-composite sorter unavailable and splat_count=%d, render() "
                    "PUBLISHED a frame (valid output RID). That frame rasterizes tiles in unsorted atomic-append "
                    "order, i.e. mathematically incorrect alpha compositing presented as a normal render. It must be "
                    "rejected instead. (global_composite_rejected_frames=%d, unsorted_composite_frames %d -> %d)",
                    int(params.splat_count),
                    int(tile_renderer->get_global_composite_rejected_frames()),
                    int(unsorted_before), int(tile_renderer->get_unsorted_composite_frames()));
            return r;
        }
        // ... and the reject is visible in telemetry, per frame, not as a one-shot log line.
        const uint64_t rejected_after = tile_renderer->get_global_composite_rejected_frames();
        if (rejected_after != rejected_before + 1u) {
            r.error_message = vformat(
                    "The degraded frame was not published but global_composite_rejected_frames went %d -> %d "
                    "(expected +1). The reject is invisible in telemetry, which is half the defect.",
                    int(rejected_before), int(rejected_after));
            return r;
        }
        if (tile_renderer->get_global_composite_last_reject_reason() !=
                uint8_t(GaussianSplatting::UnsortedCompositeReason::SORTER_UNAVAILABLE)) {
            r.error_message = vformat(
                    "Reject reason was %d, expected SORTER_UNAVAILABLE (%d). Telemetry cannot attribute the "
                    "degradation without log scraping.",
                    int(tile_renderer->get_global_composite_last_reject_reason()),
                    int(GaussianSplatting::UnsortedCompositeReason::SORTER_UNAVAILABLE));
            return r;
        }
        // A rejected frame must NOT also be counted as presented-unsorted: the two counters answer
        // different questions and a frame lands in exactly one of them.
        if (tile_renderer->get_unsorted_composite_frames() != unsorted_before) {
            r.error_message = vformat(
                    "A REJECTED frame also incremented unsorted_composite_frames (%d -> %d). That counter means "
                    "\"wrong pixels were shipped\"; nothing was shipped.",
                    int(unsorted_before), int(tile_renderer->get_unsorted_composite_frames()));
            return r;
        }
        // The instruments must not lie (renderer/AGENTS.md, timing honesty). The assignment
        // stage ran up to the choke point -- count, prefix/range, emit -- so the rejected frame
        // must still report that measured CPU cost (it is what the degraded path burns every
        // frame); rasterization did not run, so it must report none rather than the healthy
        // frame's value.
        if (!(tile_renderer->get_tile_assignment_time() > 0.0f) || !(tile_renderer->get_last_setup_cpu_ms() > 0.0f)) {
            r.error_message = vformat(
                    "The rejected frame reports no assignment cost (assignment=%f setup_cpu=%f) although count/prefix/emit "
                    "ran before the reject; hiding that work misreports the degraded path as free.",
                    double(tile_renderer->get_tile_assignment_time()), double(tile_renderer->get_last_setup_cpu_ms()));
            return r;
        }
        if (tile_renderer->get_rasterization_time() != 0.0f) {
            r.error_message = vformat(
                    "The rejected frame still reports a rasterization time (%f ms); rasterization never ran, so it is "
                    "describing the last successful frame.",
                    double(tile_renderer->get_rasterization_time()));
            return r;
        }

        // ---- Phase 3: recovery control. With the flag cleared, frames must publish again. ----
        // Proves the reject is a per-frame decision driven by sorter availability, not a one-way
        // kill switch. The config is back at its defaults, so ensure_resources rebuilds a sorter at
        // the scene's demand on this render (the next frame serial is 1 after the failure, inside
        // the backoff window, so without this flag flip the production retry would NOT fire yet --
        // which is what makes the accessor necessary here).
        sort_resources.sorter_available = true;
        RID recovered_output = tile_renderer->render(p_rd, params);
        if (!recovered_output.is_valid()) {
            r.error_message = "Recovery control failed: with sorter_available restored and the default sorter config, "
                              "render() still published nothing. The reject is not recovering, which would black-screen "
                              "the renderer.";
            return r;
        }
        if (!sort_resources.sorter.is_valid()) {
            r.error_message = "Recovery control is vacuous: no sorter was rebuilt, so the recovered frame did not prove "
                              "the sorted path came back.";
            return r;
        }
        if (tile_renderer->get_global_composite_rejected_frames() != rejected_after) {
            r.error_message = vformat(
                    "The recovered frame was ALSO counted as rejected (%d -> %d) even though it published.",
                    int(rejected_after), int(tile_renderer->get_global_composite_rejected_frames()));
            return r;
        }
        // The timing invalidation must clear timings for rejected frames only, not wedge them at 0.
        if (!(tile_renderer->get_tile_assignment_time() > 0.0f) || !(tile_renderer->get_rasterization_time() > 0.0f)) {
            r.error_message = vformat(
                    "The recovered, PUBLISHED frame reports no stage timings (assignment=%f raster=%f).",
                    double(tile_renderer->get_tile_assignment_time()), double(tile_renderer->get_rasterization_time()));
            return r;
        }

        r.passed = true;
        return r;
    }();

    free_scene();
    return result;
}

TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_sorter_unavailable_retries_and_recovers(RenderingDevice *p_rd) {
    // #586 PR 2 on-GPU proof: the tile sorter's "unavailable" state is a RETRY state on the
    // shared GPU-003 backoff, not a latch.
    //
    // At the base of this change TileGlobalSortResources::ensure_resources() never called
    // create_sorter() again once disable_sorter() had cleared sorter_available: the only
    // creation branch sat behind `else if`, so one failure -- a refused grow, a transient
    // allocation failure -- left the renderer with no sorter until reset_state(), i.e. every
    // translucent frame rejected (#976) for the rest of the session. The PR-1 case above
    // proves the reject; it has to clear the latch THROUGH THE TEST ACCESSOR to show the
    // sorted path can come back. This case removes that intervention: after the same real
    // failure, the production code must rebuild the sorter by itself, and it must do so on
    // the policy's schedule -- not before the backoff window has elapsed (the rate bound
    // that keeps a persistent failure from becoming an allocation/compile storm) and not
    // later than it (the retry that ends the episode).
    //
    // The frame serial is driven explicitly through RenderParams::frame_serial, so the
    // backoff window is measured in exactly the frames the policy counts; no wall clock,
    // no machine-speed dependence.
    //
    // Phases:
    //   1. HEALTHY CONTROL -- publishes; sorter live; failure count 0.
    //   2. FAILURE through the production path -- the same refused capacity as the PR-1
    //      case (34M records at radix 8 x wg 64 x 64-bit keys is above the RenderingDevice
    //      32-bit buffer limit). Premises: sorter gone, sorter_available false, failure
    //      count 1, key/value buffers live (so frames reject at the choke point), failure
    //      frame stamped with the healthy frame's serial.
    //   3. INSIDE THE BACKOFF WINDOW -- render frames failure+1 .. failure+backoff-1 with the
    //      DEFAULT configuration restored: every one must be rejected, and no sorter may be
    //      rebuilt. A retry gate that fires every frame (the M2 mutation) goes RED here.
    //   4. AT failure+backoff -- ensure_resources() is driven (through the production
    //      function) at the SAME capacity the buffers were sized for, which fits again now
    //      that the default configuration is back: the retry must succeed, close the
    //      episode (failure count 0, recoveries +1), and KEEP the existing key/value/tile
    //      buffers -- a transient failure recovers at an unchanged capacity and key layout,
    //      and reallocating hundreds of MB under the same pressure would turn the recovery
    //      into another rejected frame (#977 round 2). A deleted retry (the M1 mutation,
    //      also the base behaviour) goes RED here; so does a success path that still
    //      reallocates (the M3 mutation).
    //   5. STEADY STATE -- the next frame publishes with the recovered sorter and the
    //      preserved buffers, counters unchanged.
    TestResult result;

    Error err = tile_renderer->initialize(p_rd, Vector2i(TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT), TEST_TILE_SIZE);
    if (err != OK) {
        result.error_message = "Failed to initialize tile renderer for the #586 sorter-retry test";
        return result;
    }

    const uint32_t splat_count = 4096u;
    const uint32_t REFUSED_CAPACITY = 34000000u;
    const uint64_t FIRST_FRAME_SERIAL = 1000u;

    Vector<Gaussian> gaussians = generate_test_gaussians(splat_count);
    RID gaussian_buffer = create_test_gaussian_buffer(p_rd, gaussians);
    RID sorted_indices = create_test_sorted_indices(p_rd, splat_count);
    InstancePipelineTestInputs instance_inputs = create_instance_pipeline_test_inputs(p_rd, splat_count);
    auto free_scene = [&]() {
        if (gaussian_buffer.is_valid()) {
            p_rd->free(gaussian_buffer);
            gaussian_buffer = RID();
        }
        if (sorted_indices.is_valid()) {
            p_rd->free(sorted_indices);
            sorted_indices = RID();
        }
        instance_inputs.free(p_rd);
    };
    if (!gaussian_buffer.is_valid() || !sorted_indices.is_valid() || !instance_inputs.is_valid()) {
        free_scene();
        result.error_message = "Failed to create scene buffers for the #586 sorter-retry test";
        return result;
    }

    TileRenderer::RenderParams params = make_render_params(gaussian_buffer, sorted_indices, splat_count,
            TEST_VIEWPORT_WIDTH, TEST_VIEWPORT_HEIGHT, TEST_TILE_SIZE);
    bind_instance_pipeline_inputs(params, instance_inputs, splat_count);

    auto &sort_resources = tile_renderer->_test_global_sort_resources();
    ProjectSettings *ps = ProjectSettings::get_singleton();

    result = [&]() -> TestResult {
        TestResult r;

        // ---- Phase 1: healthy control. ----
        params.frame_serial = FIRST_FRAME_SERIAL;
        RID healthy_output = tile_renderer->render(p_rd, params);
        if (!healthy_output.is_valid()) {
            r.error_message = "Healthy control frame did not publish: render() returned an invalid RID with a "
                              "working sorter. The scene setup, not the retry policy, is at fault.";
            return r;
        }
        if (!sort_resources.sorter.is_valid() || !sort_resources.sorter_available ||
                tile_renderer->get_global_sort_sorter_init_failure_count() != 0u) {
            r.error_message = vformat(
                    "Premise failed: after the healthy control frame the global-composite sorter is not live or "
                    "already counts failures (sorter_valid=%s sorter_available=%s failures=%d).",
                    sort_resources.sorter.is_valid() ? "true" : "false",
                    sort_resources.sorter_available ? "true" : "false",
                    int(tile_renderer->get_global_sort_sorter_init_failure_count()));
            return r;
        }
        const uint64_t recoveries_before = tile_renderer->get_global_sort_sorter_recoveries();

        // ---- Phase 2: provoke the failure through the production path. ----
        {
            ProjectSettingGuard preset_guard(ps, GPUSortingConfig::GPU_PRESET_PATH);
            ProjectSettingGuard radix_guard(ps, GPUSortingConfig::RADIX_BITS_PATH);
            ProjectSettingGuard workgroup_guard(ps, GPUSortingConfig::WORKGROUP_SIZE_PATH);
            ProjectSettingGuard key_bits_guard(ps, GPUSortingConfig::KEY_BITS_PATH);
            ProjectSettingGuard overlap_guard(ps, GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH);
            if (ps) {
                ps->set_setting(GPUSortingConfig::GPU_PRESET_PATH, "custom");
                ps->set_setting(GPUSortingConfig::RADIX_BITS_PATH, 8);
                ps->set_setting(GPUSortingConfig::WORKGROUP_SIZE_PATH, 64);
                ps->set_setting(GPUSortingConfig::KEY_BITS_PATH, 64);
                ps->set_setting(GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH, 200000000);
            }
            g_gpu_sorting_config.load_from_project_settings();
            if (GPUSortingConstants::sort_path_allocation_fits_device_size(uint64_t(REFUSED_CAPACITY),
                        g_gpu_sorting_config.radix_bits, g_gpu_sorting_config.workgroup_size, g_gpu_sorting_config.key_bits)) {
                r.error_message = vformat(
                        "Premise failed: %d records at radix_bits=%d workgroup_size=%d key_bits=%d fit the device size limit, "
                        "so no creation failure could be provoked (ProjectSettings present=%s).",
                        int(REFUSED_CAPACITY), int(g_gpu_sorting_config.radix_bits), int(g_gpu_sorting_config.workgroup_size),
                        int(g_gpu_sorting_config.key_bits), ps ? "true" : "false");
                return r;
            }
            tile_renderer->_test_ensure_global_sort_resources(REFUSED_CAPACITY);
        }
        g_gpu_sorting_config.load_from_project_settings();

        if (sort_resources.sorter.is_valid() || sort_resources.sorter_available) {
            r.error_message = "Premise failed: the refused capacity request did not disable the global-composite sorter.";
            return r;
        }
        if (tile_renderer->get_global_sort_sorter_init_failure_count() != 1u) {
            r.error_message = vformat(
                    "Premise failed: after one refused creation the failure count is %d, expected 1; the failure was "
                    "not recorded, so no backoff schedule exists to test.",
                    int(tile_renderer->get_global_sort_sorter_init_failure_count()));
            return r;
        }
        if (!sort_resources.keys_buffer.is_valid() || !sort_resources.values_buffer.is_valid() ||
                !sort_resources.tile_ranges_buffer.is_valid() || !sort_resources.indirect_dispatch_buffer.is_valid()) {
            r.error_message = "Premise failed: the key/value/tile/indirect sort buffers are not all live after the failure, so "
                              "frames would exit at the pre-binning check rather than the choke point (out of VRAM?).";
            return r;
        }
        const uint64_t failure_frame = sort_resources.last_sorter_init_failure_frame;
        if (failure_frame != FIRST_FRAME_SERIAL) {
            r.error_message = vformat(
                    "Premise failed: the failure was stamped with frame %d, expected the healthy frame's serial %d; the "
                    "backoff window below would be measured from the wrong frame.",
                    int(failure_frame), int(FIRST_FRAME_SERIAL));
            return r;
        }
        const uint64_t backoff = GaussianSplatting::sorter_init_backoff_frames(1u);
        if (backoff < 2u) {
            r.error_message = vformat("Premise failed: the first backoff is %d frames, too short to observe a window.", int(backoff));
            return r;
        }

        // ---- Phase 3: inside the backoff window, every frame rejects and nothing is rebuilt. ----
        const uint64_t rejected_before_window = tile_renderer->get_global_composite_rejected_frames();
        uint64_t frames_in_window = 0;
        for (uint64_t serial = failure_frame + 1u; serial < failure_frame + backoff; serial++) {
            params.frame_serial = serial;
            RID output = tile_renderer->render(p_rd, params);
            frames_in_window++;
            if (sort_resources.sorter.is_valid() || sort_resources.sorter_available) {
                r.error_message = vformat(
                        "RATE BOUND VIOLATED: a sorter was rebuilt at frame %d, only %d frame(s) after the failure at %d, "
                        "inside the %d-frame backoff window. A persistent failure would be re-attempted every frame.",
                        int(serial), int(serial - failure_frame), int(failure_frame), int(backoff));
                return r;
            }
            if (output.is_valid()) {
                r.error_message = vformat(
                        "Frame %d inside the backoff window PUBLISHED with no sorter (%d frame(s) after the failure).",
                        int(serial), int(serial - failure_frame));
                return r;
            }
        }
        if (tile_renderer->get_global_composite_rejected_frames() != rejected_before_window + frames_in_window) {
            r.error_message = vformat(
                    "Inside the backoff window %d frames were rendered but global_composite_rejected_frames moved %d -> %d.",
                    int(frames_in_window), int(rejected_before_window), int(tile_renderer->get_global_composite_rejected_frames()));
            return r;
        }

        // ---- Phase 4: at failure+backoff the production code rebuilds at the same capacity and keeps the buffers. ----
        const RID keys_before = sort_resources.keys_buffer;
        const RID values_before = sort_resources.values_buffer;
        const RID ranges_before = sort_resources.tile_ranges_buffer;
        const RID indirect_before = sort_resources.indirect_dispatch_buffer;
        const uint32_t capacity_before = sort_resources.capacity;
        const uint64_t rejected_before_retry = tile_renderer->get_global_composite_rejected_frames();
        tile_renderer->set_frame_serial(failure_frame + backoff);
        tile_renderer->_test_ensure_global_sort_resources(REFUSED_CAPACITY);
        if (!sort_resources.sorter.is_valid() || !sort_resources.sorter_available) {
            r.error_message = vformat(
                    "#586 RETRY REGRESSION: %d frames after the creation failure (backoff %d) the renderer still has no "
                    "global-composite sorter (sorter_valid=%s sorter_available=%s failures=%d). The unavailable state "
                    "latched; translucent frames stay rejected for the session.",
                    int(backoff), int(backoff), sort_resources.sorter.is_valid() ? "true" : "false",
                    sort_resources.sorter_available ? "true" : "false",
                    int(tile_renderer->get_global_sort_sorter_init_failure_count()));
            return r;
        }
        if (sort_resources.capacity != capacity_before) {
            r.error_message = vformat(
                    "Premise failed: the retry changed the sort capacity (%d -> %d); this phase is about a recovery at an "
                    "unchanged capacity, so the buffer-preservation assertion below would not apply.",
                    int(capacity_before), int(sort_resources.capacity));
            return r;
        }
        if (sort_resources.keys_buffer != keys_before || sort_resources.values_buffer != values_before ||
                sort_resources.tile_ranges_buffer != ranges_before || sort_resources.indirect_dispatch_buffer != indirect_before) {
            r.error_message = vformat(
                    "BUFFER CHURN: a retry that succeeded at the unchanged capacity %d and key layout freed and reallocated "
                    "the sort buffers (keys %s, values %s, ranges %s, indirect %s). Under the VRAM pressure that caused the "
                    "failure, that reallocation is what turns a recovery into another rejected frame.",
                    int(capacity_before),
                    sort_resources.keys_buffer != keys_before ? "changed" : "kept",
                    sort_resources.values_buffer != values_before ? "changed" : "kept",
                    sort_resources.tile_ranges_buffer != ranges_before ? "changed" : "kept",
                    sort_resources.indirect_dispatch_buffer != indirect_before ? "changed" : "kept");
            return r;
        }
        if (tile_renderer->get_global_sort_sorter_init_failure_count() != 0u) {
            r.error_message = vformat(
                    "The sorter was rebuilt but the failure count is still %d; the episode was not closed, so the next "
                    "failure would start from a longer backoff than it should.",
                    int(tile_renderer->get_global_sort_sorter_init_failure_count()));
            return r;
        }
        if (tile_renderer->get_global_sort_sorter_recoveries() != recoveries_before + 1u) {
            r.error_message = vformat(
                    "The sorter was rebuilt after a failure but global_sort_sorter_recoveries went %d -> %d (expected +1); "
                    "the recovery is invisible in telemetry.",
                    int(recoveries_before), int(tile_renderer->get_global_sort_sorter_recoveries()));
            return r;
        }
        if (tile_renderer->get_global_composite_rejected_frames() != rejected_before_retry) {
            r.error_message = vformat(
                    "The recovered frame was ALSO counted as rejected (%d -> %d) even though it published.",
                    int(rejected_before_retry), int(tile_renderer->get_global_composite_rejected_frames()));
            return r;
        }

        // ---- Phase 5: the next frame publishes with the recovered sorter and the preserved buffers. ----
        params.frame_serial = failure_frame + backoff + 1u;
        RID steady_output = tile_renderer->render(p_rd, params);
        if (!steady_output.is_valid() || !sort_resources.sorter.is_valid() ||
                sort_resources.keys_buffer != keys_before ||
                tile_renderer->get_global_composite_rejected_frames() != rejected_before_retry ||
                tile_renderer->get_global_sort_sorter_init_failure_count() != 0u) {
            r.error_message = "The frame after the recovery did not stay healthy (published=%s sorter_valid=%s rejected=%d failures=%d).";
            r.error_message = vformat(r.error_message, steady_output.is_valid() ? "true" : "false",
                    sort_resources.sorter.is_valid() ? "true" : "false",
                    int(tile_renderer->get_global_composite_rejected_frames()),
                    int(tile_renderer->get_global_sort_sorter_init_failure_count()));
            return r;
        }

        r.passed = true;
        return r;
    }();

    free_scene();
    return result;
}

bool TileRendererRegressionTest::validate_against_reference(RID output_texture, const String &reference_name) {
    // Stub implementation
    return true;
}

// TEST_CASE wrapper to integrate with doctest framework
#include "test_macros.h"

TEST_CASE("[TileRenderer] Range pipeline regression test") {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (!rs) {
        MESSAGE("[TileRenderer] RenderingServer not available, skipping regression tests");
        return;
    }

    RenderingDevice *rd = rs->create_local_rendering_device();
    if (!rd) {
        MESSAGE("[TileRenderer] Could not create local rendering device, skipping regression tests");
        return;
    }

    Ref<TileRendererRegressionTest> regression_test;
    regression_test.instantiate();

    bool all_passed = regression_test->run_all_tests(rd);

    memdelete(rd);

    CHECK(all_passed);
}

// Force-link anchor (#178): a doctest TEST_CASE registers via a file-scope static
// initializer; MSVC drops this whole object from the module static library when
// nothing references it, silently discarding the cases. test_gaussian_splatting.h
// calls this symbol so the linker keeps the object and the cases actually run.
extern "C" int tile_renderer_regression_test_cpp_force_link() {
    return 0;
}

// C4b / exit criterion G4 ("no silent degradation"), Channel A on-GPU evidence. Tagged
// [RequiresGPU] so the self-hosted "GPU Harness + Visual Gate" lane runs it (the gs-gpu-test
// runner's TileRenderer batch filters `*TileRenderer*][RequiresGPU]*`). It drives
// TileRenderer::render() through the real instance pipeline, provokes a genuine binning
// overlap-record drop, and asserts the always-on resident-signal telemetry counter
// (overflow_drop_events) goes non-zero -- plus a control scene showing it stays 0 without a drop.
//
// Device acquisition uses REQUIRE_LOCAL_GPU_DEVICE() rather than
// RenderingServer::get_singleton(): under --gs-gpu-test there is NO RenderingServer (the test
// listener builds one only for [SceneTree] cases, tests/test_main.cpp), so the old
// RenderingServer gate early-returned with ZERO assertions on the harness -- a vacuous pass
// (#727). REQUIRE_LOCAL_GPU_DEVICE() creates a local device from the harness-bootstrapped
// RenderingDevice::get_singleton() instead, so the case actually runs its assertions there and
// only skips in the agent's rasterless environment where no device bootstrap exists.
TEST_CASE("[GaussianSplatting][TileRenderer][RequiresGPU] Overflow-record drop raises the C4b telemetry counter") {
    REQUIRE_LOCAL_GPU_DEVICE();

    Ref<TileRendererRegressionTest> regression_test;
    regression_test.instantiate();

    TileRendererRegressionTest::TestResult result = regression_test->test_overflow_drop_telemetry(local_device);

    // Release the regression test (its ~TileRenderer runs cleanup() on the device) BEFORE the
    // ScopedLocalRD guard frees local_device at scope exit -- the guard is declared first by the
    // macro, so it destructs last. Matches the other [RequiresGPU] teardown idiom (e.g. bm.unref()
    // in test_gpu_buffer_manager_lifetime.h); running ~TileRenderer after the device is freed
    // would dereference a freed device (use-after-free at teardown).
    regression_test.unref();

    if (!result.passed) {
        MESSAGE(result.error_message.utf8().get_data());
    }
    CHECK(result.passed);
}

// #586: the global-composite sorter-unavailable reject, on a real device. See
// test_sorter_unavailable_rejects_frame for the phases and for why the sorter is made
// unavailable through the production create_sorter() refusal rather than a test hook.
//
// Device acquisition: the TileRenderer constructor dereferences the RenderingDevice
// singleton through upstream ShaderRD (see REQUIRE_RENDERING_DEVICE_SINGLETON), so its
// absence is an unmet precondition and FAILs; the owned local device is then taken through
// the same ScopedLocalRD RAII as the other [RequiresGPU] cases in this file and FAILs
// (never skips) if it cannot be created. Headless lanes exclude *][RequiresGPU]*, so no
// headless lane can reach this case; the --gs-gpu-test harness (tests/ci/run_gpu_harness.py,
// TileRenderer batch) is where it runs.
TEST_CASE("[GaussianSplatting][TileRenderer][RequiresGPU] Sorter-unavailable global composite rejects the frame instead of rasterizing unsorted tiles (#586)") {
    REQUIRE_RENDERING_DEVICE_SINGLETON();

    // Declared before the Ref below so it destructs LAST: ~TileRenderer runs cleanup() on this
    // device, so freeing the device first would be a use-after-free at teardown.
    ScopedLocalRD local_rd_scope;
    RenderingDevice *local_device = local_rd_scope.rd;
    if (local_device == nullptr) {
        FAIL("Could not create a local RenderingDevice from the harness-bootstrapped singleton. "
             "This [RequiresGPU] case must run under tests/ci/run_gpu_harness.py.");
        return;
    }

    Ref<TileRendererRegressionTest> regression_test;
    regression_test.instantiate();

    TileRendererRegressionTest::TestResult result = regression_test->test_sorter_unavailable_rejects_frame(local_device);

    // Same teardown ordering constraint as the cases above: ~TileRenderer must run cleanup()
    // while local_device is still alive.
    regression_test.unref();

    // ::String is stringified by tests/test_macros.h; the .utf8().get_data() idiom used by
    // older cases in this file prints the pointer, not the text.
    if (!result.passed) {
        MESSAGE(result.error_message);
    }
    CHECK(result.passed);
}

// #586 PR 2: the tile sorter's unavailable state retries on the shared GPU-003 backoff and
// recovers without any test intervention. See test_sorter_unavailable_retries_and_recovers
// for the phases; same device/teardown idiom as the reject case above.
TEST_CASE("[GaussianSplatting][TileRenderer][RequiresGPU] Sorter-unavailable global composite retries on the shared backoff and recovers without intervention (#586)") {
    REQUIRE_RENDERING_DEVICE_SINGLETON();

    ScopedLocalRD local_rd_scope;
    RenderingDevice *local_device = local_rd_scope.rd;
    if (local_device == nullptr) {
        FAIL("Could not create a local RenderingDevice from the harness-bootstrapped singleton. "
             "This [RequiresGPU] case must run under tests/ci/run_gpu_harness.py.");
        return;
    }

    Ref<TileRendererRegressionTest> regression_test;
    regression_test.instantiate();

    TileRendererRegressionTest::TestResult result = regression_test->test_sorter_unavailable_retries_and_recovers(local_device);

    regression_test.unref();

    if (!result.passed) {
        MESSAGE(result.error_message);
    }
    CHECK(result.passed);
}

// C4b / exit criterion G4 ("no silent degradation"), Channel A OVER-COUNT de-dup (PR #508 review,
// tile_render_debug_stats.cpp:181). The overlap-record drop signal is STICKY (the binning EMIT pass
// raises it with atomicMax; clear_counters leaves it intact on normal frames), so once a readback
// has COUNTED it, the SSBO flag STILL reads 1 until the next frame-start clear_counters re-arms it.
// The pre-fix poll gate was `!pending` only, so in that awaiting-re-arm window poll re-enqueued a
// readback of the SAME already-counted 1, and on_overflow_signal_readback counted the ORIGINAL drop
// a SECOND time (over-count). This drives the REAL state machine end-to-end -- poll -> async
// readback -> callback -> count -> clear_counters re-arm -> new drop -- on a real device and asserts
// that one sticky drop is counted EXACTLY once across a pre-re-arm re-poll, while a genuinely new
// post-re-arm drop is counted again.
//
// Two discriminating checks isolate the fix without relying on async-completion timing:
//   1. The poll ENQUEUE decision: after the drop is counted (needs_clear set) and BEFORE the re-arm,
//      a re-poll of the still-sticky signal must NOT enqueue (overflow_signal_readback.pending stays
//      false). Pre-fix it enqueues (pending true) -- the readback that then double-counts.
//   2. The drop-event counter: exactly base+1 after the re-poll (pre-fix base+2), and base+2 only
//      after a real re-arm + new drop.
//
// [RequiresGPU] + REQUIRE_RENDERING_DEVICE_SINGLETON(): constructing TileRenderer dereferences the
// RD singleton (ShaderRD ctor), which only the --gs-gpu-test harness (tests/ci/run_gpu_harness.py)
// bootstraps; it matches the harness TileRenderer batch (`*TileRenderer*][RequiresGPU]*`).
TEST_CASE("[GaussianSplatting][TileRenderer][RequiresGPU] Sticky overflow drop is counted exactly once across a pre-re-arm re-poll (C4b/G4 over-count fix)") {
    REQUIRE_RENDERING_DEVICE_SINGLETON();
    RenderingDevice *rd = RenderingDevice::get_singleton();

    TileRenderer renderer;
    auto &ds = renderer._test_debug_stats();
    ds.create_buffers(rd);
    if (!ds.overflow_statistics_buffer.is_valid()) {
        FAIL("Failed to create overflow statistics buffer for the C4b over-count reproduction");
        return;
    }

    const uint32_t signal_offset = (uint32_t)offsetof(TileRenderer::OverflowStatsSnapshot, overflow_drop_signal);

    auto write_signal = [&](uint32_t v) {
        rd->buffer_update(ds.overflow_statistics_buffer, signal_offset, sizeof(uint32_t), &v);
    };
    // Fire any pending async overflow-signal readback. A synchronous buffer_get_data stalls all
    // frames (RenderingDevice::_flush_and_stall_for_all_frames -> _stall_for_previous_frames), which
    // flushes the recorded async download callbacks. Two reads guarantee the frame carrying the
    // async copy has been both submitted and stalled, regardless of the frame-ring depth.
    auto flush = [&]() {
        for (int i = 0; i < 2; ++i) {
            rd->buffer_get_data(ds.overflow_statistics_buffer, signal_offset, sizeof(uint32_t));
        }
    };

    // A drop set the sticky signal on an earlier frame; it persists in the SSBO.
    write_signal(1u);
    flush();
    const uint32_t base = renderer.get_overflow_drop_events();

    // "Frame 10": poll enqueues the readback; its callback counts the drop exactly once and requests
    // the re-arm (overflow_signal_needs_clear).
    ds.poll_overflow_drop_signal(rd, 10);
    CHECK(ds.overflow_signal_readback.pending); // a readback is in flight
    flush();
    CHECK(renderer.get_overflow_drop_events() == base + 1u);
    CHECK(ds.overflow_signal_needs_clear);
    CHECK_FALSE(ds.overflow_signal_readback.pending);

    // "Frame 11": the re-arm (frame-start clear_counters) has NOT run yet, so the SSBO signal is
    // STILL 1. Pre-fix, poll re-enqueues a readback of that same already-counted 1; post-fix it is
    // gated on overflow_signal_needs_clear.
    ds.poll_overflow_drop_signal(rd, 11);
    CHECK_FALSE(ds.overflow_signal_readback.pending); // DISCRIMINATOR 1: pre-fix true (re-enqueued)
    flush(); // pre-fix: drains the stale re-read, whose callback double-counts
    CHECK(renderer.get_overflow_drop_events() == base + 1u); // DISCRIMINATOR 2: pre-fix base+2

    // Re-arm: clear_counters consumes needs_clear and full-clears the SSBO (signal -> 0). A drop
    // AFTER re-arm is a NEW interval and must count again -- proving the gate does not wedge the
    // counter (no over-suppression / missed re-arm).
    ds.clear_counters(rd);
    flush();
    CHECK_FALSE(ds.overflow_signal_needs_clear);

    write_signal(1u); // genuinely new drop, after re-arm
    flush();
    ds.poll_overflow_drop_signal(rd, 12);
    CHECK(ds.overflow_signal_readback.pending);
    flush();
    CHECK(renderer.get_overflow_drop_events() == base + 2u);

    ds.free_buffers(rd);
}
