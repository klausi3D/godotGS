#include "test_macros.h"
#include "gs_hlod_edit_shadow_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gs_payload_page_store.h"
#include "core/object/callable_method_pointer.h"
#include "core/os/mutex.h"

#include <cstring>

extern "C" int test_hlod_edit_shadow_transfer_gpu_cpp_force_link() { return 0; }

namespace TestHlodEditShadowTransferGpu {

using TestHlodEditShadowTransfer::Fixture;
using Streamer = HlodNodeStreamer;
using gs_hlod_runtime::NodeResidency;

struct Snapshot {
	Vector<uint8_t> metadata;
	Vector<uint8_t> quantization;
	Vector<uint8_t> atlas;
};

// A callback owns only this box, never the device or a stack variable. The
// streamer does not advance between submission and consumption of a snapshot.
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
	if (box.is_null()) { FAIL("D3-3 GPU asynchronous readback owner allocation failed"); return false; }
	box->expected_bytes[0] = p_nodes * sizeof(ChunkMetaGPU);
	box->expected_bytes[1] = p_nodes * sizeof(ChunkQuantizationGPU);
	box->expected_bytes[2] = uint32_t(p_runtime.streamer.atlas_bytes_capacity());
	const RID buffers[3] = { p_runtime.buffers.chunk_meta, p_runtime.buffers.quantization, p_runtime.buffers.atlas };
	for (int slot = 0; slot < 3; ++slot) {
		if (p_runtime.rd->buffer_get_data_async(buffers[slot],
					callable_mp_static(&FrameReadback::receive).bind(box, slot), 0, box->expected_bytes[slot]) != OK) {
			FAIL("D3-3 GPU metadata/quantization/atlas asynchronous readback submission failed"); return false;
		}
	}
	const auto completed = TestGaussianSplatting::gs_pump_until([&]() {
		p_runtime.rd->swap_buffers(false);
		return box->complete();
	});
	if (!completed.ready()) {
		FAIL("D3-3 GPU metadata/quantization/atlas readbacks never completed: ", completed.describe()); return false;
	}
	MutexLock lock(box->mutex);
	if (box->failed || box->received != 7u) {
		FAIL("D3-3 GPU readback did not contain one complete snapshot of all three buffers"); return false;
	}
	r_snapshot.metadata = box->bytes[0];
	r_snapshot.quantization = box->bytes[1];
	r_snapshot.atlas = box->bytes[2];
	return true;
}

enum RevisionViolation {
	REVISION_VALID,
	BAD_SHAPE,
	UNKNOWN_METADATA,
	MIXED_REVISION,
	WRONG_REVISION,
	WRONG_QUANTIZATION,
};

// Positive GPU observations and the mixed-readback control use this exact
// checker. Unchanged records are neutral; edited leaves and stale ancestors
// must all belong to one revision, chosen by the begin_frame commit boundary.
static RevisionViolation check_revision(const Snapshot &p_observed, const Snapshot &p_old,
		const Snapshot &p_new, bool p_committed) {
	if (p_old.metadata.is_empty() || p_old.metadata.size() != p_new.metadata.size() ||
			p_old.metadata.size() != p_observed.metadata.size() ||
			uint64_t(p_old.metadata.size()) % sizeof(ChunkMetaGPU) != 0 ||
			p_old.quantization.is_empty() ||
			uint64_t(p_old.quantization.size()) != uint64_t(p_old.metadata.size()) / sizeof(ChunkMetaGPU) * sizeof(ChunkQuantizationGPU) ||
			p_old.quantization.size() != p_new.quantization.size() ||
			p_old.quantization.size() != p_observed.quantization.size()) {
		return BAD_SHAPE;
	}
	uint32_t old_records = 0, new_records = 0;
	for (uint64_t offset = 0; offset < uint64_t(p_old.metadata.size()); offset += sizeof(ChunkMetaGPU)) {
		const uint8_t *before = p_old.metadata.ptr() + offset;
		const uint8_t *after = p_new.metadata.ptr() + offset;
		const uint8_t *observed = p_observed.metadata.ptr() + offset;
		const bool is_old = memcmp(before, observed, sizeof(ChunkMetaGPU)) == 0;
		const bool is_new = memcmp(after, observed, sizeof(ChunkMetaGPU)) == 0;
		if (!is_old && !is_new) { return UNKNOWN_METADATA; }
		if (memcmp(before, after, sizeof(ChunkMetaGPU)) == 0) { continue; }
		old_records += is_old ? 1u : 0u;
		new_records += is_new ? 1u : 0u;
	}
	if (old_records != 0 && new_records != 0) { return MIXED_REVISION; }
	if (old_records + new_records == 0) { return BAD_SHAPE; }
	if ((p_committed && old_records != 0) || (!p_committed && new_records != 0)) { return WRONG_REVISION; }
	const Vector<uint8_t> &expected_quantization = p_committed ? p_new.quantization : p_old.quantization;
	if (memcmp(p_observed.quantization.ptr(), expected_quantization.ptr(), size_t(expected_quantization.size())) != 0) {
		return WRONG_QUANTIZATION;
	}
	return REVISION_VALID;
}

struct ShadowGolden {
	NodeResidency::Ticket ticket;
	Vector<uint8_t> packed;
	uint64_t offset = 0;
};

// Compare the entire atlas, including padding and all old resident runs. Only
// the payload span of a reserved shadow run may change before publication.
static bool check_atlas(const Snapshot &p_observed, const Snapshot &p_old,
		const LocalVector<ShadowGolden> &p_goldens, bool p_require_complete,
		uint32_t &r_complete, uint64_t &r_changed) {
	if (p_old.atlas.is_empty() || p_goldens.is_empty() || p_observed.atlas.size() != p_old.atlas.size()) { return false; }
	r_complete = 0;
	r_changed = 0;
	for (const ShadowGolden &golden : p_goldens) {
		if (golden.packed.is_empty() || golden.offset > uint64_t(p_observed.atlas.size()) ||
				uint64_t(golden.packed.size()) > uint64_t(p_observed.atlas.size()) - golden.offset) { return false; }
		const bool complete = memcmp(p_observed.atlas.ptr() + golden.offset,
				golden.packed.ptr(), size_t(golden.packed.size())) == 0;
		if (p_require_complete && !complete) { return false; }
		r_complete += complete ? 1u : 0u;
	}
	for (uint64_t offset = 0; offset < uint64_t(p_observed.atlas.size()); ++offset) {
		const uint8_t observed = p_observed.atlas[int64_t(offset)];
		const uint8_t before = p_old.atlas[int64_t(offset)];
		if (observed == before) { continue; }
		bool in_payload = false;
		for (const ShadowGolden &golden : p_goldens) {
			if (offset >= golden.offset && offset - golden.offset < uint64_t(golden.packed.size())) {
				if (observed != golden.packed[int64_t(offset - golden.offset)]) { return false; }
				in_payload = true;
				break;
			}
		}
		if (!in_payload) { return false; }
		++r_changed;
	}
	return true;
}

// WorldSceneTree supplies the real --gs-gpu-test RenderingDevice separately
// from its Dummy RenderingServer, just as the D3-2 page-store GPU case does.
TEST_CASE("[GaussianSplatting][HLOD][Edit][D3Shadow][World][SceneTree][RequiresGPU] D3-3 incremental shadow transfer preserves one GPU revision until atomic commit") {
	Fixture fixture;
	Runtime runtime;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd || !WorkerThreadPool::get_singleton()) {
		FAIL("D3-3 GPU shadow transfer requires the GPU harness device and worker pool"); return;
	}
	if (!fixture.build()) { return; }
	if (fixture.source.snapshot.is_null() || fixture.source.source.is_null() || fixture.edited.is_null() ||
			fixture.leaves.size() < 2 || fixture.stale.is_empty()) {
		FAIL("D3-3 GPU revision checker needs edited leaves, stale ancestors and complete source snapshots"); return;
	}
	const uint32_t nodes = fixture.source.node_count;
	if (nodes == 0 || uint64_t(fixture.source.snapshot->nodes.size()) != nodes || uint64_t(fixture.edited->nodes.size()) != nodes) {
		FAIL("D3-3 GPU fixture must have matching nonempty source and edited node tables"); return;
	}
	Streamer::Config config = fixture.config(true);
	uint64_t total_packed_bytes = 0;
	for (uint32_t leaf : fixture.leaves) {
		if (leaf >= uint32_t(fixture.edited->nodes.size())) { FAIL("D3-3 GPU edited leaf is outside the fixture"); return; }
		total_packed_bytes += uint64_t(fixture.edited->nodes[leaf].topology.payload_count) * sizeof(PackedGaussianQuantized);
	}
	config.upload_bytes_per_frame = MAX(fixture.publication_bytes(true), total_packed_bytes / 4u);
	if (total_packed_bytes <= config.upload_bytes_per_frame || fixture.publication_bytes(true) > config.upload_bytes_per_frame) {
		FAIL("D3-3 GPU fixture must exceed the upload allowance while its atomic metadata publication fits"); return;
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
		FAIL("D3-3 GPU shadow transfer could not initialize the real streamer and page store"); return;
	}
	const Error observation = runtime.streamer._test_observe_shadow_transfers();
	CHECK_MESSAGE(observation == OK, "D3-3 phase-1 stub: GPU shadow transfer observation is unavailable");
	if (observation != OK) { return; }
	runtime.buffers.atlas = runtime.zeroed_buffer(uint32_t(runtime.streamer.atlas_bytes_capacity()));
	runtime.buffers.chunk_meta = runtime.zeroed_buffer(nodes * sizeof(ChunkMetaGPU));
	runtime.buffers.quantization = runtime.zeroed_buffer(nodes * sizeof(ChunkQuantizationGPU));
	if (!runtime.buffers.atlas.is_valid() || !runtime.buffers.chunk_meta.is_valid() || !runtime.buffers.quantization.is_valid()) {
		FAIL("D3-3 GPU shadow transfer requires real zeroed atlas, metadata and quantization buffers"); return;
	}
	gs_hlod_runtime::CutRequest view;
	view.camera = Vector3(8, 8, 64);
	view.frustum_enabled = false;
	view.focal_pixels = 100;
	view.tau_pixels = 1.0e9;
	view.record_budget = 1.0e9;
	view.splat_budget = uint64_t(nodes) * fixture.source.splats_per_node;
	view.node_budget = nodes;
	view.page_splats = config.page_splats;
	view.page_budget = config.atlas_pages;
	runtime.streamer.register_view(1, view);
	auto update = [&]() {
		LocalVector<gs_hlod_runtime::DesiredRecord> desired;
		for (uint32_t node = 0; node < nodes; ++node) {
			if (runtime.streamer.get_data()->is_stale(node)) { continue; }
			gs_hlod_runtime::DesiredRecord want;
			want.node = node;
			desired.push_back(want);
		}
		runtime.streamer.update(rd, runtime.buffers, desired.ptr(), desired.size(), nullptr, 0);
	};
	const auto loaded = TestGaussianSplatting::gs_pump_until([&]() {
		runtime.streamer.begin_frame(rd, &runtime.buffers);
		update();
		rd->swap_buffers(false);
		return runtime.streamer.get_telemetry().resident == nodes;
	});
	if (!loaded.ready()) { FAIL("D3-3 GPU old revision never became wholly resident: ", loaded.describe()); return; }
	Snapshot old_snapshot;
	if (!read_frame(runtime, nodes, old_snapshot)) { return; }
	Vector<Streamer::TestWrite> warmup_writes;
	const Error warmup_trace = runtime.streamer._test_shadow_writes(warmup_writes);
	CHECK_MESSAGE(warmup_trace == OK, "D3-3 phase-1 stub: GPU ordered write observation is unavailable");
	if (warmup_trace != OK) { return; }
	int64_t writes_seen = warmup_writes.size();
	for (uint32_t stale : fixture.stale) {
		if (stale >= nodes) { FAIL("D3-3 GPU stale ancestor is outside the source"); return; }
		ChunkMetaGPU old_meta = {};
		memcpy(&old_meta, old_snapshot.metadata.ptr() + uint64_t(stale) * sizeof(ChunkMetaGPU), sizeof(old_meta));
		if (old_meta.splat_count == 0) { FAIL("D3-3 GPU stale-ancestor check requires its old revision to be visible"); return; }
	}
	if (runtime.streamer.stage_edit(fixture.edited) != Streamer::EDIT_STAGED) {
		FAIL("D3-3 GPU oversized edit must be admitted in full"); return;
	}
	Vector<NodeResidency::Ticket> runs;
	const Error run_observation = runtime.streamer._test_shadow_runs(runs);
	CHECK_MESSAGE(run_observation == OK, "D3-3 phase-1 stub: GPU reserved shadow-run observation is unavailable");
	if (run_observation != OK) { return; }
	if (runs.size() != fixture.leaves.size()) { FAIL("D3-3 GPU oracle needs every reserved shadow run"); return; }
	Snapshot new_snapshot = old_snapshot;
	LocalVector<ShadowGolden> goldens;
	for (uint32_t leaf : fixture.leaves) {
		const NodeResidency::Ticket *ticket = nullptr;
		for (const NodeResidency::Ticket &run : runs) {
			if (run.node == leaf) { ticket = &run; break; }
		}
		if (!ticket || !ticket->valid() || !ticket->shadow) { FAIL("D3-3 GPU edited leaf has no valid reserved shadow ticket"); return; }
		ShadowGolden golden;
		golden.ticket = *ticket;
		golden.offset = uint64_t(ticket->first_page) * config.page_splats * sizeof(PackedGaussianQuantized);
		ChunkMetaGPU meta = {};
		ChunkQuantizationGPU quant = {};
		if (!fixture.pack(leaf, true, config, ticket->first_page * config.page_splats, golden.packed, meta, quant)) { return; }
		if (uint64_t(golden.packed.size()) > uint64_t(ticket->page_count) * config.page_splats * sizeof(PackedGaussianQuantized)) {
			FAIL("D3-3 GPU packed oracle exceeds the leaf's reserved shadow run"); return;
		}
		const uint64_t shadow_end = golden.offset + uint64_t(ticket->page_count) * config.page_splats * sizeof(PackedGaussianQuantized);
		if (shadow_end > uint64_t(old_snapshot.atlas.size())) {
			FAIL("D3-3 GPU shadow reservation must fit the actual atlas buffer"); return;
		}
		for (uint32_t node = 0; node < nodes; ++node) {
			ChunkMetaGPU old_meta = {};
			memcpy(&old_meta, old_snapshot.metadata.ptr() + uint64_t(node) * sizeof(ChunkMetaGPU), sizeof(old_meta));
			const uint64_t old_begin = uint64_t(old_meta.atlas_base) * sizeof(PackedGaussianQuantized);
			const uint64_t old_end = old_begin + uint64_t(old_meta.splat_count) * sizeof(PackedGaussianQuantized);
			if (old_meta.splat_count != 0 && golden.offset < old_end && old_begin < shadow_end) {
				FAIL("D3-3 GPU invisible shadow reservation overlaps a visible old-revision atlas payload"); return;
			}
		}
		memcpy(new_snapshot.metadata.ptrw() + uint64_t(leaf) * sizeof(ChunkMetaGPU), &meta, sizeof(meta));
		memcpy(new_snapshot.quantization.ptrw() + uint64_t(leaf) * sizeof(ChunkQuantizationGPU), &quant, sizeof(quant));
		goldens.push_back(std::move(golden));
	}
	for (uint32_t stale : fixture.stale) {
		memset(new_snapshot.metadata.ptrw() + uint64_t(stale) * sizeof(ChunkMetaGPU), 0, sizeof(ChunkMetaGPU));
		memset(new_snapshot.quantization.ptrw() + uint64_t(stale) * sizeof(ChunkQuantizationGPU), 0, sizeof(ChunkQuantizationGPU));
	}
	if (check_revision(old_snapshot, old_snapshot, new_snapshot, false) != REVISION_VALID) {
		FAIL("D3-3 GPU revision checker must accept the complete real old readback"); return;
	}
	Snapshot mixed = old_snapshot;
	const uint64_t mixed_offset = uint64_t(fixture.leaves[0]) * sizeof(ChunkMetaGPU);
	memcpy(mixed.metadata.ptrw() + mixed_offset, new_snapshot.metadata.ptr() + mixed_offset, sizeof(ChunkMetaGPU));
	CHECK_MESSAGE(check_revision(mixed, old_snapshot, new_snapshot, false) == MIXED_REVISION,
			"D3-3 GPU revision checker must reject a mixed-revision readback (non-vacuity control)");
	bool failed = false, committed = false;
	uint64_t precommit_changed_bytes = 0, partial_frames = 0, observed_frames = 0;
	uint64_t write_frame = 0, frame_written_bytes = 0, precommit_atlas_writes = 0, commit_metadata_writes = 0;
	uint32_t complete_before_boundary = 0;
	auto verify_writes = [&]() {
		Vector<Streamer::TestWrite> writes;
		if (runtime.streamer._test_shadow_writes(writes) != OK) {
			FAIL("D3-3 phase-1 stub: GPU ordered write observation is unavailable"); failed = true; return false;
		}
		if (writes.size() < writes_seen) {
			FAIL("D3-3 GPU ordered write observer must remain cumulative through commit"); failed = true; return false;
		}
		for (int64_t index = writes_seen; index < writes.size(); ++index) {
			const Streamer::TestWrite &write = writes[index];
			if (write.frame < write_frame || write.frame == 0 || write.bytes == 0) {
				FAIL("D3-3 GPU actual write observations must have monotonic frame order and nonempty ranges"); failed = true; return false;
			}
			if (write.frame != write_frame) {
				write_frame = write.frame;
				frame_written_bytes = 0;
			}
			if (write.bytes > config.upload_bytes_per_frame - frame_written_bytes) {
				FAIL("D3-3 GPU actual payload plus metadata/quantization writes exceeded the unchanged frame allowance"); failed = true; return false;
			}
			frame_written_bytes += write.bytes;
			if (write.kind == Streamer::TestWrite::ATLAS) {
				if (!write.shadow || write.during_commit || committed) {
					FAIL("D3-3 GPU edited payloads must transfer into shadow runs during update before commit"); failed = true; return false;
				}
				const ShadowGolden *golden = nullptr;
				for (const ShadowGolden &candidate : goldens) {
					if (candidate.ticket.node == write.node) { golden = &candidate; break; }
				}
				if (!golden) {
					FAIL("D3-3 GPU payload write names no edited leaf with a reserved shadow run"); failed = true; return false;
				}
				if (write.offset < golden->offset || write.offset - golden->offset > uint64_t(golden->packed.size()) ||
						write.bytes > uint64_t(golden->packed.size()) - (write.offset - golden->offset)) {
					FAIL("D3-3 GPU payload write escaped the atlas payload span of its reserved shadow run"); failed = true; return false;
				}
				++precommit_atlas_writes;
			} else {
				if (!write.during_commit || !committed) {
					FAIL("D3-3 GPU edit metadata and quantization may be written only at the commit boundary"); failed = true; return false;
				}
				++commit_metadata_writes;
			}
		}
		writes_seen = writes.size();
		return true;
	};
	auto observe = [&](bool p_boundary) {
		Snapshot observed;
		if (!read_frame(runtime, nodes, observed)) { failed = true; return false; }
		const RevisionViolation revision = check_revision(observed, old_snapshot, new_snapshot, committed);
		if (revision != REVISION_VALID) {
			FAIL("D3-3 GPU metadata/quantization escaped the atomic old/new revision boundary, violation=", int(revision));
			failed = true; return false;
		}
		uint32_t complete = 0;
		uint64_t changed = 0;
		if (!check_atlas(observed, old_snapshot, goldens, committed, complete, changed)) {
			FAIL("D3-3 GPU atlas changed outside reserved payload spans or differs from the packed golden"); failed = true; return false;
		}
		if (!committed) {
			precommit_changed_bytes = MAX(precommit_changed_bytes, changed);
			complete_before_boundary = complete;
			if (!p_boundary && complete > 0 && complete < goldens.size()) { ++partial_frames; }
		}
		++observed_frames;
		return true;
	};
	const auto transferred = TestGaussianSplatting::gs_pump_until([&]() {
		runtime.streamer.begin_frame(rd, &runtime.buffers);
		const Streamer::EditCommitResult result = runtime.streamer.get_last_commit_result();
		if (result == Streamer::COMMIT_DONE) {
			if (committed || complete_before_boundary != goldens.size()) {
				FAIL("D3-3 GPU commit must occur once, after every shadow payload was observed transferred in update");
				failed = true; return true;
			}
			committed = true;
		} else if (result != Streamer::COMMIT_NOTHING || !runtime.streamer.is_active()) {
			FAIL("D3-3 GPU incremental edit unexpectedly cancelled or became unusable"); failed = true; return true;
		}
		if (!observe(true)) { return true; }
		update();
		if (runtime.streamer.get_telemetry().uploaded_bytes_this_frame > config.upload_bytes_per_frame) {
			FAIL("D3-3 GPU shadow transfers exceeded the unchanged per-frame upload allowance"); failed = true; return true;
		}
		if (!observe(false)) { return true; }
		if (!verify_writes()) { return true; }
		return committed;
	});
	if (!transferred.ready()) { FAIL("D3-3 GPU edit larger than its upload allowance never committed: ", transferred.describe()); return; }
	if (failed) { return; }
	CHECK_MESSAGE(committed, "D3-3 GPU incremental edit must reach its begin_frame commit");
	CHECK_MESSAGE(precommit_changed_bytes > 0, "D3-3 GPU atlas must contain shadow payload writes before commit");
	CHECK_MESSAGE(precommit_atlas_writes > 0, "D3-3 GPU exact upload accounting must observe real shadow atlas writes before commit");
	CHECK_MESSAGE(commit_metadata_writes == uint64_t(fixture.leaves.size() + fixture.stale.size()) * 2u,
			"D3-3 GPU commit must publish each edited leaf and clear every stale ancestor in both metadata buffers");
	CHECK_MESSAGE(partial_frames > 0, "D3-3 GPU edit must be observed spanning multiple incremental transfer frames");
	CHECK_MESSAGE(observed_frames > 2, "D3-3 GPU revision assertion must inspect multiple real asynchronous frame readbacks");
	CHECK_MESSAGE(runtime.streamer.get_data() == fixture.edited, "D3-3 GPU commit must serve the edited snapshot");
}

} // namespace TestHlodEditShadowTransferGpu
