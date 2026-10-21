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
    // this test covers publication -> orchestrator -> real depth/sidecar/sort,
    // not snapshot admission, CPU selection or the final tile/raster image.
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
    const VisibleChunkRefGPU draw_list[2] = { {0, 0}, {0, 1} };
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
        renderer->publish_hlod_bundle(true);
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
TEST_CASE("[GaussianSplatting][HLOD][World][SceneTree][RequiresGPU] resident World3D refuses HLOD without main-view render buffers and keeps its canonical contract") {
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
    // lacks render buffers and must refuse HLOD while rendering canonically.
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
        return state.hlod_prepared_snapshot.is_valid() &&
                state.hlod_reason == "unsupported_view_or_material" && renderer->has_rendered_content();
    });
    if (!ready.ready()) {
        FAIL("resident unsupported-view canonical fallback not reached: ", renderer->get_resource_state().hlod_reason.utf8().get_data(),
                " ", ready.describe().utf8().get_data());
        return;
    }
    CHECK(renderer->get_device_state().rd == rd);
    for (uint32_t frame = 0; frame < 2; ++frame) {
        renderer->render_scene_instance(&render);
        const auto &state = renderer->get_resource_state();
        CHECK_FALSE(state.hlod_frame_active);
        CHECK(state.hlod_reason == String("unsupported_view_or_material"));
        CHECK(state.hlod_selected_nodes == 0);
        CHECK(state.hlod_selected_splats == 0);
        CHECK(state.hlod_selected_parents == 0);
        CHECK_FALSE(state.hlod_canonical.buffers.world_submission_active);
        CHECK_FALSE(state.hlod_relative.buffers.world_submission_active);
        CHECK(renderer->get_debug_state().sort_route_uid == String(RenderRouteUID::INSTANCE_SORT_GPU));
        CHECK(renderer->get_sorting_state().sorted_splat_count == 2);
        CHECK(renderer->has_rendered_content());
        CHECK_FALSE(state.hlod_active); // Complete canonical bundle restored after the actual main-view pass.
        CHECK(renderer->get_instance_pipeline_buffers().atlas_gaussian_buffer.is_valid());
        CHECK(state.hlod_canonical.buffers.atlas_gaussian_buffer.is_valid());
        CHECK(renderer->get_instance_pipeline_buffers().atlas_gaussian_buffer == state.hlod_canonical.buffers.atlas_gaussian_buffer);
    }
}
}
