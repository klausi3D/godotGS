#include "test_macros.h"
#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gs_project_settings.h"
#include "../io/gs_payload_page_store.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/quantization_config.h"
#include "core/config/project_settings.h"
#include "core/object/callable_method_pointer.h"
#include "core/os/mutex.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering_server.h"

#include <cstring>

extern "C" int test_hlod_page_store_gpu_cpp_force_link() { return 0; }

namespace TestHlodPageStoreGpu {

// Each queued Callable retains only this box. A late callback never dereferences
// a renderer, RenderingDevice or stack variable. All callback-visible state is
// protected by mutex, following HlodPublicationObservation's ownership pattern.
class FrameReadback : public RefCounted {
	GDCLASS(FrameReadback, RefCounted);
	static void _bind_methods() {}

public:
	Mutex mutex;
	uint32_t received = 0;
	uint32_t expected_bytes[2] = {};
	Vector<uint8_t> bytes[2];
	bool failed = false;

	static void receive(const Vector<uint8_t> &p_bytes, const Ref<FrameReadback> &p_box, int p_slot) {
		if (p_box.is_null()) { return; }
		MutexLock lock(p_box->mutex);
		if (p_slot < 0 || p_slot >= 2 || (p_box->received & (1u << p_slot)) != 0 ||
				uint64_t(p_bytes.size()) != p_box->expected_bytes[p_slot]) {
			p_box->failed = true;
			return;
		}
		p_box->bytes[p_slot] = p_bytes;
		p_box->received |= 1u << p_slot;
	}

	bool complete() {
		MutexLock lock(mutex);
		return failed || received == 3u;
	}
};

struct Settings {
	ProjectSettings *settings;
	const String keys[4] = {
		"rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled",
		"rendering/gaussian_splatting/streaming/route_policy",
		"rendering/gaussian_splatting/instance_pipeline/enabled",
		"rendering/gaussian_splatting/sorting/force_cpu_sort",
	};
	Variant previous[4];
	bool existed[4] = {};
	QuantizationConfig quantization = g_quantization_config;

	explicit Settings(ProjectSettings *p_settings) : settings(p_settings) {
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
	~Settings() {
		for (uint32_t i = 0; i < 4; ++i) {
			if (existed[i]) { settings->set_setting(keys[i], previous[i]); } else { settings->clear(keys[i]); }
		}
		settings->emit_signal("settings_changed");
		g_quantization_config = quantization;
	}
};

struct Runtime {
	SceneTree *tree = nullptr;
	GaussianSplatManager *manager = nullptr;
	GaussianSplatManager *owned_manager = nullptr;
	GaussianSplatSceneDirector *owned_director = nullptr;
	RenderingDevice *previous_device = nullptr;
	GaussianSplatWorld3D *node = nullptr;
	Ref<GaussianSplatRenderer> renderer;
	Ref<RenderSceneBuffersRD> render_buffers;
	RendererRD::TextureStorage *textures = nullptr;
	RID target;

	~Runtime() {
		if (renderer.is_valid()) {
			renderer->release_hlod_resources(); // Join workers before destroying their ResourceState-owned store.
			renderer->get_resource_state().hlod_page_store.reset();
		}
		if (node) {
			if (node->get_parent()) { node->get_parent()->remove_child(node); }
			memdelete(node);
			tree->process(0.0);
		}
		renderer.unref();
		render_buffers.unref();
		if (textures && target.is_valid()) { textures->render_target_free(target); }
		if (owned_director) { memdelete(owned_director); }
		if (manager) { manager->set_primary_rendering_device_for_testing(previous_device); }
		if (owned_manager) { memdelete(owned_manager); }
	}
};

// Observe the actual production publication, not a list manufactured by this
// test: the publisher writes bundle.draw_entries and visible_chunk_buffer. The
// CPU model/mask and CPU-published draw prefix are frozen BEFORE the GPU copies.
// Async bytes are compared with that frame, never with a later resident mask.
static bool observe_frame(GaussianSplatRenderer *p_renderer, RenderingDevice *p_rd,
		uint64_t &r_drawn_records, uint64_t &r_checked_frames) {
	auto &state = p_renderer->get_resource_state();
	if (state.hlod_streamer.is_null() || state.hlod_prepared_snapshot.is_null()) {
		FAIL("D3-2 GPU invariant requires the real paged streamer and prepared snapshot"); return false;
	}
	if (p_renderer->get_device_state().rd != p_rd || state.hlod_relative.device_id != p_rd->get_device_instance_id()) {
		FAIL("D3-2 GPU invariant requires the production bundle's current device generation"); return false;
	}
	const auto &streamer = state.hlod_streamer;
	const uint32_t count = state.hlod_prepared_snapshot->nodes.size();
	const Vector<uint8_t> mask = streamer->resident_mask();
	if (count == 0 || uint64_t(mask.size()) != count || streamer->get_residency().node_count() != count) {
		FAIL("D3-2 GPU invariant requires one residency entry per V4 node"); return false;
	}
	Vector<uint8_t> model;
	if (model.resize(count) != OK) { FAIL("D3-2 GPU residency snapshot allocation failed"); return false; }
	for (uint32_t n = 0; n < count; ++n) {
		model.write[n] = streamer->get_residency().state(n) == gs_hlod_runtime::NodeResidency::RESIDENT ? 1u : 0u;
		if (mask[n] != model[n]) { FAIL("D3-2 GPU resident_mask disagrees with the residency model"); return false; }
	}
	const uint32_t draws = state.hlod_frame_active ? state.hlod_world_draw_records : 0u;
	if (draws > uint32_t(state.hlod_relative.draw_entries.size()) || draws > count) {
		FAIL("D3-2 GPU published draw prefix exceeds the real bundle or V4 node count"); return false;
	}
	Vector<DrawnNodeGPU> published;
	if (published.resize(draws) != OK) { FAIL("D3-2 GPU draw snapshot allocation failed"); return false; }
	for (uint32_t i = 0; i < draws; ++i) { published.write[i] = state.hlod_relative.draw_entries[i]; }
	const RID metadata = state.hlod_relative.buffers.chunk_meta_buffer;
	const RID draw_buffer = state.hlod_relative.buffers.visible_chunk_buffer;
	if (!metadata.is_valid() || !p_rd->buffer_is_valid(metadata) ||
			(draws != 0 && (!draw_buffer.is_valid() || !p_rd->buffer_is_valid(draw_buffer)))) {
		FAIL("D3-2 GPU invariant requires live production metadata and draw-list buffers"); return false;
	}
	const uint64_t frame = p_renderer->get_frame_state().frame_counter;
	Ref<FrameReadback> box;
	box.instantiate();
	if (box.is_null()) { FAIL("D3-2 GPU invariant could not allocate its asynchronous readback owner"); return false; }
	box->expected_bytes[0] = count * sizeof(ChunkMetaGPU);
	box->expected_bytes[1] = draws * sizeof(DrawnNodeGPU);
	if (draws == 0) { box->received |= 2u; }
	if (p_rd->buffer_get_data_async(metadata, callable_mp_static(&FrameReadback::receive).bind(box, 0),
				0, box->expected_bytes[0]) != OK ||
			(draws != 0 && p_rd->buffer_get_data_async(draw_buffer,
					callable_mp_static(&FrameReadback::receive).bind(box, 1), 0, box->expected_bytes[1]) != OK)) {
		FAIL("D3-2 GPU invariant asynchronous readback submission failed"); return false;
	}
	const auto completed = TestGaussianSplatting::gs_pump_until([&]() {
		// Advance the harness's ordinary frame lifecycle; never synchronously
		// read a buffer or call sync() to force this observation to complete.
		p_rd->swap_buffers(false);
		return box->complete();
	});
	if (!completed.ready()) {
		FAIL("D3-2 GPU metadata and draw-list readbacks never completed: ", completed.describe()); return false;
	}
	MutexLock lock(box->mutex);
	if (box->failed || box->received != 3u || p_renderer->get_frame_state().frame_counter != frame) {
		FAIL("D3-2 GPU invariant readback was incomplete, duplicated or crossed renderer frames"); return false;
	}
	if (uint64_t(box->bytes[0].size()) != uint64_t(count) * sizeof(ChunkMetaGPU) ||
			uint64_t(box->bytes[1].size()) != uint64_t(draws) * sizeof(DrawnNodeGPU)) {
		FAIL("D3-2 GPU invariant readback size changed before indexing"); return false;
	}
	for (uint32_t n = 0; n < count; ++n) {
		ChunkMetaGPU meta = {};
		memcpy(&meta, box->bytes[0].ptr() + uint64_t(n) * sizeof(ChunkMetaGPU), sizeof(meta));
		if (mask[n] == 0) {
			// Check every byte, including bounds and atlas address, rather than
			// treating splat_count alone as proof of the zero-metadata contract.
			for (uint32_t b = 0; b < sizeof(ChunkMetaGPU); ++b) {
				if (box->bytes[0][uint64_t(n) * sizeof(ChunkMetaGPU) + b] != 0) {
					FAIL("D3-2 nonresident V4 node has nonzero GPU metadata"); return false;
				}
			}
		} else if (meta.splat_count == 0) {
			FAIL("D3-2 RESIDENT V4 node has zero GPU metadata"); return false;
		}
	}
	for (uint32_t i = 0; i < draws; ++i) {
		DrawnNodeGPU draw = {};
		memcpy(&draw, box->bytes[1].ptr() + uint64_t(i) * sizeof(DrawnNodeGPU), sizeof(draw));
		if (memcmp(&draw, &published[i], sizeof(draw)) != 0) {
			FAIL("D3-2 GPU draw-list bytes disagree with production publication"); return false;
		}
		if (draw.chunk_id >= count) { FAIL("D3-2 GPU draw record names a node outside the V4 package"); return false; }
		if (model[draw.chunk_id] == 0 || mask[draw.chunk_id] == 0) {
			FAIL("D3-2 GPU draw list contains a nonresident V4 node"); return false;
		}
		ChunkMetaGPU meta = {};
		memcpy(&meta, box->bytes[0].ptr() + uint64_t(draw.chunk_id) * sizeof(ChunkMetaGPU), sizeof(meta));
		if (meta.splat_count == 0) { FAIL("D3-2 drawn V4 node has zero GPU metadata"); return false; }
	}
	r_drawn_records += draws;
	++r_checked_frames;
	CHECK_MESSAGE(state.hlod_stream_nonresident_drawn == 0, "D3-2 production publisher rejected a nonresident draw");
	return state.hlod_stream_nonresident_drawn == 0;
}

// The literal tag triple is selected by the existing required WorldSceneTree
// batch. This case deliberately fails closed if its device/SceneTree is absent.
TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] D3-2 held V4 uploads publish only resident GPU draw records") {
	SceneTree *tree = SceneTree::get_singleton();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!tree || !tree->get_root() || !rd || !ps || !RenderingServer::get_singleton() ||
			!RendererRD::TextureStorage::get_singleton()) {
		FAIL("D3-2 GPU draw invariant requires the WorldSceneTree harness and a real RenderingDevice"); return;
	}
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(true, 2, 256, 3)) { return; } // Production V4 writer, GSQ payloads.
	Settings settings(ps);
	Runtime runtime;
	runtime.tree = tree;
	runtime.manager = GaussianSplatManager::get_singleton();
	if (!runtime.manager) { runtime.manager = runtime.owned_manager = memnew(GaussianSplatManager); }
	runtime.previous_device = runtime.manager->set_primary_rendering_device_for_testing(rd);
	if (!GaussianSplatSceneDirector::get_singleton()) { runtime.owned_director = memnew(GaussianSplatSceneDirector); }
	g_quantization_config.per_chunk_quantization = false;
	runtime.node = memnew(GaussianSplatWorld3D);
	runtime.node->set_auto_apply_on_ready(false);
	runtime.node->set_hlod_tau_px(0.01f);
	runtime.node->set_world(fixture.world);
	tree->get_root()->add_child(runtime.node);
	tree->process(0.0);
	runtime.node->apply_world();
	runtime.renderer = runtime.node->get_renderer();
	if (runtime.renderer.is_null()) { FAIL("D3-2 GPU fixture did not obtain its real World3D renderer"); return; }
	runtime.textures = RendererRD::TextureStorage::get_singleton();
	runtime.target = runtime.textures->render_target_create();
	if (!runtime.target.is_valid()) { FAIL("D3-2 GPU fixture render target creation failed"); return; }
	runtime.textures->render_target_set_use_hdr(runtime.target, true);
	runtime.textures->render_target_set_transparent(runtime.target, false);
	runtime.textures->render_target_set_size(runtime.target, 64, 64, 1);
	runtime.render_buffers.instantiate();
	Ref<RenderSceneBuffersConfiguration> buffer_config;
	buffer_config.instantiate();
	if (runtime.render_buffers.is_null() || buffer_config.is_null()) {
		FAIL("D3-2 GPU fixture could not allocate render buffers and configuration"); return;
	}
	buffer_config->set_render_target(runtime.target);
	buffer_config->set_internal_size(Size2i(64, 64));
	buffer_config->set_target_size(Size2i(64, 64));
	buffer_config->set_view_count(1);
	runtime.render_buffers->configure(buffer_config.ptr());
	if (!runtime.render_buffers->has_internal_texture()) { FAIL("D3-2 GPU fixture render buffers did not configure"); return; }
	RenderSceneDataRD scene;
	scene.cam_transform = Transform3D(Basis(), Vector3(8.0f, 8.0f, 64.0f));
	scene.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 256.0f);
	RenderDataRD render;
	render.scene_data = &scene;
	render.render_buffers = runtime.render_buffers;
	// Provision the actual bundle without admitting any payload jobs. This
	// existing seam affects the real publisher before its select/update step.
	runtime.renderer->test_set_hlod_hold_world_idle(true);
	runtime.renderer->render_scene_instance(&render);
	auto &state = runtime.renderer->get_resource_state();
	if (!state.hlod_streaming || state.hlod_streamer.is_null() || state.hlod_snapshot.is_null() ||
			!state.hlod_snapshot->is_paged() || state.hlod_relative.quantized) {
		FAIL("D3-2 GPU fixture never provisioned the real raw paged HLOD bundle"); return;
	}
	// Require automatic integration: manually installing a test store/streamer
	// here would let deletion of the production publisher's wiring stay green.
	if (!state.hlod_page_store) {
		FAIL("D3-2 renderer ResourceState must own the active V4 runtime page store (phase-1 stub unpopulated)"); return;
	}
	HlodNodeStreamer::TestControl control;
	control.hold_uploads = true;
	if (state.hlod_streamer->_test_set_control(control) != OK) { FAIL("D3-2 GPU upload hold must control the real streamer"); return; }
	runtime.renderer->test_set_hlod_hold_world_idle(false);
	uint64_t drawn_records = 0;
	uint64_t checked_frames = 0;
	bool failed = false;
	const Ref<HlodNodeStreamer> initialized_streamer = state.hlod_streamer;
	const PayloadPageStore *initialized_store = state.hlod_page_store.get();
	auto frame = [&]() {
		tree->process(0.0);
		runtime.renderer->render_scene_instance(&render);
		if (state.hlod_streamer != initialized_streamer || state.hlod_page_store.get() != initialized_store) {
			FAIL("D3-2 GPU renderer replaced the streamer or runtime store under observation");
			failed = true; return false;
		}
		if (!observe_frame(runtime.renderer.ptr(), rd, drawn_records, checked_frames)) {
			failed = true; return false;
		}
		return true;
	};
	const auto held = TestGaussianSplatting::gs_pump_until([&]() {
		if (!frame()) { return true; }
		const auto observed = state.hlod_streamer->_test_snapshot();
		if (observed.observation_error != OK) {
			FAIL("D3-2 GPU retained-output observation must be available"); failed = true; return true;
		}
		if (state.hlod_streamer->get_telemetry().resident != 0 || drawn_records != 0) {
			FAIL("D3-2 held GPU uploads published residency or a draw record"); failed = true; return true;
		}
		return observed.ready_bytes != 0;
	});
	if (!held.ready()) { FAIL("D3-2 GPU held output never reached the ordinary ready queue: ", held.describe()); return; }
	if (failed) { return; }
	control.hold_uploads = false;
	if (state.hlod_streamer->_test_set_control(control) != OK) { FAIL("D3-2 GPU upload hold must release the real streamer"); return; }
	const auto resumed = TestGaussianSplatting::gs_pump_until([&]() {
		if (!frame()) { return true; }
		return state.hlod_frame_active && state.hlod_world_draw_records >= 2u && runtime.renderer->has_rendered_content();
	});
	if (!resumed.ready()) { FAIL("D3-2 GPU V4 stream never published a refined resident draw list: ", resumed.describe()); return; }
	if (failed) { return; }
	CHECK_MESSAGE(checked_frames >= 2u, "D3-2 GPU invariant must observe held and resumed production frames");
	CHECK_MESSAGE(drawn_records > 0, "D3-2 GPU invariant must inspect nonempty actual GPU draw-list bytes");
	CHECK_MESSAGE(state.hlod_streamer->get_telemetry().payload_reads_total > 0, "D3-2 GPU invariant must read V4 payloads");
	CHECK_MESSAGE(state.hlod_page_store->stats().pages_decoded > 0, "D3-2 GPU invariant must decode GSQ through the runtime store");
}

} // namespace TestHlodPageStoreGpu
