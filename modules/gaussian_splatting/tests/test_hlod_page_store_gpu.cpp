#include "test_macros.h"
#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gs_payload_page_store.h"
#include "../lod/hlod_runtime_selection.h"
#include "../renderer/hlod_node_streamer.h"
#include "core/object/callable_method_pointer.h"
#include "core/os/mutex.h"

#include <cstring>

extern "C" int test_hlod_page_store_gpu_cpp_force_link() { return 0; }

namespace TestHlodPageStoreGpu {

using gs_hlod_runtime::CutHistory;
using gs_hlod_runtime::DrawRecord;
using gs_hlod_runtime::NodeResidency;

// A late callback owns only this box, never a device or stack variable. Every
// callback-visible field is protected by mutex; no streamer frame advances
// between submission and consumption of this frame's metadata/atlas snapshot.
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

struct Runtime {
	RenderingDevice *rd = nullptr;
	HlodNodeStreamer::Buffers buffers;
	PayloadPageStore store;
	HlodNodeStreamer streamer;

	~Runtime() {
		// Worker drain -> store -> our device buffers, on every exit path.
		streamer.shutdown();
		store.shutdown();
		if (rd) {
			if (buffers.chunk_meta.is_valid()) { rd->free(buffers.chunk_meta); }
			if (buffers.atlas.is_valid()) { rd->free(buffers.atlas); }
		}
	}

	RID zeroed_buffer(uint32_t p_bytes) {
		Vector<uint8_t> bytes;
		if (bytes.resize(p_bytes) != OK) { return RID(); }
		bytes.fill(0);
		return rd->storage_buffer_create(p_bytes, bytes);
	}
};

enum FrameViolation {
	FRAME_VALID,
	BAD_SHAPE,
	BAD_RESIDENT_MASK,
	DRAW_OUT_OF_RANGE,
	NONRESIDENT_DRAW,
	NONRESIDENT_METADATA,
	RESIDENT_METADATA,
	ATLAS_IDENTITY,
};

// This is also the mutation checker: the control changes its draw-list input,
// never the model, GPU bytes, expected records or checker implementation.
static FrameViolation check_frame(const LocalVector<DrawRecord> &p_drawn, const Vector<uint8_t> &p_mask,
		const Vector<uint8_t> &p_model, const Vector<uint8_t> &p_metadata, const Vector<uint8_t> &p_atlas,
		const LocalVector<Vector<PackedGaussian>> &p_goldens) {
	const uint32_t nodes = p_goldens.size();
	if (nodes == 0 || uint64_t(p_mask.size()) != nodes || uint64_t(p_model.size()) != nodes ||
			uint64_t(p_metadata.size()) != uint64_t(nodes) * sizeof(ChunkMetaGPU)) { return BAD_SHAPE; }
	for (const DrawRecord &draw : p_drawn) {
		if (draw.node >= nodes) { return DRAW_OUT_OF_RANGE; }
		if (p_mask[draw.node] == 0 || p_model[draw.node] == 0) { return NONRESIDENT_DRAW; }
	}
	for (uint32_t node = 0; node < nodes; ++node) {
		if (p_mask[node] != p_model[node]) { return BAD_RESIDENT_MASK; }
		const uint8_t *meta_bytes = p_metadata.ptr() + uint64_t(node) * sizeof(ChunkMetaGPU);
		if (p_mask[node] == 0) {
			for (uint32_t b = 0; b < sizeof(ChunkMetaGPU); ++b) {
				if (meta_bytes[b] != 0) { return NONRESIDENT_METADATA; }
			}
			continue;
		}
		ChunkMetaGPU meta = {};
		memcpy(&meta, meta_bytes, sizeof(meta));
		if (p_goldens[node].is_empty() || meta.splat_count != uint64_t(p_goldens[node].size())) { return RESIDENT_METADATA; }
		const uint64_t offset = uint64_t(meta.atlas_base) * sizeof(PackedGaussian);
		const uint64_t bytes = uint64_t(meta.splat_count) * sizeof(PackedGaussian);
		if (offset > uint64_t(p_atlas.size()) || bytes > uint64_t(p_atlas.size()) - offset) { return RESIDENT_METADATA; }
		if (memcmp(p_atlas.ptr() + offset, p_goldens[node].ptr(), size_t(bytes)) != 0) { return ATLAS_IDENTITY; }
	}
	return FRAME_VALID;
}

static const char *violation_message(FrameViolation p_violation) {
	switch (p_violation) {
		case BAD_SHAPE: return "D3-2 GPU invariant requires complete metadata and residency snapshots";
		case BAD_RESIDENT_MASK: return "D3-2 GPU resident_mask disagrees with the RESIDENT model state";
		case DRAW_OUT_OF_RANGE: return "D3-2 GPU draw record names a node outside the V4 package";
		case NONRESIDENT_DRAW: return "D3-2 GPU draw list contains a nonresident V4 node";
		case NONRESIDENT_METADATA: return "D3-2 nonresident V4 node has nonzero GPU metadata";
		case RESIDENT_METADATA: return "D3-2 RESIDENT V4 node has missing or invalid GPU metadata";
		case ATLAS_IDENTITY: return "D3-2 resident V4 GPU atlas bytes differ from independent packed identity records";
		default: return "D3-2 GPU invariant checker returned an unknown violation";
	}
}

static bool observe_frame(Runtime &p_runtime, const LocalVector<DrawRecord> &p_drawn,
		const LocalVector<Vector<PackedGaussian>> &p_goldens, bool p_mutate_draw_list, bool &r_mutation_detected) {
	const uint32_t nodes = p_goldens.size();
	const Vector<uint8_t> mask = p_runtime.streamer.resident_mask();
	if (nodes == 0 || uint64_t(mask.size()) != nodes || p_runtime.streamer.get_residency().node_count() != nodes) {
		FAIL("D3-2 GPU invariant requires one residency entry per V4 node"); return false;
	}
	Vector<uint8_t> model;
	if (model.resize(nodes) != OK) { FAIL("D3-2 GPU residency snapshot allocation failed"); return false; }
	for (uint32_t node = 0; node < nodes; ++node) {
		model.write[node] = p_runtime.streamer.get_residency().state(node) == NodeResidency::RESIDENT ? 1u : 0u;
	}
	Ref<FrameReadback> box;
	box.instantiate();
	if (box.is_null()) { FAIL("D3-2 GPU invariant could not allocate its asynchronous readback owner"); return false; }
	box->expected_bytes[0] = nodes * sizeof(ChunkMetaGPU);
	box->expected_bytes[1] = uint32_t(p_runtime.streamer.atlas_bytes_capacity());
	if (p_runtime.rd->buffer_get_data_async(p_runtime.buffers.chunk_meta,
				callable_mp_static(&FrameReadback::receive).bind(box, 0), 0, box->expected_bytes[0]) != OK ||
			p_runtime.rd->buffer_get_data_async(p_runtime.buffers.atlas,
					callable_mp_static(&FrameReadback::receive).bind(box, 1), 0, box->expected_bytes[1]) != OK) {
		FAIL("D3-2 GPU metadata/atlas asynchronous readback submission failed"); return false;
	}
	const auto completed = TestGaussianSplatting::gs_pump_until([&]() {
		// The --gs-gpu-test singleton advances with swap_buffers(false), as in
		// gs_gpu_test_runner.cpp. No target, synchronous buffer read or sync().
		p_runtime.rd->swap_buffers(false);
		return box->complete();
	});
	if (!completed.ready()) {
		FAIL("D3-2 GPU metadata and atlas readbacks never completed: ", completed.describe()); return false;
	}
	MutexLock lock(box->mutex);
	if (box->failed || box->received != 3u || uint64_t(box->bytes[1].size()) != box->expected_bytes[1]) {
		FAIL("D3-2 GPU metadata/atlas readback was incomplete or duplicated"); return false;
	}
	const FrameViolation violation = check_frame(p_drawn, mask, model, box->bytes[0], box->bytes[1], p_goldens);
	if (violation != FRAME_VALID) { FAIL(violation_message(violation)); return false; }
	if (p_mutate_draw_list) {
		uint32_t absent = 0;
		while (absent < nodes && model[absent] != 0) { ++absent; }
		if (absent == nodes) { FAIL("D3-2 draw-list control requires a known nonresident node"); return false; }
		LocalVector<DrawRecord> invalid_drawn;
		DrawRecord invalid;
		invalid.node = absent;
		invalid_drawn.push_back(invalid);
		r_mutation_detected = check_frame(invalid_drawn, mask, model, box->bytes[0], box->bytes[1], p_goldens) == NONRESIDENT_DRAW;
		if (!r_mutation_detected) {
			FAIL("D3-2 GPU draw-list checker must reject a known nonresident draw (non-vacuity control)"); return false;
		}
	}
	return true;
}

// WorldSceneTree selects the unchanged tags. Its RenderingServer is Dummy;
// --gs-gpu-test supplies the real device independently. Production target-backed
// publisher wiring is exercised by the required native streamed-resize scenario.
TEST_CASE("[GaussianSplatting][HLOD][PagedV4][World][SceneTree][RequiresGPU] D3-2 held V4 uploads publish only resident GPU draw records") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || !WorkerThreadPool::get_singleton()) {
		FAIL("D3-2 GPU draw invariant requires the GPU harness RenderingDevice and worker pool"); return;
	}
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(true, 2, 256, 3)) { return; }
	if (fixture.snapshot.is_null() || fixture.source.is_null() || fixture.node_count == 0 ||
			uint64_t(fixture.snapshot->nodes.size()) != fixture.node_count) {
		FAIL("D3-2 GPU invariant requires a complete GSQ V4 fixture and paged snapshot"); return;
	}
	Runtime runtime;
	runtime.rd = rd;
	HlodNodeStreamer::Config config;
	config.atlas_pages = fixture.node_count; // Each fixture node fits one default page.
	PayloadPageStore::Config store_config;
	store_config.reader_count = config.max_jobs_in_flight;
	store_config.max_bytes_in_flight = MAX(config.max_inflight_read_bytes, fixture.source->get_max_record_bytes());
	store_config.max_idle_handles = 0;
	store_config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
	store_config.decoder_retention_cap = 0;
	if (runtime.store.start(store_config) != OK || runtime.streamer.init_with_page_store(fixture.snapshot, config,
				uint64_t(fixture.snapshot->get_instance_id()), rd->get_device_instance_id(), runtime.store) != OK) {
		FAIL("D3-2 GPU invariant could not initialize the real V4 store and streamer"); return;
	}
	runtime.buffers.atlas = runtime.zeroed_buffer(uint32_t(runtime.streamer.atlas_bytes_capacity()));
	runtime.buffers.chunk_meta = runtime.zeroed_buffer(fixture.node_count * sizeof(ChunkMetaGPU));
	if (!runtime.buffers.atlas.is_valid() || !runtime.buffers.chunk_meta.is_valid() ||
			!rd->buffer_is_valid(runtime.buffers.atlas) || !rd->buffer_is_valid(runtime.buffers.chunk_meta)) {
		FAIL("D3-2 GPU invariant requires real zeroed atlas and ChunkMeta storage buffers"); return;
	}
	LocalVector<Vector<PackedGaussian>> goldens;
	goldens.resize(fixture.node_count);
	for (uint32_t node = 0; node < fixture.node_count; ++node) {
		Vector<Gaussian> gaussians;
		Vector<Vector3> sh;
		if (!fixture.expected_payload(node, gaussians, sh)) { return; }
		if (goldens[node].resize(fixture.splats_per_node) != OK) { FAIL("D3-2 GPU packed oracle allocation failed"); return; }
		gs_hlod_pack::Params params;
		params.relative = true;
		ChunkMetaGPU meta = {};
		SHCompressionMetrics metrics;
		gs_hlod_pack::Output out;
		out.raw = goldens[node].ptrw();
		out.meta = &meta;
		out.metrics = &metrics;
		gs_hlod_pack::PayloadView payload;
		payload.gaussians = gaussians.ptr();
		payload.sh = sh.ptr();
		if (!gs_hlod_pack::pack_node(*fixture.snapshot.ptr(), node, params, 0, out, &payload)) {
			FAIL("D3-2 GPU independent V4 packed identity oracle failed"); return;
		}
	}
	CutHistory history;
	if (!history.reserve(fixture.node_count)) { FAIL("D3-2 GPU selector workspace unavailable"); return; }
	HlodNodeStreamer::TestControl control;
	control.hold_uploads = true;
	if (runtime.streamer._test_set_control(control) != OK) { FAIL("D3-2 GPU upload hold must control the real streamer"); return; }
	bool failed = false, mutation_detected = false;
	uint64_t held_demand_frames = 0, drawn_frames = 0;
	uint32_t last_draw_count = 0;
	auto frame = [&]() {
		runtime.streamer.begin_frame(rd, &runtime.buffers);
		gs_hlod_runtime::CutRequest request;
		request.camera = Vector3(8.0f, 8.0f, 64.0f);
		request.frustum_enabled = false;
		request.focal_pixels = 100.0;
		request.tau_pixels = 0.01;
		request.record_budget = 1.0e9;
		request.splat_budget = uint64_t(fixture.node_count) * fixture.splats_per_node;
		request.node_budget = fixture.node_count;
		request.page_splats = config.page_splats;
		request.page_budget = config.atlas_pages;
		request.resident = runtime.streamer.resident_mask().ptr();
		request.resident_count = runtime.streamer.resident_mask().size();
		const CutHistory::Result cut = history.select(*fixture.snapshot.ptr(), request);
		if (cut.status != CutHistory::COMPLETE && cut.status != CutHistory::UNAVAILABLE) {
			FAIL("D3-2 GPU real selector did not produce a complete or residency-waiting cut"); failed = true; return false;
		}
		LocalVector<DrawRecord> drawn;
		if (cut.status == CutHistory::COMPLETE) {
			for (uint32_t i = 0; i < cut.selected_count; ++i) {
				DrawRecord draw;
				draw.node = history.selected_node(i);
				drawn.push_back(draw);
			}
		}
		LocalVector<gs_hlod_runtime::DesiredRecord> desired;
		for (uint32_t i = 0; i < history.demand_count(); ++i) {
			gs_hlod_runtime::DesiredRecord want;
			want.node = history.demand_node(i);
			if (want.node >= uint32_t(runtime.streamer.resident_mask().size())) {
				FAIL("D3-2 GPU real selector requested a node outside the residency mask"); failed = true; return false;
			}
			if (control.hold_uploads && runtime.streamer.resident_mask()[want.node] == 0) { ++held_demand_frames; }
			desired.push_back(want);
		}
		runtime.streamer.update(rd, runtime.buffers, desired.ptr(), desired.size(), drawn.ptr(), drawn.size());
		if (!observe_frame(runtime, drawn, goldens, control.hold_uploads && !mutation_detected, mutation_detected)) {
			failed = true; return false;
		}
		last_draw_count = drawn.size();
		if (!drawn.is_empty()) { ++drawn_frames; }
		return true;
	};
	const auto held = TestGaussianSplatting::gs_pump_until([&]() {
		if (!frame()) { return true; }
		const auto observed = runtime.streamer._test_snapshot();
		if (observed.observation_error != OK || runtime.streamer.get_telemetry().resident != 0 || drawn_frames != 0) {
			FAIL("D3-2 held GPU uploads must keep every demanded node nonresident and undrawn"); failed = true; return true;
		}
		return observed.ready_bytes != 0 && held_demand_frames > 0;
	});
	if (!held.ready()) { FAIL("D3-2 GPU held demand never reached real ready output: ", held.describe()); return; }
	if (failed) { return; }
	control.hold_uploads = false;
	if (runtime.streamer._test_set_control(control) != OK) { FAIL("D3-2 GPU upload hold must release the real streamer"); return; }
	const auto resumed = TestGaussianSplatting::gs_pump_until([&]() {
		if (!frame()) { return true; }
		return last_draw_count >= 2 && runtime.store.stats().pages_decoded > 0;
	});
	if (!resumed.ready()) { FAIL("D3-2 GPU V4 stream never produced a refined resident cut: ", resumed.describe()); return; }
	if (failed) { return; }
	CHECK_MESSAGE(held_demand_frames > 0, "D3-2 GPU invariant must observe nonresident demand while uploads are held");
	CHECK_MESSAGE(drawn_frames > 0, "D3-2 GPU invariant must inspect nonempty real selector draw records after release");
	CHECK_MESSAGE(mutation_detected, "D3-2 GPU draw-list non-vacuity control never rejected a known nonresident node");
	CHECK_MESSAGE(runtime.streamer.get_telemetry().payload_reads_total > 0, "D3-2 GPU invariant must read V4 payloads");
	CHECK_MESSAGE(runtime.store.stats().pages_decoded > 0, "D3-2 GPU invariant must decode GSQ through the real store");
}

} // namespace TestHlodPageStoreGpu
