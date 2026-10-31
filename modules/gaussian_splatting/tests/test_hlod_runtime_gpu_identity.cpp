#include "test_macros.h"
#include "../renderer/gpu_sorter.h"
#include "../renderer/tile_renderer.h"
#include "../renderer/instance_pipeline_contract.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../renderer/quantization_config.h"
#include "../renderer/splat_ref_encoding.h"
#include "../renderer/pipeline_io_contracts.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "core/config/project_settings.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../interfaces/output_compositor.h"
#include "../io/gaussian_splat_world_io.h"
#include "gs_test_pump.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include <cstring>

extern "C" int test_hlod_runtime_gpu_identity_cpp_force_link() { return 0; }

namespace TestHlodRuntimeGpuIdentity {
// The required GpuSorting harness owns an RD without a RenderingServer. The
// sorter's manager-acquired submission device must be that same live device.
class ScopedSortDevice {
    GaussianSplatManager *manager = nullptr;
    RenderingDevice *previous = nullptr;
    bool owned = false;
public:
    explicit ScopedSortDevice(RenderingDevice *p_rd) {
        manager = GaussianSplatManager::get_singleton();
        if (!manager) { manager = memnew(GaussianSplatManager); owned = true; }
        previous = manager->set_primary_rendering_device_for_testing(p_rd);
    }
    ~ScopedSortDevice() {
        manager->set_primary_rendering_device_for_testing(previous);
        if (owned) { memdelete(manager); }
    }
};
TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] allocated sorter byte ledger excludes external buffers and survives shutdown") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) { FAIL("GpuSorting harness RenderingDevice required"); return; }
	ScopedSortDevice manager_device(rd);
	Ref<RadixSort> sorter;
	sorter.instantiate();
	const uint64_t before = rd->get_memory_usage(RD::MEMORY_BUFFERS);
	const Error initialized = sorter->initialize(rd, 4096);
	if (initialized != OK) { FAIL("RadixSort initialization failed"); sorter->shutdown(); return; }
	uint64_t bytes = 0;
	CHECK(sorter->get_allocated_buffer_bytes(bytes));
	CHECK(bytes > 0);
	CHECK(bytes == rd->get_memory_usage(RD::MEMORY_BUFFERS) - before);
	const RID external = rd->storage_buffer_create(256);
	if (!external.is_valid()) { FAIL("external control buffer allocation failed"); sorter->shutdown(); return; }
	uint64_t after_external = 0;
	CHECK(sorter->get_allocated_buffer_bytes(after_external));
	CHECK(after_external == bytes);
	sorter->shutdown();
	uint64_t after_shutdown = UINT64_MAX;
	CHECK(sorter->get_allocated_buffer_bytes(after_shutdown));
	CHECK(after_shutdown == 0);
	rd->free(external);
	sorter.unref();
}
TEST_CASE("[GaussianSplatting][HLOD] unchanged RIDs with new cut content invalidate binding identity") {
	Vector<TileRenderer::RenderParams> sequence;
	sequence.resize(4); // Constant identity sequence; no GPU resource is dereferenced.
	sequence.write[0].hlod_relative = true;
	sequence.write[0].hlod_source_generation = 7;
	sequence.write[0].hlod_content_generation = 10;
	sequence.write[1] = sequence[0];
	sequence.write[2] = sequence[0];
	sequence.write[2].hlod_content_generation = 11;
	sequence.write[3] = sequence[2];
	sequence.write[3].hlod_relative = false;
	const Vector<uint64_t> trace = TileRenderer::_test_instance_pipeline_binding_generation_trace(sequence);
	if (trace.size() != 4) { FAIL("complete identity trace missing"); return; }
	CHECK(trace[0] == 1);
	CHECK(trace[1] == 1);
	CHECK(trace[2] == 2);
	CHECK(trace[3] == 3);
	CHECK(sizeof(SplatRefGPU) == 8);
	CHECK(sizeof(ChunkMetaGPU) == 64);
	CHECK(offsetof(ChunkMetaGPU, node_center_x) == 48);
	CHECK(offsetof(ChunkMetaGPU, node_center_y) == 52);
	CHECK(offsetof(ChunkMetaGPU, node_center_z) == 56);
}
}

namespace TestHlodRuntimeGpuIdentity {
TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] nonempty relative draw list executes a fresh instance sort with no legacy culled indices") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!rd || !ps) { FAIL("GpuSorting harness device and settings required"); return; }
    ScopedSortDevice manager_device(rd);
    struct RestoreSettings {
        ProjectSettings *ps;
        String key = "rendering/gaussian_splatting/sorting/force_cpu_sort";
        bool had;
        Variant previous;
        decltype(g_quantization_config) quantization = g_quantization_config;
        explicit RestoreSettings(ProjectSettings *p_ps) : ps(p_ps), had(ps->has_setting(key)) {
            if (had) { previous = ps->get_setting(key); }
            ps->set_setting(key, false);
            g_quantization_config.per_chunk_quantization = false;
        }
        ~RestoreSettings() {
            if (had) { ps->set_setting(key, previous); } else { ps->clear(key); }
            g_quantization_config = quantization;
        }
    } settings(ps);
    Ref<GaussianSplatRenderer> renderer;
    renderer.instantiate(rd);
    renderer->get_performance_settings().max_splats = 256;
    renderer->initialize();
    Ref<GPUSortingPipeline> pipeline = renderer->get_subsystem_state().sorting_pipeline;
    if (pipeline.is_null() || renderer->get_subsystem_state().gpu_culler.is_null()) {
        FAIL("actual renderer sort pipeline unavailable"); return;
    }
    renderer->refresh_gpu_sorter("hlod_nonempty_sort_control");
    pipeline->ensure_sort_buffers(256);
    if (!pipeline->get_buffer_handles().valid) { FAIL("actual sort buffers unavailable"); return; }
    renderer->set_frustum_culling(false);
    renderer->set_tiny_splat_screen_radius(0.0f);
    renderer->set_lod_min_screen_size(0.0f);
    renderer->get_view_state().last_camera_projection.set_perspective(60.0, 1.0, 0.1, 100.0);
    renderer->get_view_state().manual_viewport_override = Size2i(64, 64);
    auto &state = renderer->get_resource_state();
    auto &bundle = state.hlod_relative;
    auto &buffers = bundle.buffers;
    bundle.device_id = rd->get_device_instance_id();
    bundle.generation = 1;
    // Only the already-prepared marker is needed to exercise the regression:
    // this test covers explicit sort inputs -> orchestrator -> real depth/sidecar/sort,
    // not production bundle admission, CPU selection or the final tile/raster image.
    // Production publication requires a complete current world and is exercised
    // by the target-backed native main-view transition scenario.
    state.hlod_prepared_snapshot.instantiate();
    auto create = [&](uint32_t p_bytes, const void *p_data = nullptr, bool p_indirect = false) {
        Vector<uint8_t> bytes;
        bytes.resize(p_bytes);
        if (p_data) { std::memcpy(bytes.ptrw(), p_data, p_bytes); } else { bytes.fill(0); }
        RID rid = rd->storage_buffer_create(p_bytes, bytes,
                p_indirect ? RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT : 0);
        if (rid.is_valid()) {
            renderer->track_resource_owner(rid, rd);
            bundle.owned[bundle.owned_count++] = rid;
            bundle.bytes += p_bytes;
        }
        return rid;
    };
    PackedGaussian splats[2] = {};
    ChunkMetaGPU chunks[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        splats[i].position[0] = -16.0f;
        splats[i].position[2] = i == 0 ? -1.0f : -0.5f;
        splats[i].opacity = 1.0f;
        splats[i].rotation[3] = 1.0f;
        for (uint32_t axis = 0; axis < 3; ++axis) { splats[i].scale[axis] = 0.125f; }
        chunks[i].atlas_base = i;
        chunks[i].splat_count = 1;
        chunks[i].node_center_x = 16.0f;
        chunks[i].node_center_z = i == 0 ? -3.0f : -7.5f;
    }
    InstanceDataGPU instance = {};
    instance.rotation[3] = instance.inv_rotation[3] = instance.translation_scale[3] = 1.0f;
    instance.params[0] = instance.params[1] = 1.0f;
    instance.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
    // Identity camera: the per-draw camera offset equals the node centre the chunk table carries.
    const DrawnNodeGPU draw_list[2] = {
        { 0u, 0u, 1.0f, 0u, { 16.0f, 0.0f, -3.0f, 0.0f } },
        { 0u, 1u, 1.0f, 0u, { 16.0f, 0.0f, -7.5f, 0.0f } } };
    const uint32_t counters[3] = { 2, 0, 0 };
    buffers.atlas_gaussian_buffer = create(sizeof(splats), splats);
    buffers.instance_buffer = create(sizeof(instance), &instance);
    buffers.chunk_meta_buffer = create(sizeof(chunks), chunks);
    buffers.visible_chunk_buffer = create(sizeof(draw_list), draw_list);
    buffers.splat_ref_buffer = create(256 * sizeof(SplatRefGPU));
    buffers.hlod_draw_entry_buffer = create(256 * sizeof(uint32_t));
    buffers.counter_buffer = create(sizeof(counters), counters);
    buffers.chunk_dispatch_buffer = create(3 * sizeof(uint32_t), nullptr, true);
    buffers.indirect_count_buffer = create(sizeof(GaussianSplatting::IndirectDispatchLayout), nullptr, true);
    buffers.instance_count_buffer = create(sizeof(GaussianSplatting::IndirectDispatchLayout));
    buffers.hlod_relative = true;
    buffers.hlod_source_generation = 1;
    buffers.hlod_draw_count = buffers.max_visible_chunks = buffers.dispatch_chunk_count = 2;
    buffers.max_visible_splats = buffers.atlas_gaussian_count = 2;
    buffers.instance_count = buffers.max_chunk_splats = 1;
    buffers.world_submission_active = true;
    renderer->get_subsystem_state().gpu_culler->get_state().culled_indices.clear();
    // Same camera and storage on both passes: HLOD reuse must remain disabled.
    for (uint32_t pass = 0; pass < 2; ++pass) {
        if (rd->buffer_update(buffers.counter_buffer, 0, sizeof(counters), counters) != OK) {
            FAIL("cut counter upload failed"); return;
        }
        const SortBufferHandles sort_handles = pipeline->get_buffer_handles();
        buffers.sort_key_buffer = sort_handles.keys_buffer;
        buffers.sort_value_buffer = sort_handles.indices_buffer;
        buffers.hlod_content_generation = pass + 1;
        GaussianSplatRenderer::PublishedInstanceAssetRemap remap;
        remap.valid = true;
        remap.generation = 1;
        remap.asset_to_dense_id.insert(0, 0);
        renderer->publish_instance_pipeline_contract(buffers, remap, GaussianSplatRenderer::InstanceBackendPolicy::RESIDENT,
                pass + 1, "relative_sort_component_test");
        if (!GaussianSplatting::InstancePipelineContract::has_sort_buffers(renderer->get_instance_pipeline_buffers())) {
            FAIL("actual relative sort inputs incomplete"); return;
        }
        const auto result = renderer->test_sort_for_view(Transform3D(), GaussianRenderState::IndexDomain::CHUNK_REF);
        CHECK(result.did_execute);
        CHECK(renderer->get_debug_state().sort_route_uid == String(RenderRouteUID::INSTANCE_SORT_GPU));
        const Vector<uint8_t> count_bytes = rd->buffer_get_data(buffers.instance_count_buffer);
        const Vector<uint8_t> sorted_bytes = rd->buffer_get_data(pipeline->get_sort_indices_buffer(), 0, 2 * sizeof(uint32_t));
        const Vector<uint8_t> ref_bytes = rd->buffer_get_data(buffers.splat_ref_buffer, 0, 2 * sizeof(SplatRefGPU));
        const Vector<uint8_t> sidecar_bytes = rd->buffer_get_data(buffers.hlod_draw_entry_buffer, 0, 2 * sizeof(uint32_t));
        if (count_bytes.size() != int(sizeof(GaussianSplatting::IndirectDispatchLayout)) ||
                sorted_bytes.size() != 8 || ref_bytes.size() != 16 || sidecar_bytes.size() != 8) {
            FAIL("complete GPU output required"); return;
        }
        GaussianSplatting::IndirectDispatchLayout count_header = {};
        std::memcpy(&count_header, count_bytes.ptr(), sizeof(count_header));
        CHECK(count_header.element_count == 2);
        CHECK(count_header.dispatch_x == 1); // ceil(2 / 256), distinct from element_count.
        CHECK(count_header.dispatch_y == 1);
        CHECK(count_header.dispatch_z == 1);
        CHECK(count_header.overflow_flag == 0);
        CHECK(count_header.unclamped_total == 2);
        const auto *sorted = reinterpret_cast<const uint32_t *>(sorted_bytes.ptr());
        const auto *refs = reinterpret_cast<const SplatRefGPU *>(ref_bytes.ptr());
        const auto *entries = reinterpret_cast<const uint32_t *>(sidecar_bytes.ptr());
        for (uint32_t i = 0; i < 2; ++i) {
            CHECK(sorted[i] < 2);
            if (sorted[i] >= 2) { continue; }
            CHECK(gs_splat_ref::gs_splat_ref_atlas_index(refs[sorted[i]].atlas_index) == i);
            CHECK(entries[sorted[i]] == i);
        }
    }
    pipeline.unref();
    renderer.unref();
}
}

namespace TestHlodRuntimeGpuIdentity {
TEST_CASE("[GaussianSplatting][HLOD][World][SceneTree][RequiresGPU] resident World3D preserves canonical output while the targetless HLOD ledger is unavailable") {
    SceneTree *tree = SceneTree::get_singleton();
    RenderingServer *server = RenderingServer::get_singleton();
    RenderingDevice *rd = RenderingDevice::get_singleton();
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!tree || !server || !rd || !ps) { FAIL("WorldSceneTree GPU harness required"); return; }
    Window *root = tree->get_root();
    if (!root) { FAIL("SceneTree root required"); return; }
    struct SettingsGuard {
        ProjectSettings *settings;
        const String keys[4] = {
            "rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled",
            "rendering/gaussian_splatting/streaming/route_policy",
            "rendering/gaussian_splatting/instance_pipeline/enabled",
            "rendering/gaussian_splatting/sorting/force_cpu_sort"
        };
        Variant previous[4];
        bool existed[4] = {};
        QuantizationConfig quantization = g_quantization_config;
        explicit SettingsGuard(ProjectSettings *p_settings) : settings(p_settings) {
            for (uint32_t i = 0; i < 4; ++i) {
                existed[i] = settings->has_setting(keys[i]);
                if (existed[i]) { previous[i] = settings->get_setting(keys[i]); }
            }
            settings->set_setting(keys[0], true);
            settings->set_setting(keys[1], int64_t(gs::settings::GS_ROUTE_RESIDENT));
            settings->set_setting(keys[2], true);
            settings->set_setting(keys[3], false);
            settings->emit_signal("settings_changed");
            g_quantization_config.per_chunk_quantization = false;
        }
        ~SettingsGuard() {
            for (uint32_t i = 0; i < 4; ++i) {
                if (existed[i]) { settings->set_setting(keys[i], previous[i]); } else { settings->clear(keys[i]); }
            }
            settings->emit_signal("settings_changed");
            g_quantization_config = quantization;
        }
    } settings(ps);
    ScopedSortDevice manager_device(rd);
    struct WorldFixture {
        SceneTree *tree;
        GaussianSplatManager *owned_manager = nullptr;
        GaussianSplatSceneDirector *owned_director = nullptr;
        GaussianSplatWorld3D *node = nullptr;
        explicit WorldFixture(SceneTree *p_tree) : tree(p_tree) {
            if (!GaussianSplatManager::get_singleton()) { owned_manager = memnew(GaussianSplatManager); }
            if (!GaussianSplatSceneDirector::get_singleton()) { owned_director = memnew(GaussianSplatSceneDirector); }
        }
        ~WorldFixture() {
            if (node) {
                if (node->get_parent()) { node->get_parent()->remove_child(node); }
                memdelete(node);
                tree->process(0.0);
            }
            if (owned_director) { memdelete(owned_director); }
            if (owned_manager) { memdelete(owned_manager); }
        }
    } fixture(tree);
    g_quantization_config.per_chunk_quantization = false; // Also after any first-time manager initialization.
    Ref<GaussianData> data;
    data.instantiate();
    LocalVector<Gaussian> splats;
    splats.resize(2);
    for (uint32_t i = 0; i < 2; ++i) {
        splats[i].position = Vector3(0.125f + i * 0.25f, 0.125f, -4.0f);
        splats[i].scale = Vector3(0.125f, 0.125f, 0.125f);
        splats[i].rotation = Quaternion();
        splats[i].opacity = 0.75f;
    }
    LocalVector<Vector3> sh;
    data->set_gaussian_payload(splats, sh, 0u, 0u, false, false);
    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(data);
    if (world->bake_hlod() != OK) { FAIL("actual synthetic HLOD bake failed"); return; }
    fixture.node = memnew(GaussianSplatWorld3D);
    fixture.node->set_auto_apply_on_ready(false);
    fixture.node->set_world(world);
    root->add_child(fixture.node);
    tree->process(0.0);
    fixture.node->apply_world();
    Ref<GaussianSplatRenderer> renderer = fixture.node->get_renderer();
    if (renderer.is_null()) { FAIL("World3D did not obtain its shared renderer"); return; }
    // Positive target-backed acceptance lives in the required native Runtime
    // scenario Resident HLOD Main View. The dummy SceneTree server cannot own
    // the complete present-target stack. This actual World3D entry deliberately
    // lacks render buffers, so compositor target scratch never establishes its
    // device ledger. Prove that early refusal and canonical output here; the
    // native scenario separately checks the later unsupported-view gate.
    renderer->get_view_state().manual_viewport_override = Size2i(64, 64);
    RenderSceneDataRD scene;
    scene.cam_transform = Transform3D();
    scene.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
    RenderDataRD render;
    render.scene_data = &scene;
    const auto ready = TestGaussianSplatting::gs_pump_until([&]() {
        tree->process(0.0);
        renderer->render_scene_instance(&render);
        const auto &state = renderer->get_resource_state();
        return state.hlod_snapshot.is_valid() && renderer->is_instance_contract_ready() && renderer->has_rendered_content();
    });
    if (!ready.ready()) {
        FAIL("resident targetless canonical output not reached: ", renderer->get_resource_state().hlod_reason,
                " ", ready.describe());
        return;
    }
    CHECK(renderer->get_device_state().rd == rd);
    for (uint32_t frame = 0; frame < 2; ++frame) {
        renderer->render_scene_instance(&render);
        const auto &state = renderer->get_resource_state();
        CHECK(bool(ps->get_setting("rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled")));
        if (state.hlod_snapshot.is_null()) { FAIL("current World3D HLOD input snapshot missing"); return; }
        CHECK(state.hlod_snapshot->is_current());
        CHECK(state.hlod_prepared_snapshot.is_null());
        uint64_t route_bytes = 0;
        CHECK_FALSE(renderer->get_hlod_owned_byte_upper_bound(route_bytes));
        CHECK_FALSE(state.hlod_frame_active);
        CHECK(state.hlod_reason == String("resource_ledger_warmup_or_unavailable"));
        CHECK(state.hlod_selected_nodes == 0);
        CHECK(state.hlod_selected_splats == 0);
        CHECK(state.hlod_selected_parents == 0);
        const auto &bound = renderer->get_instance_pipeline_buffers();
        CHECK_FALSE(bound.hlod_relative);
        CHECK_FALSE(bound.world_submission_active);
        CHECK(renderer->get_debug_state().sort_route_uid == String(RenderRouteUID::INSTANCE_SORT_GPU));
        CHECK(renderer->get_sorting_state().sorted_splat_count == 2);
        CHECK(renderer->has_rendered_content());
        CHECK_FALSE(state.hlod_active);
        CHECK(bound.atlas_gaussian_buffer.is_valid());
        CHECK(state.resident_atlas_gaussian_buffer.is_valid());
        CHECK(bound.atlas_gaussian_buffer == state.resident_atlas_gaussian_buffer);
    }
}

// Streamed HLOD worlds never run a canonical frame, so the tile grid must follow each view's size: provision_hlod_route
// is what resizes it, and prepare_hlod_scratch still refuses a frame whose size differs from the grid (the old
// behaviour, which blanked a streamed world at every window size but its startup size).
TEST_CASE("[GaussianSplatting][HLOD][Resize][RequiresGPU] provisioning follows the view size and keeps the texture ledger at the live bytes") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GPU harness required"); return; }
    ScopedSortDevice manager_device(rd);
    Ref<TileRenderer> tile;
    tile.instantiate();
    const Vector2i small(256, 256);
    const Vector2i large(512, 384);
    if (tile->initialize(rd, small) != OK) { FAIL("TileRenderer init failed"); return; }
    TileRenderer::RenderParams params;
    params.total_gaussians = 64;
    params.max_visible_splats = 64;
    params.request_packed_stage_data = false;
    params.request_tighter_bounds = false;
    params.request_sh_amortization = false;
    const uint64_t headroom = uint64_t(1) << 30;
    params.viewport_size = small;
    if (tile->provision_hlod_route(small, RD::DATA_FORMAT_MAX, 4096) != OK) { FAIL("provisioning failed"); return; }
    CHECK(tile->get_provisioned_viewport() == small);
    CHECK(tile->prepare_hlod_scratch(params, headroom));
    // The grid still refuses a frame of another size: only provisioning may change it.
    params.viewport_size = large;
    CHECK_FALSE(tile->prepare_hlod_scratch(params, headroom));
    uint64_t small_bytes = 0;
    REQUIRE(tile->get_hlod_texture_byte_upper_bound(small_bytes));
    // The estimate is an upper bound of what the larger grid really charges.
    const uint64_t estimate = tile->estimate_hlod_texture_bytes_for_viewport(large);
    CHECK(estimate >= small_bytes);
    uint64_t large_bytes = 0;
    uint64_t small_again = 0;
    for (int i = 0; i < 40; ++i) {
        const Vector2i size = (i % 2 == 0) ? large : small;
        if (tile->provision_hlod_route(size, RD::DATA_FORMAT_MAX, 0) != OK) { FAIL("re-provisioning failed at ", i); return; }
        CHECK(tile->get_provisioned_viewport() == size);
        params.viewport_size = size;
        CHECK(tile->prepare_hlod_scratch(params, headroom)); // every alternation is admitted, per view
        uint64_t bytes = 0;
        if (!tile->get_hlod_texture_byte_upper_bound(bytes)) { FAIL("ledger lost validity at ", i); return; }
        if (i == 0) { large_bytes = bytes; }
        if (i == 1) { small_again = bytes; }
        if (i > 1) { CHECK(bytes == ((i % 2 == 0) ? large_bytes : small_again)); } // not one more discarded set per resize
    }
    CHECK(large_bytes <= estimate + (uint64_t(1) << 20));
    CHECK(small_again <= large_bytes);
}

// The compositor's destination-copy scratch is part of the same route ledger. Dragging a dock across many sizes used to add
// every discarded scratch again until the route limit refused every frame for good.
TEST_CASE("[GaussianSplatting][HLOD][Resize][RequiresGPU] the compositor scratch ledger follows the live textures across many sizes") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GPU harness required"); return; }
    ScopedSortDevice manager_device(rd);
    Ref<OutputCompositor> compositor;
    compositor.instantiate();
    RD::TextureFormat format;
    format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
    format.texture_type = RD::TEXTURE_TYPE_2D;
    format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
    uint64_t scratch_peak = 0;
    uint64_t last = 0;
    for (int i = 0; i < 40; ++i) {
        const Size2i extent(128 + 16 * (i % 5), 128 + 16 * (i % 7)); // many distinct extents: the pool evicts, then re-creates
        format.width = extent.x;
        format.height = extent.y;
        if (!compositor->ensure_hlod_scratch(rd, format, extent)) { FAIL("scratch creation failed"); return; }
        if (!compositor->get_hlod_scratch_byte_upper_bound(rd, last)) { FAIL("scratch ledger unavailable at ", i); return; }
        scratch_peak = MAX(scratch_peak, last);
    }
    // The pool keeps 4 textures per kind (192 x 224 x 4 B at most); 40 cumulative creations would be many times that.
    CHECK(scratch_peak <= uint64_t(4) * 192 * 224 * 4);
    compositor->clear_viewport_blit_resources();
    uint64_t after_clear = 1;
    CHECK_FALSE(compositor->get_hlod_scratch_byte_upper_bound(rd, after_clear)); // nothing live: no stale bytes, ledger re-based
    format.width = 128;
    format.height = 128;
    CHECK(compositor->ensure_hlod_scratch(rd, format, Size2i(128, 128)));
    CHECK(compositor->get_hlod_scratch_byte_upper_bound(rd, after_clear));
    CHECK(after_clear == uint64_t(128) * 128 * 4);
}

// Package E1 (V4 paged worlds) through the actual World3D entry. This harness has no render buffers, so what is proven
// here is what does not need a present target: a paged world draws NOTHING from a stand-in (it has no canonical
// payload and no warmup stub), says why, and the fallback for setups the HLOD route can never serve loads the payload
// and takes the ordinary resident route. The target-backed behavior (route published on the first frame whatever the
// camera sees, convergence) is the probe evidence of the PR (camera facing away, then turned toward the world).
struct PagedWorldHarness {
    SceneTree *tree = nullptr;
    GaussianSplatWorld3D *node = nullptr;
    GaussianSplatManager *owned_manager = nullptr;
    GaussianSplatSceneDirector *owned_director = nullptr;
    Ref<GaussianSplatWorld> world;
    ProjectSettings *ps = nullptr;
    Variant previous[3];
    bool existed[3] = {};
    const String keys[3] = {
        "rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled",
        "rendering/gaussian_splatting/streaming/route_policy",
        "rendering/gaussian_splatting/instance_pipeline/enabled"
    };
    ~PagedWorldHarness() {
        if (node) {
            if (node->get_parent()) { node->get_parent()->remove_child(node); }
            memdelete(node);
            tree->process(0.0);
        }
        if (owned_director) { memdelete(owned_director); }
        if (owned_manager) { memdelete(owned_manager); }
        for (uint32_t i = 0; i < 3 && ps; ++i) {
            if (existed[i]) { ps->set_setting(keys[i], previous[i]); } else { ps->clear(keys[i]); }
        }
        if (ps) { ps->emit_signal("settings_changed"); }
    }
    void configure(ProjectSettings *p_ps) {
        ps = p_ps;
        for (uint32_t i = 0; i < 3; ++i) {
            existed[i] = ps->has_setting(keys[i]);
            if (existed[i]) { previous[i] = ps->get_setting(keys[i]); }
        }
        ps->set_setting(keys[0], true);
        ps->set_setting(keys[1], int64_t(gs::settings::GS_ROUTE_RESIDENT));
        ps->set_setting(keys[2], true);
        ps->emit_signal("settings_changed");
    }
};

static bool make_paged_world(Ref<GaussianSplatWorld> &r_loaded, uint32_t p_count) {
    Ref<GaussianData> data;
    data.instantiate();
    LocalVector<Gaussian> splats;
    splats.resize(p_count);
    for (uint32_t i = 0; i < p_count; ++i) {
        splats[i].position = Vector3(0.125f + float(i) * 0.25f, 0.125f, -4.0f);
        splats[i].scale = Vector3(0.125f, 0.125f, 0.125f);
        splats[i].rotation = Quaternion();
        splats[i].opacity = 0.75f;
    }
    LocalVector<Vector3> sh;
    data->set_gaussian_payload(splats, sh, 0u, 0u, false, false);
    Ref<GaussianSplatWorld> source;
    source.instantiate();
    source->set_gaussian_data(data);
    if (source->bake_hlod() != OK) { return false; }
    const String path = OS::get_singleton()->get_temp_path().path_join("godotgs_v4_gpu_" + itos(OS::get_singleton()->get_ticks_usec()) + ".gsplatworld");
    if (source->save_hlod_paged(path) != OK) { return false; }
    Error err = ERR_BUG;
    ResourceFormatLoaderGaussianSplatWorld loader;
    r_loaded = loader.load(path, "", &err);
    return err == OK && r_loaded.is_valid() && r_loaded->has_hlod_paged_source();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] a paged World3D draws nothing from a stand-in and says why when no present target exists") {
    SceneTree *tree = SceneTree::get_singleton();
    RenderingDevice *rd = RenderingDevice::get_singleton();
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!tree || !RenderingServer::get_singleton() || !rd || !ps || !tree->get_root()) { FAIL("WorldSceneTree GPU harness required"); return; }
    PagedWorldHarness harness;
    harness.tree = tree;
    harness.configure(ps);
    ScopedSortDevice manager_device(rd);
    if (!GaussianSplatManager::get_singleton()) { harness.owned_manager = memnew(GaussianSplatManager); }
    if (!GaussianSplatSceneDirector::get_singleton()) { harness.owned_director = memnew(GaussianSplatSceneDirector); }
    g_quantization_config.per_chunk_quantization = false;
    if (!make_paged_world(harness.world, 4)) { FAIL("paged fixture failed"); return; }
    harness.node = memnew(GaussianSplatWorld3D);
    harness.node->set_auto_apply_on_ready(false);
    harness.node->set_world(harness.world);
    tree->get_root()->add_child(harness.node);
    tree->process(0.0);
    harness.node->apply_world();
    Ref<GaussianSplatRenderer> renderer = harness.node->get_renderer();
    if (renderer.is_null()) { FAIL("World3D did not obtain its shared renderer"); return; }
    CHECK(harness.world->has_hlod_paged_source()); // the HLOD route is usable here: no fallback
    renderer->get_view_state().manual_viewport_override = Size2i(64, 64);
    RenderSceneDataRD scene;
    scene.cam_transform = Transform3D();
    scene.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
    RenderDataRD render;
    render.scene_data = &scene;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        tree->process(0.0);
        renderer->render_scene_instance(&render);
    }
    const auto &state = renderer->get_resource_state();
    if (state.hlod_snapshot.is_null() || !state.hlod_snapshot->is_paged()) { FAIL("paged HLOD snapshot missing"); return; }
    CHECK(state.hlod_prepared_snapshot.is_null());
    CHECK_FALSE(state.hlod_frame_active);
    CHECK(state.hlod_reason == String("hlod_paged_provisioning_needs_render_target")); // not overwritten by the warmup reason
    // No stand-in: no canonical contract, nothing sorted, nothing rendered.
    CHECK_FALSE(renderer->is_instance_contract_ready());
    CHECK_FALSE(renderer->has_rendered_content());
    CHECK(renderer->get_sorting_state().sorted_splat_count == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] a paged World3D with LOD off loads its payload and draws on the resident route") {
    SceneTree *tree = SceneTree::get_singleton();
    RenderingDevice *rd = RenderingDevice::get_singleton();
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!tree || !RenderingServer::get_singleton() || !rd || !ps || !tree->get_root()) { FAIL("WorldSceneTree GPU harness required"); return; }
    PagedWorldHarness harness;
    harness.tree = tree;
    harness.configure(ps);
    ScopedSortDevice manager_device(rd);
    if (!GaussianSplatManager::get_singleton()) { harness.owned_manager = memnew(GaussianSplatManager); }
    if (!GaussianSplatSceneDirector::get_singleton()) { harness.owned_director = memnew(GaussianSplatSceneDirector); }
    g_quantization_config.per_chunk_quantization = false;
    if (!make_paged_world(harness.world, 4)) { FAIL("paged fixture failed"); return; }
    harness.node = memnew(GaussianSplatWorld3D);
    harness.node->set_auto_apply_on_ready(false);
    harness.node->set_lod_enabled(false); // the HLOD route can never serve this setup
    harness.node->set_world(harness.world);
    tree->get_root()->add_child(harness.node);
    tree->process(0.0);
    harness.node->apply_world();
    Ref<GaussianSplatRenderer> renderer = harness.node->get_renderer();
    if (renderer.is_null()) { FAIL("World3D did not obtain its shared renderer"); return; }
    // The fallback ran at submission: the world is the resident v3 shape now.
    CHECK_FALSE(harness.world->has_hlod_paged_source());
    if (harness.world->get_gaussian_data().is_null()) { FAIL("fallback did not materialize the payload"); return; }
    CHECK(harness.world->get_gaussian_data()->get_count() == 4);
    renderer->get_view_state().manual_viewport_override = Size2i(64, 64);
    RenderSceneDataRD scene;
    scene.cam_transform = Transform3D();
    scene.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
    RenderDataRD render;
    render.scene_data = &scene;
    const auto ready = TestGaussianSplatting::gs_pump_until([&]() {
        tree->process(0.0);
        renderer->render_scene_instance(&render);
        return renderer->is_instance_contract_ready() && renderer->has_rendered_content();
    });
    if (!ready.ready()) { FAIL("resident fallback output not reached: ", ready.describe()); return; }
    CHECK(renderer->get_sorting_state().sorted_splat_count == 4);
    const auto &state = renderer->get_resource_state();
    CHECK((state.hlod_snapshot.is_null() || !state.hlod_snapshot->is_paged()));
}

// N1: the HLOD texture ledgers must track the textures still owned. Resizing a viewport many times (an editor session)
// used to add every discarded texture again, until the route's byte limit refused every frame for good.
TEST_CASE("[GaussianSplatting][HLOD][Ledger][World][SceneTree][RequiresGPU] repeated viewport resizes keep the texture and scratch ledgers at the live bytes") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GPU harness required"); return; }
    ScopedSortDevice manager_device(rd);
    Ref<TileRenderer> tile;
    tile.instantiate();
    if (tile->initialize(rd, Vector2i(256, 256)) != OK) { FAIL("TileRenderer init failed"); return; }
    uint64_t base = 0;
    if (!tile->get_hlod_texture_byte_upper_bound(base) || base == 0) { FAIL("tile ledger unavailable after init"); return; }
    uint64_t after_first_cycle = 0;
    for (int i = 0; i < 40; ++i) {
        const Vector2i size = (i % 2 == 0) ? Vector2i(512, 384) : Vector2i(256, 256);
        if (tile->resize(size, RD::DATA_FORMAT_MAX) != OK) { FAIL("resize failed"); return; }
        uint64_t bytes = 0;
        if (!tile->get_hlod_texture_byte_upper_bound(bytes)) { FAIL("tile ledger lost validity at resize ", i); return; }
        if (i == 1) { after_first_cycle = bytes; }
        if (i > 1 && i % 2 == 1) {
            // The same viewport again: exactly the same live bytes, not one more discarded set per resize.
            CHECK(bytes == after_first_cycle);
        }
    }
    CHECK(after_first_cycle <= 2 * base + (uint64_t(1) << 20));

    Ref<OutputCompositor> compositor;
    compositor.instantiate();
    RD::TextureFormat format;
    format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
    format.texture_type = RD::TEXTURE_TYPE_2D;
    format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
    uint64_t scratch_peak = 0;
    uint64_t last = 0;
    for (int i = 0; i < 40; ++i) {
        const Size2i extent(128 + 16 * (i % 5), 128 + 16 * (i % 7)); // many distinct extents: the pool evicts, then re-creates
        format.width = extent.x;
        format.height = extent.y;
        if (!compositor->ensure_hlod_scratch(rd, format, extent)) { FAIL("scratch creation failed"); return; }
        if (!compositor->get_hlod_scratch_byte_upper_bound(rd, last)) { FAIL("scratch ledger unavailable at ", i); return; }
        scratch_peak = MAX(scratch_peak, last);
    }
    // The pool keeps 4 textures per kind (192 x 224 x 4 B at most); 40 cumulative creations would be many times that.
    CHECK(scratch_peak <= uint64_t(4) * 192 * 224 * 4);
    compositor->clear_viewport_blit_resources();
    uint64_t after_clear = 1;
    CHECK_FALSE(compositor->get_hlod_scratch_byte_upper_bound(rd, after_clear)); // nothing live: no stale bytes, ledger re-based
    format.width = 128;
    format.height = 128;
    CHECK(compositor->ensure_hlod_scratch(rd, format, Size2i(128, 128)));
    CHECK(compositor->get_hlod_scratch_byte_upper_bound(rd, after_clear));
    CHECK(after_clear == uint64_t(128) * 128 * 4);
}
}
