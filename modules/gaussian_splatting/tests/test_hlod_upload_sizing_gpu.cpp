#include "test_macros.h"
#include "gs_hlod_edit_shadow_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gs_payload_page_store.h"
#include "core/object/callable_method_pointer.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"

#include <cstring>

extern "C" int test_hlod_upload_sizing_gpu_cpp_force_link() { return 0; }

namespace TestHlodUploadSizingGpu {

using Streamer = HlodNodeStreamer;
using Residency = gs_hlod_runtime::NodeResidency;

struct Snapshot {
	Vector<uint8_t> metadata;
	Vector<uint8_t> quantization;
	Vector<uint8_t> atlas;
};

// Callbacks own this box, never stack state, the streamer or the device. All
// callback-visible state is protected by mutex, including completion checks.
class FrameReadback : public RefCounted {
	GDCLASS(FrameReadback, RefCounted);
	static void _bind_methods() {}

public:
	Mutex mutex;
	uint32_t received = 0;
	uint32_t expected_bytes[3] = {};
	Vector<uint8_t> bytes[3];
	bool failed = false;

	static void receive(const Vector<uint8_t> &p_bytes, const Ref<FrameReadback> &p_box, int p_slot) {
		if (p_box.is_null()) { return; }
		MutexLock lock(p_box->mutex);
		if (p_slot < 0 || p_slot >= 3 || (p_box->received & (1u << p_slot)) != 0 ||
				uint64_t(p_bytes.size()) != p_box->expected_bytes[p_slot]) {
			p_box->failed = true;
			return;
		}
		p_box->bytes[p_slot] = p_bytes;
		p_box->received |= 1u << p_slot;
	}

	bool complete() {
		MutexLock lock(mutex);
		return failed || received == 7u;
	}
};

struct Runtime {
	RenderingDevice *rd = nullptr;
	Streamer::Buffers buffers;
	PayloadPageStore store;
	Streamer streamer;

	~Runtime() {
		streamer.shutdown();
		store.shutdown();
		if (rd) {
			if (buffers.quantization.is_valid()) { rd->free(buffers.quantization); }
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

static bool read_frame(Runtime &p_runtime, uint32_t p_nodes, Snapshot &r_snapshot) {
	Ref<FrameReadback> box;
	box.instantiate();
	if (box.is_null()) { FAIL("D3-4 GPU readback owner allocation failed"); return false; }
	box->expected_bytes[0] = p_nodes * sizeof(ChunkMetaGPU);
	box->expected_bytes[1] = p_nodes * sizeof(ChunkQuantizationGPU);
	box->expected_bytes[2] = uint32_t(p_runtime.streamer.atlas_bytes_capacity());
	const RID buffers[3] = { p_runtime.buffers.chunk_meta, p_runtime.buffers.quantization, p_runtime.buffers.atlas };
	for (int slot = 0; slot < 3; ++slot) {
		if (p_runtime.rd->buffer_get_data_async(buffers[slot],
					callable_mp_static(&FrameReadback::receive).bind(box, slot), 0, box->expected_bytes[slot]) != OK) {
			FAIL("D3-4 GPU metadata/quantization/atlas asynchronous readback submission failed"); return false;
		}
	}
	const auto completed = TestGaussianSplatting::gs_pump_until([&]() {
		// GPU callbacks advance without advancing a streamer frame, so these
		// three buffers describe one upload boundary. Never call sync().
		p_runtime.rd->swap_buffers(false);
		return box->complete();
	});
	if (!completed.ready()) {
		FAIL("D3-4 GPU metadata/quantization/atlas readbacks never completed: ", completed.describe()); return false;
	}
	MutexLock lock(box->mutex);
	if (box->failed || box->received != 7u) {
		FAIL("D3-4 GPU readback must contain exactly one complete snapshot of every buffer"); return false;
	}
	r_snapshot.metadata = box->bytes[0];
	r_snapshot.quantization = box->bytes[1];
	r_snapshot.atlas = box->bytes[2];
	return true;
}

struct Golden {
	uint32_t node = 0;
	uint64_t offset = 0;
	Vector<uint8_t> packed;
	ChunkMetaGPU meta = {};
	ChunkQuantizationGPU quant = {};
};

enum SliceViolation {
	SLICE_VALID,
	BAD_SHAPE,
	PREMATURE_METADATA,
	PREMATURE_QUANTIZATION,
	PREMATURE_RESIDENCY,
	FINAL_METADATA,
	FINAL_QUANTIZATION,
	FINAL_RESIDENCY,
	ATLAS_BYTES,
};

// The real readbacks and the premature-metadata control use the same checker.
// The expected prefix comes from observed atlas writes, never the residency
// predicate under test. Padding and every unrelated node must stay unchanged.
static SliceViolation check_slice(const Snapshot &p_observed, const Snapshot &p_initial,
		const Golden &p_golden, uint64_t p_uploaded, bool p_resident) {
	const uint64_t nodes = uint64_t(p_initial.metadata.size()) / sizeof(ChunkMetaGPU);
	if (nodes == 0 || uint64_t(p_initial.metadata.size()) % sizeof(ChunkMetaGPU) != 0 ||
			p_observed.metadata.size() != p_initial.metadata.size() ||
			uint64_t(p_initial.quantization.size()) != nodes * sizeof(ChunkQuantizationGPU) ||
			p_observed.quantization.size() != p_initial.quantization.size() ||
			p_initial.atlas.is_empty() || p_observed.atlas.size() != p_initial.atlas.size() ||
			p_golden.node >= nodes || p_golden.packed.is_empty() || p_uploaded > uint64_t(p_golden.packed.size()) ||
			p_golden.offset > uint64_t(p_initial.atlas.size()) ||
			uint64_t(p_golden.packed.size()) > uint64_t(p_initial.atlas.size()) - p_golden.offset) { return BAD_SHAPE; }
	const bool final = p_uploaded == uint64_t(p_golden.packed.size());
	for (uint64_t node = 0; node < nodes; ++node) {
		const uint64_t meta_offset = node * sizeof(ChunkMetaGPU);
		const uint64_t quant_offset = node * sizeof(ChunkQuantizationGPU);
		const bool published = node == p_golden.node && final;
		const void *expected_meta = published ? static_cast<const void *>(&p_golden.meta) : p_initial.metadata.ptr() + meta_offset;
		const void *expected_quant = published ? static_cast<const void *>(&p_golden.quant) : p_initial.quantization.ptr() + quant_offset;
		if (memcmp(p_observed.metadata.ptr() + meta_offset, expected_meta, sizeof(ChunkMetaGPU)) != 0) {
			return final ? FINAL_METADATA : PREMATURE_METADATA;
		}
		if (memcmp(p_observed.quantization.ptr() + quant_offset, expected_quant, sizeof(ChunkQuantizationGPU)) != 0) {
			return final ? FINAL_QUANTIZATION : PREMATURE_QUANTIZATION;
		}
	}
	if (p_resident != final) { return final ? FINAL_RESIDENCY : PREMATURE_RESIDENCY; }
	for (uint64_t offset = 0; offset < uint64_t(p_initial.atlas.size()); ++offset) {
		const bool in_prefix = offset >= p_golden.offset && offset - p_golden.offset < p_uploaded;
		const uint8_t expected = in_prefix ? p_golden.packed[int64_t(offset - p_golden.offset)] : p_initial.atlas[int64_t(offset)];
		if (p_observed.atlas[int64_t(offset)] != expected) { return ATLAS_BYTES; }
	}
	return SLICE_VALID;
}

// WorldSceneTree provides the real --gs-gpu-test device. The ordinary upload
// path is observed directly; no shadow edit or CPU substitute performs writes.
TEST_CASE("[GaussianSplatting][HLOD][D3Sizing][World][SceneTree][RequiresGPU] D3-4 ordinary node slices publish GPU metadata only with final payload bytes") {
	TestHlodEditShadowTransfer::Fixture fixture;
	Runtime runtime;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || !WorkerThreadPool::get_singleton()) {
		FAIL("D3-4 GPU sizing requires the GPU harness device and worker pool"); return;
	}
	if (!fixture.build(1, 256, 1)) { FAIL("D3-4 GPU sizing fixture construction failed"); return; }
	if (fixture.source.snapshot.is_null() || fixture.source.source.is_null() || fixture.source.node_count == 0) {
		FAIL("D3-4 GPU sizing needs the production-written ordinary V4 source"); return;
	}
	const uint32_t nodes = fixture.source.node_count;
	const uint32_t node = 0;
	Streamer::Config config = fixture.config(true);
	const uint64_t stride = sizeof(PackedGaussianQuantized);
	const uint64_t node_bytes = uint64_t(fixture.source.splats_per_node) * stride;
	const uint64_t publication_bytes = sizeof(ChunkMetaGPU) + sizeof(ChunkQuantizationGPU);
	config.upload_bytes_per_frame = 80u * stride + publication_bytes;
	if (config.upload_bytes_per_frame >= node_bytes / 2u || publication_bytes >= config.upload_bytes_per_frame) {
		FAIL("D3-4 GPU allowance must force at least three payload slices and fit final publication"); return;
	}
	runtime.rd = rd;
	PayloadPageStore::Config store_config;
	store_config.reader_count = config.max_jobs_in_flight;
	store_config.max_bytes_in_flight = MAX(config.max_inflight_read_bytes, fixture.source.source->get_max_record_bytes());
	store_config.max_idle_handles = 0;
	store_config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
	store_config.decoder_retention_cap = 0;
	if (runtime.store.start(store_config) != OK || runtime.streamer.init_with_page_store(fixture.source.snapshot, config,
				uint64_t(fixture.source.snapshot->get_instance_id()), rd->get_device_instance_id(), runtime.store) != OK) {
		FAIL("D3-4 GPU sizing could not initialize the real streamer and page store"); return;
	}
	if (runtime.streamer._test_observe_shadow_transfers() != OK) {
		FAIL("D3-4 GPU sizing needs the existing real device-write trace"); return;
	}
	const uint64_t atlas_bytes = runtime.streamer.atlas_bytes_capacity();
	if (atlas_bytes == 0 || atlas_bytes > UINT32_MAX) { FAIL("D3-4 GPU fixture atlas must fit an actual device buffer"); return; }
	runtime.buffers.atlas = runtime.zeroed_buffer(uint32_t(atlas_bytes));
	runtime.buffers.chunk_meta = runtime.zeroed_buffer(nodes * sizeof(ChunkMetaGPU));
	runtime.buffers.quantization = runtime.zeroed_buffer(nodes * sizeof(ChunkQuantizationGPU));
	if (!runtime.buffers.atlas.is_valid() || !runtime.buffers.chunk_meta.is_valid() || !runtime.buffers.quantization.is_valid()) {
		FAIL("D3-4 GPU sizing requires real zeroed atlas, metadata and quantization buffers"); return;
	}
	Streamer::TestControl control;
	control.hold_uploads = true;
	if (runtime.streamer._test_set_control(control) != OK) { FAIL("D3-4 GPU sizing could not hold ordinary uploads"); return; }
	gs_hlod_runtime::DesiredRecord want;
	want.node = node;
	auto update = [&]() {
		runtime.streamer.begin_frame(rd, &runtime.buffers);
		runtime.streamer.update(rd, runtime.buffers, &want, 1, nullptr, 0);
	};
	// Poll worker completion within one logical frame: a faster poll loop must
	// not age frame-counted cancellation, retry or retirement timeouts.
	runtime.streamer.begin_frame(rd, &runtime.buffers);
	const auto packed = TestGaussianSplatting::gs_pump_until([&]() {
		runtime.streamer.update(rd, runtime.buffers, &want, 1, nullptr, 0);
		Thread::yield();
		rd->swap_buffers(false);
		return runtime.streamer._test_snapshot().ready_outputs == 1;
	});
	if (!packed.ready()) { FAIL("D3-4 GPU ordinary node never reached held packed-ready state: ", packed.describe()); return; }
	uint32_t first_page = 0, page_count = 0;
	if (!runtime.streamer.get_residency().run(node, first_page, page_count) || page_count == 0) {
		FAIL("D3-4 GPU ordinary node must own its live atlas reservation before slicing"); return;
	}
	Golden golden;
	golden.node = node;
	golden.offset = uint64_t(first_page) * config.page_splats * stride;
	if (!fixture.pack(node, false, config, first_page * config.page_splats, golden.packed, golden.meta, golden.quant)) {
		FAIL("D3-4 GPU independent packed golden construction failed"); return;
	}
	if (uint64_t(golden.packed.size()) != node_bytes || node_bytes > uint64_t(page_count) * config.page_splats * stride ||
			golden.meta.splat_count == 0) {
		FAIL("D3-4 GPU independent packed golden must match and fit the live ordinary node run"); return;
	}
	Snapshot initial;
	if (!read_frame(runtime, nodes, initial)) { FAIL("D3-4 GPU held-upload initial readback failed"); return; }
	for (uint8_t byte : initial.metadata) {
		if (byte != 0) { FAIL("D3-4 GPU held node must begin with completely empty ChunkMetaGPU records"); return; }
	}
	for (uint8_t byte : initial.quantization) {
		if (byte != 0) { FAIL("D3-4 GPU quantization must stay empty before the final slice"); return; }
	}
	Vector<Streamer::TestWrite> held_writes;
	if (runtime.streamer._test_shadow_writes(held_writes) != OK || !held_writes.is_empty()) {
		FAIL("D3-4 GPU held-upload setup must not issue any device writes"); return;
	}
	if (check_slice(initial, initial, golden, 0, false) != SLICE_VALID) {
		FAIL("D3-4 GPU checker must accept the real held-upload initial readback"); return;
	}
	// Inject the real packed metadata into an actual pre-upload GPU snapshot.
	// The identical checker must identify early publication before any slice.
	Snapshot premature = initial;
	memcpy(premature.metadata.ptrw() + uint64_t(node) * sizeof(ChunkMetaGPU), &golden.meta, sizeof(golden.meta));
	CHECK_MESSAGE(check_slice(premature, initial, golden, 0, false) == PREMATURE_METADATA,
			"D3-4 GPU slice checker must reject a premature-metadata readback (non-vacuity control)");
	control.hold_uploads = false;
	if (runtime.streamer._test_set_control(control) != OK) { FAIL("D3-4 GPU sizing could not release ordinary uploads"); return; }
	bool failed = false;
	uint64_t uploaded = 0, payload_frames = 0, partial_frames = 0, last_payload_frame = 0;
	uint32_t meta_writes = 0, quant_writes = 0;
	int64_t writes_seen = 0;
	const auto resident = TestGaussianSplatting::gs_pump_until([&]() {
		update();
		Vector<Streamer::TestWrite> writes;
		if (runtime.streamer._test_shadow_writes(writes) != OK || writes.size() < writes_seen) {
			FAIL("D3-4 GPU ordered device-write observation must remain cumulative"); failed = true; return true;
		}
		uint64_t frame_bytes = 0;
		bool payload_this_frame = false;
		for (int64_t index = writes_seen; index < writes.size(); ++index) {
			const Streamer::TestWrite &write = writes[index];
			if (write.node != node || write.shadow || write.during_commit || write.frame == 0 || write.bytes == 0 ||
					write.frame != runtime.streamer.get_residency().frame()) {
				FAIL("D3-4 GPU trace must describe nonempty ordinary writes to the requested node"); failed = true; return true;
			}
			if (frame_bytes > config.upload_bytes_per_frame || write.bytes > config.upload_bytes_per_frame - frame_bytes) {
				FAIL("D3-4 GPU actual payload plus metadata/quantization writes exceeded the constrained frame allowance"); failed = true; return true;
			}
			frame_bytes += write.bytes;
			if (write.kind == Streamer::TestWrite::ATLAS) {
				if (write.offset != golden.offset + uploaded || write.offset % stride != 0 || write.bytes % stride != 0 ||
						write.bytes > node_bytes - uploaded || meta_writes != 0 || quant_writes != 0) {
					FAIL("D3-4 GPU atlas slices must be contiguous, stride-aligned and inside the node run before publication"); failed = true; return true;
				}
				uploaded += write.bytes;
				last_payload_frame = write.frame;
				payload_this_frame = true;
			} else {
				if (uploaded != node_bytes || write.frame != last_payload_frame) {
					FAIL("D3-4 GPU metadata/quantization must follow the final payload slice in its frame"); failed = true; return true;
				}
				if (write.kind == Streamer::TestWrite::QUANTIZATION) { ++quant_writes; }
				if (write.kind == Streamer::TestWrite::METADATA) {
					if (quant_writes != 1) { FAIL("D3-4 GPU metadata must follow one completed quantization write"); failed = true; return true; }
					++meta_writes;
				}
			}
		}
		writes_seen = writes.size();
		if (payload_this_frame) { ++payload_frames; }
		const Vector<uint8_t> &mask = runtime.streamer.resident_mask();
		if (uint64_t(mask.size()) != nodes) { FAIL("D3-4 GPU sizing requires one resident-mask entry per source node"); failed = true; return true; }
		const bool is_resident = runtime.streamer.get_residency().state(node) == Residency::RESIDENT;
		if ((mask[node] != 0) != is_resident) {
			FAIL("D3-4 GPU partial-node mask disagrees with real residency"); failed = true; return true;
		}
		Snapshot observed;
		if (!read_frame(runtime, nodes, observed)) {
			FAIL("D3-4 GPU slice boundary readback failed"); failed = true; return true;
		}
		const SliceViolation violation = check_slice(observed, initial, golden, uploaded, is_resident);
		if (violation != SLICE_VALID) {
			FAIL("D3-4 GPU readback violated payload-first final-slice publication, violation=", int(violation)); failed = true; return true;
		}
		if (uploaded != 0 && uploaded < node_bytes) {
			if (runtime.streamer.get_residency().state(node) != Residency::LOADING &&
					runtime.streamer.get_residency().state(node) != Residency::UPLOADING) {
				FAIL("D3-4 GPU partially uploaded node must stay LOADING or UPLOADING"); failed = true; return true;
			}
			++partial_frames;
		}
		return is_resident;
	});
	if (!resident.ready()) { FAIL("D3-4 GPU sliced ordinary node never became resident: ", resident.describe()); return; }
	if (failed) { return; }
	CHECK(uploaded == node_bytes);
	CHECK(payload_frames >= 3);
	CHECK(partial_frames >= 2);
	CHECK(meta_writes == 1);
	CHECK(quant_writes == 1);
	CHECK(runtime.streamer.get_telemetry().uploaded_bytes_total == node_bytes);
}

} // namespace TestHlodUploadSizingGpu
