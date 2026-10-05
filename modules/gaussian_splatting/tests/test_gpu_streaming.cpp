#include "test_macros.h"
#include "gs_test_pump.h"
#include "../core/gaussian_streaming.h"
#include "../renderer/gpu_memory_stream.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "../renderer/resource_owner_mismatch_contract.h"
#include "../renderer/quantization_config.h"
#include "../core/gaussian_data.h"
#include "../core/residency_budget_controller.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_world.h"
#include "../core/streaming_queue_pressure_controller.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "core/config/project_settings.h"
#include "servers/rendering/rendering_device.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <random>

extern "C" int test_gpu_streaming_cpp_force_link() {
    return 0;
}

// Helper to create test gaussian data
LocalVector<Gaussian> create_test_gaussians(uint32_t count) {
    LocalVector<Gaussian> gaussians;
    gaussians.resize(count);

    for (uint32_t i = 0; i < count; i++) {
        Gaussian &g = gaussians[i];
        g.position = Vector3(i * 0.1f, i * 0.2f, i * 0.3f);
        g.scale = Vector3(0.5f, 0.5f, 0.5f);
        g.rotation = Quaternion();
        g.opacity = 0.9f;
        g.sh_dc = Color(1.0f, 0.5f, 0.2f, 0.9f);
        g.normal = Vector3(0, 1, 0);
        g.area = 0.25f;
    }

    return gaussians;
}

Ref<::GaussianData> create_test_gaussian_data(uint32_t count) {
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(create_test_gaussians(count));
    return data;
}

Ref<::GaussianData> create_clustered_test_gaussian_data(uint32_t count, const Vector3 &center) {
    LocalVector<Gaussian> gaussians;
    gaussians.resize(count);

    for (uint32_t i = 0; i < count; i++) {
        Gaussian &g = gaussians[i];
        const float x = float(i % 16) * 0.01f;
        const float y = float((i / 16) % 16) * 0.01f;
        const float z = float(i / 256) * 0.01f;
        g.position = center + Vector3(x, y, z);
        g.scale = Vector3(0.05f, 0.05f, 0.05f);
        g.rotation = Quaternion();
        g.opacity = 0.95f;
        g.sh_dc = Color(0.8f, 0.7f, 0.6f, 0.95f);
        g.normal = Vector3(0, 1, 0);
        g.area = 0.01f;
    }

    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(gaussians);
    return data;
}

GaussianSplatRenderer::StaticChunk make_test_static_chunk(uint32_t count, const AABB &bounds) {
    GaussianSplatRenderer::StaticChunk chunk;
    chunk.bounds = bounds;
    chunk.center = bounds.get_center();
    chunk.radius = bounds.get_longest_axis_size() * 0.5f;
    chunk.indices.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        chunk.indices.write[i] = i;
    }
    return chunk;
}

Ref<GaussianSplatWorld> make_world_resource(const Ref<GaussianData> &data, bool include_static_chunk = false) {
    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(data);
    world->set_bounds(data->get_aabb());
    if (include_static_chunk) {
        Vector<GaussianSplatRenderer::StaticChunk> chunks;
        chunks.push_back(make_test_static_chunk(data->get_count(), data->get_aabb()));
        world->set_static_chunks(chunks);
    }
    return world;
}

class ScopedStreamingManagerDevice {
    GaussianSplatManager *manager = nullptr;
    bool owns_manager = false;
    RenderingDevice *previous_device = nullptr;
    bool injected = false;

public:
    explicit ScopedStreamingManagerDevice(RenderingDevice *p_device) {
        manager = GaussianSplatManager::get_singleton();
        if (!manager) {
            manager = memnew(GaussianSplatManager);
            owns_manager = true;
        }
        if (p_device) {
            previous_device = manager->set_primary_rendering_device_for_testing(p_device);
            injected = true;
        }
    }

    ~ScopedStreamingManagerDevice() {
        // The harness owns the injected device; restore before manager teardown.
        if (injected) {
            manager->set_primary_rendering_device_for_testing(previous_device);
        }
        if (owns_manager) {
            memdelete(manager);
        }
    }

    GaussianSplatManager *get() const { return manager; }
};

struct WorldBackedRendererHarness {
    SceneTree *tree = nullptr;
    Window *root = nullptr;
    Node3D *scene_root = nullptr;
    GaussianSplatWorld3D *world_node = nullptr;
    Ref<GaussianSplatRenderer> renderer;

    ~WorldBackedRendererHarness() {
        teardown();
    }

    bool setup(const Ref<GaussianData> &data, bool include_static_chunk = false,
            const Vector<GaussianSplatRenderer::StaticChunk> &static_chunks = Vector<GaussianSplatRenderer::StaticChunk>()) {
        tree = SceneTree::get_singleton();
        if (tree == nullptr) {
            return false;
        }
        root = tree->get_root();
        if (root == nullptr) {
            return false;
        }

        scene_root = memnew(Node3D);
        root->add_child(scene_root);

        world_node = memnew(GaussianSplatWorld3D);
        scene_root->add_child(world_node);

        world_node->set_auto_apply_on_ready(false);
        Ref<GaussianSplatWorld> world = make_world_resource(data, include_static_chunk);
        if (!static_chunks.is_empty()) {
            world->set_static_chunks(static_chunks);
        }
        world_node->set_world(world);
        world_node->apply_world();
        tree->process(0.0);

        renderer = world_node->get_renderer();
        if (!renderer.is_valid()) {
            teardown();
            return false;
        }
        return true;
    }

    void teardown() {
        if (scene_root == nullptr && world_node == nullptr) {
            renderer.unref();
            tree = nullptr;
            root = nullptr;
            return;
        }

        if (scene_root != nullptr && root != nullptr) {
            if (world_node != nullptr) {
                scene_root->remove_child(world_node);
            }
            root->remove_child(scene_root);
        }
        if (world_node != nullptr) {
            memdelete(world_node);
            world_node = nullptr;
        }
        if (scene_root != nullptr) {
            memdelete(scene_root);
            scene_root = nullptr;
        }
        renderer.unref();
        tree = nullptr;
        root = nullptr;
    }
};

namespace {

bool _prepare_async_chunk_load(GaussianStreamingSystem &p_system, uint32_t p_asset_id) {
    auto &uploads = p_system._internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("Async fixture requires a running pack worker");
        return false;
    }
    auto *asset = p_system._test_get_asset_state(p_asset_id);
    if (!asset) {
        FAIL("Async fixture has no registered asset");
        return false;
    }
    auto &chunks = p_system._test_get_asset_chunks(*asset);
    if (chunks.is_empty()) {
        FAIL("Async fixture has no registered chunk");
        return false;
    }
    const auto *request = asset->requested_chunk_state.getptr(0);
    if (!request || request->stamp == 0) {
        FAIL("Async fixture has no explicit residency request");
        return false;
    }
    const uint64_t request_generation = request->stamp;
    // Run the real request producer without the frame's upload consumer. It
    // stamps the queued chunk before the stale/cancel mutation can happen.
    p_system._test_apply_requested_residency_async();
    CHECK(chunks[0].upload_pending);
    CHECK(chunks[0].explicit_request_generation == request_generation);
    if (!chunks[0].upload_pending || chunks[0].explicit_request_generation != request_generation) {
        FAIL("Async fixture did not queue a producer-stamped explicit request");
        return false;
    }
    // Only drive the real worker here: the next update consumes its completed
    // upload, so the stale/cancel mutation must happen before that update.
    const auto prepared = TestGaussianSplatting::gs_pump_until([&]() {
        Thread::yield();
        return p_system.get_pending_upload_jobs() > 0 &&
                uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 0;
    });
    if (!prepared.ready()) {
        FAIL("Async fixture did not prepare its upload ", prepared.describe());
        return false;
    }
    CHECK(p_system.get_pending_upload_jobs() > 0);
    return true;
}

bool _run_missing_buffer_abort_cycle(GaussianStreamingSystem &p_system, uint32_t p_asset_id) {
    auto *asset = p_system._test_get_asset_state(p_asset_id);
    if (!asset) {
        FAIL("Missing-buffer abort fixture has no registered asset");
        return false;
    }
    auto &chunks = p_system._test_get_asset_chunks(*asset);
    if (chunks.is_empty()) {
        FAIL("Missing-buffer abort fixture has no registered chunk");
        return false;
    }

    auto &uploads = p_system._internal_get_upload_pipeline();
    const bool queued = uploads.queue_chunk_load(p_system, p_asset_id, 0);
    CHECK(queued);
    if (!queued) {
        FAIL("Missing-buffer abort fixture could not queue its upload");
        return false;
    }
    const auto prepared = TestGaussianSplatting::gs_pump_until([&]() {
        Thread::yield();
        return uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 0 &&
                p_system.get_pending_upload_jobs() > 0;
    });
    if (!prepared.ready()) {
        FAIL("Missing-buffer abort fixture produced no prepared upload ", prepared.describe());
        return false;
    }

    CHECK(chunks[0].upload_pending);
    CHECK_FALSE(chunks[0].is_loaded);
    CHECK(p_system.get_pending_upload_jobs() > 0);
    const auto aborted = TestGaussianSplatting::gs_pump_until([&]() {
        uploads.process_upload_queue(p_system);
        Thread::yield();
        return uploads.pack_jobs_in_flight.load(std::memory_order_acquire) == 0 &&
                p_system.get_pending_pack_jobs() == 0 && p_system.get_pending_upload_jobs() == 0 &&
                !chunks[0].upload_pending && chunks[0].buffer_slot == UINT32_MAX;
    });
    if (!aborted.ready()) {
        FAIL("Missing-buffer upload abort did not clear its pending work ", aborted.describe());
        return false;
    }
    CHECK_FALSE(chunks[0].upload_pending);
    CHECK_FALSE(chunks[0].is_loaded);
    CHECK(chunks[0].buffer_slot == UINT32_MAX);
    CHECK(p_system.get_loaded_chunks() == 0);
    return true;
}

bool _is_equal_approx_vec3(const Vector3 &p_a, const Vector3 &p_b) {
    return Math::is_equal_approx(p_a.x, p_b.x) &&
            Math::is_equal_approx(p_a.y, p_b.y) &&
            Math::is_equal_approx(p_a.z, p_b.z);
}

bool _is_equal_approx_color(const Color &p_a, const Color &p_b) {
    return Math::is_equal_approx(p_a.r, p_b.r) &&
            Math::is_equal_approx(p_a.g, p_b.g) &&
            Math::is_equal_approx(p_a.b, p_b.b) &&
            Math::is_equal_approx(p_a.a, p_b.a);
}

class ScopedProjectSettingRestore {
    ProjectSettings *settings = nullptr;
    String setting_path;
    Variant previous_value;
    bool had_previous_value = false;

public:
    ScopedProjectSettingRestore(ProjectSettings *p_settings, const String &p_setting_path) :
            settings(p_settings),
            setting_path(p_setting_path) {
        if (settings && settings->has_setting(setting_path)) {
            previous_value = settings->get_setting(setting_path);
            had_previous_value = true;
        }
    }

    ~ScopedProjectSettingRestore() {
        if (!settings) {
            return;
        }

        if (had_previous_value) {
            settings->set_setting(setting_path, previous_value);
        } else {
            settings->clear(setting_path);
        }
    }
};

struct SnapshotPositionStressContext {
    Ref<::GaussianData> data;
    const PackedVector3Array *pattern_a = nullptr;
    const PackedVector3Array *pattern_b = nullptr;
    uint32_t splat_count = 0;

    std::atomic<bool> stop{false};
    std::atomic<bool> writer_failed{false};
    std::atomic<bool> reader_failed{false};

    Semaphore writer_begin;
    Semaphore writer_done;
    Semaphore reader_begin;
    Semaphore reader_done;
};

bool _snapshot_matches_position_pattern(const LocalVector<Gaussian> &p_snapshot, const PackedVector3Array &p_pattern) {
    if ((int)p_snapshot.size() != p_pattern.size()) {
        return false;
    }
    for (uint32_t i = 0; i < p_snapshot.size(); i++) {
        if (!_is_equal_approx_vec3(p_snapshot[i].position, p_pattern[i])) {
            return false;
        }
    }
    return true;
}

bool _packed_buffer_matches_position_pattern(RenderingDevice *p_rd, RID p_buffer, const PackedVector3Array &p_pattern) {
    if (p_rd == nullptr || !p_buffer.is_valid()) {
        return false;
    }

    const uint32_t splat_count = p_pattern.size();
    const uint64_t byte_count = uint64_t(splat_count) * sizeof(PackedGaussian);
    if (byte_count > UINT32_MAX) {
        return false;
    }

    Vector<uint8_t> bytes = p_rd->buffer_get_data(p_buffer, 0, static_cast<uint32_t>(byte_count));
    if (bytes.size() != static_cast<int>(byte_count)) {
        return false;
    }

    Vector<PackedGaussian> packed;
    packed.resize(splat_count);
    memcpy(packed.ptrw(), bytes.ptr(), static_cast<size_t>(byte_count));

    const PackedGaussian *packed_read = packed.ptr();
    for (uint32_t i = 0; i < splat_count; i++) {
        const PackedGaussian &packed_gaussian = packed_read[i];
        const Vector3 packed_position(
                packed_gaussian.position[0],
                packed_gaussian.position[1],
                packed_gaussian.position[2]);
        if (!_is_equal_approx_vec3(packed_position, p_pattern[i])) {
            return false;
        }
    }

    return true;
}

void _snapshot_position_writer_thread(void *p_userdata) {
    SnapshotPositionStressContext *ctx = static_cast<SnapshotPositionStressContext *>(p_userdata);
    if (!ctx || !ctx->data.is_valid() || !ctx->pattern_a || !ctx->pattern_b) {
        return;
    }

    uint32_t iteration = 0;
    while (true) {
        ctx->writer_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        const PackedVector3Array &pattern = (iteration % 2 == 0) ? *ctx->pattern_a : *ctx->pattern_b;
        if (pattern.size() != int(ctx->splat_count)) {
            ctx->writer_failed.store(true, std::memory_order_release);
            ctx->writer_done.post();
            continue;
        }

        ctx->data->set_positions(pattern);
        iteration++;
        ctx->writer_done.post();
    }
}

void _snapshot_position_reader_thread(void *p_userdata) {
    SnapshotPositionStressContext *ctx = static_cast<SnapshotPositionStressContext *>(p_userdata);
    if (!ctx || !ctx->data.is_valid() || !ctx->pattern_a || !ctx->pattern_b) {
        return;
    }

    while (true) {
        ctx->reader_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        LocalVector<Gaussian> gaussians_snapshot;
        LocalVector<Vector3> sh_snapshot;
        uint32_t sh_first = 0;
        uint32_t sh_high = 0;
        const bool capture_ok = ctx->data->capture_chunk_snapshot(0, ctx->splat_count,
                gaussians_snapshot, sh_snapshot, sh_first, sh_high);
        if (!capture_ok) {
            ctx->reader_failed.store(true, std::memory_order_release);
            ctx->reader_done.post();
            continue;
        }

        const bool matches_a = _snapshot_matches_position_pattern(gaussians_snapshot, *ctx->pattern_a);
        const bool matches_b = _snapshot_matches_position_pattern(gaussians_snapshot, *ctx->pattern_b);
        if (!matches_a && !matches_b) {
            ctx->reader_failed.store(true, std::memory_order_release);
        }

        ctx->reader_done.post();
    }
}

struct SnapshotSHStressContext {
    Ref<::GaussianData> data;
    const PackedFloat32Array *dc_only_data = nullptr;
    const PackedFloat32Array *full_data = nullptr;
    uint32_t splat_count = 0;

    std::atomic<bool> stop{false};
    std::atomic<bool> writer_failed{false};
    std::atomic<bool> reader_failed{false};

    Semaphore writer_begin;
    Semaphore writer_done;
    Semaphore reader_begin;
    Semaphore reader_done;
};

struct StreamingPipelineHammerContext {
    Ref<StreamingPipeline> pipeline;
    uint32_t total_splats = 0;

    std::atomic<bool> stop{false};
    std::atomic<uint32_t> iteration{0};

    Semaphore lod_begin;
    Semaphore lod_done;
    Semaphore range_begin;
    Semaphore range_done;
};

Vector<GaussianStreamingSystem::ChunkLayoutHint> _build_partitioned_hints(uint32_t p_total_splats, uint32_t p_chunk_cap, std::mt19937 &p_rng) {
    Vector<GaussianStreamingSystem::ChunkLayoutHint> hints;
    uint32_t remaining = p_total_splats;
    uint32_t cursor = 0;

    while (remaining > 0) {
        const uint32_t step_cap = MIN(p_chunk_cap, remaining);
        std::uniform_int_distribution<uint32_t> count_dist(1u, step_cap);
        const uint32_t count = count_dist(p_rng);

        GaussianStreamingSystem::ChunkLayoutHint hint;
        hint.start_idx = cursor;
        hint.count = count;
        hint.bounds = AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
        hint.center = Vector3();
        hint.radius = 1.0f;
        hints.push_back(hint);

        cursor += count;
        remaining -= count;
    }

    return hints;
}

String _layout_hint_last_reason(const Ref<GaussianStreamingSystem> &p_system) {
    Dictionary stats = p_system->get_chunk_culling_stats();
    Dictionary validation = stats.get("layout_hint_validation", Dictionary());
    return validation.get("last_reason", String("none"));
}

int64_t _layout_hint_reason_count(const Ref<GaussianStreamingSystem> &p_system, const String &p_reason) {
    Dictionary stats = p_system->get_chunk_culling_stats();
    Dictionary validation = stats.get("layout_hint_validation", Dictionary());
    Dictionary reason_counts = validation.get("reason_counts", Dictionary());
    return reason_counts.get(p_reason, int64_t(0));
}

struct ConcurrentStreamingContext {
    Ref<GaussianMemoryStream> stream;
    uint32_t uploads_per_thread = 0;
    uint32_t gaussians_per_upload = 0;
    std::atomic<int> successful_uploads{0};
    std::atomic<int> failed_uploads{0};
    Semaphore begin;
    Semaphore done;
};

void _concurrent_streaming_worker(void *p_userdata) {
    ConcurrentStreamingContext *ctx = static_cast<ConcurrentStreamingContext *>(p_userdata);
    if (!ctx || !ctx->stream.is_valid()) {
        return;
    }

    ctx->begin.wait();
    for (uint32_t i = 0; i < ctx->uploads_per_thread; i++) {
        LocalVector<Gaussian> gaussians = create_test_gaussians(ctx->gaussians_per_upload);
        Error upload_err = ctx->stream->stream_gaussians_async(gaussians);
        if (upload_err == OK) {
            ctx->successful_uploads.fetch_add(1, std::memory_order_relaxed);
        } else {
            ctx->failed_uploads.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ctx->done.post();
}

void _compute_streaming_pipeline_hammer_values(uint32_t p_iteration, uint32_t p_total_splats,
        uint32_t &r_lod, uint32_t &r_start, uint32_t &r_count) {
    r_lod = p_iteration % 6;

    r_count = 128 + ((p_iteration % 5) * 64);
    if (r_count > p_total_splats) {
        r_count = p_total_splats;
    }

    if (p_total_splats > r_count) {
        const uint32_t max_start = p_total_splats - r_count;
        r_start = (p_iteration * 131u + 17u) % (max_start + 1);
    } else {
        r_start = 0;
    }
}

void _streaming_pipeline_lod_hammer_thread(void *p_userdata) {
    StreamingPipelineHammerContext *ctx = static_cast<StreamingPipelineHammerContext *>(p_userdata);
    if (!ctx || !ctx->pipeline.is_valid()) {
        return;
    }

    while (true) {
        ctx->lod_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        const uint32_t iteration = ctx->iteration.load(std::memory_order_acquire);
        ctx->pipeline->set_lod_level(iteration % 6);
        ctx->lod_done.post();
    }
}

void _streaming_pipeline_visible_range_hammer_thread(void *p_userdata) {
    StreamingPipelineHammerContext *ctx = static_cast<StreamingPipelineHammerContext *>(p_userdata);
    if (!ctx || !ctx->pipeline.is_valid()) {
        return;
    }

    while (true) {
        ctx->range_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        uint32_t start = 0;
        uint32_t count = 0;
        const uint32_t iteration = ctx->iteration.load(std::memory_order_acquire);
        count = 128 + ((iteration % 5) * 64);
        if (count > ctx->total_splats) {
            count = ctx->total_splats;
        }
        if (ctx->total_splats > count) {
            const uint32_t max_start = ctx->total_splats - count;
            start = (iteration * 131u + 17u) % (max_start + 1);
        }
        ctx->pipeline->update_visible_range(start, count);
        ctx->range_done.post();
    }
}

Color _dc_only_color_for_index(uint32_t p_index) {
    const float base = float(p_index) * 0.0001f;
    return Color(0.1f + base, 0.2f + base, 0.3f + base, 1.0f);
}

Color _full_color_for_index(uint32_t p_index) {
    const float base = float(p_index) * 0.0001f;
    return Color(0.4f + base, 0.5f + base, 0.6f + base, 1.0f);
}

Vector3 _full_first_order_for_index(uint32_t p_index, uint32_t p_term) {
    const float base = float(p_index) * 0.0002f;
    return Vector3(1.0f + float(p_term) + base,
            2.0f + float(p_term) + base,
            3.0f + float(p_term) + base);
}

Vector3 _full_high_order_for_index(uint32_t p_index, uint32_t p_term) {
    const float base = float(p_index) * 0.0003f;
    if (p_term == 0) {
        return Vector3(7.0f + base, 8.0f + base, 9.0f + base);
    }
    return Vector3(10.0f + base, 11.0f + base, 12.0f + base);
}

void _snapshot_sh_writer_thread(void *p_userdata) {
    SnapshotSHStressContext *ctx = static_cast<SnapshotSHStressContext *>(p_userdata);
    if (!ctx || !ctx->data.is_valid() || !ctx->dc_only_data || !ctx->full_data) {
        return;
    }

    uint32_t iteration = 0;
    while (true) {
        ctx->writer_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        const PackedFloat32Array &sh_data = (iteration % 2 == 0) ? *ctx->dc_only_data : *ctx->full_data;
        ctx->data->set_spherical_harmonics(sh_data);
        iteration++;
        ctx->writer_done.post();
    }
}

void _snapshot_sh_reader_thread(void *p_userdata) {
    SnapshotSHStressContext *ctx = static_cast<SnapshotSHStressContext *>(p_userdata);
    if (!ctx || !ctx->data.is_valid()) {
        return;
    }

    while (true) {
        ctx->reader_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        LocalVector<Gaussian> gaussians_snapshot;
        LocalVector<Vector3> sh_snapshot;
        uint32_t sh_first = 0;
        uint32_t sh_high = 0;
        const bool capture_ok = ctx->data->capture_chunk_snapshot(0, ctx->splat_count,
                gaussians_snapshot, sh_snapshot, sh_first, sh_high);
        if (!capture_ok || gaussians_snapshot.size() != int(ctx->splat_count)) {
            ctx->reader_failed.store(true, std::memory_order_release);
            ctx->reader_done.post();
            continue;
        }

        bool snapshot_valid = true;
        if (sh_high == 0) {
            snapshot_valid = sh_first == 0 && sh_snapshot.is_empty();
            for (uint32_t i = 0; snapshot_valid && i < ctx->splat_count; i++) {
                const Gaussian &g = gaussians_snapshot[i];
                if (!_is_equal_approx_color(g.sh_dc, _dc_only_color_for_index(i))) {
                    snapshot_valid = false;
                    break;
                }
                for (uint32_t j = 0; j < 3; j++) {
                    if (!_is_equal_approx_vec3(g.sh_1[j], Vector3())) {
                        snapshot_valid = false;
                        break;
                    }
                }
            }
        } else if (sh_high == 2) {
            snapshot_valid = sh_first == 3 && sh_snapshot.size() == int(ctx->splat_count * 2);
            for (uint32_t i = 0; snapshot_valid && i < ctx->splat_count; i++) {
                const Gaussian &g = gaussians_snapshot[i];
                if (!_is_equal_approx_color(g.sh_dc, _full_color_for_index(i))) {
                    snapshot_valid = false;
                    break;
                }
                for (uint32_t j = 0; j < 3; j++) {
                    if (!_is_equal_approx_vec3(g.sh_1[j], _full_first_order_for_index(i, j))) {
                        snapshot_valid = false;
                        break;
                    }
                }
                const uint32_t base = i * 2;
                if (!_is_equal_approx_vec3(sh_snapshot[base + 0], _full_high_order_for_index(i, 0)) ||
                        !_is_equal_approx_vec3(sh_snapshot[base + 1], _full_high_order_for_index(i, 1))) {
                    snapshot_valid = false;
                    break;
                }
            }
        } else {
            snapshot_valid = false;
        }

        if (!snapshot_valid) {
            ctx->reader_failed.store(true, std::memory_order_release);
        }

        ctx->reader_done.post();
    }
}

} // namespace

TEST_CASE("[GPU Memory Stream] Initialization") {
    Ref<GaussianMemoryStream> stream;
    stream.instantiate();

    // Test initialization without RenderingDevice (should fail)
    Error err = stream->initialize(nullptr, 100000, 256);
    CHECK(err == ERR_INVALID_PARAMETER);

    // Get or create RenderingDevice
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        // Create local rendering device for testing
        RenderingServer *rs = RenderingServer::get_singleton();
        if (rs) {
            rd = rs->create_local_rendering_device();
        }
    }

    if (rd) {
        // Test valid initialization
        err = stream->initialize(rd, 100000, 256);
        CHECK(err == OK);

        // Verify initialization
        CHECK(stream->get_max_gaussians() == 100000);
        CHECK(stream->get_allocated_memory_mb() > 0);

        stream->shutdown();
    }
}

TEST_CASE("[GPU Memory Stream] Triple Buffering") {
    Ref<GaussianMemoryStream> stream;
    stream.instantiate();

    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        RenderingServer *rs = RenderingServer::get_singleton();
        if (rs) {
            rd = rs->create_local_rendering_device();
        }
    }

    if (rd) {
        Error err = stream->initialize(rd, 10000, 64);
        CHECK(err == OK);
        if (err != OK) {
            return;
        }

        // Create test data
        LocalVector<Gaussian> gaussians = create_test_gaussians(1000);

        // Test triple buffer rotation
        for (int i = 0; i < 5; i++) {
            err = stream->stream_gaussians_async(gaussians);
            CHECK(err == OK);

            // Simulate frame
            stream->begin_frame(i);
            stream->swap_buffers();
            stream->end_frame();
        }

        // Check no stalls occurred
        StreamingStats stats = stream->get_stats();
        CHECK(stats.stalls == 0);
        CHECK(stats.buffer_switches >= 5);

        stream->shutdown();
    }
}

TEST_CASE("[GPU Memory Stream] Memory Pool Allocation") {
    // Test memory pool functionality
    GaussianMemoryStream::MemoryPool pool;
    pool.total_size = 1024 * 1024; // 1MB
    pool.blocks.push_back({0, pool.total_size, true, 0});

    // Test allocations
    uint32_t offset1 = pool.allocate(1024, 16);
    CHECK(offset1 != UINT32_MAX);
    CHECK(offset1 == 0);

    uint32_t offset2 = pool.allocate(2048, 16);
    CHECK(offset2 != UINT32_MAX);
    CHECK(offset2 >= 1024);

    // Test deallocation
    pool.deallocate(offset1);

    // Allocate in freed space
    uint32_t offset3 = pool.allocate(512, 16);
    CHECK(offset3 == 0); // Should reuse freed space

    // Test fragmentation calculation
    float frag = pool.get_fragmentation_ratio();
    CHECK(frag >= 0.0f);
    CHECK(frag <= 1.0f);
}

TEST_CASE("[GPU Memory Stream] Streaming Performance") {
    Ref<GaussianMemoryStream> stream;
    stream.instantiate();

    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        RenderingServer *rs = RenderingServer::get_singleton();
        if (rs) {
            rd = rs->create_local_rendering_device();
        }
    }

    if (rd) {
        // Initialize for 100K gaussians
        Error err = stream->initialize(rd, 100000, 256);
        CHECK(err == OK);
        if (err != OK) {
            return;
        }

        // Create 100K test gaussians
        LocalVector<Gaussian> gaussians = create_test_gaussians(100000);

        // Measure streaming time
        uint64_t start = OS::get_singleton()->get_ticks_usec();

        err = stream->stream_gaussians_async(gaussians);
        CHECK(err == OK);

        stream->wait_for_all_uploads();

        uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
        float ms = elapsed / 1000.0f;

        // Should complete in reasonable time (< 100ms for 100K splats)
        CHECK_MESSAGE(ms < 100.0f,
            vformat("Streaming 100K gaussians took %.2f ms (target < 100ms)", ms));

        // Check memory efficiency
        float efficiency = stream->get_memory_efficiency();
        CHECK(efficiency > 0.0f);

        stream->shutdown();
    }
}

TEST_CASE("[GPU Memory Stream] Memory Defragmentation") {
    GaussianMemoryStream::MemoryPool pool;
    pool.total_size = 1024 * 1024; // 1MB
    pool.blocks.clear();

    // Create fragmented memory layout
    pool.blocks.push_back({0, 1024, false, 0}); // Allocated
    pool.blocks.push_back({1024, 512, true, 0}); // Free
    pool.blocks.push_back({1536, 2048, false, 0}); // Allocated
    pool.blocks.push_back({3584, 256, true, 0}); // Free
    pool.blocks.push_back({3840, 1024, false, 0}); // Allocated
    pool.used_size = 1024 + 2048 + 1024;

    // Check fragmentation before
    float frag_before = pool.get_fragmentation_ratio();
    CHECK(frag_before > 0.5f); // Should be fragmented

    // Defragment
    pool.defragment();

    // Check fragmentation after
    float frag_after = pool.get_fragmentation_ratio();
    CHECK(frag_after < frag_before); // Should be less fragmented

    // Verify blocks are compacted
    bool found_large_free = false;
    for (const auto &block : pool.blocks) {
        if (block.free && block.size >= (pool.total_size - pool.used_size)) {
            found_large_free = true;
        }
    }
    CHECK(found_large_free);
}

TEST_CASE("[GPU Memory Stream] Concurrent Streaming") {
    Ref<GaussianMemoryStream> stream;
    stream.instantiate();

    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        RenderingServer *rs = RenderingServer::get_singleton();
        if (rs) {
            rd = rs->create_local_rendering_device();
        }
    }

    if (rd) {
        Error err = stream->initialize(rd, 50000, 128);
        CHECK(err == OK);
        if (err != OK) {
            return;
        }

        ConcurrentStreamingContext ctx;
        ctx.stream = stream;
        ctx.uploads_per_thread = 10;
        ctx.gaussians_per_upload = 1000;

        Thread worker_a;
        Thread worker_b;
        const Thread::ID worker_a_id = worker_a.start(_concurrent_streaming_worker, &ctx);
        const Thread::ID worker_b_id = worker_b.start(_concurrent_streaming_worker, &ctx);
        const bool worker_a_started = worker_a_id != Thread::UNASSIGNED_ID;
        const bool worker_b_started = worker_b_id != Thread::UNASSIGNED_ID;
        CHECK(worker_a_started);
        CHECK(worker_b_started);
        if (!worker_a_started || !worker_b_started) {
            if (worker_a_started) {
                ctx.begin.post();
                worker_a.wait_to_finish();
            }
            if (worker_b_started) {
                ctx.begin.post();
                worker_b.wait_to_finish();
            }
            stream->shutdown();
            return;
        }

        ctx.begin.post(2);
        ctx.done.wait();
        ctx.done.wait();
        worker_a.wait_to_finish();
        worker_b.wait_to_finish();

        const int successful_uploads = ctx.successful_uploads.load(std::memory_order_relaxed);
        const int failed_uploads = ctx.failed_uploads.load(std::memory_order_relaxed);
        const int total_uploads = int(ctx.uploads_per_thread * 2);
        CHECK(successful_uploads > 0);
        CHECK_MESSAGE(failed_uploads < total_uploads / 2,
                "Too many failed uploads in concurrent streaming test");

        stream->shutdown();
    }
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Basic Operations") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("RenderingDevice required by the StreamingPipeline GPU batch");
        return;
    }

    Ref<StreamingPipeline> pipeline;
    pipeline.instantiate();

    Ref<GaussianMemoryStream> stream;
    stream.instantiate();

    Ref<::GaussianData> data;
    data.instantiate();
    data->resize(10000);

    if (rd) {
        // Initialize stream
        Error err = stream->initialize(rd, 10000, 64);
        CHECK(err == OK);
        if (err != OK) {
            return;
        }

        // Initialize pipeline
        err = pipeline->initialize(stream, data);
        CHECK(err == OK);

        // Start streaming
        pipeline->start_streaming();

        // Update visible range
        pipeline->update_visible_range(0, 1000);

        // Wait a bit for streaming
        OS::get_singleton()->delay_usec(10 * 1000);

        // Get stats
        Dictionary stats = pipeline->get_streaming_stats();
        CHECK(stats.has("visible_count"));
        CHECK(int(stats["visible_count"]) == 1000);

        // Stop streaming
        pipeline->stop_streaming();

        pipeline->shutdown();
    }
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Concurrent LOD and visibility updates remain coherent while worker is active") {
#ifndef THREADS_ENABLED
    FAIL("THREADS_ENABLED is not enabled in this build");
    return;
#endif

    ScopedLocalRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;

    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    const uint32_t total_splats = 32768;

    Ref<GaussianMemoryStream> stream;
    stream.instantiate();
    Error err = stream->initialize(rd, total_splats, 64);
    CHECK(err == OK);
    if (err != OK) {
        return;
    }

    Ref<StreamingPipeline> pipeline;
    pipeline.instantiate();
    Ref<::GaussianData> data = create_test_gaussian_data(total_splats);
    err = pipeline->initialize(stream, data);
    CHECK(err == OK);
    if (err != OK) {
        stream->shutdown();
        return;
    }

    pipeline->start_streaming();

    StreamingPipelineHammerContext ctx;
    ctx.pipeline = pipeline;
    ctx.total_splats = total_splats;

    Thread lod_thread;
    Thread range_thread;
    const Thread::ID lod_thread_id = lod_thread.start(_streaming_pipeline_lod_hammer_thread, &ctx);
    const Thread::ID range_thread_id = range_thread.start(_streaming_pipeline_visible_range_hammer_thread, &ctx);
    const bool lod_thread_started = lod_thread_id != Thread::UNASSIGNED_ID;
    const bool range_thread_started = range_thread_id != Thread::UNASSIGNED_ID;
    CHECK(lod_thread_started);
    CHECK(range_thread_started);
    if (!lod_thread_started || !range_thread_started) {
        ctx.stop.store(true, std::memory_order_release);
        if (lod_thread_started) {
            ctx.lod_begin.post();
            lod_thread.wait_to_finish();
        }
        if (range_thread_started) {
            ctx.range_begin.post();
            range_thread.wait_to_finish();
        }
        pipeline->stop_streaming();
        pipeline->shutdown();
        return;
    }

    constexpr uint32_t iterations = 192;
    for (uint32_t i = 0; i < iterations; i++) {
        ctx.iteration.store(i, std::memory_order_release);
        ctx.lod_begin.post();
        ctx.range_begin.post();
        Thread::yield();
        ctx.lod_done.wait();
        ctx.range_done.wait();
    }

    ctx.stop.store(true, std::memory_order_release);
    ctx.lod_begin.post();
    ctx.range_begin.post();
    lod_thread.wait_to_finish();
    range_thread.wait_to_finish();

    uint32_t expected_lod = 0;
    uint32_t expected_start = 0;
    uint32_t expected_count = 0;
    _compute_streaming_pipeline_hammer_values(iterations - 1, total_splats, expected_lod, expected_start, expected_count);

    const auto worker_drain = TestGaussianSplatting::gs_pump_until([&]() {
        Thread::yield();
        CHECK(pipeline->process_uploads() == OK);
        Dictionary stats = pipeline->get_streaming_stats();
        return !bool(stats["is_streaming"]);
    });
    const bool drained = worker_drain.ready();
    CHECK(drained);
    if (!drained) {
        FAIL("Concurrent LOD worker did not drain ", worker_drain.describe());
        pipeline->stop_streaming();
        pipeline->shutdown();
        return;
    }

    stream->wait_for_all_uploads();

    Dictionary stats = pipeline->get_streaming_stats();
    CHECK(uint32_t(int(stats["current_lod"])) == expected_lod);
    CHECK(uint32_t(int(stats["visible_start"])) == expected_start);
    CHECK(uint32_t(int(stats["visible_count"])) == expected_count);
    CHECK(stream->get_stats().buffer_switches > 0);

    pipeline->stop_streaming();
    pipeline->shutdown();
}

TEST_CASE("[GPU Memory Stream] Memory Leak Detection") {
    // Track initial memory usage
    uint64_t initial_static_memory = OS::get_singleton()->get_static_memory_usage();

    {
        Ref<GaussianMemoryStream> stream;
        stream.instantiate();

        RenderingDevice *rd = RenderingDevice::get_singleton();
        if (!rd) {
            RenderingServer *rs = RenderingServer::get_singleton();
            if (rs) {
                rd = rs->create_local_rendering_device();
            }
        }

        if (rd) {
            // Perform multiple init/shutdown cycles
            for (int i = 0; i < 5; i++) {
                Error err = stream->initialize(rd, 10000, 32);
                CHECK(err == OK);

                // Stream some data
                LocalVector<Gaussian> gaussians = create_test_gaussians(1000);
                stream->stream_gaussians_async(gaussians);
                stream->wait_for_all_uploads();

                stream->shutdown();
            }
        }

        // Stream goes out of scope
    }

    // Check memory wasn't leaked
    uint64_t final_static_memory = OS::get_singleton()->get_static_memory_usage();
    int64_t memory_diff = final_static_memory - initial_static_memory;

    // Allow small variation (< 1MB)
    CHECK_MESSAGE(Math::abs(memory_diff) < 1024 * 1024,
                 vformat("Memory leak detected: %d bytes difference", memory_diff));
}

TEST_CASE("[Streaming Pipeline] Chunk snapshot stays coherent under concurrent position mutations") {
#ifndef THREADS_ENABLED
    FAIL("THREADS_ENABLED is not enabled in this build");
    return;
#endif

    const uint32_t splat_count = GaussianStreamingSystem::CHUNK_SIZE;
    Ref<::GaussianData> data = create_test_gaussian_data(splat_count);

    PackedVector3Array pattern_a;
    PackedVector3Array pattern_b;
    pattern_a.resize(splat_count);
    pattern_b.resize(splat_count);
    Vector3 *pattern_a_write = pattern_a.ptrw();
    Vector3 *pattern_b_write = pattern_b.ptrw();
    for (uint32_t i = 0; i < splat_count; i++) {
        pattern_a_write[i] = Vector3(11.0f, float(i), -3.0f);
        pattern_b_write[i] = Vector3(-7.0f, float(i) + 0.25f, 5.0f);
    }
    data->set_positions(pattern_a);

    SnapshotPositionStressContext ctx;
    ctx.data = data;
    ctx.pattern_a = &pattern_a;
    ctx.pattern_b = &pattern_b;
    ctx.splat_count = splat_count;

    Thread writer_thread;
    Thread reader_thread;
    writer_thread.start(_snapshot_position_writer_thread, &ctx);
    reader_thread.start(_snapshot_position_reader_thread, &ctx);

    constexpr uint32_t iterations = 96;
    for (uint32_t i = 0; i < iterations; i++) {
        if ((i % 2) == 0) {
            ctx.writer_begin.post();
            Thread::yield();
            ctx.reader_begin.post();
        } else {
            ctx.reader_begin.post();
            Thread::yield();
            ctx.writer_begin.post();
        }
        ctx.writer_done.wait();
        ctx.reader_done.wait();
    }

    ctx.stop.store(true, std::memory_order_release);
    ctx.writer_begin.post();
    ctx.reader_begin.post();
    writer_thread.wait_to_finish();
    reader_thread.wait_to_finish();

    CHECK_FALSE(ctx.writer_failed.load(std::memory_order_acquire));
    CHECK_FALSE(ctx.reader_failed.load(std::memory_order_acquire));
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Worker uploads remain coherent under concurrent position mutations") {
#ifndef THREADS_ENABLED
    FAIL("THREADS_ENABLED is not enabled in this build");
    return;
#endif

    ScopedLocalRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;

    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    const uint32_t splat_count = 32768;

    Ref<GaussianMemoryStream> stream;
    stream.instantiate();
    Error err = stream->initialize(rd, splat_count, 64);
    CHECK(err == OK);
    if (err != OK) {
        return;
    }
    stream->set_async_upload(false);

    Ref<StreamingPipeline> pipeline;
    pipeline.instantiate();
    Ref<::GaussianData> data = create_test_gaussian_data(splat_count);

    PackedVector3Array pattern_a;
    PackedVector3Array pattern_b;
    pattern_a.resize(splat_count);
    pattern_b.resize(splat_count);
    Vector3 *pattern_a_write = pattern_a.ptrw();
    Vector3 *pattern_b_write = pattern_b.ptrw();
    for (uint32_t i = 0; i < splat_count; i++) {
        pattern_a_write[i] = Vector3(25.0f, float(i) * 0.5f, -9.0f);
        pattern_b_write[i] = Vector3(-13.0f, float(i) * 0.5f + 0.125f, 17.0f);
    }
    data->set_positions(pattern_a);

    err = pipeline->initialize(stream, data);
    CHECK(err == OK);
    if (err != OK) {
        stream->shutdown();
        return;
    }

    pipeline->start_streaming();
    pipeline->update_visible_range(0, splat_count);

    const auto initial_drain = TestGaussianSplatting::gs_pump_until([&]() {
        Thread::yield();
        CHECK(pipeline->process_uploads() == OK);
        Dictionary stats = pipeline->get_streaming_stats();
        return !bool(stats["is_streaming"]);
    });
    const bool drained = initial_drain.ready();
    CHECK(drained);
    if (!drained) {
        FAIL("Position-mutation worker did not finish its initial upload ", initial_drain.describe());
        pipeline->stop_streaming();
        pipeline->shutdown();
        return;
    }
    stream->wait_for_all_uploads();

    SnapshotPositionStressContext ctx;
    ctx.data = data;
    ctx.pattern_a = &pattern_a;
    ctx.pattern_b = &pattern_b;
    ctx.splat_count = splat_count;

    Thread writer_thread;
    const Thread::ID writer_thread_id = writer_thread.start(_snapshot_position_writer_thread, &ctx);
    CHECK(writer_thread_id != Thread::UNASSIGNED_ID);
    if (writer_thread_id == Thread::UNASSIGNED_ID) {
        pipeline->stop_streaming();
        pipeline->shutdown();
        return;
    }

    constexpr uint32_t iterations = 16;
    for (uint32_t i = 0; i < iterations; i++) {
        ctx.writer_begin.post();
        pipeline->set_lod_level((i % 5) + 1);
        Thread::yield();
        ctx.writer_done.wait();

        const auto mutation_drain = TestGaussianSplatting::gs_pump_until([&]() {
            Thread::yield();
            CHECK(pipeline->process_uploads() == OK);
            Dictionary stats = pipeline->get_streaming_stats();
            return !bool(stats["is_streaming"]);
        });
        const bool iteration_drained = mutation_drain.ready();
        CHECK(iteration_drained);
        if (!iteration_drained) {
            FAIL("Position-mutation worker did not drain ", mutation_drain.describe());
            break;
        }

        stream->wait_for_all_uploads();
        RID buffer = stream->get_current_gpu_buffer();
        CHECK(buffer.is_valid());
        if (!buffer.is_valid()) {
            break;
        }

        const bool matches_a = _packed_buffer_matches_position_pattern(rd, buffer, pattern_a);
        const bool matches_b = _packed_buffer_matches_position_pattern(rd, buffer, pattern_b);
        const bool either_matches = matches_a || matches_b;
        CHECK(either_matches);
    }

    ctx.stop.store(true, std::memory_order_release);
    ctx.writer_begin.post();
    writer_thread.wait_to_finish();

    CHECK_FALSE(ctx.writer_failed.load(std::memory_order_acquire));

    pipeline->stop_streaming();
    pipeline->shutdown();
}

TEST_CASE("[Streaming Pipeline] Chunk snapshot stays coherent under concurrent SH mutations") {
#ifndef THREADS_ENABLED
    FAIL("THREADS_ENABLED is not enabled in this build");
    return;
#endif

    const uint32_t splat_count = 8192;
    Ref<::GaussianData> data = create_test_gaussian_data(splat_count);

    PackedFloat32Array dc_only_data;
    dc_only_data.resize(splat_count * 3);
    float *dc_write = dc_only_data.ptrw();

    constexpr uint32_t full_sh_high_order = 2;
    constexpr uint32_t full_sh_first_order = 3;
    constexpr uint32_t full_floats_per_gaussian = (1 + full_sh_first_order + full_sh_high_order) * 3;
    PackedFloat32Array full_data;
    full_data.resize(splat_count * full_floats_per_gaussian);
    float *full_write = full_data.ptrw();

    for (uint32_t i = 0; i < splat_count; i++) {
        const Color dc_only_color = _dc_only_color_for_index(i);
        const uint32_t dc_base = i * 3;
        dc_write[dc_base + 0] = dc_only_color.r;
        dc_write[dc_base + 1] = dc_only_color.g;
        dc_write[dc_base + 2] = dc_only_color.b;

        const uint32_t full_base = i * full_floats_per_gaussian;
        const Color full_color = _full_color_for_index(i);
        full_write[full_base + 0] = full_color.r;
        full_write[full_base + 1] = full_color.g;
        full_write[full_base + 2] = full_color.b;
        for (uint32_t j = 0; j < full_sh_first_order; j++) {
            const Vector3 coeff = _full_first_order_for_index(i, j);
            const uint32_t coeff_base = full_base + 3 + (j * 3);
            full_write[coeff_base + 0] = coeff.x;
            full_write[coeff_base + 1] = coeff.y;
            full_write[coeff_base + 2] = coeff.z;
        }
        for (uint32_t j = 0; j < full_sh_high_order; j++) {
            const Vector3 coeff = _full_high_order_for_index(i, j);
            const uint32_t coeff_base = full_base + 3 + (full_sh_first_order * 3) + (j * 3);
            full_write[coeff_base + 0] = coeff.x;
            full_write[coeff_base + 1] = coeff.y;
            full_write[coeff_base + 2] = coeff.z;
        }
    }

    data->set_spherical_harmonics(dc_only_data);

    SnapshotSHStressContext ctx;
    ctx.data = data;
    ctx.dc_only_data = &dc_only_data;
    ctx.full_data = &full_data;
    ctx.splat_count = splat_count;

    Thread writer_thread;
    Thread reader_thread;
    writer_thread.start(_snapshot_sh_writer_thread, &ctx);
    reader_thread.start(_snapshot_sh_reader_thread, &ctx);

    constexpr uint32_t iterations = 72;
    for (uint32_t i = 0; i < iterations; i++) {
        if ((i % 2) == 0) {
            ctx.writer_begin.post();
            Thread::yield();
            ctx.reader_begin.post();
        } else {
            ctx.reader_begin.post();
            Thread::yield();
            ctx.writer_begin.post();
        }
        ctx.writer_done.wait();
        ctx.reader_done.wait();
    }

    ctx.stop.store(true, std::memory_order_release);
    ctx.writer_begin.post();
    ctx.reader_begin.post();
    writer_thread.wait_to_finish();
    reader_thread.wait_to_finish();

    CHECK_FALSE(ctx.writer_failed.load(std::memory_order_acquire));
    CHECK_FALSE(ctx.reader_failed.load(std::memory_order_acquire));
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Stale generation upload jobs are dropped") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }
    const String async_pack_setting = "rendering/gaussian_splatting/streaming/async_pack_enabled";
    ScopedProjectSettingRestore async_pack_guard(project_settings, async_pack_setting);
    project_settings->set_setting(async_pack_setting, true);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    const uint32_t asset_id = 7;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(asset_id, 0, 0) == OK);
    system->finalize_residency_requests();
    if (!_prepare_async_chunk_load(*system.ptr(), asset_id)) {
        return;
    }

    system->unregister_asset(asset_id);
    system->register_asset(asset_id, create_test_gaussian_data(1024));

    const auto stale_work_drained = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->_internal_get_upload_pipeline().pack_jobs_in_flight.load(std::memory_order_acquire) == 0 &&
                system->get_pending_pack_jobs() == 0 && system->get_pending_upload_jobs() == 0 &&
                system->get_pending_upload_retirement_slots() == 0;
    });
    if (!stale_work_drained.ready()) {
        FAIL("Stale generation work did not drain ", stale_work_drained.describe());
        return;
    }

    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 0);
    for (int i = 0; i < 8; i++) {
        system->update_streaming(camera_transform, projection);
        CHECK(system->get_pending_pack_jobs() == 0);
        CHECK(system->get_pending_upload_jobs() == 0);
        CHECK(system->get_loaded_chunks() == 0);
    }

    system->begin_residency_requests();
    system->request_chunk_residency(asset_id, 0, 0);
    system->finalize_residency_requests();
    const auto fresh_residency = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->get_loaded_chunks() > 0;
    });
    if (!fresh_residency.ready()) {
        FAIL("Fresh residency after stale generation did not complete ", fresh_residency.describe());
        return;
    }

    CHECK(system->get_loaded_chunks() > 0);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Residency finalize without requests leaves no pending work") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_with_device(create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE), rd);

    system->begin_residency_requests();
    system->finalize_residency_requests();

    Dictionary status = system->get_residency_request_status(0, 0);
    CHECK_FALSE(bool(status.get("requested", false)));
    CHECK_FALSE(bool(status.get("request_pending", true)));
    CHECK(String(status.get("request_state_name", String())) == "idle");
    CHECK(String(status.get("request_result_name", String())) == "idle");
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Stale residency completion remains visible in request status") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String async_pack_setting = "rendering/gaussian_splatting/streaming/async_pack_enabled";
    ScopedProjectSettingRestore async_pack_guard(project_settings, async_pack_setting);
    project_settings->set_setting(async_pack_setting, true);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_with_device(create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE), rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(0, 0, 0) == OK);
    system->finalize_residency_requests();

    if (!_prepare_async_chunk_load(*system.ptr(), 0)) {
        return;
    }

    system->begin_residency_requests();
    system->finalize_residency_requests();

    const auto stale_completion = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->get_loaded_chunks() > 0 &&
                system->_internal_get_upload_pipeline().pack_jobs_in_flight.load(std::memory_order_acquire) == 0 &&
                system->get_pending_pack_jobs() == 0 &&
                system->get_pending_upload_jobs() == 0 &&
                system->get_pending_upload_retirement_slots() == 0;
    });

    if (!stale_completion.ready()) {
        FAIL("Stale request completion did not complete ", stale_completion.describe());
        return;
    }

    Dictionary status = system->get_residency_request_status(0, 0);
    CHECK_FALSE(bool(status.get("requested", true)));
    CHECK(String(status.get("request_state_name", String())) == "satisfied");
    CHECK(String(status.get("request_result_name", String())) == "satisfied");
    CHECK_FALSE(bool(status.get("request_status_current_generation", true)));
    CHECK((int64_t)status.get("request_generation_current", -1) == 0);
    CHECK((int64_t)status.get("request_status_generation", 0) > 0);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Cancelled pending chunk uploads do not count as evictions") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String async_pack_setting = "rendering/gaussian_splatting/streaming/async_pack_enabled";
    ScopedProjectSettingRestore async_pack_guard(project_settings, async_pack_setting);
    project_settings->set_setting(async_pack_setting, true);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_a = 17;
    const uint32_t asset_b = 18;
    system->register_asset(asset_a, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    system->register_asset(asset_b, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(asset_a, 0, 0) == OK);
    system->finalize_residency_requests();

    if (!_prepare_async_chunk_load(*system.ptr(), asset_a)) {
        return;
    }

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(asset_b, 0, 0) == OK);
    system->finalize_residency_requests();

    const uint32_t evictions_before = system->get_chunks_evicted_this_frame();
    system->update_streaming(camera_transform, projection);
    const uint32_t evictions_after = system->get_chunks_evicted_this_frame();

    CHECK(evictions_after == evictions_before);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Null-device atlas sync invalidates stale publication state") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 91;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    system->_test_sync_global_atlas_state(rd);
    const uint64_t generation_before = system->get_atlas_generation();
    CHECK(generation_before > 0);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
    const RID asset_meta_before = system->get_asset_meta_buffer();
    const RID chunk_meta_before = system->get_chunk_meta_buffer();
    const RID chunk_index_before = system->get_asset_chunk_index_buffer();
    CHECK(rd->buffer_is_valid(asset_meta_before));
    CHECK(rd->buffer_is_valid(chunk_meta_before));
    CHECK(rd->buffer_is_valid(chunk_index_before));

    // Even a clean atlas must stop publishing while the device is unavailable.
    system->_test_sync_global_atlas_state(nullptr);

    CHECK(system->get_atlas_generation() == 0);
    CHECK_FALSE(system->get_asset_meta_buffer().is_valid());
    CHECK_FALSE(system->get_chunk_meta_buffer().is_valid());
    CHECK_FALSE(system->get_asset_chunk_index_buffer().is_valid());
    CHECK(rd->buffer_is_valid(asset_meta_before));
    CHECK(rd->buffer_is_valid(chunk_meta_before));
    CHECK(rd->buffer_is_valid(chunk_index_before));

    system->_test_sync_global_atlas_state(rd);
    CHECK(system->get_atlas_generation() > generation_before);
    const RID asset_meta_restored = system->get_asset_meta_buffer();
    const RID chunk_meta_restored = system->get_chunk_meta_buffer();
    const RID chunk_index_restored = system->get_asset_chunk_index_buffer();
    CHECK(rd->buffer_is_valid(asset_meta_restored));
    CHECK(rd->buffer_is_valid(chunk_meta_restored));
    CHECK(rd->buffer_is_valid(chunk_index_restored));

    system.unref();
    CHECK_FALSE(rd->buffer_is_valid(asset_meta_before));
    CHECK_FALSE(rd->buffer_is_valid(chunk_meta_before));
    CHECK_FALSE(rd->buffer_is_valid(chunk_index_before));
    CHECK_FALSE(rd->buffer_is_valid(asset_meta_restored));
    CHECK_FALSE(rd->buffer_is_valid(chunk_meta_restored));
    CHECK_FALSE(rd->buffer_is_valid(chunk_index_restored));
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Invalid atlas dirty marks force a rebuild path") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 123;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    system->_test_sync_global_atlas_state(rd);
    const uint64_t generation_before = system->get_atlas_generation();
    CHECK(generation_before > 0);
    system->_test_mark_chunk_meta_dirty(asset_id, GaussianStreamingSystem::CHUNK_SIZE + 3);
    system->_test_sync_global_atlas_state(rd);
    CHECK(system->get_atlas_generation() > generation_before);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());

    const uint64_t generation_before_null = system->get_atlas_generation();
    const RID asset_meta_before = system->get_asset_meta_buffer();
    const RID chunk_meta_before = system->get_chunk_meta_buffer();
    const RID chunk_index_before = system->get_asset_chunk_index_buffer();
    CHECK(rd->buffer_is_valid(asset_meta_before));
    CHECK(rd->buffer_is_valid(chunk_meta_before));
    CHECK(rd->buffer_is_valid(chunk_index_before));

    // The invalid-index rebuild above is independent of this null-lifetime round.
    system->_test_mark_chunk_meta_dirty(asset_id, 0);
    system->_test_sync_global_atlas_state(nullptr);
    CHECK(system->get_atlas_generation() == 0);
    CHECK_FALSE(system->get_asset_meta_buffer().is_valid());
    CHECK_FALSE(system->get_chunk_meta_buffer().is_valid());
    CHECK_FALSE(system->get_asset_chunk_index_buffer().is_valid());
    CHECK(rd->buffer_is_valid(asset_meta_before));
    CHECK(rd->buffer_is_valid(chunk_meta_before));
    CHECK(rd->buffer_is_valid(chunk_index_before));
    system->_test_sync_global_atlas_state(rd);

    CHECK(system->get_atlas_generation() > generation_before_null);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
    const RID asset_meta_restored = system->get_asset_meta_buffer();
    const RID chunk_meta_restored = system->get_chunk_meta_buffer();
    const RID chunk_index_restored = system->get_asset_chunk_index_buffer();
    CHECK(rd->buffer_is_valid(asset_meta_restored));
    CHECK(rd->buffer_is_valid(chunk_meta_restored));
    CHECK(rd->buffer_is_valid(chunk_index_restored));

    system.unref();
    CHECK_FALSE(rd->buffer_is_valid(asset_meta_before));
    CHECK_FALSE(rd->buffer_is_valid(chunk_meta_before));
    CHECK_FALSE(rd->buffer_is_valid(chunk_index_before));
    CHECK_FALSE(rd->buffer_is_valid(asset_meta_restored));
    CHECK_FALSE(rd->buffer_is_valid(chunk_meta_restored));
    CHECK_FALSE(rd->buffer_is_valid(chunk_index_restored));
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Primary explicit residency requests expose request status") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_with_device(create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE), rd);

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(0, 0, 0) == OK);
    CHECK(system->request_asset_residency(0, 0) == OK);

    Dictionary status = system->get_residency_request_status(0, 0);
    CHECK(bool(status.get("requested", false)));
    CHECK(bool(status.get("request_pending", false)));
    CHECK(String(status.get("request_state_name", String())) == "collected");
    CHECK(String(status.get("request_result_name", String())) == "collected");
    CHECK((int64_t)status.get("lod_mask", 0) != 0);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Primary explicit residency bypasses sync-fallback visibility gating") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String async_pack_setting = "rendering/gaussian_splatting/streaming/async_pack_enabled";
    ScopedProjectSettingRestore async_pack_guard(project_settings, async_pack_setting);
    project_settings->set_setting(async_pack_setting, false);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_with_device(
            create_clustered_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE, Vector3(5000.0f, 5000.0f, 5000.0f)),
            rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(0, 0, 0) == OK);
    system->finalize_residency_requests();

    const auto explicit_residency = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->get_loaded_chunks() > 0;
    });
    if (!explicit_residency.ready()) {
        FAIL("Explicit residency bypass did not complete ", explicit_residency.describe());
        return;
    }

    Dictionary status = system->get_residency_request_status(0, 0);
    CHECK(system->get_loaded_chunks() > 0);
    CHECK(String(status.get("request_state_name", String())) == "satisfied");
    CHECK(String(status.get("request_result_name", String())) == "satisfied");
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Hard explicit residency load failures surface as failed status") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String async_pack_setting = "rendering/gaussian_splatting/streaming/async_pack_enabled";
    ScopedProjectSettingRestore async_pack_guard(project_settings, async_pack_setting);
    project_settings->set_setting(async_pack_setting, false);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_with_device(create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE), rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->_test_force_next_chunk_upload_failure();
    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(0, 0, 0) == OK);
    system->finalize_residency_requests();

    system->update_streaming(camera_transform, projection);
    Dictionary failed_status = system->get_residency_request_status(0, 0);
    CHECK(bool(failed_status.get("requested", false)));
    CHECK(String(failed_status.get("request_state_name", String())) == "failed");
    CHECK(String(failed_status.get("request_result_name", String())) == "failed");
    CHECK(bool(failed_status.get("request_status_current_generation", false)));

    // The injected failure is one-shot. Keep the fault present for the second
    // observation, then remove it below to distinguish failure from recovery.
    system->_test_force_next_chunk_upload_failure();
    system->update_streaming(camera_transform, projection);
    Dictionary settled_status = system->get_residency_request_status(0, 0);
    CHECK_FALSE(bool(settled_status.get("request_pending", true)));
    CHECK(bool(settled_status.get("requested", false)));
    CHECK(String(settled_status.get("request_state_name", String())) == "failed");
    CHECK(String(settled_status.get("request_result_name", String())) == "failed");
    CHECK((int64_t)settled_status.get("request_status_generation", 0) ==
            (int64_t)settled_status.get("request_generation_current", -1));
    CHECK(system->get_loaded_chunks() == 0);

    const auto recovered = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        const Dictionary status = system->get_residency_request_status(0, 0);
        return system->get_loaded_chunks() > 0 &&
                String(status.get("request_state_name", String())) == "satisfied";
    });
    if (!recovered.ready()) {
        FAIL("Removing the hard upload fault did not restore residency ", recovered.describe());
        return;
    }
    CHECK(system->get_loaded_chunks() > 0);
    const Dictionary recovered_status = system->get_residency_request_status(0, 0);
    CHECK(String(recovered_status.get("request_state_name", String())) == "satisfied");
    CHECK(String(recovered_status.get("request_result_name", String())) == "satisfied");
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Cancelled pending chunk loads do not count as evictions") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 808;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));

    auto *asset = system->_test_get_asset_state(asset_id);
    CHECK(asset != nullptr);
    if (!asset) {
        FAIL("Cancellation fixture has no registered asset");
        return;
    }
    auto &asset_chunks = system->_test_get_asset_chunks(*asset);
    if (asset_chunks.is_empty()) {
        FAIL("Cancellation fixture has no registered chunk");
        return;
    }

    auto &chunk = asset_chunks[0];
    const uint64_t chunk_key = system->_test_make_chunk_key(asset_id, 0);
    uint32_t buffer_slot = UINT32_MAX;
    const bool slot_allocated = system->_test_atlas_allocator().allocate_slot(
            chunk_key, GaussianStreamingSystem::atlas_pages_for_splats(chunk.count), buffer_slot);
    CHECK(slot_allocated);
    if (!slot_allocated) {
        FAIL("Cancellation fixture could not reserve an atlas run");
        return;
    }
    const bool upload_started = system->_test_begin_chunk_upload(asset_id, 0, chunk, buffer_slot);
    CHECK(upload_started);
    if (!upload_started) {
        FAIL("Cancellation fixture could not enter pending upload state");
        return;
    }
    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(system->get_chunks_evicted_this_frame() == 0);

    system->_test_evict_unrequested_chunks(asset_id, *asset, asset_chunks);

    CHECK(system->get_chunks_evicted_this_frame() == 0);
    CHECK_FALSE(chunk.is_loaded);
    CHECK_FALSE(chunk.upload_pending);
    CHECK(chunk.buffer_slot == UINT32_MAX);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Invalid chunk meta dirty marks force a safe atlas rebuild") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 809;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));

    const uint64_t generation_before = system->get_atlas_generation();
    system->_test_mark_chunk_meta_dirty(asset_id, 99);

    CHECK(system->_test_get_atlas_registry_dirty());
    CHECK(system->_test_get_chunk_meta_dirty_all());
    CHECK(system->_test_get_chunk_meta_dirty_indices().is_empty());

    system->_test_sync_global_atlas_state(rd);

    CHECK(system->get_atlas_generation() > generation_before);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
}

TEST_CASE("[Streaming Pipeline] Sparse chunk meta sync planning uses cached topology") {
	Ref<GaussianStreamingSystem> system;
	system.instantiate();
	system->initialize_empty(nullptr);

    constexpr uint32_t asset_count = 64;
    constexpr uint32_t first_asset_id = 9000;
    for (uint32_t i = 0; i < asset_count; i++) {
        system->register_asset(first_asset_id + i, create_test_gaussian_data(1));
    }

    system->_test_build_global_atlas_cpu_state();
    system->_test_clear_atlas_cpu_dirty_state();

    system->_test_mark_chunk_meta_dirty(first_asset_id + 3, 0);
    system->_test_mark_chunk_meta_dirty(first_asset_id + 17, 0);
    system->_test_mark_chunk_meta_dirty(first_asset_id + 51, 0);

    const StreamingGlobalAtlasRegistry::ChunkMetaUploadPlan plan = system->_test_plan_chunk_meta_sync();
    const StreamingGlobalAtlasRegistry::SyncDiagnostics diagnostics = system->_test_get_atlas_sync_diagnostics();

    CHECK(plan.dirty_count == 3);
    CHECK(plan.contiguous_range_count == 3);
    CHECK_FALSE(plan.full_update);
    CHECK(diagnostics.used_cached_topology);
    CHECK_FALSE(diagnostics.forced_full_rebuild);
    CHECK(diagnostics.topology_scan_asset_count == 0);
    CHECK(diagnostics.topology_scan_chunk_count == 0);
    CHECK(diagnostics.chunk_meta_dirty_count == 3);
	CHECK(diagnostics.chunk_meta_range_count == 3);
	CHECK_FALSE(diagnostics.chunk_meta_full_update);
}

TEST_CASE("[Streaming Pipeline] Full chunk meta sync planning reports full-update diagnostics") {
	Ref<GaussianStreamingSystem> system;
	system.instantiate();
	system->initialize_empty(nullptr);

	constexpr uint32_t asset_id = 9060;
	system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE * 2));

	system->_test_build_global_atlas_cpu_state();

	const StreamingGlobalAtlasRegistry::ChunkMetaUploadPlan plan = system->_test_plan_chunk_meta_sync();
	const StreamingGlobalAtlasRegistry::SyncDiagnostics diagnostics = system->_test_get_atlas_sync_diagnostics();

	CHECK(system->_test_get_chunk_meta_dirty_all());
	CHECK(plan.full_update);
	CHECK(plan.dirty_count == 2);
	CHECK(plan.contiguous_range_count == 1);
	CHECK(diagnostics.chunk_meta_full_update);
	CHECK(diagnostics.chunk_meta_dirty_count == 2);
	CHECK(diagnostics.chunk_meta_range_count == 1);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Dirty atlas publication is invalidated when GPU sync is skipped") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint64_t generation_before = system->get_atlas_generation();
    CHECK(generation_before > 0);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
    if (generation_before == 0 || !system->get_asset_meta_buffer().is_valid() ||
            !system->get_chunk_meta_buffer().is_valid() || !system->get_asset_chunk_index_buffer().is_valid()) {
        FAIL("Dirty-atlas fixture did not publish its initial GPU metadata");
        return;
    }
    const RID asset_meta_before = system->_test_get_registry_asset_meta_buffer();
    const RID chunk_meta_before = system->_test_get_registry_chunk_meta_buffer();
    const RID chunk_index_before = system->_test_get_registry_asset_chunk_index_buffer();
    CHECK(rd->buffer_is_valid(asset_meta_before));
    CHECK(rd->buffer_is_valid(chunk_meta_before));
    CHECK(rd->buffer_is_valid(chunk_index_before));

    system->register_asset(810, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    system->_test_sync_global_atlas_state(nullptr);

    CHECK_FALSE(system->get_asset_meta_buffer().is_valid());
    CHECK_FALSE(system->get_chunk_meta_buffer().is_valid());
    CHECK_FALSE(system->get_asset_chunk_index_buffer().is_valid());
    CHECK(system->_test_get_registry_asset_meta_buffer() == asset_meta_before);
    CHECK(system->_test_get_registry_chunk_meta_buffer() == chunk_meta_before);
    CHECK(system->_test_get_registry_asset_chunk_index_buffer() == chunk_index_before);
    CHECK(rd->buffer_is_valid(asset_meta_before));
    CHECK(rd->buffer_is_valid(chunk_meta_before));
    CHECK(rd->buffer_is_valid(chunk_index_before));
    CHECK(system->get_atlas_generation() == 0);

    system->_test_sync_global_atlas_state(rd);

    CHECK(system->get_atlas_generation() > generation_before);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
    const RID asset_meta_restored = system->get_asset_meta_buffer();
    const RID chunk_meta_restored = system->get_chunk_meta_buffer();
    const RID chunk_index_restored = system->get_asset_chunk_index_buffer();
    CHECK(rd->buffer_is_valid(asset_meta_restored));
    CHECK(rd->buffer_is_valid(chunk_meta_restored));
    CHECK(rd->buffer_is_valid(chunk_index_restored));
    system.unref();
    CHECK_FALSE(rd->buffer_is_valid(asset_meta_before));
    CHECK_FALSE(rd->buffer_is_valid(chunk_meta_before));
    CHECK_FALSE(rd->buffer_is_valid(chunk_index_before));
    CHECK_FALSE(rd->buffer_is_valid(asset_meta_restored));
    CHECK_FALSE(rd->buffer_is_valid(chunk_meta_restored));
    CHECK_FALSE(rd->buffer_is_valid(chunk_index_restored));
}

TEST_CASE("[Streaming Pipeline] Upload abort clears pending chunk state") {
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(nullptr);

    if (system->get_frame_buffer().is_valid()) {
        FAIL("The missing-buffer abort fixture unexpectedly has a streaming buffer");
        return;
    }

    const uint32_t asset_id = 21;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads.start_pack_threads(*system.ptr());
    CHECK(uploads.pack_thread_running.load(std::memory_order_acquire));
    if (!uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("The missing-buffer abort fixture could not start its pack worker");
        return;
    }

    if (!_run_missing_buffer_abort_cycle(*system.ptr(), asset_id)) {
        return;
    }
    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 0);
}

TEST_CASE("[Streaming Pipeline] Repeated upload aborts do not leave pending jobs stuck") {
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(nullptr);

    if (system->get_frame_buffer().is_valid()) {
        FAIL("The missing-buffer abort fixture unexpectedly has a streaming buffer");
        return;
    }

    const uint32_t asset_id = 22;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads.start_pack_threads(*system.ptr());
    CHECK(uploads.pack_thread_running.load(std::memory_order_acquire));
    if (!uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("The missing-buffer abort fixture could not start its pack worker");
        return;
    }

    constexpr int planned_cycles = 5;
    int observed_cycles = 0;
    for (int cycle = 0; cycle < planned_cycles; cycle++) {
        if (!_run_missing_buffer_abort_cycle(*system.ptr(), asset_id)) {
            FAIL("Repeated upload abort cycle ", cycle + 1, " did not complete");
            return;
        }
        observed_cycles++;
    }
    CHECK(observed_cycles == planned_cycles);
    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 0);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Atlas generation bumps on quantization buffer resize") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    const QuantizationConfig saved_quantization_config = g_quantization_config;
    g_quantization_config.per_chunk_quantization = true;
    g_quantization_config.position_bits = 16;
    g_quantization_config.scale_bits = 12;
    g_quantization_config.quantize_scales = false;

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    const uint32_t asset_id = 11;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));
    system->update_streaming(camera_transform, projection);

    const uint64_t generation_before_resize = system->get_atlas_generation();
    const RID quant_buffer_before_resize = system->get_atlas_quantization_buffer();
    CHECK(quant_buffer_before_resize.is_valid());

    system->unregister_asset(asset_id);
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE + 1));
    system->update_streaming(camera_transform, projection);

    const uint64_t generation_after_resize = system->get_atlas_generation();
    const RID quant_buffer_after_resize = system->get_atlas_quantization_buffer();
    CHECK(quant_buffer_after_resize.is_valid());
    CHECK(generation_after_resize > generation_before_resize);

    g_quantization_config = saved_quantization_config;
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Mixed DC encoding disables per-chunk quantization fallback") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    const QuantizationConfig saved_quantization_config = g_quantization_config;
    g_quantization_config.per_chunk_quantization = true;
    g_quantization_config.position_bits = 16;
    g_quantization_config.scale_bits = 12;
    g_quantization_config.quantize_scales = false;

    Ref<::GaussianData> mixed_data = create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE);
    CHECK(mixed_data.is_valid());
    if (!mixed_data.is_valid()) {
        g_quantization_config = saved_quantization_config;
        FAIL("Mixed-encoding fixture has no GaussianData");
        return;
    }
    Gaussian linear = mixed_data->get_gaussian(0);
    linear.render_meta = gaussian_set_dc_encoding(linear.render_meta, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    mixed_data->set_gaussian(0, linear);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    const uint32_t asset_id = 23;
    system->register_asset(asset_id, mixed_data);
    CHECK_FALSE(system->is_per_chunk_quantization_enabled());
    CHECK_FALSE(system->get_atlas_quantization_buffer().is_valid());

    g_quantization_config = saved_quantization_config;
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] IO layout hints clamp chunk size to CHUNK_SIZE") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    GaussianStreamingSystem::ConfigOverrides overrides;
    overrides.override_io_source = true;
    system->set_config_overrides(overrides);

    const uint32_t asset_id = 4242;
    const uint32_t oversized_count = GaussianStreamingSystem::CHUNK_SIZE + 1024;
    Vector<GaussianStreamingSystem::ChunkLayoutHint> hints;
    hints.resize(1);
    hints.write[0].start_idx = 0;
    hints.write[0].count = oversized_count;
    hints.write[0].bounds = AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
    hints.write[0].center = Vector3();
    hints.write[0].radius = 1.0f;
    system->set_io_chunk_layout_hints(hints, asset_id);

    system->register_asset(asset_id, create_test_gaussian_data(oversized_count));

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);
    system->update_streaming(camera_transform, projection);

    CHECK(system->get_max_chunk_splats() == GaussianStreamingSystem::CHUNK_SIZE);
    CHECK(system->get_max_chunk_count_per_asset() >= 2u);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Randomized IO layout hint cases keep stable fallback reasons and chunk counts") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    const uint32_t ordered_total = 2048;
    const uint32_t fallback_total = GaussianStreamingSystem::CHUNK_SIZE * 2 + 17;
    const uint32_t fallback_chunk_count = (fallback_total + GaussianStreamingSystem::CHUNK_SIZE - 1) / GaussianStreamingSystem::CHUNK_SIZE;
    const uint32_t oversize_total = GaussianStreamingSystem::CHUNK_SIZE * 3 + 50;
    const uint32_t oversize_chunk_count = 5;
    const uint32_t seeds[] = { 7u, 42u, 99u, 1337u, 9001u };

    for (uint32_t seed : seeds) {
        std::mt19937 rng(seed);

        {
            Ref<GaussianStreamingSystem> system;
            system.instantiate();
            system->initialize_empty(rd);

            GaussianStreamingSystem::ConfigOverrides overrides;
            overrides.override_io_source = true;
            system->set_config_overrides(overrides);

            const uint32_t asset_id = 1000u + seed;

            Vector<GaussianStreamingSystem::ChunkLayoutHint> ordered_hints;
            ordered_hints.resize(4);
            const uint32_t segment = ordered_total / 4;
            for (int i = 0; i < 4; i++) {
                ordered_hints.write[i].start_idx = uint32_t(i) * segment;
                ordered_hints.write[i].count = (i == 3) ? (ordered_total - uint32_t(i) * segment) : segment;
                ordered_hints.write[i].bounds = AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
                ordered_hints.write[i].center = Vector3();
                ordered_hints.write[i].radius = 1.0f;
            }
            std::shuffle(ordered_hints.ptrw(), ordered_hints.ptrw() + ordered_hints.size(), rng);
            system->set_io_chunk_layout_hints(ordered_hints, asset_id);
            system->register_asset(asset_id, create_test_gaussian_data(ordered_total));

            Transform3D camera_transform;
            camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
            Projection projection;
            projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);
            system->update_streaming(camera_transform, projection);

            CHECK(_layout_hint_last_reason(system) == "none");
            CHECK(system->get_max_chunk_count_per_asset() == 4u);
        }

        {
            Ref<GaussianStreamingSystem> system;
            system.instantiate();
            system->initialize_empty(rd);

            GaussianStreamingSystem::ConfigOverrides overrides;
            overrides.override_io_source = true;
            system->set_config_overrides(overrides);

            const uint32_t asset_id = 2000u + seed;

            Vector<GaussianStreamingSystem::ChunkLayoutHint> overlap_hints = _build_partitioned_hints(fallback_total, 128, rng);
            if (overlap_hints.size() <= 1) {
                FAIL("Overlap fixture needs at least two layout hints");
                return;
            }
            const int mutate_index = int(seed % uint32_t(overlap_hints.size() - 1)) + 1;
            overlap_hints.write[mutate_index].start_idx = overlap_hints[mutate_index - 1].start_idx;
            system->set_io_chunk_layout_hints(overlap_hints, asset_id);
            system->register_asset(asset_id, create_test_gaussian_data(fallback_total));

            Transform3D camera_transform;
            camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
            Projection projection;
            projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);
            system->update_streaming(camera_transform, projection);

            CHECK(_layout_hint_last_reason(system) == "hint_overlapping_ranges");
            CHECK(_layout_hint_reason_count(system, "hint_overlapping_ranges") == 1);
            CHECK(system->get_max_chunk_count_per_asset() == fallback_chunk_count);
        }

        {
            Ref<GaussianStreamingSystem> system;
            system.instantiate();
            system->initialize_empty(rd);

            GaussianStreamingSystem::ConfigOverrides overrides;
            overrides.override_io_source = true;
            system->set_config_overrides(overrides);

            const uint32_t asset_id = 3000u + seed;

            Vector<GaussianStreamingSystem::ChunkLayoutHint> remap_hints = _build_partitioned_hints(fallback_total, 192, rng);
            const int remap_index = int(seed % uint32_t(remap_hints.size()));
            remap_hints.write[remap_index].source_indices_remapped = true;
            remap_hints.write[remap_index].source_index_offset = remap_hints[remap_index].start_idx;
            system->set_io_chunk_layout_hints(remap_hints, asset_id);
            system->register_asset(asset_id, create_test_gaussian_data(fallback_total));

            Transform3D camera_transform;
            camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
            Projection projection;
            projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);
            system->update_streaming(camera_transform, projection);

            CHECK(_layout_hint_last_reason(system) == "remap_flag_unexpected");
            CHECK(_layout_hint_reason_count(system, "remap_flag_unexpected") == 1);
            CHECK(system->get_max_chunk_count_per_asset() == fallback_chunk_count);
        }

        {
            Ref<GaussianStreamingSystem> system;
            system.instantiate();
            system->initialize_empty(rd);

            GaussianStreamingSystem::ConfigOverrides overrides;
            overrides.override_io_source = true;
            system->set_config_overrides(overrides);

            const uint32_t asset_id = 4000u + seed;

            std::uniform_int_distribution<uint32_t> first_count_dist(
                    GaussianStreamingSystem::CHUNK_SIZE + 1,
                    GaussianStreamingSystem::CHUNK_SIZE + 32);
            const uint32_t first_count = first_count_dist(rng);
            const uint32_t second_count = oversize_total - first_count;

            Vector<GaussianStreamingSystem::ChunkLayoutHint> oversize_hints;
            oversize_hints.resize(2);
            oversize_hints.write[0].start_idx = 0;
            oversize_hints.write[0].count = first_count;
            oversize_hints.write[0].bounds = AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
            oversize_hints.write[0].center = Vector3();
            oversize_hints.write[0].radius = 1.0f;

            oversize_hints.write[1].start_idx = first_count;
            oversize_hints.write[1].count = second_count;
            oversize_hints.write[1].bounds = AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
            oversize_hints.write[1].center = Vector3();
            oversize_hints.write[1].radius = 1.0f;
            system->set_io_chunk_layout_hints(oversize_hints, asset_id);
            system->register_asset(asset_id, create_test_gaussian_data(oversize_total));

            Transform3D camera_transform;
            camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
            Projection projection;
            projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);
            system->update_streaming(camera_transform, projection);

            CHECK(_layout_hint_last_reason(system) == "none");
            CHECK(system->get_max_chunk_count_per_asset() == oversize_chunk_count);
        }
    }
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] VRAM accounting includes auxiliary atlas overhead") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    const uint32_t asset_id = 77;
    system->register_asset(asset_id, create_test_gaussian_data(1024));

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    // One update is enough to materialize atlas metadata buffers.
    system->update_streaming(camera_transform, projection);
    Dictionary pre_residency_stats = system->get_chunk_culling_stats();
    const double payload_before_mb = pre_residency_stats.get("vram_payload_mb", 0.0);
    const double overhead_before_mb = pre_residency_stats.get("vram_overhead_mb", 0.0);
    const uint64_t total_before = system->get_vram_usage();
    CHECK(overhead_before_mb > 0.0);
    CHECK(total_before > 0);

    system->begin_residency_requests();
    system->request_chunk_residency(asset_id, 0, 0);
    system->finalize_residency_requests();

    const auto residency = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->get_loaded_chunks() > 0;
    });
    if (!residency.ready()) {
        FAIL("Residency chunk failed to load ", residency.describe());
        return;
    }

    Dictionary post_residency_stats = system->get_chunk_culling_stats();
    const double payload_after_mb = post_residency_stats.get("vram_payload_mb", 0.0);
    const double overhead_after_mb = post_residency_stats.get("vram_overhead_mb", 0.0);
    const uint64_t total_after = system->get_vram_usage();

    CHECK(payload_after_mb >= payload_before_mb);
    CHECK(overhead_after_mb >= overhead_before_mb);
    // get_vram_usage() reports the real allocation now: MAX(loaded payload,
    // persistent-buffer allocation) folded together with the auxiliary atlas
    // overhead. Loading a single sub-capacity chunk need not grow the reported
    // total — the up-front persistent buffer dominates the tiny 1024-splat payload,
    // so the MAX is unchanged. Assert the accounting invariants instead of strict
    // per-payload growth: the total never shrinks, and because the auxiliary
    // overhead is non-zero it stays strictly above the persistent allocation alone
    // (i.e. the overhead really is folded in on top of the buffer allocation).
    const uint32_t persistent_bytes = system->_test_get_persistent_buffer_size();
    CHECK(total_after >= total_before);
    CHECK(total_after > uint64_t(persistent_bytes));
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Invalid camera/projection input is rejected safely") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    Transform3D valid_transform;
    valid_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection valid_projection;
    valid_projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    Dictionary before_stats = system->get_chunk_culling_stats();
    const int invalid_before = int(before_stats.get("invalid_camera_input_events", 0));

    Transform3D invalid_transform = valid_transform;
    invalid_transform.origin.x = std::numeric_limits<float>::quiet_NaN();
    system->update_streaming(invalid_transform, valid_projection);

    Dictionary after_transform_stats = system->get_chunk_culling_stats();
    const int invalid_after_transform = int(after_transform_stats.get("invalid_camera_input_events", 0));
    CHECK(invalid_after_transform == invalid_before + 1);

    Projection invalid_projection = valid_projection;
    invalid_projection.columns[0][0] = std::numeric_limits<float>::infinity();
    system->update_streaming(valid_transform, invalid_projection);

    Dictionary after_projection_stats = system->get_chunk_culling_stats();
    const int invalid_after_projection = int(after_projection_stats.get("invalid_camera_input_events", 0));
    CHECK(invalid_after_projection == invalid_before + 2);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] VRAM debug stats expose reported usage vs capacity semantics") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    Dictionary vram_stats = system->get_vram_debug_stats();
    CHECK(vram_stats.has("device_reported_usage_bytes"));
    CHECK(vram_stats.has("device_capacity_bytes"));
    CHECK(vram_stats.has("device_capacity_known"));
    CHECK(vram_stats.has("device_total_semantics"));
    CHECK(vram_stats.has("device_total_bytes"));

    const int64_t reported_usage_bytes = int64_t(vram_stats.get("device_reported_usage_bytes", int64_t(-1)));
    const int64_t capacity_bytes = int64_t(vram_stats.get("device_capacity_bytes", int64_t(-1)));
    const bool capacity_known = bool(vram_stats.get("device_capacity_known", false));

    CHECK(reported_usage_bytes >= 0);
    CHECK(capacity_bytes >= 0);
    CHECK(String(vram_stats.get("device_total_semantics", String())) == String("reported_usage"));
    CHECK(int64_t(vram_stats.get("device_total_bytes", int64_t(-1))) == reported_usage_bytes);
    if (!capacity_known) {
        CHECK(capacity_bytes == 0);
    }
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Effective max chunks are clamped to runtime buffer capacity") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();

    GaussianStreamingSystem::ConfigOverrides low_capacity_overrides;
    low_capacity_overrides.override_vram_budget = true;
    low_capacity_overrides.vram_budget_config.auto_regulate_enabled = false;
    low_capacity_overrides.vram_budget_config.budget_mb = 1024;
    low_capacity_overrides.vram_budget_config.min_chunks = 4;
    low_capacity_overrides.vram_budget_config.max_chunks = 4;
    system->set_config_overrides(low_capacity_overrides);
    system->initialize_empty(rd);

    // #1088: the runtime capacity is in atlas pages; every resident chunk owns at least one
    // page, so the page count is the most chunks the buffer can ever hold.
    const uint32_t runtime_capacity_chunks = static_cast<uint32_t>(
            uint64_t(system->get_buffer_capacity_splats()) / uint64_t(GaussianStreamingSystem::ATLAS_PAGE_SPLATS));
    if (runtime_capacity_chunks == 0) {
        FAIL("Runtime streaming buffer capacity is zero");
        return;
    }

    GaussianStreamingSystem::ConfigOverrides high_capacity_overrides = low_capacity_overrides;
    high_capacity_overrides.vram_budget_config.min_chunks = runtime_capacity_chunks;
    high_capacity_overrides.vram_budget_config.max_chunks = runtime_capacity_chunks + 32;
    system->set_config_overrides(high_capacity_overrides);

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);
    system->update_streaming(camera_transform, projection, 1.0f / 30.0f);

    const uint32_t effective_max_chunks = system->get_effective_max_chunks();
    CHECK(high_capacity_overrides.vram_budget_config.max_chunks > runtime_capacity_chunks);
    CHECK(effective_max_chunks == runtime_capacity_chunks);
    CHECK(effective_max_chunks < high_capacity_overrides.vram_budget_config.max_chunks);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Budget eviction prioritizes non-primary chunks under regulator pressure") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String async_pack_setting = "rendering/gaussian_splatting/streaming/async_pack_enabled";
    ScopedProjectSettingRestore async_pack_guard(project_settings, async_pack_setting);
    project_settings->set_setting(async_pack_setting, false);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();

    GaussianStreamingSystem::ConfigOverrides relaxed_overrides;
    relaxed_overrides.override_vram_budget = true;
    relaxed_overrides.vram_budget_config.auto_regulate_enabled = false;
    relaxed_overrides.vram_budget_config.budget_mb = 1024;
    relaxed_overrides.vram_budget_config.min_chunks = 1;
    relaxed_overrides.vram_budget_config.max_chunks = 64;
    system->set_config_overrides(relaxed_overrides);
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 909;
    system->register_asset(asset_id, create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE));

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_residency_requests();
    system->request_chunk_residency(asset_id, 0, 0);
    system->finalize_residency_requests();
    const auto non_primary_residency = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->get_loaded_chunks() > 0;
    });
    if (!non_primary_residency.ready()) {
        FAIL("Non-primary residency chunk failed to load ", non_primary_residency.describe());
        return;
    }

    const uint32_t loaded_before = system->get_loaded_chunks();
    CHECK(loaded_before > 0);

    // A current explicit request protects this chunk from budget eviction.
    // End that request before testing reclamation of non-primary residency.
    system->begin_residency_requests();
    system->finalize_residency_requests();

    auto *asset = system->_test_get_asset_state(asset_id);
    if (!asset) {
        FAIL("Budget eviction fixture lost its registered asset");
        return;
    }
    auto &asset_chunks = system->_test_get_asset_chunks(*asset);
    if (asset_chunks.size() != 1 || !asset_chunks[0].is_loaded) {
        FAIL("Budget eviction fixture requires one resident non-primary chunk");
        return;
    }
    // Non-primary registry chunks default to visible; retiring a request does
    // not change that metadata. This fixture tests invisible reclamation.
    asset_chunks[0].is_visible = false;
    system->_test_mark_chunk_meta_dirty(asset_id, 0);
    CHECK_FALSE(asset_chunks[0].is_visible);

    GaussianStreamingSystem::ConfigOverrides constrained_overrides = relaxed_overrides;
    constrained_overrides.vram_budget_config.budget_mb = 1;
    system->set_config_overrides(constrained_overrides);

    const auto budget_eviction = TestGaussianSplatting::gs_pump_until([&]() {
        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();
        return system->get_loaded_chunks() < loaded_before;
    });
    const bool observed_budget_eviction = budget_eviction.ready();
    if (!observed_budget_eviction) {
        FAIL("Non-primary budget eviction did not complete ", budget_eviction.describe());
        return;
    }

    CHECK(observed_budget_eviction);
    CHECK(system->get_loaded_chunks() == 0);
    CHECK(system->get_visible_chunks_evicted_this_frame() == 0);

    Dictionary analytics = system->get_streaming_analytics();
    CHECK(int64_t(analytics.get("scheduler_non_primary_scan_chunks", int64_t(0))) > 0);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] Tier presets apply streaming caps while project overrides remain traceable") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String tier_preset_setting = "rendering/gaussian_splatting/quality/tier_preset";
    const String tier_apply_setting = "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets";
    const String upload_frame_setting = "rendering/gaussian_splatting/streaming/max_upload_mb_per_frame";
    const String upload_slice_setting = "rendering/gaussian_splatting/streaming/max_upload_mb_per_slice";
    const String upload_bandwidth_setting = "rendering/gaussian_splatting/streaming/max_upload_mb_per_second";
    const String vram_budget_setting = "rendering/gaussian_splatting/streaming/vram_budget_mb";
    const String min_chunks_setting = "rendering/gaussian_splatting/streaming/min_chunks_in_vram";
    const String max_chunks_setting = "rendering/gaussian_splatting/streaming/max_chunks_in_vram";

    ScopedProjectSettingRestore tier_preset_guard(project_settings, tier_preset_setting);
    ScopedProjectSettingRestore tier_apply_guard(project_settings, tier_apply_setting);
    ScopedProjectSettingRestore upload_frame_guard(project_settings, upload_frame_setting);
    ScopedProjectSettingRestore upload_slice_guard(project_settings, upload_slice_setting);
    ScopedProjectSettingRestore upload_bandwidth_guard(project_settings, upload_bandwidth_setting);
    ScopedProjectSettingRestore vram_budget_guard(project_settings, vram_budget_setting);
    ScopedProjectSettingRestore min_chunks_guard(project_settings, min_chunks_setting);
    ScopedProjectSettingRestore max_chunks_guard(project_settings, max_chunks_setting);

    project_settings->set_setting(tier_apply_setting, true);
    project_settings->set_setting(tier_preset_setting, "low");
    project_settings->set_setting(upload_frame_setting, 128);
    project_settings->set_setting(upload_slice_setting, 16);
    project_settings->set_setting(upload_bandwidth_setting, 0);
    project_settings->set_setting(vram_budget_setting, int64_t(STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB));
    project_settings->set_setting(min_chunks_setting, 4);
    project_settings->set_setting(max_chunks_setting, 128);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);

    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    for (int i = 0; i < 2; i++) {
        system->begin_frame();
        system->update_streaming(camera_transform, projection, 1.0f / 60.0f);
        system->end_frame();
    }

    Dictionary analytics = system->get_streaming_analytics();
    CHECK(analytics.has("cap_tier_preset"));
    CHECK(analytics.has("cap_tier_active"));
    CHECK(String(analytics.get("cap_tier_preset", String())) == String("low"));
    CHECK(bool(analytics.get("cap_tier_active", false)));
    CHECK(int64_t(analytics.get("effective_upload_cap_mb_per_frame", int64_t(-1))) == 32);
    CHECK(int64_t(analytics.get("effective_upload_cap_mb_per_slice", int64_t(-1))) == 8);
    CHECK(int64_t(analytics.get("effective_upload_cap_mb_per_second", int64_t(-1))) == 256);
    CHECK(int64_t(analytics.get("effective_vram_budget_mb", int64_t(-1))) == 2048);
    CHECK(int64_t(analytics.get("effective_vram_min_chunks", int64_t(-1))) == 2);
    CHECK(int64_t(analytics.get("effective_vram_max_chunks", int64_t(-1))) == 32);
    CHECK(String(analytics.get("cap_source_upload_mb_per_frame", String())) == String("tier_preset"));
    CHECK(String(analytics.get("cap_source_upload_mb_per_slice", String())) == String("tier_preset"));
    CHECK(String(analytics.get("cap_source_upload_mb_per_second", String())) == String("tier_preset"));
    CHECK(String(analytics.get("cap_source_vram_budget_mb", String())) == String("tier_preset"));
    CHECK(String(analytics.get("cap_source_vram_min_chunks", String())) == String("tier_preset"));
    CHECK(String(analytics.get("cap_source_vram_max_chunks", String())) == String("tier_preset"));

    project_settings->set_setting(upload_frame_setting, 77);
    project_settings->set_setting(vram_budget_setting, 3333);
    project_settings->emit_signal("settings_changed");

    for (int i = 0; i < 2; i++) {
        system->begin_frame();
        system->update_streaming(camera_transform, projection, 1.0f / 60.0f);
        system->end_frame();
    }

    analytics = system->get_streaming_analytics();
    CHECK(int64_t(analytics.get("effective_upload_cap_mb_per_frame", int64_t(-1))) == 77);
    CHECK(int64_t(analytics.get("effective_upload_cap_mb_per_slice", int64_t(-1))) == 8);
    CHECK(int64_t(analytics.get("effective_vram_budget_mb", int64_t(-1))) == 3333);
    CHECK(String(analytics.get("cap_source_upload_mb_per_frame", String())) == String("project_override"));
    CHECK(String(analytics.get("cap_source_upload_mb_per_slice", String())) == String("tier_preset"));
    CHECK(String(analytics.get("cap_source_vram_budget_mb", String())) == String("project_override"));
    CHECK(String(analytics.get("cap_source_vram_max_chunks", String())) == String("tier_preset"));
}

TEST_CASE("[Streaming Pipeline] Queue pressure summary and latch invariants are deterministic") {
    StreamingQueuePressureController::PressureSample idle_sample;
    const StreamingQueuePressureController::PressureSummary idle_summary =
            StreamingQueuePressureController::summarize(idle_sample);
    CHECK_FALSE(idle_summary.active);
    CHECK(idle_summary.source == String(StreamingQueuePressureController::SOURCE_NONE));
    CHECK(idle_summary.reason == String(StreamingQueuePressureController::REASON_NONE));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(idle_summary, idle_sample));

    StreamingQueuePressureController::PressureSample pack_backlog_sample;
    pack_backlog_sample.pack_queue_depth = 5;
    const StreamingQueuePressureController::PressureSummary pack_backlog_summary =
            StreamingQueuePressureController::summarize(pack_backlog_sample);
    CHECK(pack_backlog_summary.active);
    CHECK(pack_backlog_summary.source == String(StreamingQueuePressureController::SOURCE_PACK));
    CHECK(pack_backlog_summary.reason == String(StreamingQueuePressureController::REASON_PACK_QUEUE_BACKLOG));
    CHECK(pack_backlog_summary.backlog_depth == 5);
    CHECK(StreamingQueuePressureController::validate_summary_invariants(pack_backlog_summary, pack_backlog_sample));

    StreamingQueuePressureController::PressureSample queue_and_caps_sample;
    queue_and_caps_sample.pack_queue_depth = 2;
    queue_and_caps_sample.upload_frame_cap_hit = true;
    const StreamingQueuePressureController::PressureSummary queue_and_caps_summary =
            StreamingQueuePressureController::summarize(queue_and_caps_sample);
    CHECK(queue_and_caps_summary.active);
    CHECK(queue_and_caps_summary.source == String(StreamingQueuePressureController::SOURCE_COMBINED));
    CHECK(queue_and_caps_summary.reason == String(StreamingQueuePressureController::REASON_QUEUE_AND_CAPS));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(queue_and_caps_summary, queue_and_caps_sample));

    bool latch_active = false;
    String latch_source = "bad_source";
    String latch_reason = "bad_reason";
    StreamingQueuePressureController::reset_latched_state(latch_active, latch_source, latch_reason);
    CHECK_FALSE(latch_active);
    CHECK(latch_source == String(StreamingQueuePressureController::SOURCE_NONE));
    CHECK(latch_reason == String(StreamingQueuePressureController::REASON_NONE));

    StreamingQueuePressureController::latch_summary(pack_backlog_summary, latch_active, latch_source, latch_reason);
    CHECK(latch_active);
    CHECK(latch_source == String(StreamingQueuePressureController::SOURCE_PACK));
    CHECK(latch_reason == String(StreamingQueuePressureController::REASON_PACK_QUEUE_BACKLOG));
    CHECK(StreamingQueuePressureController::validate_latched_state_invariants(latch_active, latch_source, latch_reason));

    // A later idle summary in the same frame should not clear the latch.
    StreamingQueuePressureController::latch_summary(idle_summary, latch_active, latch_source, latch_reason);
    CHECK(latch_active);
    CHECK(latch_source == String(StreamingQueuePressureController::SOURCE_PACK));
    CHECK(latch_reason == String(StreamingQueuePressureController::REASON_PACK_QUEUE_BACKLOG));
}

TEST_CASE("[Streaming Pipeline] Queue pressure scan-budget throttle transitions are deterministic") {
    StreamingQueuePressureController::ScanBudgetInput baseline_input;
    baseline_input.base_scan_budget = 10;
    baseline_input.throttle_enabled = false;
    const StreamingQueuePressureController::ScanBudgetResult baseline_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(baseline_input);
    CHECK(baseline_result.scan_budget == 10);
    CHECK_FALSE(baseline_result.throttle_active);

    StreamingQueuePressureController::ScanBudgetInput throttled_input;
    throttled_input.base_scan_budget = 10;
    throttled_input.throttle_enabled = true;
    throttled_input.throttle_min_queue_depth = 4;
    throttled_input.observed_queue_depth = 4;
    throttled_input.throttle_scan_cap = 8;
    throttled_input.scanned_this_frame = 2;
    throttled_input.enqueue_headroom = UINT32_MAX;
    const StreamingQueuePressureController::ScanBudgetResult throttled_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(throttled_input);
    CHECK(throttled_result.throttle_active);
    CHECK(throttled_result.scan_budget == 6);

    throttled_input.observed_queue_depth = 6;
    const StreamingQueuePressureController::ScanBudgetResult deep_pressure_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(throttled_input);
    CHECK(deep_pressure_result.throttle_active);
    CHECK(deep_pressure_result.scan_budget == 2);

    throttled_input.enqueue_headroom = 0;
    const StreamingQueuePressureController::ScanBudgetResult zero_headroom_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(throttled_input);
    CHECK(zero_headroom_result.throttle_active);
    CHECK(zero_headroom_result.scan_budget == 1);
}

TEST_CASE("[Streaming Pipeline] Residency admission controller remains sole visible-eviction authority") {
    ResidencyBudgetController::AdmissionPolicy policy;
    policy.can_replace_without_eviction = false;
    policy.enforce_vram_regulator_gate = false;
    policy.vram_regulator_allows_load = true;

    ResidencyBudgetController::AdmissionFrameBudget frame_budget =
            ResidencyBudgetController::make_frame_budget(8, 2, false);
    ResidencyBudgetController::AdmissionGate below_capacity_gate =
            ResidencyBudgetController::compute_admission_gate(4, frame_budget, policy);
    CHECK(below_capacity_gate.decision == ResidencyBudgetController::AdmissionDecision::LoadDirect);
    CHECK_FALSE(ResidencyBudgetController::should_attempt_visible_evict_fallback(below_capacity_gate));

    ResidencyBudgetController::AdmissionPolicy atlas_full_policy = policy;
    atlas_full_policy.atlas_slots_full = true;
    ResidencyBudgetController::AdmissionGate atlas_full_gate =
            ResidencyBudgetController::compute_admission_gate(4, frame_budget, atlas_full_policy);
    CHECK(atlas_full_gate.decision == ResidencyBudgetController::AdmissionDecision::EvictThenLoad);
    CHECK(ResidencyBudgetController::should_attempt_visible_evict_fallback(atlas_full_gate));

    ResidencyBudgetController::AdmissionFrameBudget atlas_full_blocked_budget =
            ResidencyBudgetController::make_frame_budget(8, 0, false);
    ResidencyBudgetController::AdmissionGate atlas_full_blocked_gate =
            ResidencyBudgetController::compute_admission_gate(4, atlas_full_blocked_budget, atlas_full_policy);
    CHECK(atlas_full_blocked_gate.decision == ResidencyBudgetController::AdmissionDecision::Skip);
    CHECK_FALSE(ResidencyBudgetController::should_attempt_visible_evict_fallback(atlas_full_blocked_gate));

    ResidencyBudgetController::AdmissionGate at_capacity_gate =
            ResidencyBudgetController::compute_admission_gate(8, frame_budget, policy);
    CHECK(at_capacity_gate.decision == ResidencyBudgetController::AdmissionDecision::EvictThenLoad);
    CHECK(ResidencyBudgetController::should_attempt_visible_evict_fallback(at_capacity_gate));

    ResidencyBudgetController::note_successful_eviction(frame_budget);
    CHECK(frame_budget.evictions_left == 1);

    ResidencyBudgetController::note_blocked_eviction(frame_budget);
    CHECK(frame_budget.eviction_blocked);
    ResidencyBudgetController::AdmissionGate blocked_eviction_gate =
            ResidencyBudgetController::compute_admission_gate(8, frame_budget, policy);
    CHECK(blocked_eviction_gate.decision == ResidencyBudgetController::AdmissionDecision::Skip);
    CHECK_FALSE(ResidencyBudgetController::should_attempt_visible_evict_fallback(blocked_eviction_gate));

    ResidencyBudgetController::AdmissionPolicy replacement_policy;
    replacement_policy.can_replace_without_eviction = true;
    replacement_policy.enforce_vram_regulator_gate = true;
    replacement_policy.vram_regulator_allows_load = true;
    ResidencyBudgetController::AdmissionGate replacement_gate =
            ResidencyBudgetController::compute_admission_gate(8, frame_budget, replacement_policy);
    CHECK(replacement_gate.decision == ResidencyBudgetController::AdmissionDecision::LoadDirect);
    CHECK_FALSE(ResidencyBudgetController::should_attempt_visible_evict_fallback(replacement_gate));

    ResidencyBudgetController::AdmissionPolicy regulator_gate_policy = policy;
    regulator_gate_policy.enforce_vram_regulator_gate = true;
    regulator_gate_policy.vram_regulator_allows_load = false;
    ResidencyBudgetController::AdmissionFrameBudget regulator_frame_budget =
            ResidencyBudgetController::make_frame_budget(8, 2, false);
    ResidencyBudgetController::AdmissionGate regulator_gate =
            ResidencyBudgetController::compute_admission_gate(4, regulator_frame_budget, regulator_gate_policy);
    CHECK(regulator_gate.decision == ResidencyBudgetController::AdmissionDecision::EvictThenLoad);
    CHECK(ResidencyBudgetController::should_attempt_visible_evict_fallback(regulator_gate));

    ResidencyBudgetController::AdmissionFrameBudget regulator_blocked_budget =
            ResidencyBudgetController::make_frame_budget(8, 0, false);
    ResidencyBudgetController::AdmissionGate regulator_blocked_gate =
            ResidencyBudgetController::compute_admission_gate(4, regulator_blocked_budget, regulator_gate_policy);
    CHECK(regulator_blocked_gate.decision == ResidencyBudgetController::AdmissionDecision::Skip);
    CHECK_FALSE(ResidencyBudgetController::should_attempt_visible_evict_fallback(regulator_blocked_gate));
}

TEST_CASE("[Streaming Pipeline] Owner mismatch contract encodes remediation paths deterministically") {
    ResourceOwnerMismatchContract::Inputs invalid_rid_inputs;
    invalid_rid_inputs.rid_valid = false;
    const ResourceOwnerMismatchContract::Decision invalid_rid_decision =
            ResourceOwnerMismatchContract::evaluate(invalid_rid_inputs);
    CHECK_FALSE(invalid_rid_decision.mismatch_detected);
    CHECK(ResourceOwnerMismatchContract::validate(invalid_rid_inputs, invalid_rid_decision));

    ResourceOwnerMismatchContract::Inputs matched_owner_inputs;
    matched_owner_inputs.rid_valid = true;
    matched_owner_inputs.has_owner = true;
    matched_owner_inputs.owner_instance_id = 42;
    matched_owner_inputs.active_instance_id = 42;
    const ResourceOwnerMismatchContract::Decision matched_owner_decision =
            ResourceOwnerMismatchContract::evaluate(matched_owner_inputs);
    CHECK_FALSE(matched_owner_decision.mismatch_detected);
    CHECK(ResourceOwnerMismatchContract::validate(matched_owner_inputs, matched_owner_decision));

    ResourceOwnerMismatchContract::Inputs foreign_owner_inputs;
    foreign_owner_inputs.rid_valid = true;
    foreign_owner_inputs.has_owner = true;
    foreign_owner_inputs.owner_instance_id = 100;
    foreign_owner_inputs.active_instance_id = 200;
    const ResourceOwnerMismatchContract::Decision foreign_owner_decision =
            ResourceOwnerMismatchContract::evaluate(foreign_owner_inputs);
    CHECK(foreign_owner_decision.mismatch_detected);
    CHECK(foreign_owner_decision.should_attempt_release);
    CHECK_FALSE(foreign_owner_decision.should_force_invalidate_after_release);
    CHECK(ResourceOwnerMismatchContract::validate(foreign_owner_inputs, foreign_owner_decision));

    ResourceOwnerMismatchContract::Inputs missing_owner_inputs;
    missing_owner_inputs.rid_valid = true;
    missing_owner_inputs.has_owner = false;
    missing_owner_inputs.owner_instance_id = 0;
    missing_owner_inputs.active_instance_id = 300;
    const ResourceOwnerMismatchContract::Decision missing_owner_decision =
            ResourceOwnerMismatchContract::evaluate(missing_owner_inputs);
    CHECK(missing_owner_decision.mismatch_detected);
    CHECK(missing_owner_decision.should_attempt_release);
    CHECK(missing_owner_decision.should_force_invalidate_after_release);
    CHECK(ResourceOwnerMismatchContract::validate(missing_owner_inputs, missing_owner_decision));
}

TEST_CASE("[Streaming Pipeline][SceneTree][RequiresGPU] Instance content generation tracks instance pipeline budget changes") {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (rs == nullptr) {
        FAIL("Rendering server unavailable");
        return;
    }

    ScopedStreamingManagerDevice manager_scope(RenderingDevice::get_singleton());
    GaussianSplatManager *manager = manager_scope.get();
    CHECK(manager != nullptr);
    if (manager == nullptr) {
        return;
    }

    RenderingDevice *primary_device = manager->get_primary_rendering_device();
    if (primary_device == nullptr) {
        FAIL("Primary rendering device unavailable");
        return;
    }

    const uint32_t total_gaussians = GaussianStreamingSystem::CHUNK_SIZE * 2;
    Ref<GaussianData> data = create_clustered_test_gaussian_data(total_gaussians, Vector3(-0.08f, -0.08f, -10.0f));
    WorldBackedRendererHarness harness;
    if (!harness.setup(data, true)) {
        FAIL("World-backed renderer unavailable");
        return;
    }
    Ref<GaussianSplatRenderer> renderer = harness.renderer;
    CHECK(renderer.is_valid());
    if (!renderer.is_valid()) {
        harness.teardown();
        return;
    }

    Transform3D cam_transform;
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 5000.0f);

    uint64_t stable_generation_prev = 0;
    uint64_t stable_generation_curr = 0;
    bool rendered_stable_frame = false;
    const auto stable_generation = TestGaussianSplatting::gs_pump_until([&]() {
        rendered_stable_frame = renderer->render_for_view(cam_transform, projection, RID(), Size2i(512, 512));
        if (!rendered_stable_frame) {
            return false;
        }
        if (renderer->has_rendered_content()) {
            stable_generation_prev = stable_generation_curr;
            stable_generation_curr = renderer->get_instance_pipeline_content_generation();
        }
        return stable_generation_prev != 0 && stable_generation_prev == stable_generation_curr;
    });

    if (!stable_generation.ready()) {
        FAIL("Instance pipeline content generation did not stabilize ", stable_generation.describe());
        renderer.unref();
        return;
    }
    CHECK(rendered_stable_frame);

    const uint64_t generation_before_budget_change = stable_generation_curr;
    const int previous_max_splats = renderer->get_max_splats();
    renderer->set_max_splats(MAX(1024, previous_max_splats / 2));
    renderer->clear_instance_pipeline_buffers();

    uint64_t generation_after_budget_change = generation_before_budget_change;
    bool rendered_changed_frame = false;
    const auto changed_generation = TestGaussianSplatting::gs_pump_until([&]() {
        rendered_changed_frame = renderer->render_for_view(cam_transform, projection, RID(), Size2i(512, 512));
        if (!rendered_changed_frame) {
            return false;
        }
        generation_after_budget_change = renderer->get_instance_pipeline_content_generation();
        return generation_after_budget_change != generation_before_budget_change;
    });
    if (!changed_generation.ready()) {
        FAIL("Instance pipeline budget change did not advance content generation ", changed_generation.describe());
        return;
    }

    CHECK(rendered_changed_frame);
    CHECK(generation_after_budget_change != generation_before_budget_change);

    harness.teardown();
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] LOD debug stats track transitions_this_frame across camera moves") {
    ScopedFallbackRD rd_scope;
    RenderingDevice *rd = rd_scope.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }

    const String lod_enabled_setting = "rendering/gaussian_splatting/lod/enabled";
    const String lod_levels_setting = "rendering/gaussian_splatting/lod/num_levels";
    const String lod_base_threshold_setting = "rendering/gaussian_splatting/lod/base_threshold";
    const String lod_max_distance_setting = "rendering/gaussian_splatting/lod/max_distance";

    ScopedProjectSettingRestore lod_enabled_guard(project_settings, lod_enabled_setting);
    ScopedProjectSettingRestore lod_levels_guard(project_settings, lod_levels_setting);
    ScopedProjectSettingRestore lod_base_threshold_guard(project_settings, lod_base_threshold_setting);
    ScopedProjectSettingRestore lod_max_distance_guard(project_settings, lod_max_distance_setting);

    project_settings->set_setting(lod_enabled_setting, true);
    project_settings->set_setting(lod_levels_setting, 8);
    project_settings->set_setting(lod_base_threshold_setting, 4.0f);
    project_settings->set_setting(lod_max_distance_setting, 80.0f);

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    // LOD transition telemetry is the primary chunk visibility controller's
    // counter; registered non-primary assets are not that controller's workset.
    system->initialize_with_device(create_clustered_test_gaussian_data(1024, Vector3(0.0f, 0.0f, 0.0f)), rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    Transform3D near_camera_transform;
    near_camera_transform.origin = Vector3(0.0f, 0.0f, 4.0f);

    Transform3D far_camera_transform;
    far_camera_transform.origin = Vector3(0.0f, 0.0f, 60.0f);

    system->update_streaming(near_camera_transform, projection);
    Dictionary near_stats = system->get_lod_debug_stats();
    CHECK(near_stats.has("transitions_this_frame"));

    system->update_streaming(far_camera_transform, projection);
    Dictionary transition_stats = system->get_lod_debug_stats();
    const int transitions_after_move = int(transition_stats.get("transitions_this_frame", 0));
    CHECK(transitions_after_move > 0);

    system->update_streaming(far_camera_transform, projection);
    Dictionary stable_stats = system->get_lod_debug_stats();
    CHECK(int(stable_stats.get("transitions_this_frame", -1)) == 0);
}

TEST_CASE("[Streaming Pipeline] Large-world LOD updates scan visible working set after warmup") {
    const uint32_t chunk_count = 80;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();

    GaussianStreamingSystem::ConfigOverrides overrides;
    overrides.override_lod_config = true;
    overrides.lod_config.enabled = true;
    overrides.lod_config.num_levels = 4;
    overrides.lod_config.base_threshold = 4.0f;
    overrides.lod_config.max_distance = 80.0f;
    system->set_config_overrides(overrides);
    system->set_lod_blend_enabled(true);

    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(chunk_count);
    for (uint32_t i = 0; i < chunk_count; i++) {
        const bool near_camera = i < 8;
        const Vector3 center = near_camera
                ? Vector3(float(i) * 0.25f, 0.0f, 0.0f)
                : Vector3(1000.0f + float(i), 0.0f, 0.0f);
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk.start_idx = i;
        chunk.count = 1;
        chunk.center = center;
        chunk.bounds = AABB(center - Vector3(0.05f, 0.05f, 0.05f), Vector3(0.1f, 0.1f, 0.1f));
        chunk.max_radius = 0.05f;
        chunk.distance = 0.0f;
        chunk.is_loaded = false;
        chunk.is_visible = true;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
        chunk.current_lod_level = 0;
        chunk.target_lod_level = 0;
        chunk.sh_band_level = 3;
        chunk.splat_skip_factor = 1;
        chunk.opacity_multiplier = 1.0f;
        chunk.effective_count = chunk.count;
    }

    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 100.0f);
    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    StreamingVisibilityController &visibility = system->_test_get_visibility_controller();

    visibility.update_chunk_visibility(*system.ptr(), camera_transform, projection);
    visibility.update_chunk_lod_parameters(*system.ptr(), camera_transform.origin);
    visibility.update_chunk_lod_blend_factors(*system.ptr(), camera_transform.origin);
    Dictionary warmup_stats = system->get_chunk_culling_stats();
    CHECK(int(warmup_stats.get("total_chunks", 0)) == int(chunk_count));
    CHECK(int(warmup_stats.get("lod_parameter_update_scan_count", 0)) == int(chunk_count));

    visibility.update_chunk_visibility(*system.ptr(), camera_transform, projection);
    visibility.update_chunk_lod_parameters(*system.ptr(), camera_transform.origin);
    visibility.update_chunk_lod_blend_factors(*system.ptr(), camera_transform.origin);
    Dictionary steady_stats = system->get_chunk_culling_stats();
    const int visible_chunks = int(steady_stats.get("visible_chunks", 0));
    const int lod_parameter_scans = int(steady_stats.get("lod_parameter_update_scan_count", 0));
    const int lod_blend_scans = int(steady_stats.get("lod_blend_update_scan_count", 0));

    CHECK(visible_chunks > 0);
    CHECK(visible_chunks < int(chunk_count));
    CHECK(lod_parameter_scans >= visible_chunks);
    CHECK(lod_parameter_scans < int(chunk_count));
    CHECK(lod_blend_scans >= visible_chunks);
    CHECK(lod_blend_scans < int(chunk_count));
}

TEST_CASE("[Streaming Pipeline] Primary eviction scans resident chunks instead of total chunks") {
    const uint32_t chunk_count = 100;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();

    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(chunk_count);
    for (uint32_t i = 0; i < chunk_count; i++) {
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk.start_idx = i;
        chunk.count = 1;
        chunk.center = Vector3(float(i), 0.0f, 0.0f);
        chunk.bounds = AABB(chunk.center, Vector3(0.1f, 0.1f, 0.1f));
        chunk.is_loaded = false;
        chunk.is_visible = false;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
	}
	system->_test_register_primary_asset_for_chunks();
	system->_test_reset_atlas_allocator(chunk_count);
	for (int i = 0; i < 10; i++) {
		system->begin_frame();
	}

    system->_test_mark_chunk_loaded_for_eviction(0, 7, false, 0, 30, 10.0f);
    system->_test_mark_chunk_loaded_for_eviction(0, 20, false, 0, 10, 5.0f);
    system->_test_mark_chunk_loaded_for_eviction(0, 88, true, 0, 1, 100.0f);

    const auto result = system->_test_evict_least_recently_used(false);

    CHECK(result == StreamingEvictionController::EvictionResult::EvictedNonVisible);
    CHECK_FALSE(chunks[20].is_loaded);
    CHECK(chunks[88].is_loaded);
    CHECK(system->_test_get_primary_eviction_scan_count() == 3);
    CHECK(system->_test_get_primary_eviction_candidate_count() == 3);
}

TEST_CASE("[Streaming Pipeline] Non-primary eviction scans resident chunks and preserves request exclusions") {
    const uint32_t asset_id = 777;
    const uint32_t chunk_count = 100;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->register_asset(asset_id, create_test_gaussian_data(1));

	auto *asset = system->_test_get_asset_state(asset_id);
	REQUIRE(asset != nullptr);
	system->_test_reset_atlas_allocator(chunk_count);
	LocalVector<GaussianStreamingTypes::StreamingChunk> &asset_chunks = system->_test_get_asset_chunks(*asset);
    asset_chunks.resize(chunk_count);
    for (uint32_t i = 0; i < chunk_count; i++) {
        GaussianStreamingTypes::StreamingChunk &chunk = asset_chunks[i];
        chunk.start_idx = i;
        chunk.count = 1;
        chunk.center = Vector3(float(i), 0.0f, 0.0f);
        chunk.bounds = AABB(chunk.center, Vector3(0.1f, 0.1f, 0.1f));
        chunk.is_loaded = false;
        chunk.is_visible = false;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
    }

    system->begin_residency_requests();
    CHECK(system->request_chunk_residency(asset_id, 33, 0) == OK);
    system->finalize_residency_requests();
    for (int i = 0; i < 10; i++) {
        system->begin_frame();
    }

    system->_test_mark_chunk_loaded_for_eviction(asset_id, 10, false, 0, 40, 10.0f);
    system->_test_mark_chunk_loaded_for_eviction(asset_id, 33, false, 0, 1, 99.0f);
    system->_test_mark_chunk_loaded_for_eviction(asset_id, 44, false, 0, 20, 20.0f);
    system->_test_mark_chunk_loaded_for_eviction(asset_id, 80, true, 0, 30, 30.0f);

    const auto result = system->_test_evict_non_primary_lru();

    CHECK(result == StreamingEvictionController::EvictionResult::EvictedNonVisible);
    CHECK(asset_chunks[33].is_loaded);
    CHECK_FALSE(asset_chunks[44].is_loaded);
    CHECK(system->_test_get_non_primary_eviction_scan_count() == 4);
    CHECK(system->_test_get_non_primary_eviction_candidate_count() == 3);
}

TEST_CASE("[Streaming Pipeline][SceneTree][RequiresGPU] Renderer renders streamed non-zero chunk") {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (rs == nullptr) {
        FAIL("Rendering server unavailable");
        return;
    }

    ScopedStreamingManagerDevice manager_scope(RenderingDevice::get_singleton());
    GaussianSplatManager *manager = manager_scope.get();
    CHECK(manager != nullptr);
    if (manager == nullptr) {
        return;
    }

    RenderingDevice *primary_device = manager->get_primary_rendering_device();
    if (primary_device == nullptr) {
        FAIL("Primary rendering device unavailable");
        return;
    }

    const uint32_t chunk_size = GaussianStreamingSystem::CHUNK_SIZE;
    const uint32_t total_gaussians = chunk_size * 2; // Ensure at least one non-zero chunk

    LocalVector<Gaussian> gaussians;
    gaussians.resize(total_gaussians);

    for (uint32_t i = 0; i < total_gaussians; i++) {
        Gaussian &g = gaussians[i];
        g = Gaussian{};
        const bool in_first_chunk = i < chunk_size;
        const float base_x = in_first_chunk ? 0.0f : 1000.0f;
        const uint32_t local_index = in_first_chunk ? i : (i - chunk_size);
        g.position = Vector3(base_x + float(local_index % 16) * 0.01f,
                float((local_index / 16) % 16) * 0.01f,
                -10.0f + float(local_index / 256) * 0.001f);
        g.scale = Vector3(0.1f, 0.1f, 0.1f);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 1.0f, 1.0f, 1.0f);
        g.normal = Vector3(0, 1, 0);
        g.area = 0.01f;
        g.brush_axes = Vector2(1.0f, 0.0f);
        g.painterly_meta = gaussian_pack_painterly_meta(0);
    }

    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(gaussians);
    Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
    for (uint32_t band = 0; band < 2; band++) {
        const uint32_t first_index = band * chunk_size;
        AABB bounds(gaussians[first_index].position, Vector3());
        for (uint32_t i = 1; i < chunk_size; i++) {
            bounds.expand_to(gaussians[first_index + i].position);
        }
        GaussianSplatRenderer::StaticChunk chunk = make_test_static_chunk(chunk_size, bounds);
        for (uint32_t i = 0; i < chunk_size; i++) {
            chunk.indices.write[i] = first_index + i;
        }
        static_chunks.push_back(chunk);
    }
    if (static_chunks.size() != 2) {
        FAIL("Non-zero chunk fixture requires two static chunks");
        return;
    }
    if (static_chunks[1].indices.is_empty()) {
        FAIL("Non-zero chunk fixture requires a populated second chunk");
        return;
    }
    CHECK(static_chunks[1].indices[0] == chunk_size);
    CHECK(static_chunks[0].bounds.get_end().x < static_chunks[1].bounds.position.x);
    WorldBackedRendererHarness harness;
    if (!harness.setup(data, false, static_chunks)) {
        FAIL("World-backed renderer unavailable");
        return;
    }
    Ref<GaussianSplatRenderer> renderer = harness.renderer;
    CHECK(renderer.is_valid());
    if (!renderer.is_valid()) {
        harness.teardown();
        return;
    }

    // Keep this regression deterministic by disabling Stage-B filters for the baseline.
    renderer->set_frustum_culling(false);
    renderer->set_lod_enabled(false);
    renderer->set_lod_min_screen_size(0.0f);
    renderer->set_lod_max_distance(0.0f);
    renderer->set_tiny_splat_screen_radius(0.0f);

    Transform3D camera_to_world;
    camera_to_world.origin = Vector3(1000.0f, 0.0f, 0.0f);
    const Transform3D world_to_camera = camera_to_world.affine_inverse();
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 5000.0f);

    CHECK_FALSE(renderer->has_rendered_content());

    bool rendered = false;
    const auto streamed_content = TestGaussianSplatting::gs_pump_until([&]() {
        rendered = renderer->render_for_view(world_to_camera, projection, RID(), Size2i(512, 512));
        return rendered && renderer->has_rendered_content() && renderer->get_visible_splat_count() == chunk_size;
    });
    if (!streamed_content.ready()) {
        FAIL("The non-zero streaming chunk did not become visible ", streamed_content.describe());
        return;
    }

    CHECK(rendered);
    CHECK(renderer->has_rendered_content());
    CHECK(renderer->get_visible_splat_count() == chunk_size);

    harness.teardown();
}

TEST_CASE("[Streaming Pipeline][SceneTree][RequiresGPU] Instance depth Stage-B applies frustum/screen/distance culling") {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (rs == nullptr) {
        FAIL("Rendering server unavailable");
        return;
    }

    ScopedStreamingManagerDevice manager_scope(RenderingDevice::get_singleton());
    GaussianSplatManager *manager = manager_scope.get();
    CHECK(manager != nullptr);
    if (manager == nullptr) {
        return;
    }

    RenderingDevice *primary_device = manager->get_primary_rendering_device();
    if (primary_device == nullptr) {
        FAIL("Primary rendering device unavailable");
        return;
    }
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings) {
        FAIL("ProjectSettings unavailable");
        return;
    }
    const String route_policy_setting = "rendering/gaussian_splatting/streaming/route_policy";
    const String instance_pipeline_setting = "rendering/gaussian_splatting/instance_pipeline/enabled";
    ScopedProjectSettingRestore route_guard(project_settings, route_policy_setting);
    ScopedProjectSettingRestore instance_pipeline_guard(project_settings, instance_pipeline_setting);
    project_settings->set_setting(route_policy_setting, int64_t(gs::settings::GS_ROUTE_RESIDENT));
    project_settings->set_setting(instance_pipeline_setting, true);
    project_settings->emit_signal("settings_changed");

    const uint32_t total_gaussians = 8192;
    LocalVector<Gaussian> gaussians;
    gaussians.resize(total_gaussians);
    const uint32_t inside_count = total_gaussians / 2;
    for (uint32_t i = 0; i < total_gaussians; i++) {
        Gaussian &g = gaussians[i];
        g = Gaussian{};
        const bool inside_frustum_band = i < inside_count;
        const float band_x = inside_frustum_band ? -0.8f : 30.0f;
        const float local_x = float(i % 64) * 0.025f;
        const float local_y = (float((i / 64) % 64) - 32.0f) * 0.03f;
        g.position = Vector3(band_x + local_x, local_y, -10.0f);
        // At depth 10 and a 512px viewport, the 64px tiny threshold removes
        // the small splats but retains large splats in both spatial bands.
        const float radius_scale = (i % 2) == 0 ? 1.0f : 0.06f;
        g.scale = Vector3(radius_scale, radius_scale, radius_scale);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 1.0f, 1.0f, 1.0f);
        g.normal = Vector3(0, 1, 0);
        g.area = 0.01f;
        g.brush_axes = Vector2(1.0f, 0.0f);
        g.painterly_meta = gaussian_pack_painterly_meta(0);
    }

    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(gaussians);
    // World submissions intentionally disable per-splat Stage-B filters. The
    // resident atlas-shaped instance contract exercises the same depth shader
    // with those filters enabled, without that world-only exemption.
    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate(primary_device);
    CHECK(renderer.is_valid());
    if (!renderer.is_valid()) {
        FAIL("Stage-B renderer unavailable");
        return;
    }
    renderer->initialize();
    const Error set_data_error = renderer->set_gaussian_data(data);
    CHECK(set_data_error == OK);
    if (set_data_error != OK) {
        FAIL("Stage-B renderer could not accept its fixture");
        return;
    }
    renderer->set_max_splats(total_gaussians);

    renderer->set_lod_enabled(true);
    renderer->set_lod_bias(1.0f);
    renderer->set_lod_min_screen_size(0.0f);
    renderer->set_lod_max_distance(0.0f);
    renderer->set_tiny_splat_screen_radius(0.0f);
    renderer->set_frustum_culling(false);

    Transform3D cam_transform;
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);

    bool rendered_sample = false;
    auto render_sample = [&]() {
        rendered_sample = renderer->render_for_view(cam_transform, projection, RID(), Size2i(512, 512));
        return rendered_sample;
    };

    uint32_t baseline_visible = 0;
    const auto instance_pipeline_ready = TestGaussianSplatting::gs_pump_until([&]() {
        if (!render_sample()) {
            return false;
        }
        const uint32_t visible = renderer->get_visible_splat_count();
        if (renderer->has_instance_pipeline_buffers() && renderer->has_rendered_content() && visible > 0) {
            baseline_visible = visible;
            return true;
        }
        return false;
    });

    if (!instance_pipeline_ready.ready()) {
        FAIL("Instance pipeline did not become ready in Stage-B culling regression test ", instance_pipeline_ready.describe());
        return;
    }
    const auto &instance_buffers = renderer->get_instance_pipeline_buffers();
    CHECK(rendered_sample);
    CHECK_FALSE(instance_buffers.world_submission_active);
    CHECK(renderer->get_instance_backend_policy() == GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT);
    CHECK(renderer->is_instance_contract_ready());
    const String cull_route = renderer->get_render_stats().get("cull_route_uid", String());
    CHECK(cull_route == String(RenderRouteUID::INSTANCE_CULL_GPU));
    if (instance_buffers.world_submission_active ||
            renderer->get_instance_backend_policy() != GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT ||
            !renderer->is_instance_contract_ready() || cull_route != String(RenderRouteUID::INSTANCE_CULL_GPU)) {
        FAIL("Stage-B fixture did not select its non-world instance contract");
        return;
    }

    renderer->set_frustum_culling(true);
    uint32_t frustum_visible = baseline_visible;
    const auto frustum_culled = TestGaussianSplatting::gs_pump_until([&]() {
        if (!render_sample()) {
            return false;
        }
        frustum_visible = renderer->get_visible_splat_count();
        return frustum_visible < baseline_visible;
    });
    if (!frustum_culled.ready()) {
        FAIL("Frustum culling did not reduce the visible count ", frustum_culled.describe());
        return;
    }
    CHECK(rendered_sample);

    renderer->set_frustum_culling(false);
    const auto frustum_reset = TestGaussianSplatting::gs_pump_until([&]() {
        return render_sample() && renderer->get_visible_splat_count() == baseline_visible;
    });
    if (!frustum_reset.ready()) {
        FAIL("Disabling frustum culling did not restore the baseline count ", frustum_reset.describe());
        return;
    }
    renderer->set_tiny_splat_screen_radius(64.0f);
    uint32_t tiny_visible = baseline_visible;
    const auto tiny_culled = TestGaussianSplatting::gs_pump_until([&]() {
        if (!render_sample()) {
            return false;
        }
        tiny_visible = renderer->get_visible_splat_count();
        return tiny_visible < baseline_visible;
    });
    if (!tiny_culled.ready()) {
        FAIL("Tiny-splat culling did not reduce the visible count ", tiny_culled.describe());
        return;
    }
    CHECK(rendered_sample);

    renderer->set_tiny_splat_screen_radius(0.0f);
    const auto tiny_reset = TestGaussianSplatting::gs_pump_until([&]() {
        return render_sample() && renderer->get_visible_splat_count() == baseline_visible;
    });
    if (!tiny_reset.ready()) {
        FAIL("Disabling tiny-splat culling did not restore the baseline count ", tiny_reset.describe());
        return;
    }
    renderer->set_lod_max_distance(15.0f);
    uint32_t distance_visible = baseline_visible;
    const auto distance_culled = TestGaussianSplatting::gs_pump_until([&]() {
        if (!render_sample()) {
            return false;
        }
        distance_visible = renderer->get_visible_splat_count();
        return distance_visible < baseline_visible && distance_visible > 0;
    });
    if (!distance_culled.ready()) {
        FAIL("Distance culling did not leave a reduced non-zero visible count ", distance_culled.describe());
        return;
    }
    CHECK(rendered_sample);

    CHECK(frustum_visible < baseline_visible);
    CHECK(tiny_visible < baseline_visible);
    CHECK(tiny_visible > 0);
    CHECK(distance_visible < baseline_visible);
    CHECK(distance_visible > 0);

    renderer.unref();
}

TEST_CASE("[Streaming Pipeline] Pressure sample total_pending_chunks includes pack_jobs_in_flight") {
    StreamingQueuePressureController::PressureSample sample;
    sample.pack_queue_depth = 3;
    sample.upload_queue_depth = 2;
    sample.sync_fallback_queue_depth = 1;
    sample.pack_jobs_in_flight = 4;

    const StreamingQueuePressureController::PressureSummary summary =
            StreamingQueuePressureController::summarize(sample);

    CHECK(summary.backlog_depth == 3); // MAX(1, MAX(3, 2))
    CHECK(summary.total_pending_chunks == 10); // 3 + 4 + 2 + 1
    CHECK(summary.active);
    CHECK(StreamingQueuePressureController::validate_summary_invariants(summary, sample));
}

TEST_CASE("[Streaming Pipeline] Pressure sample visible_eviction_active contributes to cap_active") {
    StreamingQueuePressureController::PressureSample sample;
    sample.visible_eviction_active = true;

    const StreamingQueuePressureController::PressureSummary summary =
            StreamingQueuePressureController::summarize(sample);

    CHECK(summary.active);
    CHECK(summary.cap_active);
    CHECK(summary.source == String(StreamingQueuePressureController::SOURCE_CAP));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(summary, sample));
}

TEST_CASE("[Streaming Pipeline] Idle sample has zero total_pending_chunks") {
    StreamingQueuePressureController::PressureSample idle_sample;
    const StreamingQueuePressureController::PressureSummary idle_summary =
            StreamingQueuePressureController::summarize(idle_sample);

    CHECK(idle_summary.total_pending_chunks == 0);
    CHECK(idle_summary.backlog_depth == 0);
    CHECK_FALSE(idle_summary.active);
    CHECK(StreamingQueuePressureController::validate_summary_invariants(idle_summary, idle_sample));
}

TEST_CASE("[Streaming Pipeline] Combined pressure with in-flight pack jobs validates invariants") {
    StreamingQueuePressureController::PressureSample sample;
    sample.pack_queue_depth = 2;
    sample.upload_queue_depth = 5;
    sample.sync_fallback_queue_depth = 1;
    sample.pack_jobs_in_flight = 3;
    sample.pack_inflight_saturated = true;
    sample.upload_frame_cap_hit = true;
    sample.visible_eviction_active = true;

    const StreamingQueuePressureController::PressureSummary summary =
            StreamingQueuePressureController::summarize(sample);

    CHECK(summary.active);
    CHECK(summary.cap_active);
    CHECK(summary.total_pending_chunks == 11); // 2 + 3 + 5 + 1
    CHECK(summary.backlog_depth == 5); // MAX(1, MAX(2, 5))
    CHECK(summary.source == String(StreamingQueuePressureController::SOURCE_COMBINED));
    CHECK(summary.reason == String(StreamingQueuePressureController::REASON_QUEUE_AND_CAPS));
    CHECK(StreamingQueuePressureController::validate_summary_invariants(summary, sample));
}

TEST_CASE("[Streaming VRAM] Reported total folds in the persistent buffer allocation") {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (!rs) {
        MESSAGE("Skipping - Rendering server unavailable");
        return;
    }

    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        rd = rs->create_local_rendering_device();
    }
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_with_device(create_test_gaussian_data(GaussianStreamingSystem::CHUNK_SIZE), rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    // The persistent storage buffer is allocated at init even before any chunk
    // payload is uploaded. It is the single largest streaming VRAM allocation,
    // so the reported total must account for the whole allocation, not just the
    // (currently zero) loaded payload. Regression guard for the under-count:
    // pre-fix get_vram_usage() returned only the auxiliary overhead here.
    const uint32_t persistent_bytes = system->_test_get_persistent_buffer_size();
    REQUIRE(persistent_bytes > 0u);
    const uint64_t reported = system->get_vram_usage();
    CHECK(reported >= uint64_t(persistent_bytes));
    CHECK(reported > 0u);
}

// ---------------------------------------------------------------------------
// #1086: forward-progress telemetry for the open-world proof lane.
//
// The lane used to score residency as visible splats over the whole world, count
// every frame with zero completions as "no progress" (frame-rate dependent: a
// pipeline completing ~37 chunks/s at 200 fps completes nothing on ~80% of its
// frames), and count every frame whose scan was cut to one chunk as "starved"
// (which is exactly what the pack-saturation throttle is designed to do). These
// cases pin what each signal is supposed to mean, using the engine's own code.
// ---------------------------------------------------------------------------

namespace {

void _setup_needed_set_chunk(GaussianStreamingTypes::StreamingChunk &r_chunk, uint32_t p_index,
        const Vector3 &p_center, bool p_loaded, bool p_gpu_resident, bool p_upload_pending) {
    r_chunk.start_idx = p_index;
    r_chunk.count = 1;
    r_chunk.center = p_center;
    r_chunk.bounds = AABB(p_center - Vector3(0.05f, 0.05f, 0.05f), Vector3(0.1f, 0.1f, 0.1f));
    r_chunk.max_radius = 0.05f;
    r_chunk.distance = 0.0f;
    r_chunk.is_loaded = p_loaded;
    r_chunk.gpu_resident = p_gpu_resident;
    r_chunk.upload_pending = p_upload_pending;
    r_chunk.is_visible = false;
    r_chunk.buffer_slot = p_loaded ? p_index : UINT32_MAX;
    r_chunk.effective_count = r_chunk.count;
}

} // namespace

TEST_CASE("[Streaming Pipeline] Needed-set residency counts resident needed chunks, not the whole world (#1086)") {
    // 10 chunks in front of the camera (the needed set) and 20 loaded, GPU-resident
    // chunks behind it. The needed set is 4 resident + 2 loaded-but-not-yet-GPU-
    // resident + 2 upload-pending + 2 unserved.
    const uint32_t needed_count = 10;
    const uint32_t behind_count = 20;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();

    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(needed_count + behind_count);
    for (uint32_t i = 0; i < needed_count; i++) {
        const Vector3 center(float(i % 5) * 0.2f - 0.4f, 0.0f, -5.0f - float(i));
        const bool resident = i < 4;
        const bool loaded_not_gpu = i == 4 || i == 5;
        const bool pending = i == 6 || i == 7;
        _setup_needed_set_chunk(chunks[i], i, center, resident || loaded_not_gpu, resident, pending);
    }
    for (uint32_t i = 0; i < behind_count; i++) {
        const uint32_t idx = needed_count + i;
        _setup_needed_set_chunk(chunks[idx], idx, Vector3(0.0f, 0.0f, 50.0f + float(i)), true, true, false);
    }

    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 1000.0f);
    Transform3D camera_transform; // at the origin, looking down -Z
    system->begin_frame();
    system->_test_get_visibility_controller().update_chunk_visibility(*system.ptr(), camera_transform, projection);
    const Dictionary culling = system->get_chunk_culling_stats();
    if (int(culling.get("visible_chunks", -1)) != int(needed_count)) {
        FAIL("fixture precondition: expected exactly the ", needed_count, " front chunks to be visible, got ",
                int(culling.get("visible_chunks", -1)));
        return;
    }

    system->_test_set_visible_scan_result(false, 0);
    system->_test_build_visible_chunk_list();
    system->end_frame();
    const Dictionary analytics = system->get_streaming_analytics();

    CHECK(int64_t(analytics.get("needed_chunks", int64_t(-1))) == int64_t(needed_count));
    // Only GPU-resident needed chunks count: not the 20 resident chunks behind the
    // camera, not the loaded-but-not-GPU-resident pair, not the pending pair.
    CHECK(int64_t(analytics.get("needed_resident_chunks", int64_t(-1))) == 4);
    // Unserved = neither loaded nor already in the pipeline.
    CHECK(int64_t(analytics.get("needed_unserved_chunks", int64_t(-1))) == 2);
    // The whole-world view the old metric was built on cannot express this: 26
    // chunks are loaded, more than twice the needed set.
    CHECK(int(culling.get("loaded_chunks", -1)) == int(4 + 2 + behind_count));

    // Once the whole needed set is resident the ratio is exactly 1, however large
    // the world behind the camera is.
    for (uint32_t i = 0; i < needed_count; i++) {
        chunks[i].is_loaded = true;
        chunks[i].gpu_resident = true;
        chunks[i].upload_pending = false;
    }
    system->begin_frame();
    system->_test_get_visibility_controller().update_chunk_visibility(*system.ptr(), camera_transform, projection);
    system->_test_set_visible_scan_result(false, 0);
    system->_test_build_visible_chunk_list();
    system->end_frame();
    const Dictionary complete = system->get_streaming_analytics();
    CHECK(int64_t(complete.get("needed_chunks", int64_t(-1))) == int64_t(needed_count));
    CHECK(int64_t(complete.get("needed_resident_chunks", int64_t(-1))) == int64_t(needed_count));
    CHECK(int64_t(complete.get("needed_unserved_chunks", int64_t(-1))) == 0);
}

TEST_CASE("[Streaming Pipeline] Scan starvation excludes pack saturation but catches a throttled near-prefix scan (#1086)") {
    // Pack saturation: when pack jobs in flight reach max_pack_jobs_in_flight the
    // throttle cuts the scan to one chunk with zero headroom. The old lane metric
    // (scheduler_visible_scan_budget_effective <= 1) scored that as starvation.
    StreamingQueuePressureController::ScanBudgetInput saturated;
    saturated.base_scan_budget = 64;
    saturated.throttle_enabled = true;
    saturated.throttle_min_queue_depth = 1;
    saturated.observed_queue_depth = 0;
    saturated.throttle_scan_cap = 1024;
    saturated.enqueue_headroom = 0;
    const StreamingQueuePressureController::ScanBudgetResult saturated_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(saturated);
    CHECK(saturated_result.scan_budget == 1);
    CHECK_FALSE(StreamingQueuePressureController::visible_scan_had_capacity(true, saturated.enqueue_headroom));
    CHECK_FALSE(StreamingQueuePressureController::visible_scan_had_capacity(false, UINT32_MAX));

    StreamingQueuePressureController::VisibleScanStarvationInput input;
    input.needed_unserved_chunks = 5;
    input.load_candidates = 0;
    input.scan_had_capacity = false;
    CHECK_FALSE(StreamingQueuePressureController::is_visible_scan_starvation_eligible(input));
    CHECK_FALSE(StreamingQueuePressureController::is_visible_scan_starved(input));

    // A queue-depth throttle with headroom left: the budget shrinks below the visible
    // count, so the scan restarts at the nearest prefix. If that prefix is resident
    // the demand is never reached. This must count: the throttle being on is no
    // excuse while the pipeline has room.
    StreamingQueuePressureController::ScanBudgetInput throttled;
    throttled.base_scan_budget = 64;
    throttled.throttle_enabled = true;
    throttled.throttle_min_queue_depth = 1;
    throttled.observed_queue_depth = 3;
    throttled.throttle_scan_cap = 8;
    throttled.enqueue_headroom = 2;
    const StreamingQueuePressureController::ScanBudgetResult throttled_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(throttled);
    CHECK(throttled_result.throttle_active);
    CHECK(throttled_result.scan_budget < throttled.base_scan_budget);
    input.scan_had_capacity = StreamingQueuePressureController::visible_scan_had_capacity(true, throttled.enqueue_headroom);
    CHECK(input.scan_had_capacity);
    CHECK(StreamingQueuePressureController::is_visible_scan_starvation_eligible(input));
    CHECK(StreamingQueuePressureController::is_visible_scan_starved(input));
    // The scan reached the demand: eligible, not starved.
    input.load_candidates = 2;
    CHECK(StreamingQueuePressureController::is_visible_scan_starvation_eligible(input));
    CHECK_FALSE(StreamingQueuePressureController::is_visible_scan_starved(input));
    // No unserved demand: not even eligible.
    input.load_candidates = 0;
    input.needed_unserved_chunks = 0;
    CHECK_FALSE(StreamingQueuePressureController::is_visible_scan_starvation_eligible(input));

    // Through the system: published flags follow the needed set it computed, and a
    // frame on which the needed set was never built says so instead of reading as
    // "no demand".
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(3);
    for (uint32_t i = 0; i < 3; i++) {
        _setup_needed_set_chunk(chunks[i], i, Vector3(0.0f, 0.0f, -5.0f - float(i)), i == 0, i == 0, false);
    }
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 1000.0f);
    Transform3D camera_transform;

    system->begin_frame();
    system->end_frame(); // update_streaming never ran this frame
    const Dictionary unmeasured = system->get_streaming_analytics();
    CHECK_FALSE(bool(unmeasured.get("needed_set_measured", true)));
    CHECK_FALSE(bool(unmeasured.get("scheduler_visible_scan_starvation_eligible", true)));

    // The visible list is nearest-first: [0 (resident), 1 (unserved), 2 (unserved)].
    const auto scan_frame = [&](bool p_had_capacity, uint32_t p_candidates, uint32_t p_scanned) -> Dictionary {
        system->begin_frame();
        system->_test_get_visibility_controller().update_chunk_visibility(*system.ptr(), camera_transform, projection);
        system->_test_record_visible_scan_starvation(p_had_capacity, p_candidates, 0, p_scanned);
        system->_test_build_visible_chunk_list();
        system->end_frame();
        return system->get_streaming_analytics();
    };

    // Pack-saturated (no headroom) scan of the nearest chunk only: backpressure.
    const Dictionary saturated_frame = scan_frame(false, 0, 1);
    CHECK(bool(saturated_frame.get("needed_set_measured", false)));
    CHECK(int64_t(saturated_frame.get("needed_unserved_chunks", int64_t(-1))) == 2);
    CHECK_FALSE(bool(saturated_frame.get("scheduler_visible_scan_starvation_eligible", true)));
    CHECK_FALSE(bool(saturated_frame.get("scheduler_visible_scan_starved", true)));

    // Room, but a capped scan restarted at the resident nearest chunk and stopped:
    // the two unserved chunks behind it were never reached. Starved.
    const Dictionary starved = scan_frame(true, 0, 1);
    CHECK(bool(starved.get("scheduler_visible_scan_starvation_eligible", false)));
    CHECK(bool(starved.get("scheduler_visible_scan_starved", false)));

    // A full scan that found no candidate cannot have starved, even though chunks
    // are unserved by the time the needed set is built (e.g. evicted at upload
    // admission after the scan). This is the false positive the first round-2 GPU
    // run showed when starvation was judged at build time.
    const Dictionary full_scan = scan_frame(true, 0, 3);
    CHECK(int64_t(full_scan.get("needed_unserved_chunks", int64_t(-1))) == 2);
    CHECK_FALSE(bool(full_scan.get("scheduler_visible_scan_starvation_eligible", true)));
    CHECK_FALSE(bool(full_scan.get("scheduler_visible_scan_starved", true)));

    // The scan reached the demand: eligible, not starved.
    const Dictionary reached = scan_frame(true, 2, 3);
    CHECK(bool(reached.get("scheduler_visible_scan_starvation_eligible", false)));
    CHECK_FALSE(bool(reached.get("scheduler_visible_scan_starved", true)));
}

namespace {

struct NeededSetRun {
    uint32_t stalled_frames = 0;
    uint32_t zero_completion_frames = 0;
    float first_stalled_time = -1.0f;
};

// Drives advance_needed_set_progress for p_frames frames at p_dt. A needed chunk
// completes on every p_completion_period-th frame (never when the period is 0);
// with p_churn, each completion refills a slot freed by evicting a needed chunk
// p_lag frames earlier.
NeededSetRun _run_needed_set_progress(uint32_t p_frames, float p_dt, uint32_t p_completion_period, bool p_churn,
        uint32_t p_lag = 3) {
    NeededSetRun run;
    StreamingQueuePressureController::NeededSetProgressState state;
    for (uint32_t frame = 0; frame < p_frames; frame++) {
        StreamingQueuePressureController::NeededSetProgressInput in;
        in.needed_chunks = 1000;
        in.needed_resident_chunks = 384; // capacity-bound: the needed set cannot complete
        const bool completes = p_completion_period > 0 && frame % p_completion_period == p_completion_period - 1;
        const bool evicts = p_churn && p_completion_period > 0 &&
                (frame + p_lag) % p_completion_period == p_completion_period - 1;
        in.needed_chunks_completed = completes ? 1u : 0u;
        in.needed_chunks_evicted = evicts ? 1u : 0u;
        in.in_flight_loads = 12;
        in.frame_delta_seconds = p_dt;
        StreamingQueuePressureController::advance_needed_set_progress(state, in);
        run.zero_completion_frames += completes ? 0u : 1u;
        if (state.stall_seconds >= StreamingQueuePressureController::NEEDED_SET_STALL_THRESHOLD_SECONDS) {
            run.stalled_frames++;
            if (run.first_stalled_time < 0.0f) {
                run.first_stalled_time = float(frame + 1) * p_dt;
            }
        }
    }
    return run;
}

} // namespace

TEST_CASE("[Streaming Pipeline] Needed-set progress: throttled loading is progress, needed-set churn and prefetch are not (#1086)") {
    const float threshold = StreamingQueuePressureController::NEEDED_SET_STALL_THRESHOLD_SECONDS;
    const float dt_200 = 1.0f / 200.0f;

    // Throttled but genuine progress: 200 fps, a needed chunk every 5th frame
    // (40/s; the corridor measured ~37/s), nothing needed evicted.
    const NeededSetRun healthy = _run_needed_set_progress(2000, dt_200, 5, false);
    CHECK(healthy.stalled_frames == 0);
    // The old per-frame count on the same run: 80% of frames.
    CHECK(healthy.zero_completion_frames == 1600);

    // Same completion rate, but every completion refills a slot freed by evicting
    // another needed chunk (capacity-bound evict/reload churn): no progress. The
    // first version of this metric reset on ANY completion and could not see this.
    const NeededSetRun churn = _run_needed_set_progress(2000, dt_200, 5, true);
    CHECK(churn.stalled_frames > 0);
    CHECK(churn.first_stalled_time >= threshold - 0.001f);
    CHECK(churn.first_stalled_time < threshold + 2.0f * dt_200);

    // Only prefetch (non-needed) completions: needed_chunks_completed stays 0.
    const NeededSetRun prefetch_only = _run_needed_set_progress(2000, dt_200, 0, false);
    CHECK(prefetch_only.stalled_frames > 0);

    // The onset is time-based: the same churn trips at ~0.5 s at 30 fps too.
    const NeededSetRun churn_30 = _run_needed_set_progress(300, 1.0f / 30.0f, 2, true, 1);
    CHECK(churn_30.first_stalled_time >= threshold - 0.001f);
    CHECK(churn_30.first_stalled_time < threshold + 2.0f / 30.0f);

    // A complete needed set is never a stall.
    StreamingQueuePressureController::NeededSetProgressState state;
    state.stall_seconds = 10.0f;
    StreamingQueuePressureController::NeededSetProgressInput complete;
    complete.needed_chunks = 100;
    complete.needed_resident_chunks = 100;
    complete.frame_delta_seconds = dt_200;
    StreamingQueuePressureController::advance_needed_set_progress(state, complete);
    CHECK(state.stall_seconds == 0.0f);

    // Displacement debt is capped by the loads in flight, so a reload that never
    // arrives cannot suppress progress forever.
    StreamingQueuePressureController::NeededSetProgressInput evicted;
    evicted.needed_chunks = 100;
    evicted.needed_resident_chunks = 50;
    evicted.needed_chunks_evicted = 5;
    evicted.in_flight_loads = 0;
    evicted.frame_delta_seconds = dt_200;
    StreamingQueuePressureController::advance_needed_set_progress(state, evicted);
    CHECK(state.displacement_debt == 0);
    StreamingQueuePressureController::NeededSetProgressInput completes;
    completes.needed_chunks = 100;
    completes.needed_resident_chunks = 51;
    completes.needed_chunks_completed = 1;
    completes.frame_delta_seconds = dt_200;
    state.stall_seconds = 1.0f;
    CHECK(StreamingQueuePressureController::advance_needed_set_progress(state, completes) == 1);
    CHECK(state.stall_seconds == 0.0f);
}
