#include "test_macros.h"
#include "../renderer/gpu_sorter.h"
#include "../renderer/shader_compilation_helper.h"
#include "../renderer/hlod_shadow_caster_plan.h"
#include "../renderer/gs_shadow_caster.h"
#include "../lod/hlod_page_pool.h"
#include "../shaders/gs_shadow_caster.glsl.gen.h"
#include "../shaders/tile_binning.glsl.gen.h"
#include "../compute/depth_compute.glsl.gen.h"
#include "../renderer/tile_renderer.h"
#include "../renderer/instance_pipeline_contract.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gs_region_override_buffer.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../renderer/quantization_config.h"
#include "../renderer/splat_ref_encoding.h"
#include "../renderer/pipeline_io_contracts.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "../interfaces/debug_overlay_interfaces.h"
#include "../renderer/splat_identity_test_control.h"
#include "core/config/project_settings.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../nodes/gaussian_splat_node_3d.h"
#include "../core/gaussian_splat_asset.h"
#include "../interfaces/output_compositor.h"
#include "../io/gaussian_splat_world_io.h"
#include "gs_test_pump.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include <cmath>
#include <cstring>

TEST_CASE("[GaussianSplatting][HLOD] stable splat identity survives scattered page relocation and row reorder") {
	using namespace GSSplatIdentityTestControl;
	set_force_address_keying(false);
	const auto first = keys(7, 1029, 0x123456789ULL, 3 * 1024 + 5, 65521, 1);
	const auto relocated = keys(7, 1029, 0x123456789ULL, 19 * 1024 + 5, 65521, 19);
	CHECK(first.tie_break == relocated.tie_break);
	CHECK(first.history_slot == 3077u);
	CHECK(relocated.history_slot == 19461u);
	CHECK(first.history_key == relocated.history_key);
	CHECK(first.tie_break == ((7u << 14u) | 1029u)); // Same vector is executed from the GLSL in Python.
	CHECK(first.history_slot == 3077u);
	CHECK(first.history_key == 133501757u);
	for (const auto other : { keys(8, 1029, 0x123456789ULL, 3 * 1024 + 5, 65521),
			keys(7, 1030, 0x123456789ULL, 3 * 1024 + 5, 65521),
			keys(7, 1029, 0x223456789ULL, 3 * 1024 + 5, 65521) }) {
		CHECK(first.history_slot == other.history_slot);
		CHECK(first.history_key != other.history_key);
	}
}

TEST_CASE("[GaussianSplatting][HLOD] address identity forcing control is observable and discriminates relocation") {
	using namespace GSSplatIdentityTestControl;
	struct Restore { ~Restore() { set_force_address_keying(false); } } restore;
	CHECK_FALSE(get_force_address_keying());
	set_force_address_keying(true);
	CHECK(get_force_address_keying());
	const auto first = keys(7, 1029, 17, 3 * 1024 + 5, 65521);
	const auto relocated = keys(7, 1029, 17, 19 * 1024 + 5, 65521);
	CHECK(first.tie_break != relocated.tie_break);
	CHECK(first.history_slot != relocated.history_slot);
	CHECK(first.history_key != relocated.history_key);
	set_force_address_keying(false);
	const auto stable = keys(7, 1029, 17, 3 * 1024 + 5, 65521);
	CHECK(stable.tie_break != first.tie_break);
}

TEST_CASE("[GaussianSplatting][HLOD] A2 split tie budgets fit Tier A and refuse exact overflow with named dimensions") {
    using namespace GSSplatIdentityTestControl;
    CHECK(world_layout_refusal(24290, 16384).is_empty()); // Tier A: 1+15+14=30.
    CHECK(world_layout_refusal(131072, 16384).is_empty()); // 1+17+14=32.
    CHECK(world_layout_refusal(131073, 16384).contains("splat_identity_world_tie_overflow:N=18,L=14"));
    CHECK(layout_refusal(32, 1, 1516).is_empty()); // GS-GAME-1: 1+6+0+14=21.
    CHECK(layout_refusal(33, 1, 1516).is_empty()); // t=10 spawn keeps R=6.
    CHECK(layout_refusal(32767, 1, 65536).is_empty()); // 1 + R15 + C0 + L16 = 32.
    const String reason = layout_refusal(32768, 1, 65536);
    CHECK(reason.contains("splat_identity_instance_tie_overflow"));
    CHECK(reason.contains("R=16"));
    CHECK(reason.contains("C=0"));
    CHECK(reason.contains("L=16"));
    CHECK(keys(24289, 16383, 0, 5, 65521).tie_break == ((24289u << 14u) | 16383u));
    CHECK(keys(0, 0, 1, 0, 65521, 0, 0x80004000u).tie_break == 0x80004000u);
    CHECK(keys(0, 16383, 1, 16383, 65521, 0, 0x80004000u).tie_break == 0x80007fffu);
}

TEST_CASE("[GaussianSplatting][HLOD] A2 world tie node folding preserves local bits and reports only overflowing worlds") {
    for (const uint32_t splats : { 16384u, 65536u, 0x40000001u }) {
        const uint32_t bits = gs_splat_identity::world_local_bits(splats);
        const uint32_t capacity = 1u << (31u - bits);
        const auto exact = gs_splat_identity::world_layout(capacity, splats);
        CHECK(exact.fits());
        CHECK(exact.refusal().is_empty());
        CHECK_FALSE(exact.node_id_folded);
        Dictionary stats;
        exact.append_stats(stats);
        CHECK(stats.has("tie_world_node_folded"));
        CHECK_FALSE(bool(stats["tie_world_node_folded"]));
        for (const uint32_t local : { 0u, (1u << bits) - 1u }) {
            for (const uint32_t node : { 0u, capacity - 1u }) {
                CHECK(exact.sort_key(node, local) == ((node << bits) | local));
            }
            const auto folded = gs_splat_identity::world_layout(capacity + 1u, splats);
            CHECK(folded.fits());
            CHECK(folded.refusal().is_empty()); // Node count never refuses an otherwise valid world.
            CHECK(folded.node_id_folded);
            folded.append_stats(stats);
            CHECK(bool(stats["tie_world_node_folded"]));
            CHECK(folded.sort_key(capacity, local) == local);
            CHECK(folded.sort_key(capacity, local) == gs_splat_identity::world_layout(capacity + 2u, splats).sort_key(capacity, local));
            CHECK((folded.sort_key(0xffffffffu, local) & 0x80000000u) == 0u);
            CHECK((folded.sort_key(0xffffffffu, local) & ((1u << bits) - 1u)) == local);
        }
        // Reporting an exact or absent world clears a previously true value.
        exact.append_stats(stats);
        CHECK_FALSE(bool(stats["tie_world_node_folded"]));
        gs_splat_identity::world_layout(0, splats).append_stats(stats);
        CHECK_FALSE(bool(stats["tie_world_node_folded"]));
    }
    const auto tier_a = gs_splat_identity::world_layout(24290, 16384);
    CHECK_FALSE(tier_a.node_id_folded);
    CHECK(tier_a.sort_key(24289, 16383) == ((24289u << 14u) | 16383u));
    const auto large = gs_splat_identity::world_layout(262144, 16384);
    CHECK(large.node_id_folded);
    CHECK(large.sort_key(7, 1029) == ((7u << 14u) | 1029u));
    CHECK(large.sort_key(7u + (1u << 17u), 1029) == large.sort_key(7, 1029));
    const auto invalid_local = gs_splat_identity::world_layout(2, 0x80000001u);
    CHECK_FALSE(invalid_local.fits());
    CHECK_FALSE(invalid_local.node_id_folded);
    CHECK(invalid_local.refusal().contains("splat_identity_world_local_tie_overflow:L=32"));
}

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
	CHECK(offsetof(ChunkMetaGPU, page_table_ref) == 48);
	CHECK(offsetof(ChunkMetaGPU, reserved_node_y) == 52);
	CHECK(offsetof(ChunkMetaGPU, reserved_node_z) == 56);
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
    PackedGaussian splats[4] = {};
    ChunkMetaGPU chunks[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        splats[i].position[0] = -16.0f;
        splats[i].position[2] = i == 0 ? -1.0f : -0.5f;
        splats[i].opacity = 1.0f;
        splats[i].rotation[3] = 1.0f;
        for (uint32_t axis = 0; axis < 3; ++axis) { splats[i].scale[axis] = 0.125f; }
        chunks[i].atlas_base = i;
        chunks[i].splat_count = 1;
        chunks[i].reserved_node_y = i;
        chunks[i].reserved_node_z = 14;
    }
    InstanceDataGPU instance = {};
    instance.rotation[3] = instance.inv_rotation[3] = instance.translation_scale[3] = 1.0f;
    instance.params[0] = instance.params[1] = 1.0f;
    instance.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
    // Identity camera: the per-draw camera offset places each node.
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
    const uint32_t free_page = UINT32_MAX;
    buffers.hlod_page_table_buffer = create(sizeof(uint32_t), &free_page);
    buffers.hlod_page_owner_buffer = create(sizeof(uint32_t), &free_page);
    buffers.counter_buffer = create(sizeof(counters), counters);
    buffers.chunk_dispatch_buffer = create(3 * sizeof(uint32_t), nullptr, true);
    buffers.indirect_count_buffer = create(sizeof(GaussianSplatting::IndirectDispatchLayout), nullptr, true);
    buffers.instance_count_buffer = create(sizeof(GaussianSplatting::IndirectDispatchLayout));
    buffers.hlod_relative = true;
    buffers.hlod_source_generation = 1;
    buffers.hlod_draw_count = buffers.max_visible_chunks = buffers.dispatch_chunk_count = 2;
    buffers.max_visible_splats = buffers.atlas_gaussian_count = 2;
    buffers.instance_count = buffers.max_chunk_splats = 1;
    buffers.atlas_gaussian_count = 4;
    buffers.world_submission_active = true;
    renderer->get_subsystem_state().gpu_culler->get_state().culled_indices.clear();
    // Same camera, relocated payload. Compare actual GPU key words, then repeat
    // with the observable address-keying variant as the negative control.
    struct RestoreIdentity { ~RestoreIdentity() { GSSplatIdentityTestControl::set_force_address_keying(false); } } restore_identity;
    uint32_t first_ties[2] = {};
    for (uint32_t pass = 0; pass < 4; ++pass) {
        const bool forced = pass >= 2;
        GSSplatIdentityTestControl::set_force_address_keying(forced);
        CHECK(GSSplatIdentityTestControl::get_force_address_keying() == forced);
        const uint32_t placement = (pass & 1u) * 2u;
        for (uint32_t node = 0; node < 2; ++node) {
            splats[placement + node] = splats[node];
            chunks[node].atlas_base = placement + node;
        }
        if (rd->buffer_update(buffers.atlas_gaussian_buffer, 0, sizeof(splats), splats) != OK ||
                rd->buffer_update(buffers.chunk_meta_buffer, 0, sizeof(chunks), chunks) != OK) {
            FAIL("identity relocation upload failed"); return;
        }
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
        CHECK(pipeline->_test_uses_address_identity() == forced);
        CHECK(renderer->get_debug_state().sort_route_uid == String(RenderRouteUID::INSTANCE_SORT_GPU));
        const Vector<uint8_t> count_bytes = rd->buffer_get_data(buffers.instance_count_buffer);
        const Vector<uint8_t> sorted_bytes = rd->buffer_get_data(pipeline->get_sort_indices_buffer(), 0, 2 * sizeof(uint32_t));
        const Vector<uint8_t> ref_bytes = rd->buffer_get_data(buffers.splat_ref_buffer, 0, 2 * sizeof(SplatRefGPU));
        const Vector<uint8_t> sidecar_bytes = rd->buffer_get_data(buffers.hlod_draw_entry_buffer, 0, 2 * sizeof(uint32_t));
        const Vector<uint8_t> key_bytes = rd->buffer_get_data(buffers.sort_key_buffer, 0, 4 * sizeof(uint32_t));
        if (count_bytes.size() != int(sizeof(GaussianSplatting::IndirectDispatchLayout)) ||
                sorted_bytes.size() != 8 || ref_bytes.size() != 16 || sidecar_bytes.size() != 8 || key_bytes.size() != 16) {
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
        uint32_t key_words[4] = {};
        std::memcpy(key_words, key_bytes.ptr(), sizeof(key_words));
        // Depths are distinct, so sorted key order identifies node 0 then node 1.
        // Neither word's value is inferred from whether a dispatch was attempted.
        for (uint32_t i = 0; i < 2; ++i) {
            CHECK(sorted[i] < 2);
            if (sorted[i] >= 2) { continue; }
            CHECK(gs_splat_ref::gs_splat_ref_atlas_index(refs[sorted[i]].atlas_index) == placement + i);
            CHECK(entries[sorted[i]] == i);
            const uint32_t tie = key_words[2 * i];
            if (pass == 0) { first_ties[i] = tie; CHECK(tie == (i << 14u)); }
            if (pass == 1) { CHECK(tie == first_ties[i]); }
            if (forced) {
                CHECK(tie == placement + i);
                if (placement != 0 || i != 0) { CHECK(tie != first_ties[i]); }
            }
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

// Codex F3: the route grows the overlap storage with slack, else exactly (resident_instance_contract_publisher.cpp, #1472).
// A growth whose sorter cannot be built must not report success: the exact size must still be tried, the capacity must
// hold what was reported, and a measured overflow whose growth failed must stay pending.
TEST_CASE("[GaussianSplatting][HLOD][Resize][RequiresGPU] a refused padded overlap growth falls back to the exact size and reports only real growth") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GPU harness required"); return; }
    ScopedSortDevice manager_device(rd);
    const Vector2i view(256, 256);
    const uint64_t headroom = uint64_t(1) << 30;
    auto provisioned_tile = [&](Ref<TileRenderer> &r_tile) {
        r_tile.instantiate();
        return r_tile->initialize(rd, view) == OK && r_tile->provision_hlod_route(view, RD::DATA_FORMAT_MAX, 4096) == OK &&
                r_tile->get_allocated_overlap_capacity() >= 4096u;
    };
    {
        Ref<TileRenderer> tile;
        if (!provisioned_tile(tile)) { FAIL("TileRenderer provisioning failed"); return; }
        auto &sort = tile->_test_global_sort_resources();
        const uint32_t before = tile->get_allocated_overlap_capacity();
        const uint32_t exact = before * 2u;
        const uint32_t padded = before * 3u;
        // Premise: neither size is clamped by the configured cap, so the padded size really is the larger request.
        if (tile->clamp_hlod_overlap_records(padded) != padded) { FAIL("max_overlap_records clamps the padded request"); return; }
        sort.test_refuse_sorter_above_elements = exact; // the padded build fails; the exact one can be built
        using Growth = TileRenderer::HlodOverlapGrowth;
        CHECK(tile->ensure_hlod_overlap_capacity(exact, padded, headroom) == Growth::GREW);
        CHECK(tile->get_allocated_overlap_capacity() >= exact);
        CHECK(tile->get_allocated_overlap_capacity() < padded);
        CHECK(sort.sorter.is_valid());
        // Every build refused: nothing grows, and nothing may say it did.
        const uint32_t held = tile->get_allocated_overlap_capacity();
        sort.test_refuse_sorter_above_elements = held;
        CHECK(tile->ensure_hlod_overlap_capacity(held * 2u, held * 3u, headroom) == Growth::ALLOCATION_FAILED);
        CHECK(tile->get_allocated_overlap_capacity() == held);
        CHECK(sort.sorter.is_valid()); // the working sorter is kept
        // Headroom too small for the exact size: refused before any allocation.
        sort.test_refuse_sorter_above_elements = 0;
        CHECK(tile->ensure_hlod_overlap_capacity(held * 2u, held * 3u, uint64_t(held) * TileRenderer::HLOD_OVERLAP_GROWTH_BYTES_PER_RECORD) ==
                Growth::REFUSED_HEADROOM);
        CHECK(tile->get_allocated_overlap_capacity() == held);
        // A storage that already holds the request allocates nothing.
        CHECK(tile->ensure_hlod_overlap_capacity(held, held * 2u, headroom) == Growth::HELD);
    }
    {
        Ref<TileRenderer> tile;
        if (!provisioned_tile(tile)) { FAIL("TileRenderer provisioning failed"); return; }
        auto &sort = tile->_test_global_sort_resources();
        auto &overflow = tile->_test_async_readback().overflow_state;
        const uint32_t before = tile->get_allocated_overlap_capacity();
        overflow.first_frame_complete = true;
        overflow.overflow_detected = true;
        overflow.last_unclamped_total = before * 2u;
        sort.test_refuse_sorter_above_elements = before; // every growth fails
        CHECK_FALSE(tile->grow_hlod_overlap_storage(headroom));
        CHECK(tile->get_allocated_overlap_capacity() == before);
        CHECK(overflow.overflow_detected); // the measured demand is still pending, so the next safe point retries it
    }
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

// Model a device whose sorter cannot grow beyond the drawn budget. Keep real GPU
// buffers and sorting; only allocation requests above the limit are refused. This
// makes the leaf-count regression deterministic without allocating a Tier A world.
class BudgetLimitedSortPipeline : public GPUSortingPipeline {
public:
    static constexpr uint32_t LIMIT = 16384;
    uint32_t largest_request = 0;
    Ref<GPUSortingPipeline> actual;

    void rebuild_sorter(uint32_t p_capacity) override {
        largest_request = MAX(largest_request, p_capacity);
        if (p_capacity <= LIMIT) { actual->rebuild_sorter(p_capacity); }
    }
    void ensure_buffers(uint32_t p_capacity) override {
        largest_request = MAX(largest_request, p_capacity);
        if (p_capacity <= LIMIT) { actual->ensure_buffers(p_capacity); }
    }
    uint32_t get_max_elements() const override { return actual->get_max_elements(); }
    SortBufferHandles get_buffer_handles() const override { return actual->get_buffer_handles(); }
    RenderingDevice *get_sort_resource_device() const override { return actual->get_sort_resource_device(); }
};

static void paged_sort_budget_regression(bool p_overflow, bool p_binding_limit = false) {
    SceneTree *tree = SceneTree::get_singleton();
    RenderingDevice *rd = RenderingDevice::get_singleton();
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!tree || !tree->get_root() || !RenderingServer::get_singleton() || !rd || !ps) {
        FAIL("WorldSceneTree GPU harness required"); return;
    }
    ScopedSortDevice manager_device(rd);
    PagedWorldHarness harness;
    harness.tree = tree;
    harness.configure(ps);
    struct RestoreSortSettings {
        ProjectSettings *ps;
        String key = "rendering/gaussian_splatting/sorting/force_cpu_sort";
        bool had;
        Variant previous;
        decltype(g_quantization_config) quantization = g_quantization_config;
        explicit RestoreSortSettings(ProjectSettings *p_ps) : ps(p_ps), had(ps->has_setting(key)) {
            if (had) { previous = ps->get_setting(key); }
            ps->set_setting(key, false);
            g_quantization_config.per_chunk_quantization = false;
        }
        ~RestoreSortSettings() {
            if (had) { ps->set_setting(key, previous); } else { ps->clear(key); }
            g_quantization_config = quantization;
        }
    } settings(ps);
    if (!GaussianSplatSceneDirector::get_singleton()) { harness.owned_director = memnew(GaussianSplatSceneDirector); }
    if (!make_paged_world(harness.world, 20000)) { FAIL("paged fixture failed"); return; }
    harness.node = memnew(GaussianSplatWorld3D);
    harness.node->set_auto_apply_on_ready(false);
    harness.node->set_world(harness.world);
    tree->get_root()->add_child(harness.node);
    tree->process(0.0);
    harness.node->apply_world();
    Ref<GaussianSplatRenderer> renderer = harness.node->get_renderer();
    if (renderer.is_null()) { FAIL("World3D renderer missing"); return; }
    auto &state = renderer->get_resource_state();
    if (state.hlod_snapshot.is_null() || !state.hlod_snapshot->is_paged() || state.hlod_snapshot->nodes.size() < 2) {
        FAIL("current multi-node paged snapshot required"); return;
    }
    // The real writer/reader supplied a complete root cut that fits, while the
    // leaves do not fit the modeled sorter. No package counts are fabricated.
    CHECK(state.hlod_snapshot->is_current());
    CHECK(state.hlod_snapshot->leaf_count > BudgetLimitedSortPipeline::LIMIT);
    CHECK(state.hlod_snapshot->nodes[0].topology.payload_count > 0);
    CHECK(state.hlod_snapshot->nodes[0].topology.payload_count <= BudgetLimitedSortPipeline::LIMIT);
    renderer->get_performance_settings().max_splats = BudgetLimitedSortPipeline::LIMIT;
    // Existing drawn budget: modules/gaussian_splatting/renderer/hlod_world_instances.h:218,224,
    // world_need_splats/relative_capacity bound world + admitted rows by max_splats
    // and get_hlod_view_splat_cap(); total leaf count is not a sort requirement.
    const uint32_t budget = gs_hlod_bundle::relative_capacity(state.hlod_snapshot->leaf_count,
            renderer->get_performance_settings().max_splats, renderer->get_hlod_view_splat_cap(), {});
    CHECK(budget == BudgetLimitedSortPipeline::LIMIT);
    Ref<BudgetLimitedSortPipeline> pipeline;
    pipeline.instantiate();
    Ref<GPUSortingPipeline> actual = renderer->get_subsystem_state().sorting_pipeline;
    if (actual.is_null()) { FAIL("renderer sorter missing"); return; }
    actual->set_forced_sort_algorithm(GPUSorterFactory::ALGORITHM_RADIX);
    actual->mark_sorter_dirty();
    pipeline->actual = actual;
    if (actual->initialize(rd, budget) != OK || !pipeline->get_buffer_handles().valid ||
            pipeline->get_buffer_handles().capacity != budget) {
        FAIL("bounded GPU sorter initialization failed"); return;
    }
    // The orchestrator caches the original pipeline pointer. The allocation
    // facade forwards to that SAME owner, so publication and execution share
    // resources, and teardown restores the original subsystem reference.
    struct RestorePipeline {
        GaussianSplatRenderer *renderer;
        Ref<GPUSortingPipeline> original;
        ~RestorePipeline() {
            renderer->release_hlod_resources();
            renderer->get_subsystem_state().sorting_pipeline = original;
        }
    } restore_pipeline{ renderer.ptr(), actual };
    renderer->get_subsystem_state().sorting_pipeline = pipeline;
    // Publication needs live tile/compositor ledgers, but no present target or
    // asynchronous residency. Provision the same resources as the paged route.
    const Size2i size(64, 64);
    Ref<TileRenderer> tile = renderer->get_tile_renderer();
    Ref<OutputCompositor> compositor = renderer->get_subsystem_state().output_compositor;
    RD::TextureFormat format;
    format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
    format.texture_type = RD::TEXTURE_TYPE_2D;
    format.width = size.x;
    format.height = size.y;
    format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
    if (tile.is_null() || compositor.is_null() ||
            tile->provision_hlod_route(size, RD::DATA_FORMAT_MAX, 4096) != OK ||
            !compositor->ensure_hlod_scratch(rd, format, size)) {
        FAIL("paged route ledger provisioning failed"); return;
    }
    String reason;
    const int published = renderer->try_publish_hlod_resident_contract(reason);
    CHECK(state.hlod_reason != String("hlod_paged_sort_allocation_failed"));
    CHECK(pipeline->largest_request <= budget);
    CHECK(pipeline->get_buffer_handles().capacity == budget);
    if (published != 1) { FAIL("bounded paged publication refused: ", state.hlod_reason.utf8().get_data()); return; }
    CHECK(state.hlod_streaming);
    CHECK(state.hlod_relative_capacity == budget);
    CHECK(state.hlod_canonical.owned_count == 0);
    CHECK(renderer->is_hlod_prepared_contract_current());
    const uint64_t builds = state.hlod_bundle_builds;
    CHECK(renderer->try_publish_hlod_resident_contract(reason) == 1);
    CHECK(renderer->is_hlod_prepared_contract_current());
    CHECK(state.hlod_bundle_builds == builds); // The prepared check must use the same bounded capacity.
    CHECK(pipeline->largest_request <= budget);
    if (p_binding_limit) {
        // Codex F7: the device's storage-buffer binding range bounds the streamed atlas. First the real query: the ledger
        // holds what the device reports (Vulkan maxStorageBufferRange), and that is not the 1 GiB every driver returns
        // for a limit it does not know, so the driver really answers it.
        const uint64_t reported = rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFER_RANGE);
        CHECK(reported != (uint64_t(1) << 30));
        CHECK(state.device_vram_ledger.max_buffer_bytes() == DeviceVramLedger::binding_limit_for_device(reported));
        // Then a range injected one byte below the atlas the unconstrained publication built: the rebuilt atlas must fit
        // it, and the published bound must name it.
        const uint64_t stride = state.hlod_relative.quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
        const uint64_t built_atlas = uint64_t(state.hlod_relative.buffers.atlas_gaussian_count) * stride;
        if (built_atlas <= 4096 * stride) { FAIL("the unconstrained atlas is too small to shrink: ", built_atlas); return; }
        const uint64_t range = built_atlas - 1;
        struct ClearRange {
            GaussianSplatRenderer *renderer;
            ~ClearRange() { renderer->test_set_storage_buffer_range_override(0); }
        } clear_range{ renderer.ptr() };
        renderer->test_set_storage_buffer_range_override(int64_t(range));
        renderer->release_hlod_resources();
        const int republished = renderer->try_publish_hlod_resident_contract(reason);
        CHECK(state.device_vram_ledger.max_buffer_bytes() == range);
        if (republished != 1) { FAIL("publication under the injected binding range refused: ", String(state.hlod_reason)); return; }
        CHECK(state.hlod_streaming);
        CHECK(uint64_t(state.hlod_relative.buffers.atlas_gaussian_count) * stride <= range);
        CHECK(state.hlod_stream_atlas_limit_bytes <= range);
        CHECK(state.hlod_stream_atlas_limit_bound == uint8_t(DeviceVramLedger::StreamedAtlasBound::BUFFER_LIMIT));
        return;
    }
    if (!p_overflow) { return; }

    // Fault injection AFTER real admission: two resident chunks draw budget + 1
    // splats. Use the admitted atlas, ref/sidecar and sort buffers, and execute the
    // real relative depth -> clamp -> sort path. No streamer requests are pending.
    auto &buffers = state.hlod_relative.buffers;
    const uint32_t demand = budget + 1;
    if (buffers.atlas_gaussian_count < demand || buffers.max_visible_chunks < 2) {
        FAIL("admitted atlas cannot hold the overflow fixture"); return;
    }
    Vector<PackedGaussian> splats;
    splats.resize(demand);
    for (uint32_t i = 0; i < demand; ++i) {
        PackedGaussian g = {};
        g.position[2] = -4.0f;
        g.opacity = 1.0f;
        g.rotation[3] = 1.0f;
        for (uint32_t axis = 0; axis < 3; ++axis) { g.scale[axis] = 0.125f; }
        splats.write[i] = g;
    }
    ChunkMetaGPU chunks[2] = {};
    chunks[0].splat_count = budget;
    chunks[1].atlas_base = budget;
    chunks[1].splat_count = 1;
    const DrawnNodeGPU draws[2] = {
        { 0u, 0u, 1.0f, 0u, { 0.0f, 0.0f, 0.0f, 0.0f } },
        { 0u, 1u, 1.0f, 0u, { 0.0f, 0.0f, 0.0f, 0.0f } }
    };
    const uint32_t counters[3] = { 2, 0, 0 };
    if (rd->buffer_update(buffers.atlas_gaussian_buffer, 0, demand * sizeof(PackedGaussian), splats.ptr()) != OK ||
            rd->buffer_update(buffers.chunk_meta_buffer, 0, sizeof(chunks), chunks) != OK ||
            rd->buffer_update(buffers.visible_chunk_buffer, 0, sizeof(draws), draws) != OK ||
            rd->buffer_update(buffers.counter_buffer, 0, sizeof(counters), counters) != OK) {
        FAIL("overflow draw upload failed"); return;
    }
    buffers.hlod_draw_count = 2;
    buffers.max_chunk_splats = budget;
    if (!renderer->publish_hlod_bundle(true)) { FAIL("overflow relative contract publication failed"); return; }
    renderer->set_frustum_culling(false);
    renderer->set_tiny_splat_screen_radius(0.0f);
    renderer->set_lod_min_screen_size(0.0f);
    renderer->get_view_state().last_camera_projection.set_perspective(60.0, 1.0, 0.1, 100.0);
    renderer->get_view_state().manual_viewport_override = size;
    const uint32_t events = actual->get_instance_count_overflow_events();
    const auto result = renderer->test_sort_for_view(Transform3D(), GaussianRenderState::IndexDomain::CHUNK_REF);
    CHECK(result.did_execute);
    CHECK(renderer->get_debug_state().sort_route_uid == String(RenderRouteUID::INSTANCE_SORT_GPU));
    CHECK(actual->get_max_elements() == budget);
    const Vector<uint8_t> sample = rd->buffer_get_data(buffers.instance_count_buffer);
    if (sample.size() != int(sizeof(GaussianSplatting::IndirectDispatchLayout))) { FAIL("complete count readback required"); return; }
    GaussianSplatting::IndirectDispatchLayout count = {};
    std::memcpy(&count, sample.ptr(), sizeof(count));
    CHECK(count.element_count == budget);
    CHECK(count.unclamped_total == demand);
    CHECK(count.overflow_flag == 1);
    // Feed the actual GPU sample through the production readback consumer. This
    // avoids depending on when an asynchronous callback happens to arrive.
    auto &readback = actual->_test_instance_count_readback_state();
    readback.pending = true;
    ++readback.generation;
    readback.pending_frame_counter = renderer->get_frame_state().frame_counter;
    actual->_on_instance_count_readback(sample, readback.generation);
    CHECK(actual->get_instance_count_overflow_events() == events + 1);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] paged publication sizes its sorter from the drawn budget, not all leaves (#1417)") {
    paged_sort_budget_regression(false);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] an admitted budget-sized paged sorter explicitly counts runtime cut overflow (#1417)") {
    // Old code fails the admission prerequisite, before the overflow exercise.
    // The existing overflow mechanism already counts drops; preserve it as the
    // sort allocation shrinks from whole-world capacity to the drawn budget.
    paged_sort_budget_regression(true);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] the streamed atlas stays within the device's storage-buffer binding range (Codex F7)") {
    paged_sort_budget_regression(false, true);
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

namespace TestHlodRuntimeGpuIdentity {
TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] noncontiguous page placement preserves drawn pixels and corrupted mapping changes them (#1514)") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GpuSorting harness RenderingDevice required"); return; }
    struct Resources {
        RenderingDevice *rd;
        LocalVector<RID> owned;
        explicit Resources(RenderingDevice *p_rd) : rd(p_rd) {}
        ~Resources() { for (uint32_t i = owned.size(); i-- > 0;) { rd->free(owned[i]); } }
        RID retain(RID p_rid) { if (p_rid.is_valid()) { owned.push_back(p_rid); } return p_rid; }
        RID buffer(uint32_t p_bytes, const void *p_data) {
            Vector<uint8_t> bytes;
            if (bytes.resize(p_bytes) != OK) { return RID(); }
            memcpy(bytes.ptrw(), p_data, p_bytes);
            return retain(rd->storage_buffer_create(p_bytes, bytes));
        }
    } resources(rd);
    RD::TextureFormat format;
    format.format = RD::DATA_FORMAT_D32_SFLOAT;
    format.width = format.height = 64;
    format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
    if (!rd->texture_is_format_supported_for_usage(format.format, format.usage_bits)) {
        FAIL("D32 shadow target with readback is required"); return;
    }
    const RID target = resources.retain(rd->texture_create(format, RD::TextureView()));
    Vector<RID> attachments;
    attachments.push_back(target);
    const RID framebuffer = target.is_valid() ? resources.retain(rd->framebuffer_create(attachments)) : RID();
    if (!framebuffer.is_valid()) { FAIL("page-table draw target unavailable"); return; }
    GsShadowCasterShaderRD source;
    Vector<String> variants;
    variants.push_back(String());
    source.initialize(variants);
    const RID version = source.version_create();
    if (!version.is_valid()) { FAIL("production shadow shader source unavailable"); return; }
    const Vector<String> stages = source.version_build_variant_stage_sources(version, 0);
    source.version_free(version);
    if (stages.size() <= RD::SHADER_STAGE_FRAGMENT) { FAIL("production shadow stages unavailable"); return; }
    String shader_error;
    const RID shader = resources.retain(ShaderCompilationHelper::compile_graphics_shader_on_device(rd,
            stages[RD::SHADER_STAGE_VERTEX], stages[RD::SHADER_STAGE_FRAGMENT], "hlod_page_table_pixels", Vector<String>(), &shader_error));
    if (!shader.is_valid()) { FAIL("production shadow compilation failed: ", shader_error.utf8().get_data()); return; }
    RD::PipelineRasterizationState raster;
    raster.cull_mode = RD::POLYGON_CULL_DISABLED;
    RD::PipelineDepthStencilState depth;
    depth.enable_depth_test = depth.enable_depth_write = true;
    depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
    const RID pipeline = resources.retain(rd->render_pipeline_create(shader, rd->framebuffer_get_format(framebuffer), RD::INVALID_ID,
            RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), depth, RD::PipelineColorBlendState::create_disabled(0)));
    if (!pipeline.is_valid()) { FAIL("production shadow pipeline unavailable"); return; }
    constexpr uint32_t page_splats = GaussianSplatHlodRuntimeData::DEFAULT_PAGE_SPLATS;
    constexpr uint32_t page_capacity = 4;
    constexpr uint32_t node_splats = page_splats + 1;
    const uint32_t slot_width = (gs_hlod::kMaxNodeSplats + page_splats - 1u) / page_splats;
    Vector<uint8_t> pixels[8];
    Vector<uint32_t> placements[2];
    // 0..4: existing page-table and current wind-time controls. 5: P3 current rows,
    // 6: force the old rows-after-shadow order, 7: world must retain its old draw offset.
    const float previous_translation_x = -0.5f;
    const float current_translation_x = 0.5f;
    const float previous_camera_x = 0.25f;
    for (uint32_t pass = 0; pass < 8; ++pass) {
        gs_hlod_runtime::HlodPagePool pool;
        if (!pool.reset(page_capacity, slot_width)) { FAIL("page-pool fixture allocation failed"); return; }
        if (pass != 0) { pool.test_reverse_free_pages(); }
        Vector<uint32_t> pages;
        uint32_t pt_base = 0;
        if (!pool.allocate(0, 2, pages, pt_base) || pages.size() != 2) { FAIL("two-page node placement unavailable"); return; }
        if (pass < 2) { placements[pass] = pages; }
        Vector<PackedGaussian> atlas;
        if (atlas.resize(page_capacity * page_splats) != OK) { FAIL("page-pool atlas allocation failed"); return; }
        atlas.fill(PackedGaussian{});
        // Only the first record of each logical page is opaque; losing either page changes the pixels.
        for (uint32_t local_page = 0; local_page < 2; ++local_page) {
            PackedGaussian &g = atlas.write[pages[local_page] * page_splats];
            g.position[0] = pass >= 5 ? 0.0f : (local_page == 0 ? -0.4f : 0.4f);
            g.position[2] = 0.5f;
            g.opacity = pass >= 5 && local_page == 1 ? 0.0f : 1.0f;
            g.rotation[3] = 1.0f;
            for (uint32_t axis = 0; axis < 3; ++axis) { g.scale[axis] = 0.1f; }
        }
        ChunkMetaGPU meta = {};
        meta.atlas_base = pages[0] * page_splats;
        meta.splat_count = node_splats;
        meta.page_table_ref = pt_base + 1u;
        CHECK(meta.page_table_ref != 0);
        for (uint32_t k = 0; k < 2; ++k) { CHECK(pool.page_table()[meta.page_table_ref - 1u + k] == pages[k]); }
        Vector<uint32_t> forward = pool.page_table();
        if (pass == 2) { forward.write[pt_base] = gs_hlod_runtime::HlodPagePool::FREE; }
        InstanceDataGPU instance = {};
        instance.rotation[3] = instance.inv_rotation[3] = instance.translation_scale[3] = 1.0f;
        instance.params[0] = 1.0f;
        instance.params[2] = pass == 3 || pass == 4 ? 1.0f : 0.0f;
        instance.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
        InstanceDataGPU rows[2] = { instance, instance };
        if (pass >= 5) {
            rows[1].translation_scale[0] = pass == 6 ? previous_translation_x : current_translation_x;
            rows[1].ids[1] &= ~GS_INSTANCE_FLAG_TRANSLATION_ZERO;
            // World payload centres live in drawn.camera_offset, never this row translation.
            rows[0].translation_scale[0] = 0.9f;
        }
        DrawnNodeGPU drawn = { 0u, 0u, 1.0f, 0u, { 0.0f, 0.0f, 0.0f, 0.0f } };
        if (pass >= 5) {
            drawn.instance_id = pass == 7 ? 0u : 1u;
            drawn.camera_offset[0] = previous_translation_x - previous_camera_x;
        }
        const uint32_t casts[2] = { 1u, 1u };
        GsSplatShadowCaster::DeformationUniform deformation;
        deformation.wind_dir_strength[0] = 1.0f;
        deformation.wind_dir_strength[3] = 0.5f;
        // Opposite near-peak phases move the footprint several pixels at this resolution.
        deformation.wind_time_config[0] = pass == 4 ? 4.71238898038f : (pass == 3 ? 1.57079632679f : 0.0f);
        deformation.wind_time_config[1] = 1.0f;
        deformation.wind_time_config[2] = 0.1f;
        deformation.wind_time_config[3] = pass == 3 || pass == 4 ? 1.0f : 0.0f;
        if (pass >= 5) { deformation.frame_anchor[0] = previous_camera_x; }
        const RegionOverrideGPU neutral_region = GSRegionOverrideBuffer::neutral();
        const RID inputs[8] = {
            resources.buffer(uint32_t(atlas.size()) * sizeof(PackedGaussian), atlas.ptr()),
            resources.buffer(sizeof(rows), rows), resources.buffer(sizeof(meta), &meta),
            resources.buffer(sizeof(drawn), &drawn), resources.buffer(sizeof(casts), &casts),
            resources.buffer(uint32_t(forward.size()) * sizeof(uint32_t), forward.ptr()),
            resources.buffer(sizeof(deformation), &deformation),
            resources.buffer(sizeof(RegionOverrideGPU), &neutral_region)
        };
        Vector<RD::Uniform> uniforms;
        for (uint32_t binding = 0; binding < 8; ++binding) {
            if (!inputs[binding].is_valid()) { FAIL("page-table draw input unavailable"); return; }
            RD::Uniform u;
            u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
            u.binding = binding;
            u.append_id(inputs[binding]);
            uniforms.push_back(u);
        }
        const RID set = resources.retain(rd->uniform_set_create(uniforms, shader, 0));
        if (!set.is_valid()) { FAIL("page-table draw uniform set unavailable"); return; }
        // atlas[0] = 0: the shader bounds by the array length (BV-17 fallback; this atlas is far below 2^31 bytes).
        struct Push { float matrix[16]; float viewport[4]; uint32_t counts[4]; uint32_t atlas[4]; } push{};
        static_assert(sizeof(Push) == 112, "shadow page-table test push layout");
        for (uint32_t axis = 0; axis < 4; ++axis) { push.matrix[axis * 5] = 1.0f; }
        // Fold the old record camera into the light transform, as the real renderer does.
        if (pass >= 5) { push.matrix[12] = previous_camera_x; }
        push.viewport[0] = push.viewport[1] = 64.0f;
        push.viewport[2] = gs_shadow_caster::kAlphaThreshold;
        push.viewport[3] = gs_shadow_caster::kLowPassTexels2;
        push.counts[0] = 1;
        push.counts[1] = node_splats;
        const float opacity = 1.0f;
        memcpy(&push.counts[3], &opacity, sizeof(opacity));
        const RD::DrawListID list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_DEPTH, VectorView<Color>(), 0.0f);
        if (list == RD::INVALID_ID) { FAIL("page-table draw list unavailable"); return; }
        rd->draw_list_bind_render_pipeline(list, pipeline);
        rd->draw_list_bind_uniform_set(list, set, 0);
        rd->draw_list_set_push_constant(list, &push, sizeof(push));
        rd->draw_list_draw(list, false, node_splats, 6);
        rd->draw_list_end();
        pixels[pass] = rd->texture_get_data(target, 0);
        if (pixels[pass].size() != 64 * 64 * int(sizeof(float))) { FAIL("complete drawn depth pixels required"); return; }
    }
    if (placements[0].size() != 2 || placements[1].size() != 2) { FAIL("both drawn page placements required"); return; }
    CHECK(placements[0][1] == placements[0][0] + 1u);
    CHECK(placements[1][1] != placements[1][0] + 1u);
    uint32_t covered = 0;
    for (uint32_t i = 0; i < 64 * 64; ++i) {
        float z = 0;
        memcpy(&z, pixels[0].ptr() + i * sizeof(float), sizeof(z));
        covered += z > 0.0f ? 1u : 0u;
    }
    CHECK(covered > 0);
    CHECK(memcmp(pixels[0].ptr(), pixels[1].ptr(), size_t(pixels[0].size())) == 0);
    // The same resident page-table draw/list uses this frame's time in the
    // production caster. A missing deformation call or a cached time makes
    // both images equal. Wind-off page relocation above remains byte identical.
    CHECK(memcmp(pixels[3].ptr(), pixels[4].ptr(), size_t(pixels[3].size())) != 0);
    CHECK(memcmp(pixels[0].ptr(), pixels[3].ptr(), size_t(pixels[0].size())) != 0);
    CHECK(memcmp(pixels[1].ptr(), pixels[2].ptr(), size_t(pixels[1].size())) != 0);
    double centroids[3] = {};
    for (uint32_t sample = 0; sample < 3; ++sample) {
        uint32_t count = 0;
        double sum_x = 0.0;
        for (uint32_t i = 0; i < 64 * 64; ++i) {
            float z = 0.0f;
            memcpy(&z, pixels[5 + sample].ptr() + i * sizeof(float), sizeof(z));
            if (z > 0.0f) { sum_x += double(i % 64) + 0.5; ++count; }
        }
        if (count == 0) { FAIL("P3 moving/world caster must produce measured depth texels, sample ", sample); return; }
        centroids[sample] = sum_x / double(count);
    }
    // Local centre is zero and wind is disabled: project the current colour row's
    // world translation into this orthographic 64-texel target. This checks the
    // production caster raster against the colour row position, not the colour raster.
    const double colour_current_x = (double(current_translation_x) + 1.0) * 32.0;
    const double previous_world_x = (double(previous_translation_x) + 1.0) * 32.0;
    CHECK(std::abs(centroids[0] - colour_current_x) <= 1.0); // assertion RED pre-P3 (32 texels stale).
    CHECK(std::abs(centroids[1] - colour_current_x) > 1.0); // same assertion would fail in the forced old order.
    CHECK(std::abs(centroids[2] - previous_world_x) <= 1.0); // row 0 keeps the previous list's world centre.
}
}

namespace TestA2History {
struct Resources {
    RenderingDevice *rd;
    LocalVector<RID> owned;
    ~Resources() { for (uint32_t i = owned.size(); i-- > 0;) { rd->free(owned[i]); } }
    RID keep(RID rid) { if (rid.is_valid()) { owned.push_back(rid); } return rid; }
    RID buffer(uint32_t bytes) { return keep(rd->storage_buffer_create(bytes)); }
    RID set(RID shader, RID a, RID b = RID()) {
        Vector<RD::Uniform> uniforms;
        for (uint32_t i = 0; i < (b.is_valid() ? 2u : 1u); ++i) {
            RD::Uniform u; u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER; u.binding = i;
            u.append_id(i == 0 ? a : b); uniforms.push_back(u);
        }
        return keep(rd->uniform_set_create(uniforms, shader, 0));
    }
};

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] A2 frozen colliding identities keep identical history under opposite EMIT orders") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GpuSorting RD required for A2 history collision readback"); return; }
    Resources resources{ rd, {} };
    TileBinningShaderRD production;
    Vector<String> variants; variants.push_back(String()); production.initialize(variants);
    const RID version = production.version_create();
    if (!version.is_valid()) { FAIL("A2 production binning source unavailable"); return; }
    const Vector<String> stages = production.version_build_variant_stage_sources(version, 0);
    production.version_free(version);
    if (stages.size() <= RD::SHADER_STAGE_COMPUTE) { FAIL("A2 production compute stage missing"); return; }
    const String body = stages[RD::SHADER_STAGE_COMPUTE];
    int first = body.find("    uint history_idx =");
    if (first < 0) { FAIL("A2 history expression missing from real binning shader"); return; }
    const String conditional = "#if defined(GS_HLOD_RELATIVE_PAYLOAD)\n";
    const int preceding = body.rfind(conditional, first);
    if (preceding >= 0 && body.substr(preceding + conditional.length(), first - preceding - conditional.length()).strip_edges().is_empty()) {
        first = preceding;
    }
    const int last = body.find("    uint history_packed =", first);
    if (last <= first) { FAIL("A2 history expression end missing"); return; }
    // Execute the actual production slot/key expressions. One invocation at a
    // time forces both legal last-writer orders, so red does not depend on GPU scheduling.
    const String source = String("#version 450\n#define GS_HLOD_RELATIVE_PAYLOAD 1\n") +
            "layout(local_size_x=1) in;\nlayout(set=0,binding=0,std430) buffer H { uint entries[]; } history;\n" +
            "layout(push_constant,std430) uniform P { uvec4 v; } p;\nvoid main() {\n" +
            "uint gaussian_idx=p.v.x; uint identity_hash=p.v.y; uint history_size=17u;\n" +
            body.substr(first, last - first) +
            "atomicExchange(history.entries[history_idx], (history_key << 1u) | 1u); }\n";
    String error;
    const RID shader = resources.keep(ShaderCompilationHelper::compile_shader_on_device(rd, source, "a2_history_collision", Vector<String>(), &error));
    if (!shader.is_valid()) { FAIL("A2 collision shader failed: ", error.utf8().get_data()); return; }
    const RID pipeline = resources.keep(rd->compute_pipeline_create(shader));
    const RID history = resources.buffer(17 * sizeof(uint32_t));
    const RID set = resources.set(shader, history);
    if (!pipeline.is_valid() || !history.is_valid() || !set.is_valid()) { FAIL("A2 collision resources missing"); return; }
    Vector<uint8_t> frames[2];
    for (uint32_t pass = 0; pass < 2; ++pass) {
        if (rd->buffer_clear(history, 0, 17 * sizeof(uint32_t)) != OK) { FAIL("A2 history reset failed"); return; }
        for (uint32_t step = 0; step < 2; ++step) {
            const bool second = (step ^ pass) != 0;
            // 1 and 18 collide under identity_hash % 17, despite distinct atlas records.
            const uint32_t push[4] = { second ? 6u : 2u, second ? 18u : 1u, 0u, 0u };
            const RD::ComputeListID list = rd->compute_list_begin();
            rd->compute_list_bind_compute_pipeline(list, pipeline);
            rd->compute_list_bind_uniform_set(list, set, 0);
            rd->compute_list_set_push_constant(list, push, sizeof(push));
            rd->compute_list_dispatch(list, 1, 1, 1);
            rd->compute_list_end();
        }
        frames[pass] = rd->buffer_get_data(history);
        if (frames[pass].size() != 17 * int(sizeof(uint32_t))) { FAIL("A2 full history readback required"); return; }
    }
    CHECK(frames[0] == frames[1]);
    uint32_t a = 0, b = 0;
    memcpy(&a, frames[0].ptr() + 2 * sizeof(uint32_t), sizeof(a));
    memcpy(&b, frames[0].ptr() + 6 * sizeof(uint32_t), sizeof(b));
    CHECK(a == 3u); CHECK(b == 37u);
}

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] A2 reassigned range is cleared before same-frame history READ and EMIT") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GpuSorting RD required for A2 reassignment ordering readback"); return; }
    Ref<TileRenderer> tile; tile.instantiate();
    if (tile->initialize(rd, Vector2i(16, 16)) != OK) { FAIL("A2 tile renderer initialization failed"); return; }
    const RID history = tile->_test_subpixel_history(17);
    if (!history.is_valid()) { FAIL("A2 real history allocation failed"); return; }
    const Vector<uint8_t> initial = rd->buffer_get_data(history);
    if (initial.size() != 17 * int(sizeof(uint32_t))) { FAIL("A2 initial history readback missing"); return; }
    for (int i = 0; i < initial.size(); ++i) { CHECK(initial[i] == 0); }
    Resources resources{ rd, {} };
    const String source = "#version 450\nlayout(local_size_x=1) in;\n"
            "layout(set=0,binding=0,std430) buffer H { uint entries[]; } h;\n"
            "layout(set=0,binding=1,std430) buffer O { uint entries[]; } o;\n"
            "void main() { uint i=gl_GlobalInvocationID.x; uint packed=h.entries[i];\n"
            "o.entries[i]=uint((packed & 1u)!=0u && (packed >> 1u)==0u);\n"
            "atomicExchange(h.entries[i], 2u); }\n";
    String error;
    const RID shader = resources.keep(ShaderCompilationHelper::compile_shader_on_device(rd, source, "a2_history_clear_order", Vector<String>(), &error));
    if (!shader.is_valid()) { FAIL("A2 ordering shader failed: ", error.utf8().get_data()); return; }
    const RID pipeline = resources.keep(rd->compute_pipeline_create(shader));
    const RID observed = resources.buffer(17 * sizeof(uint32_t));
    const RID set = resources.set(shader, history, observed);
    if (!pipeline.is_valid() || !observed.is_valid() || !set.is_valid()) { FAIL("A2 ordering resources missing"); return; }
    for (uint32_t pass = 0; pass < 2; ++pass) {
        uint32_t old[17]; for (auto &v : old) { v = 1u; }
        if (rd->buffer_update(history, 0, sizeof(old), old) != OK) { FAIL("A2 old tenant setup failed"); return; }
        // Pass 0 is the forcing control: omit the actual range hook, exposing
        // inherited was_visible even when the 31-bit tenant key collides.
        if (pass == 1) { tile->invalidate_sh_color_cache_range(3, 2); }
        // No readback/sync/submission boundary between clear and first READ/EMIT.
        const RD::ComputeListID list = rd->compute_list_begin();
        rd->compute_list_bind_compute_pipeline(list, pipeline);
        rd->compute_list_bind_uniform_set(list, set, 0);
        rd->compute_list_dispatch(list, 17, 1, 1);
        rd->compute_list_end();
        const Vector<uint8_t> bytes = rd->buffer_get_data(observed);
        if (bytes.size() != int(sizeof(old))) { FAIL("A2 full ordered readback required"); return; }
        for (uint32_t i = 0; i < 17; ++i) {
            uint32_t visible = 0; memcpy(&visible, bytes.ptr() + i * sizeof(uint32_t), sizeof(visible));
            CHECK(visible == ((pass == 1 && i >= 3 && i < 5) ? 0u : 1u));
        }
    }
}
} // namespace TestA2History

namespace TestHlodDistanceCull {
// D1 (real-scan check, 2026-11-23): the stochastic distance cull hashed the ATLAS ADDRESS, so the
// same HLOD cut drew a different subset of distant splats per process and per reload (fresh
// processes differed in 7.5 % of pixels at a far pose). On the HLOD route the same splats at a
// different atlas placement must survive identically. This executes the production binning
// statements from `float world_dist` up to `float max_sigma` for 256 splats at two placements,
// 45 m away with start 30 m and max rate 1 (cull probability 0.25).
TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] distance cull outcome does not depend on atlas placement on the HLOD route") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("RenderingDevice required to execute the production distance-cull statements"); return; }
    TestA2History::Resources resources{ rd, {} };
    TileBinningShaderRD production;
    Vector<String> variants; variants.push_back(String()); production.initialize(variants);
    const RID version = production.version_create();
    if (!version.is_valid()) { FAIL("production binning source unavailable"); return; }
    const Vector<String> stages = production.version_build_variant_stage_sources(version, 0);
    production.version_free(version);
    if (stages.size() <= RD::SHADER_STAGE_COMPUTE) { FAIL("production compute stage missing"); return; }
    const String body = stages[RD::SHADER_STAGE_COMPUTE];
    const int helpers = body.find("uint gs_hash_u32(uint v) {");
    const int helpers_end = body.find("bool gs_should_hotspot_prune_overlap(", helpers);
    if (helpers < 0 || helpers_end <= helpers) { FAIL("production distance-cull helpers missing"); return; }
    int first = body.find("    float world_dist = length(frame_camera - g.position);");
    if (first < 0) { FAIL("production distance-cull statement missing"); return; }
    const String guard = "#if !defined(GS_HLOD_RELATIVE_PAYLOAD)\n";
    const int preceding = body.rfind(guard, first);
    if (preceding >= 0 && body.substr(preceding + guard.length(), first - preceding - guard.length()).strip_edges().is_empty()) {
        first = preceding;
    }
    const int last = body.find("    float max_sigma = MAX_SIGMA;", first);
    if (last <= first) { FAIL("production distance-cull block end missing"); return; }
    // [route][placement]. Route 0 is the canonical route: it still keys the cull on the address
    // (follow-up), which doubles as the forcing control that this harness can see placement.
    Vector<uint8_t> survived[2][2];
    for (int hlod = 0; hlod < 2; ++hlod) {
        const String source = String("#version 450\n") + (hlod ? "#define GS_HLOD_RELATIVE_PAYLOAD 1\n" : "") +
                "#define GS_DEBUG_INCREMENT(counter)\n#define GS_TILE_GLOBAL_SORT_COUNT_PASS 1\n" +
                "layout(local_size_x=1) in;\n" +
                "layout(set=0,binding=0,std430) buffer O { uint entries[]; } survived;\n" +
                "layout(push_constant,std430) uniform P { vec4 distance_cull_config; uvec4 v; } params;\n" +
                body.substr(helpers, helpers_end - helpers) +
                "struct GsTestSplat { vec3 position; };\nstruct GsTestRef { uint instance_id; };\n" +
                "void main() {\nuint i = gl_GlobalInvocationID.x; uint gaussian_idx = params.v.x + i;\n" +
                "GsTestRef splat_ref = GsTestRef(0u); GsTestSplat g = GsTestSplat(vec3(0.0, 0.0, 45.0));\n" +
                "vec3 frame_camera = vec3(0.0);\n" +
                body.substr(first, last - first) +
                "survived.entries[i] = 1u; }\n";
        String error;
        const RID shader = resources.keep(ShaderCompilationHelper::compile_shader_on_device(rd, source, "hlod_distance_cull_placement", Vector<String>(), &error));
        if (!shader.is_valid()) { FAIL("distance-cull shader failed: ", error.utf8().get_data()); return; }
        const RID pipeline = resources.keep(rd->compute_pipeline_create(shader));
        const RID out = resources.buffer(256 * sizeof(uint32_t));
        const RID set = resources.set(shader, out);
        if (!pipeline.is_valid() || !out.is_valid() || !set.is_valid()) { FAIL("distance-cull resources missing"); return; }
        for (int placement = 0; placement < 2; ++placement) {
            if (rd->buffer_clear(out, 0, 256 * sizeof(uint32_t)) != OK) { FAIL("survivor reset failed"); return; }
            // x = start 30 m, y = max rate 1, z = enabled. Same splats, atlas base 1024 or 70001.
            struct Push { float config[4]; uint32_t v[4]; } push{ { 30.0f, 1.0f, 1.0f, 0.0f }, { placement ? 70001u : 1024u, 0u, 0u, 0u } };
            static_assert(sizeof(Push) == 32, "distance-cull test push layout");
            const RD::ComputeListID list = rd->compute_list_begin();
            rd->compute_list_bind_compute_pipeline(list, pipeline);
            rd->compute_list_bind_uniform_set(list, set, 0);
            rd->compute_list_set_push_constant(list, &push, sizeof(push));
            rd->compute_list_dispatch(list, 256, 1, 1);
            rd->compute_list_end();
            survived[hlod][placement] = rd->buffer_get_data(out);
            if (survived[hlod][placement].size() != 256 * int(sizeof(uint32_t))) { FAIL("full survivor readback required"); return; }
        }
    }
    CHECK(survived[0][0] != survived[0][1]); // control: the address-keyed canonical cull is visible to this harness
    CHECK(survived[1][0] == survived[1][1]); // HLOD: atlas placement must not change which splats are drawn
    int hlod_survivors = 0;
    for (int i = 0; i < 256; ++i) {
        uint32_t v = 0;
        memcpy(&v, survived[1][0].ptr() + i * sizeof(uint32_t), sizeof(v));
        hlod_survivors += int(v);
    }
    CHECK(hlod_survivors == 256); // the HLOD route never applies the stochastic distance cull
}
} // namespace TestHlodDistanceCull

namespace TestHlodLevelTint {
// The HLOD level debug view must not change the default frame. Its only input is one
// RenderParams word that used to be std140 padding (always 0); the binning shader reads
// it in a uniform branch. So: off by default everywhere, and the UBO of a frame with the
// view off is byte-identical to the default UBO, before and after the view was on.
TEST_CASE("[GaussianSplatting][HLOD] level tint debug view is off by default at every layer") {
    CHECK_FALSE(TileRenderer::RenderParams().debug_show_hlod_level_tint);
    CHECK_FALSE(DebugOverlayOptions().show_hlod_level_tint);
    CHECK(offsetof(TileRenderParamsGPU, debug_hlod_level_tint) == size_t(348));
}

// [Sort][RequiresGPU] places the GPU cases in the required GpuSorting batch, as the A2 cases above.
TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] level tint off leaves the RenderParams UBO byte-identical to the default") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("RenderingDevice required to pack production RenderParams"); return; }
    Ref<TileRenderer> tile; tile.instantiate();
    if (tile->initialize(rd, Vector2i(64, 64)) != OK) { FAIL("tile renderer initialization failed"); return; }
    TileRenderer::RenderParams params;
    params.viewport_size = Vector2i(64, 64);
    params.projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
    params.render_projection = params.projection;
    const TileRenderParamsGPU baseline = tile->_test_build_params(params, rd);
    // Determinism control: without it, the comparisons below could not see a difference.
    const TileRenderParamsGPU repeat = tile->_test_build_params(params, rd);
    if (memcmp(&baseline, &repeat, sizeof(TileRenderParamsGPU)) != 0) { FAIL("RenderParams packing is not repeatable; the identity check below would be meaningless"); return; }
    CHECK(baseline.debug_hlod_level_tint == 0u);

    params.debug_show_hlod_level_tint = true;
    const TileRenderParamsGPU tinted = tile->_test_build_params(params, rd);
    CHECK(tinted.debug_hlod_level_tint == 1u); // the view is observable in the UBO
    constexpr size_t word = offsetof(TileRenderParamsGPU, debug_hlod_level_tint);
    const uint8_t *a = reinterpret_cast<const uint8_t *>(&baseline);
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&tinted);
    CHECK(memcmp(a, b, word) == 0); // and it changes nothing else
    CHECK(memcmp(a + word + sizeof(uint32_t), b + word + sizeof(uint32_t), sizeof(TileRenderParamsGPU) - word - sizeof(uint32_t)) == 0);

    params.debug_show_hlod_level_tint = false;
    const TileRenderParamsGPU after = tile->_test_build_params(params, rd);
    CHECK(memcmp(&baseline, &after, sizeof(TileRenderParamsGPU)) == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] production binning shader gates the level tint on the RenderParams word") {
    TileBinningShaderRD production;
    Vector<String> variants; variants.push_back(String()); production.initialize(variants);
    const RID version = production.version_create();
    if (!version.is_valid()) { FAIL("production binning source unavailable"); return; }
    const Vector<String> stages = production.version_build_variant_stage_sources(version, 0);
    production.version_free(version);
    if (stages.size() <= RD::SHADER_STAGE_COMPUTE) { FAIL("production compute stage missing"); return; }
    const String body = stages[RD::SHADER_STAGE_COMPUTE];
    const int gate = body.find("if (params.debug_hlod_level_tint != 0u) {");
    if (gate < 0) { FAIL("level tint gate missing from the real binning shader"); return; }
    CHECK(body.find("sh_color = gs_hlod_level_tint_color(sh_color, selected.level);", gate) > gate);
    // The tint sits in the HLOD-relative block, after COUNT returned: colour only.
    const int block = body.rfind("#if defined(GS_HLOD_RELATIVE_PAYLOAD)", gate);
    CHECK(block >= 0);
    CHECK(body.rfind("#endif", gate) < block);
    CHECK(body.find("vec3 gs_hlod_level_tint_color(vec3 p_color, uint p_level)") >= 0);
}
} // namespace TestHlodLevelTint

namespace TestHlodFadeWind {
// Thread 4673737518: the S6 record fade is an opacity factor of the drawn record, never an input of the positional
// deformation. The legacy wind weight is 0.2 + 0.8 x (1 - opacity); depth (sort) and the shadow caster deform with the
// stored opacity, so a colour pass that deforms with the faded opacity draws a fading splat where it was not sorted.
// Executes the production statements of both passes, from the HLOD record read to the displaced position, with the
// real deformation chain (gs_deformation.glsl as inlined in each production stage). Only buffer-bound helpers are
// stubbed: region table neutral, alpha clip 0, camera-relative position = rest + per-draw offset.
static String production_compute_body(ShaderRD &p_source) {
    Vector<String> variants;
    variants.push_back(String());
    p_source.initialize(variants);
    const RID version = p_source.version_create();
    if (!version.is_valid()) { return String(); }
    const Vector<String> stages = p_source.version_build_variant_stage_sources(version, 0);
    p_source.version_free(version);
    return stages.size() > RD::SHADER_STAGE_COMPUTE ? stages[RD::SHADER_STAGE_COMPUTE] : String();
}
// Text from the first `p_from` through the first `p_to` after it; empty when either is missing.
static String span_of(const String &p_body, const String &p_from, const String &p_to) {
    const int from = p_body.find(p_from);
    const int to = from < 0 ? -1 : p_body.find(p_to, from);
    return to < 0 ? String() : p_body.substr(from, to + p_to.length() - from);
}
static String harness(const String &p_body, const String &p_locals, const String &p_span, const String &p_store) {
    String defines;
    const int main_at = p_body.find("void main()");
    const Vector<String> lines = p_body.substr(0, main_at < 0 ? p_body.length() : main_at).split("\n");
    for (const String &line : lines) {
        const String s = line.strip_edges();
        if (s.begins_with("#define GS_SEMANTIC_") || s.begins_with("#define GS_INSTANCE_WIND_MODE_") ||
                s.begins_with("#define GS_DRAWN_NODE_LEVEL_NONE") || s.begins_with("const uint GS_REGION_") ||
                s.begins_with("const uint GS_INSTANCE_FLAG_")) {
            defines += s + "\n";
        }
    }
    const int chain_from = p_body.find("uint gs_wind_hash_u32(uint v) {");
    const int apply_at = chain_from < 0 ? -1 : p_body.find("GSDeformationResult gs_apply_wind_deformation(", chain_from);
    const int chain_end = apply_at < 0 ? -1 : p_body.find("\n}\n", apply_at);
    if (chain_end < 0 || p_span.is_empty()) { return String(); }
    return String("#version 450\n#define GS_HLOD_RELATIVE_PAYLOAD 1\n#define GS_MAX_SPHERE_EFFECTORS 4\n") + defines +
            "layout(local_size_x = 1) in;\n"
            "layout(set = 0, binding = 0, std430) buffer GsTestOut { vec4 v[]; } gs_test_out;\n"
            "struct RegionOverrideGPU { uint flags; vec4 emissive_opacity; };\n"
            "struct GsTestSplat { float opacity; uint semantic; };\n"
            "struct GsTestDrawn { uint instance_id; float fade; uint level; vec4 camera_offset; };\n"
            "struct GsTestInstance { uvec4 ids; vec4 translation_scale; vec4 rotation; vec4 params; vec4 wind_params; vec4 effect_params; };\n"
            "struct GsTestParams { vec4 camera_position; vec4 camera_position_ortho; vec4 wind_dir_strength; vec4 wind_time_config;\n"
            "    vec4 effector_meta; vec4 effector_spheres[4]; vec4 effector_configs[4]; vec4 effector_opacity_configs[4]; };\n"
            "RegionOverrideGPU gs_region_resolve(GsTestInstance p_i, uint p_s, uint p_a) { return RegionOverrideGPU(0u, vec4(0.0)); }\n"
            "vec4 gs_region_emissive_opacity(vec4 p_e) { return vec4(0.0, 0.0, 0.0, 1.0); }\n"
            "uint gs_region_wind_semantic(uint p_s, uint p_f) { return p_s; }\n"
            "float gs_get_alpha_clip() { return 0.0; }\n"
            "vec3 gs_hlod_camera_relative_position(vec3 p_p, uint p_f, vec4 p_r, float p_s, vec3 p_o) { return p_p + p_o; }\n" +
            p_body.substr(chain_from, chain_end + 3 - chain_from) +
            "void main() {\n"
            "    gs_test_out.v[0] = vec4(-1.0e30);\n"
            "    GsTestInstance instance;\n"
            "    instance.ids = uvec4(0u); instance.translation_scale = vec4(0.0, 0.0, 0.0, 1.0); instance.rotation = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "    instance.params = vec4(1.0, 1.0, 1.0, float(GS_INSTANCE_WIND_MODE_INHERIT));\n"
            "    instance.wind_params = vec4(0.0); instance.effect_params = vec4(0.0);\n"
            "    GsTestParams params;\n"
            "    params.camera_position = vec4(0.0); params.camera_position_ortho = vec4(0.0);\n"
            "    params.wind_dir_strength = vec4(1.0, 0.0, 0.0, 0.5); params.wind_time_config = vec4(1.57079632679, 1.0, 0.0, 1.0);\n"
            "    params.effector_meta = vec4(0.0);\n"
            "    for (int i = 0; i < 4; ++i) { params.effector_spheres[i] = vec4(0.0); params.effector_configs[i] = vec4(0.0); params.effector_opacity_configs[i] = vec4(0.0); }\n"
            "    vec3 local_position = vec3(0.0, 0.0, 0.5);\n"
            "    GsTestDrawn drawn = GsTestDrawn(0u, gs_test_out.v[1].x, 0u, vec4(0.0));\n"
            "    GsTestSplat splat = GsTestSplat(1.0, 0u);\n" +
            p_locals + p_span + "\n" + p_store + "}\n";
}

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] a fading record under legacy wind is displaced exactly where depth sorts it") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("RenderingDevice required to execute the production colour and depth statements"); return; }
    TestA2History::Resources resources{ rd, {} };
    TileBinningShaderRD colour_source;
    DepthComputeShaderRD depth_source;
    const String colour_body = production_compute_body(colour_source);
    const String depth_body = production_compute_body(depth_source);
    if (colour_body.is_empty() || depth_body.is_empty()) { FAIL("production colour or depth compute stage unavailable"); return; }
    // Colour: every statement from the HLOD record read to the displaced position (record fade, region gate, alpha clip,
    // wind level, deformation). The span opens inside the HLOD block, so the harness reopens it.
    const String colour_span = span_of(colour_body, "    vec3 hlod_camera_offset = selected.camera_offset.xyz;", "    world_position = deformation.position;");
    const String depth_span = span_of(depth_body, "    RegionOverrideGPU region_override = gs_region_resolve(instance, gaussian.semantic, atlas_index);",
            "    world_position = deformation.position;");
    if (colour_span.is_empty() || depth_span.is_empty()) { FAIL("production colour or depth deformation statements missing"); return; }
    CHECK(colour_span.contains("selected.fade")); // The record fade is among the executed colour statements.
    const String store = "    gs_test_out.v[0] = vec4(world_position, deformation.opacity);\n";
    const String colour = harness(colour_body,
            "    GsTestDrawn selected = drawn; GsTestSplat g = splat; uint gaussian_idx = 7u; float hlod_record_fade = 1.0;\n"
            "#if defined(GS_HLOD_RELATIVE_PAYLOAD)\n",
            colour_span, store);
    const String depth = harness(depth_body, "    GsTestDrawn visible_chunk = drawn; GsTestSplat gaussian = splat; uint atlas_index = 7u;\n",
            depth_span, store);
    if (colour.is_empty() || depth.is_empty()) { FAIL("production deformation chain missing"); return; }
    const float fades[3] = { 1.0f, 0.75f, 0.5f };
    float out[2][3][4] = {}; // [colour, depth][fade][displaced xyz, opacity]
    for (int pass = 0; pass < 2; ++pass) {
        String error;
        const RID shader = resources.keep(ShaderCompilationHelper::compile_shader_on_device(rd, pass == 0 ? colour : depth,
                pass == 0 ? "hlod_fade_wind_colour" : "hlod_fade_wind_depth", Vector<String>(), &error));
        if (!shader.is_valid()) { FAIL("harness compilation failed: ", error.utf8().get_data()); return; }
        const RID pipeline = resources.keep(rd->compute_pipeline_create(shader));
        const RID buffer = resources.buffer(8 * sizeof(float));
        const RID set = buffer.is_valid() ? resources.set(shader, buffer) : RID();
        if (!pipeline.is_valid() || !set.is_valid()) { FAIL("harness resources missing"); return; }
        for (int sample = 0; sample < 3; ++sample) {
            const float in[8] = { 0.0f, 0.0f, 0.0f, 0.0f, fades[sample], 0.0f, 0.0f, 0.0f };
            if (rd->buffer_update(buffer, 0, sizeof(in), in) != OK) { FAIL("harness upload failed"); return; }
            const RD::ComputeListID list = rd->compute_list_begin();
            rd->compute_list_bind_compute_pipeline(list, pipeline);
            rd->compute_list_bind_uniform_set(list, set, 0);
            rd->compute_list_dispatch(list, 1, 1, 1);
            rd->compute_list_end();
            const Vector<uint8_t> bytes = rd->buffer_get_data(buffer, 0, 4 * sizeof(float));
            if (bytes.size() != int(4 * sizeof(float))) { FAIL("harness readback incomplete"); return; }
            memcpy(out[pass][sample], bytes.ptr(), sizeof(out[pass][sample]));
            if (!(out[pass][sample][0] > -1.0e29f)) { FAIL("pass ", pass, " returned before the displaced position, fade ", fades[sample]); return; }
        }
    }
    // Legacy wind is active: depth moves the splat off its rest position (0, 0, 0.5), so a weight change is visible.
    CHECK(std::abs(out[1][0][0]) > 0.05f);
    for (int sample = 0; sample < 3; ++sample) {
        CAPTURE(fades[sample]);
        // Colour draws the splat at its sort (depth) position whatever the record fade.
        CHECK(std::abs(out[0][sample][0] - out[1][sample][0]) <= 1.0e-6f);
        CHECK(std::abs(out[0][sample][1] - out[1][sample][1]) <= 1.0e-6f);
        CHECK(std::abs(out[0][sample][2] - out[1][sample][2]) <= 1.0e-6f);
        // The fade still reaches the colour opacity, and only it.
        CHECK(std::abs(out[0][sample][3] - fades[sample] * out[1][sample][3]) <= 1.0e-6f);
    }
}
} // namespace TestHlodFadeWind

namespace TestHlodObjectsOnlyRows {
// Codex P1 on #1571: the objects-only publisher (ADR one-route section 12.7) must record, for the rows it uploads, a row
// generation read BEFORE it collected them. Otherwise a node edited between the collection and the generation read
// leaves the old row in the bundle tagged with the new generation, and refresh_hlod_instance_rows() never re-uploads it.
// The hook moves the node exactly in that window, once; two frames later the bundle's row must be the node's current one.
TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree][RequiresGPU] objects-only rows edited during publication are re-uploaded, never kept stale") {
	SceneTree *tree = SceneTree::get_singleton();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	if (!tree || !tree->get_root() || !RenderingServer::get_singleton() || !rd || !ps || !director) {
		FAIL("Node SceneTree GPU harness with a director required");
		return;
	}
	const String cut_key = "rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled";
	const String route_key = "rendering/gaussian_splatting/streaming/route_policy";
	const Variant cut_before = ps->get_setting(cut_key);
	const Variant route_before = ps->get_setting(route_key);
	ps->set_setting(cut_key, true);
	ps->set_setting(route_key, int64_t(gs::settings::GS_ROUTE_RESIDENT));
	const bool quantized_before = g_quantization_config.per_chunk_quantization;
	g_quantization_config.per_chunk_quantization = false;
	TestHlodRuntimeGpuIdentity::ScopedSortDevice manager_device(rd);
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(4);
	PackedFloat32Array positions, scales, rotations, dc, opacity;
	for (uint32_t i = 0; i < 4; ++i) {
		positions.push_back(0.1f * float(i));
		positions.push_back(0.0f);
		positions.push_back(0.0f);
		for (int a = 0; a < 3; ++a) { scales.push_back(0.05f); dc.push_back(1.0f); }
		rotations.push_back(0.0f);
		rotations.push_back(0.0f);
		rotations.push_back(0.0f);
		rotations.push_back(1.0f);
		opacity.push_back(6.0f);
	}
	asset->set_positions(positions);
	asset->set_scales(scales);
	asset->set_rotations(rotations);
	asset->set_sh_dc_coefficients(dc);
	asset->set_opacity_logits(opacity);
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	node->set_splat_asset(asset);
	node->set_position(Vector3(1.0f, 0.0f, -4.0f));
	tree->get_root()->add_child(node);
	tree->process(0.0);
	struct Cleanup {
		SceneTree *tree;
		GaussianSplatNode3D *node;
		ProjectSettings *ps;
		String cut_key, route_key;
		Variant cut_before, route_before;
		bool quantized_before;
		~Cleanup() {
			tree->get_root()->remove_child(node);
			memdelete(node);
			tree->process(0.0);
			ps->set_setting(cut_key, cut_before);
			ps->set_setting(route_key, route_before);
			g_quantization_config.per_chunk_quantization = quantized_before;
		}
	} cleanup{ tree, node, ps, cut_key, route_key, cut_before, route_before, quantized_before };
	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (renderer.is_null()) { FAIL("the node did not obtain its shared renderer"); return; }
	const ObjectID node_id = node->get_instance_id();
	const float moved_x = 7.5f;
	bool hook_ran = false;
	RenderSceneDataRD scene;
	scene.cam_transform = Transform3D();
	scene.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
	RenderDataRD render;
	render.scene_data = &scene;
	renderer->get_view_state().manual_viewport_override = Size2i(64, 64);
	// The targetless harness has no present target, so the route's byte ledger is primed by hand (the compositor scratch
	// a target-backed frame creates); only then can a publication succeed. The hook is armed after that, so the edit lands
	// in the publication that builds the bundle.
	const auto ledger = TestGaussianSplatting::gs_pump_until([&]() {
		renderer->render_scene_instance(&render);
		OutputCompositor *compositor = renderer->get_subsystem_state().output_compositor.ptr();
		if (!compositor || renderer->get_tile_renderer().is_null()) { return false; }
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.texture_type = RD::TEXTURE_TYPE_2D;
		format.width = format.height = 64;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
		compositor->ensure_hlod_scratch(rd, format, Size2i(64, 64));
		uint64_t bytes = 0;
		return renderer->get_hlod_owned_byte_upper_bound(bytes);
	});
	if (!ledger.ready()) { FAIL("the route's byte ledger never became available: ", ledger.describe()); return; }
	renderer->test_hlod_objects_only_after_rows = [&]() {
		Transform3D t;
		t.origin = Vector3(moved_x, 0.0f, -4.0f);
		director->update_instance_transform(node_id, t);
		hook_ran = true;
	};
	const auto prepared = TestGaussianSplatting::gs_pump_until([&]() {
		renderer->render_scene_instance(&render);
		return hook_ran && renderer->get_resource_state().hlod_objects_only;
	});
	if (!prepared.ready()) {
		FAIL("no objects-only bundle after the edit (hook ran: ", hook_ran, ", reason: ", renderer->get_resource_state().hlod_reason, ") ",
				prepared.describe());
		return;
	}
	// Two more publications: the rows the bundle holds must follow the director's current row.
	for (int frame = 0; frame < 2; ++frame) { renderer->render_scene_instance(&render); }
	const auto &state = renderer->get_resource_state();
	if (!state.hlod_objects_only || state.hlod_instance_row_data.count() != 2) {
		FAIL("objects-only bundle with 2 rows expected, objects_only=", state.hlod_objects_only, " rows=", state.hlod_instance_row_data.count());
		return;
	}
	gs_hlod_bundle::Rows current;
	LocalVector<uint64_t> current_keys;
	director->build_hlod_instance_rows_for_renderer(renderer.ptr(), current.instances, current.grading, current.asset_ids, current_keys, true);
	if (current.count() != 2) { FAIL("director rows: expected 2, got ", current.count()); return; }
	// The edit stuck (otherwise this case could not see a stale row).
	CHECK(current.instances[1].translation_scale[0] == doctest::Approx(moved_x));
	CHECK_MESSAGE(state.hlod_instance_row_data.instances[1].translation_scale[0] == doctest::Approx(current.instances[1].translation_scale[0]),
			"the bundle keeps the row collected before the edit: bundle x=", state.hlod_instance_row_data.instances[1].translation_scale[0],
			", director x=", current.instances[1].translation_scale[0]);
}
} // namespace TestHlodObjectsOnlyRows
