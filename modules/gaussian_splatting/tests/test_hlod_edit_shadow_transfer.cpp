extern "C" int test_hlod_edit_shadow_transfer_cpp_force_link() { return 0; }

#include "gs_hlod_edit_shadow_test_fixture.h"
#include "gs_hlod_edit_test_pump.h"
#include "../io/gs_payload_page_store.h"

#include <initializer_list>

namespace TestHlodEditShadowTransfer {

static bool contains(const Vector<uint32_t> &p_nodes, uint32_t p_node) {
	for (uint32_t node : p_nodes) { if (node == p_node) { return true; } }
	return false;
}

static uint64_t retained(const Streamer::TestSnapshot &p) {
	return p.stored_bytes + p.decode_bytes + p.idle_decoder_bytes + p.worker_output_bytes +
			p.mailbox_bytes + p.ready_bytes + p.partial_upload_bytes + p.shadow_worker_output_bytes +
			p.shadow_mailbox_bytes + p.shadow_ready_bytes + p.shadow_partial_upload_bytes;
}

// Same accounting checker for positive progress, cancellation and the real
// leak-permit forcing control. Storage is observed separately from its charge.
static bool accounting_ok(const Streamer::TestSnapshot &p, bool p_settled = false) {
	if (p.observation_error != OK || p.shadow_observation_error != OK ||
			p.reservations_released > p.reservations_acquired ||
			p.live_reservations != p.reservations_acquired - p.reservations_released ||
			p.shadow_live_reservations > p.live_reservations || retained(p) > p.charged_bytes ||
			p.shadow_pending_payload_bytes != 0 || p.shadow_descriptor_payload_bytes != 0) { return false; }
	return !p_settled || (p.live_reservations == 0 && p.shadow_live_reservations == 0 && retained(p) == 0);
}

static bool bounded(const Streamer::TestSnapshot &p, const Streamer::Config &p_cfg, uint64_t p_cap) {
	if (!accounting_ok(p) || p.jobs_in_flight > p_cfg.max_jobs_in_flight ||
			p.peak_jobs_in_flight > p_cfg.max_jobs_in_flight || p.shadow_ready_outputs > p_cfg.max_jobs_in_flight) { return false; }
	if (p.oversized_reservation_bytes != 0) {
		return p.oversized_reservation_bytes > p_cfg.max_inflight_read_bytes && p.live_reservations == 1 &&
				p.jobs_in_flight <= 1 && p.charged_bytes <= p.oversized_reservation_bytes;
	}
	return p.charged_bytes <= p_cap && retained(p) <= p_cap;
}

static bool ready_after_transfer(const Streamer::TestSnapshot &p, bool p_ready, Streamer::EditCommitResult p_commit,
		uint32_t p_leaves, uint64_t p_payload_bytes) {
	return !(p_ready || p_commit == Streamer::COMMIT_DONE) ||
			(p.shadow_transfer_count == p_leaves && p.shadow_transferred_bytes == p_payload_bytes);
}

enum RevisionViolation { REVISION_OK, REVISION_SHAPE, EARLY_METADATA, MIXED_REVISION, WRONG_REVISION, BAD_SHADOW_RANGE, FRAME_ALLOWANCE };

struct Harness {
	Fixture fixture; // Destroy streamer/jobs before their immutable source.
	PayloadPageStore store;
	Streamer streamer;
	Streamer::Config cfg;
	Streamer::TestControl control;
	Vector<Residency::Ticket> runs;
	Vector<ChunkMetaGPU> old_meta, new_meta;
	Vector<ChunkQuantizationGPU> old_quant, new_quant;
	LocalVector<gs_hlod_runtime::DesiredRecord> demand;
	LocalVector<gs_hlod_runtime::DrawRecord> drawn;
	Vector<Streamer::TestWrite> writes;
	uint32_t baseline_writes = 0;
	uint64_t total_payload_bytes = 0;
	Streamer::TestSnapshot sample;
	Streamer::EditCommitResult commit = Streamer::COMMIT_NOTHING;
	bool observation_ok = true;
	bool ordinary_frame_ok = true;
	bool force_timeout_frames = false; // Only J's deliberate timeout wait ages worker polls.
	uint32_t observed_publications = 0;

	~Harness() { streamer.shutdown(); store.shutdown(); } // Release deliberate holds before joining.

	bool init(bool p_quantized = true, bool p_identity = false, bool p_small_publication = false,
			uint32_t p_edit_count = 16, bool p_competing_jobs = false, bool p_store_path = false, bool p_default_envelope = false,
			bool p_oversized_identity = false) {
		if (!WorkerThreadPool::get_singleton()) { FAIL("D3-3 needs the real worker pool"); return false; }
		if (!fixture.build(4, 256, p_edit_count)) { return false; }
		cfg = fixture.config(p_quantized);
		if (p_identity) {
			cfg.max_jobs_in_flight = 1; // Removes worker completion order from the identity oracle.
			const uint64_t stride = p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
			// Keep the original whole-node write identity oracle for fitting nodes;
			// exercise the replaced oversized exception separately in case H.
			cfg.upload_bytes_per_frame = p_oversized_identity ? uint64_t(fixture.source.splats_per_node - 1) * stride :
					uint64_t(fixture.source.splats_per_node) * stride + sizeof(ChunkMetaGPU) +
							(p_quantized ? sizeof(ChunkQuantizationGPU) : 0);
		}
		if (p_competing_jobs) {
			uint64_t gaussians = 0, output = 0, shadow = 0;
			const uint64_t stride = p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
			if (!gs_hlod_runtime::allocation_bytes(fixture.source.splats_per_node, sizeof(Gaussian), gaussians) ||
					!gs_hlod_runtime::allocation_bytes(fixture.source.splats_per_node, stride, output) ||
					!gs_hlod_runtime::allocation_bytes(fixture.source.splats_per_node / 2, stride, shadow)) {
				FAIL("D3-3 competing reservation oracle overflow"); return false;
			}
			const uint64_t legacy_read = fixture.source.source->get_node_read_working_set_bytes(fixture.leaves[0]);
			cfg.max_inflight_read_bytes = MAX(legacy_read + uint64_t(fixture.source.splats_per_node) * stride, gaussians + output) + shadow;
		}
		if (p_default_envelope) { cfg.max_inflight_read_bytes = Streamer::Config().max_inflight_read_bytes; }
		if (p_small_publication) { cfg.upload_bytes_per_frame = fixture.publication_bytes(p_quantized) - 1; }
		if (p_store_path) {
			PayloadPageStore::Config sc;
			sc.reader_count = cfg.max_jobs_in_flight;
			sc.max_bytes_in_flight = MAX(cfg.max_inflight_read_bytes, fixture.source.source->get_max_record_bytes());
			sc.max_idle_handles = 0;
			sc.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
			sc.decoder_retention_cap = 0;
			if (store.start(sc) != OK || streamer.init_with_page_store(fixture.source.snapshot, cfg, 21, 41, store) != OK) {
				FAIL("D3-3 real page-store streamer init failed"); return false;
			}
		} else if (!streamer.init(fixture.source.snapshot, cfg, 21, 41)) { FAIL("D3-3 real streamer init failed"); return false; }
		control.cpu_upload_sink = true;
		if (!set_control()) { return false; }
		if (p_identity && streamer._test_observe_shadow_transfers() != OK) {
			FAIL("D3-3 identity needs the actual ordinary write trace"); return false;
		}
		gs_hlod_runtime::CutRequest view;
		view.frustum_enabled = false;
		view.camera = Vector3(0, 0, 1000);
		view.focal_pixels = 100; // CutHistory rejects the zero default even with frustum culling disabled.
		view.node_budget = fixture.source.node_count;
		view.splat_budget = 1u << 30;
		view.record_budget = 1.0e9;
		view.tau_pixels = 1.0e9;
		streamer.register_view(1, view);
		for (uint32_t node = 0; node < fixture.source.node_count; ++node) {
			gs_hlod_runtime::DesiredRecord record;
			record.node = node;
			demand.push_back(record);
		}
		return true;
	}

	bool set_control() {
		if (streamer._test_set_control(control) != OK) { FAIL("D3-3 real upload/worker control unavailable"); return false; }
		return true;
	}

	bool probe() {
		const Error err = streamer._test_observe_shadow_transfers();
		CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: incremental shadow transfer observation unavailable");
		return err == OK;
	}

	void step(bool p_commit = true, bool p_observe = true) {
		const Streamer::Buffers buffers;
		commit = TestGaussianSplatting::gs_hlod_edit_begin_frame(streamer, nullptr, p_commit ? &buffers : nullptr, force_timeout_frames);
		streamer.update(nullptr, buffers, demand.ptr(), demand.size(), drawn.ptr(), drawn.size());
		if (!force_timeout_frames) {
			const auto &telemetry = streamer.get_telemetry();
			CHECK(telemetry.edit_timeouts_total == 0);
			CHECK(String(telemetry.edit_event) != "timeout");
			if (telemetry.edit_timeouts_total != 0 || String(telemetry.edit_event) == "timeout") { observation_ok = false; }
		}
		sample = streamer._test_snapshot();
		if (p_observe) {
			const Error err = streamer._test_shadow_writes(writes);
			observation_ok = observation_ok && err == OK && sample.shadow_observation_error == OK;
		} else if (cfg.max_jobs_in_flight == 1) {
			const auto publications = streamer._test_publications();
			const uint32_t count = publications.size() - observed_publications;
			const uint64_t node_bytes = uint64_t(fixture.source.splats_per_node) * streamer.record_stride();
			const uint32_t before = writes.size();
			Vector<uint64_t> transferred;
			if (streamer._test_shadow_writes(writes) != OK || writes.size() < before ||
					transferred.resize(fixture.source.node_count) != OK ||
					streamer.resident_mask().size() != fixture.source.node_count) {
				FAIL("D3-3 identity needs complete actual-write and residency observations"); observation_ok = false; return;
			}
			transferred.fill(0);
			uint64_t last_frame = UINT64_MAX, frame_bytes = 0, payload_this_frame = 0;
			for (uint32_t i = 0; i < uint32_t(writes.size()); ++i) {
				const auto &write = writes[i];
				if (write.node >= fixture.source.node_count) {
					FAIL("D3-3 identity write escaped the demanded node set"); observation_ok = false; return;
				}
				if (write.frame != last_frame) { last_frame = write.frame; frame_bytes = 0; }
				ordinary_frame_ok = ordinary_frame_ok && write.bytes <= cfg.upload_bytes_per_frame &&
						frame_bytes <= cfg.upload_bytes_per_frame - write.bytes;
				frame_bytes += write.bytes;
				if (write.kind == Streamer::TestWrite::ATLAS) {
					transferred.write[write.node] += write.bytes;
					if (i >= before) { payload_this_frame += write.bytes; }
				} else { ordinary_frame_ok = ordinary_frame_ok && transferred[write.node] == node_bytes; }
			}
			for (uint32_t node = 0; node < fixture.source.node_count; ++node) {
				if (transferred[node] > 0 && transferred[node] < node_bytes) {
					const auto state = streamer.get_residency().state(node);
					ordinary_frame_ok = ordinary_frame_ok && streamer.resident_mask()[node] == 0 &&
							(state == Residency::LOADING || state == Residency::UPLOADING);
				}
			}
			// Payload telemetry includes partial slices; completion telemetry counts
			// only metadata publication, and every real write consumes the allowance.
			ordinary_frame_ok = ordinary_frame_ok && count <= 1 &&
					streamer.get_telemetry().uploaded_bytes_this_frame == payload_this_frame &&
					streamer.get_telemetry().uploaded_this_frame == count;
			observed_publications = publications.size();
		}
	}

	bool warm(bool p_observe = true) {
		const auto loaded = gs_pump_until([&]() {
			step(false, p_observe);
			if (!observation_ok) { return true; }
			for (uint32_t node = 0; node < fixture.source.node_count; ++node) {
				if (streamer.get_residency().state(node) != Residency::RESIDENT) { return false; }
			}
			return sample.live_reservations == 0;
		});
		if (!loaded.ready() || !observation_ok) { FAIL("D3-3 old revision never became resident: ", loaded.describe()); return false; }
		if (old_meta.resize(fixture.source.node_count) != OK || old_quant.resize(fixture.source.node_count) != OK) {
			FAIL("D3-3 metadata snapshot allocation failed"); return false;
		}
		old_meta.fill(ChunkMetaGPU{});
		old_quant.fill(ChunkQuantizationGPU{});
		// Read actual CPU sink observations; never invent the old GPU revision.
		for (const auto &publication : streamer._test_publications()) {
			for (const auto &write : publication.writes) {
				if (write.node >= fixture.source.node_count) { FAIL("D3-3 ordinary sink wrote outside metadata"); return false; }
				if (write.kind == Streamer::TestWrite::METADATA) { old_meta.write[write.node] = write.meta; }
				if (write.kind == Streamer::TestWrite::QUANTIZATION) { old_quant.write[write.node] = write.quant; }
			}
		}
		for (uint32_t node = 0; node < fixture.source.node_count; ++node) {
			if (old_meta[node].splat_count == 0) { FAIL("D3-3 old metadata sink must be nonvacuous"); return false; }
			gs_hlod_runtime::DrawRecord draw;
			draw.node = node;
			drawn.push_back(draw);
		}
		baseline_writes = writes.size();
		return true;
	}

	bool stage() {
		if (streamer.stage_edit(fixture.edited) != Streamer::EDIT_STAGED) { FAIL("D3-3 feasible edit must stage whole"); return false; }
		const Error err = streamer._test_shadow_runs(runs);
		CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: reserved shadow run observation unavailable");
		if (err != OK) { return false; }
		if (runs.size() != fixture.leaves.size()) { FAIL("D3-3 needs exactly one reserved run per edited leaf"); return false; }
		new_meta = old_meta;
		new_quant = old_quant;
		for (uint32_t ancestor : fixture.stale) { new_meta.write[ancestor] = ChunkMetaGPU{}; new_quant.write[ancestor] = ChunkQuantizationGPU{}; }
		for (uint32_t i = 0; i < uint32_t(runs.size()); ++i) {
			const auto &run = runs[i];
			if (!run.valid() || !run.shadow || !contains(fixture.leaves, run.node) || run.page_count == 0 ||
					uint64_t(run.first_page) + run.page_count > cfg.atlas_pages) { FAIL("D3-3 invalid shadow reservation"); return false; }
			for (uint32_t j = 0; j < i; ++j) {
				if (run.node == runs[j].node || (run.first_page < runs[j].first_page + runs[j].page_count && runs[j].first_page < run.first_page + run.page_count)) {
					FAIL("D3-3 shadow reservations overlap"); return false;
				}
			}
			const uint64_t first = uint64_t(run.first_page) * cfg.page_splats;
			for (const auto &meta : old_meta) {
				if (meta.splat_count != 0 && first < uint64_t(meta.atlas_base) + meta.splat_count && meta.atlas_base < first + uint64_t(run.page_count) * cfg.page_splats) {
					FAIL("D3-3 shadow overlaps a served run"); return false;
				}
			}
			Vector<uint8_t> golden;
			if (!fixture.pack(run.node, true, cfg, uint32_t(first), golden, new_meta.write[run.node], new_quant.write[run.node])) { return false; }
			total_payload_bytes += golden.size();
		}
		return true;
	}

	// A single checker reconstructs the sink revision and checks write ranges,
	// order and frame bytes. Both positive cases and forcing controls call it.
	RevisionViolation revision(bool p_committed, bool p_require_old = false) const {
		if (writes.size() < baseline_writes || old_meta.size() != old_quant.size()) { return REVISION_SHAPE; }
		Vector<ChunkMetaGPU> meta = old_meta;
		Vector<ChunkQuantizationGPU> quant = old_quant;
		uint64_t last_frame = UINT64_MAX, spent = 0;
		Vector<uint64_t> transferred;
		if (transferred.resize(meta.size()) != OK) { return REVISION_SHAPE; }
		transferred.fill(0);
		for (uint32_t i = baseline_writes; i < uint32_t(writes.size()); ++i) {
			const auto &write = writes[i];
			if (write.node >= uint32_t(meta.size())) { return REVISION_SHAPE; }
			const bool affected = contains(fixture.leaves, write.node) || contains(fixture.stale, write.node);
			if (write.kind != Streamer::TestWrite::ATLAS && affected && (!write.during_commit || p_require_old)) { return EARLY_METADATA; }
			if (last_frame != UINT64_MAX && write.frame < last_frame) { return REVISION_SHAPE; }
			if (last_frame != write.frame) { last_frame = write.frame; spent = 0; }
			spent += write.bytes;
			if (spent > cfg.upload_bytes_per_frame) { return FRAME_ALLOWANCE; }
			if (write.kind == Streamer::TestWrite::ATLAS && (write.shadow || contains(fixture.leaves, write.node))) {
				if (!write.shadow || write.bytes == 0) { return BAD_SHADOW_RANGE; }
				bool in_run = false;
				for (const auto &run : runs) {
					const uint64_t first = uint64_t(run.first_page) * cfg.page_splats * streamer.record_stride();
					const uint64_t end = first + uint64_t(run.page_count) * cfg.page_splats * streamer.record_stride();
					if (run.node == write.node && write.offset == first + transferred[run.node] && write.offset <= end && write.bytes <= end - write.offset &&
							write.node < uint32_t(new_meta.size()) && transferred[run.node] + write.bytes <= uint64_t(new_meta[run.node].splat_count) * streamer.record_stride()) { in_run = true; }
				}
				if (!in_run || write.during_commit) { return BAD_SHADOW_RANGE; }
				transferred.write[write.node] += write.bytes;
			} else if (write.kind != Streamer::TestWrite::ATLAS) {
				if (affected && (!write.during_commit || p_require_old)) { return EARLY_METADATA; }
				if (write.kind == Streamer::TestWrite::METADATA) { meta.write[write.node] = write.meta; }
				if (write.kind == Streamer::TestWrite::QUANTIZATION) { quant.write[write.node] = write.quant; }
				// Even another node may not name an invisible run.
				if (!write.during_commit) {
					for (const auto &run : runs) {
						const uint64_t first = uint64_t(run.first_page) * cfg.page_splats;
						const uint64_t end = first + uint64_t(run.page_count) * cfg.page_splats;
						const uint64_t base = write.kind == Streamer::TestWrite::METADATA ? write.meta.atlas_base : write.quant.start_index;
						if (base >= first && base < end) { return EARLY_METADATA; }
					}
				}
			}
		}
		bool saw_old = false, saw_new = false;
		for (uint32_t node = 0; node < uint32_t(meta.size()); ++node) {
			if (!contains(fixture.leaves, node) && !contains(fixture.stale, node)) { continue; }
			const bool old = memcmp(&meta[node], &old_meta[node], sizeof(ChunkMetaGPU)) == 0 &&
					(!cfg.quantized || memcmp(&quant[node], &old_quant[node], sizeof(ChunkQuantizationGPU)) == 0);
			const bool next = new_meta.size() == meta.size() && memcmp(&meta[node], &new_meta[node], sizeof(ChunkMetaGPU)) == 0 &&
					(!cfg.quantized || memcmp(&quant[node], &new_quant[node], sizeof(ChunkQuantizationGPU)) == 0);
			if (!old && !next) { return WRONG_REVISION; }
			if (p_committed && contains(fixture.leaves, node) && transferred[node] != uint64_t(new_meta[node].splat_count) * streamer.record_stride()) { return BAD_SHADOW_RANGE; }
			saw_old = saw_old || old;
			saw_new = saw_new || next;
		}
		if (saw_old && saw_new) { return MIXED_REVISION; }
		return (p_committed ? saw_new : saw_old) ? REVISION_OK : WRONG_REVISION;
	}

	bool finish() {
		const auto done = gs_pump_until([&]() {
			step();
			CHECK(observation_ok);
			CHECK(bounded(sample, cfg, streamer.get_retained_byte_cap()));
			CHECK(ready_after_transfer(sample, streamer.is_edit_ready(), commit, fixture.leaves.size(), total_payload_bytes));
			CHECK(revision(streamer.get_data() == fixture.edited) == REVISION_OK);
			return !observation_ok || commit == Streamer::COMMIT_DONE || !streamer.is_active();
		});
		if (!done.ready() || !observation_ok || commit != Streamer::COMMIT_DONE) { FAIL("D3-3 edit larger than its window never committed: ", done.describe()); return false; }
		CHECK(accounting_ok(sample, true));
		CHECK(streamer.get_data() == fixture.edited);
		CHECK(sample.shadow_transfer_count == fixture.leaves.size());
		return true;
	}
};

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] A edit larger than ready window drains without deadlock") {
	Harness h;
	if (!h.init() || !h.probe() || !h.warm() || !h.stage()) { return; }
	CHECK(h.total_payload_bytes > 3 * h.cfg.max_inflight_read_bytes);
	CHECK(h.total_payload_bytes > 3 * h.cfg.upload_bytes_per_frame);
	if (!h.finish()) { return; }
	CHECK(h.sample.shadow_ready_outputs == 0);
	CHECK(h.sample.shadow_descriptor_payload_bytes == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] B old revision serves until one begin_frame commit") {
	Harness h;
	if (!h.init() || !h.probe() || !h.warm() || !h.stage()) { return; }
	const auto transferred = gs_pump_until([&]() {
		h.step(false); // update transfers; publication deliberately has no boundary yet.
		CHECK(h.revision(false, true) == REVISION_OK);
		CHECK(h.streamer.get_data() == h.fixture.source.snapshot);
		return !h.observation_ok || h.sample.shadow_transfer_count == h.fixture.leaves.size();
	});
	if (!transferred.ready() || !h.observation_ok) { FAIL("D3-3 invisible shadow transfer never completed: ", transferred.describe()); return; }
	CHECK(h.streamer.is_edit_ready());
	CHECK(accounting_ok(h.sample, true));
	const uint32_t before = h.writes.size();
	h.step();
	CHECK(h.commit == Streamer::COMMIT_DONE);
	CHECK(h.revision(true) == REVISION_OK);
	for (uint32_t i = before; i < uint32_t(h.writes.size()); ++i) { CHECK(h.writes[i].kind != Streamer::TestWrite::ATLAS); }
	// The committed revision remains stable on subsequent frame boundaries.
	h.step();
	CHECK(h.revision(true) == REVISION_OK);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] C cancel mid transfer returns permits and shadow runs for reuse") {
	Harness h;
	if (!h.init() || !h.probe() || !h.warm() || !h.stage()) { return; }
	const uint32_t free_before = h.streamer.get_residency().stats().pages_free + h.streamer.get_residency().stats().pages_shadow;
	const auto partial = gs_pump_until([&]() {
		h.step(false);
		return !h.observation_ok || h.sample.shadow_transfer_count > 0;
	});
	if (!partial.ready() || !h.observation_ok) { FAIL("D3-3 never transferred a first shadow leaf: ", partial.describe()); return; }
	if (h.sample.shadow_transfer_count >= h.fixture.leaves.size()) { FAIL("D3-3 cancel must hit a partial transfer"); return; }
	h.control.hold_uploads = true;
	if (!h.set_control()) { return; }
	h.step(false);
	const auto held_count = h.sample.shadow_transfer_count;
	h.step(false);
	CHECK(h.sample.shadow_transfer_count == held_count);
	CHECK(h.streamer.cancel_staged_edit());
	CHECK_FALSE(h.streamer.cancel_staged_edit()); // Idempotent; cannot double-release.
	h.control.hold_uploads = false;
	if (!h.set_control()) { return; }
	const auto released = gs_pump_until([&]() {
		h.step(false);
		return !h.observation_ok || (h.sample.live_reservations == 0 && h.streamer.get_residency().stats().pages_retiring == 0);
	});
	if (!released.ready() || !h.observation_ok) { FAIL("D3-3 cancelled workers/runs never retired: ", released.describe()); return; }
	CHECK(accounting_ok(h.sample, true));
	CHECK(h.revision(false, true) == REVISION_OK);
	CHECK(h.streamer.get_data() == h.fixture.source.snapshot);
	CHECK(h.streamer.get_residency().stats().pages_free == free_before);
	// Prove allocator reuse by an actual ordinary reload, then another edit.
	h.drawn.clear();
	const uint32_t reload = h.fixture.leaves[0]; // stage() guarded nonempty cardinality.
	h.streamer.begin_frame(); // Drop the previous frame's protection before evicting.
	if (h.streamer._test_evict_node(reload) != OK) { FAIL("D3-3 reuse must evict an actual served leaf"); return; }
	const auto reloaded = gs_pump_until([&]() {
		h.step(false);
		return !h.observation_ok || h.streamer.get_residency().state(reload) == Residency::RESIDENT;
	});
	if (!reloaded.ready() || !h.observation_ok) { FAIL("D3-3 ordinary reload after cancel failed: ", reloaded.describe()); return; }
	h.total_payload_bytes = 0;
	// Recapture the actually reloaded old run and reset the observation interval.
	if (!h.warm() || !h.stage() || !h.finish()) { return; }
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] D metadata failure after transfer is COMMIT_UNUSABLE") {
	Harness h;
	if (!h.init() || !h.probe() || !h.warm() || !h.stage()) { return; }
	const auto transferred = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.streamer.is_edit_ready(); });
	if (!transferred.ready() || !h.observation_ok) { FAIL("D3-3 metadata failure needs complete transfer: ", transferred.describe()); return; }
	CHECK(h.sample.shadow_transfer_count == h.fixture.leaves.size());
	CHECK(accounting_ok(h.sample, true));
	h.streamer.test_fail_commit_metadata = true;
	h.step();
	CHECK(h.commit == Streamer::COMMIT_UNUSABLE);
	CHECK_FALSE(h.streamer.is_active());
	CHECK(accounting_ok(h.sample, true));
	CHECK(h.streamer.get_data() == h.fixture.source.snapshot);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] E shadow upload failure cancels without publication") {
	Harness h;
	if (!h.init() || !h.probe() || !h.warm() || !h.stage()) { return; }
	h.control.hold_uploads = true;
	if (!h.set_control()) { return; }
	const auto packed = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.sample.shadow_ready_outputs > 0; });
	if (!packed.ready() || !h.observation_ok) { FAIL("D3-3 upload failure needs a packed shadow: ", packed.describe()); return; }
	h.control.hold_uploads = false;
	h.control.fail_upload = true;
	if (!h.set_control()) { return; }
	const auto cancelled = gs_pump_until([&]() { h.step(false); return !h.observation_ok || !h.streamer.is_edit_staged(); });
	if (!cancelled.ready() || !h.observation_ok) { FAIL("D3-3 shadow upload failure never cancelled: ", cancelled.describe()); return; }
	CHECK(String(h.streamer.get_telemetry().edit_event) == "upload_failed");
	CHECK(h.streamer.get_telemetry().edit_commit_failures_total > 0);
	CHECK(h.streamer.get_data() == h.fixture.source.snapshot);
	CHECK(h.revision(false, true) == REVISION_OK);
	h.step(false); // Transfer failure is reported at the next real begin_frame boundary.
	CHECK(h.commit == Streamer::COMMIT_UPLOAD_FAILED);
	const auto drained = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.sample.live_reservations == 0; });
	if (!drained.ready() || !h.observation_ok) { FAIL("D3-3 failed transfer permits never drained: ", drained.describe()); return; }
	CHECK(accounting_ok(h.sample, true));
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] F final publication must fit the unchanged frame allowance") {
	Harness h;
	if (!h.init(true, false, true) || !h.probe() || !h.warm()) { return; }
	const auto before = h.streamer.get_residency().stats();
	const auto acquired = h.sample.reservations_acquired;
	const auto result = h.streamer.stage_edit(h.fixture.edited);
	CHECK(result == Streamer::EDIT_REJECTED_INFEASIBLE);
	CHECK_FALSE(h.streamer.is_edit_staged());
	CHECK(h.streamer.get_data() == h.fixture.source.snapshot);
	CHECK(h.streamer.get_residency().stats().pages_free == before.pages_free);
	CHECK(h.streamer.get_residency().stats().pages_shadow == 0);
	CHECK(h.streamer._test_snapshot().reservations_acquired == acquired);
	CHECK(h.streamer._test_shadow_writes(h.writes) == OK);
	CHECK(h.writes.size() == h.baseline_writes);
	CHECK(h.revision(false, true) == REVISION_OK);
	// The exact same snapshot/view/edit is feasible at the frozen defaults.
	Streamer normal;
	Streamer::Config defaults = h.cfg;
	defaults.upload_bytes_per_frame = Streamer::Config().upload_bytes_per_frame;
	if (!normal.init(h.fixture.source.snapshot, defaults, 21, 41)) { FAIL("D3-3 default allowance init failed"); return; }
	gs_hlod_runtime::CutRequest view;
	view.frustum_enabled = false;
	view.camera = Vector3(0, 0, 1000);
	view.focal_pixels = 100;
	view.tau_pixels = 1.0e9;
	view.node_budget = h.fixture.source.node_count;
	view.splat_budget = 1u << 30;
	view.record_budget = 1.0e9;
	normal.register_view(1, view);
	CHECK(normal.stage_edit(h.fixture.edited) == Streamer::EDIT_STAGED);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] G held shadow and ordinary jobs share one reservation envelope") {
	for (bool default_envelope : { false, true }) {
		Harness h;
		if (!h.init(true, false, false, 16, true, false, default_envelope) || !h.probe() || !h.warm()) { return; }
		h.drawn.clear();
		// One variant saturates bytes; the other has the default byte envelope
		// and saturates jobs, so deleting either shared limit is observable.
		const uint32_t ordinary = h.fixture.leaves[0];
		h.streamer.begin_frame();
		if (h.streamer._test_evict_node(ordinary) != OK) { FAIL("D3-3 ordinary competition must evict a real leaf"); return; }
		if (h.streamer._test_evict_node(h.fixture.leaves[1]) != OK) { FAIL("D3-3 ordinary competition needs a second pending leaf"); return; }
		h.control.hold_workers = true;
		h.control.hold_uploads = true;
		if (!h.set_control()) { return; }
		h.step(false); // Ordinary work precedes shadow work in the same ledger.
		if (!h.stage()) { return; }
		const auto held = gs_pump_until([&]() {
			h.step(false);
			CHECK(bounded(h.sample, h.cfg, h.streamer.get_retained_byte_cap()));
			const bool saturated = default_envelope ? h.sample.jobs_in_flight == h.cfg.max_jobs_in_flight : h.sample.pending_tickets > 0;
			return !h.observation_ok || (h.sample.shadow_live_reservations > 0 && h.sample.shadow_pending_tickets > 0 && saturated);
		});
		if (!held.ready() || !h.observation_ok) { FAIL("D3-3 shared envelope never held shadow and ordinary demand: ", held.describe()); return; }
		CHECK(h.sample.jobs_in_flight <= h.cfg.max_jobs_in_flight);
		CHECK(h.sample.jobs_in_flight == h.sample.live_reservations);
		CHECK(h.sample.live_reservations > h.sample.shadow_live_reservations);
		CHECK(h.sample.shadow_worker_output_bytes == 0);
		CHECK(h.sample.shadow_pending_payload_bytes == 0);
		CHECK(h.sample.shadow_transfer_count == 0);
		CHECK_FALSE(h.streamer.is_edit_ready());
		h.streamer.shutdown(); // Held workers cannot deadlock teardown.
		CHECK(accounting_ok(h.streamer._test_snapshot(), true));
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] H no edit preserves ordinary upload and metadata identity") {
	for (bool quantized : { false, true }) {
		Harness h;
		if (!h.init(quantized, true) || !h.warm(false)) { return; }
		const auto publications = h.streamer._test_publications();
		if (publications.size() != h.fixture.source.node_count) { FAIL("D3-3 identity needs one publication per demanded node"); return; }
		uint64_t total = 0;
		const uint32_t pages = (h.fixture.source.splats_per_node + h.cfg.page_splats - 1) / h.cfg.page_splats;
		for (uint32_t node = 0; node < h.fixture.source.node_count; ++node) {
			const auto &publication = publications[node];
			const auto &writes = publication.writes;
			if (writes.size() != (quantized ? 3 : 2)) { FAIL("D3-3 ordinary sink must observe every atlas/quant/meta write in order"); return; }
			Vector<uint8_t> expected;
			ChunkMetaGPU meta = {};
			ChunkQuantizationGPU quant = {};
			if (!h.fixture.pack(node, false, h.cfg, node * pages * h.cfg.page_splats, expected, meta, quant)) { return; }
			CHECK(publication.node == node); // max_jobs=1: original demand/admission order.
			CHECK(publication.metadata_splat_count == h.fixture.source.splats_per_node);
			CHECK(writes[0].kind == Streamer::TestWrite::ATLAS);
			CHECK(writes[0].node == node);
			CHECK(writes[0].offset == uint64_t(meta.atlas_base) * h.streamer.record_stride());
			CHECK(writes[0].bytes == uint64_t(expected.size()));
			CHECK_FALSE(writes[0].shadow);
			if (quantized) {
				CHECK(writes[1].kind == Streamer::TestWrite::QUANTIZATION);
				CHECK(writes[1].offset == uint64_t(node) * sizeof(ChunkQuantizationGPU));
				CHECK(writes[1].bytes == sizeof(ChunkQuantizationGPU));
				CHECK(memcmp(&writes[1].quant, &quant, sizeof(quant)) == 0);
			}
			const auto &last = writes[writes.size() - 1];
			CHECK(last.kind == Streamer::TestWrite::METADATA);
			CHECK(last.offset == uint64_t(node) * sizeof(ChunkMetaGPU));
			CHECK(last.bytes == sizeof(ChunkMetaGPU));
			CHECK(memcmp(&last.meta, &meta, sizeof(meta)) == 0);
			total += expected.size();
		}
		CHECK(h.streamer.get_telemetry().uploaded_bytes_total == total);
		CHECK(h.ordinary_frame_ok);
		CHECK_FALSE(h.streamer.is_edit_staged());
		CHECK(h.sample.live_reservations == 0);
	}
	for (bool quantized : { false, true }) {
		Harness h;
		if (!h.init(quantized, true, false, 16, false, false, false, true) || !h.warm(false)) { return; }
		const auto publications = h.streamer._test_publications();
		if (publications.size() != h.fixture.source.node_count) { FAIL("D3-4 bounded identity needs one publication per demanded node"); return; }
		uint64_t total = 0;
		const uint64_t stride = h.streamer.record_stride();
		const uint32_t pages = (h.fixture.source.splats_per_node + h.cfg.page_splats - 1) / h.cfg.page_splats;
		for (uint32_t node = 0; node < h.fixture.source.node_count; ++node) {
			const auto &publication = publications[node];
			const auto &writes = publication.writes;
			const uint32_t publication_writes = quantized ? 2 : 1;
			if (uint32_t(writes.size()) < publication_writes + 2) { FAIL("D3-4 oversized ordinary node must use multiple actual atlas writes"); return; }
			const uint32_t atlas_writes = writes.size() - publication_writes;
			Vector<uint8_t> expected;
			ChunkMetaGPU meta = {};
			ChunkQuantizationGPU quant = {};
			if (!h.fixture.pack(node, false, h.cfg, node * pages * h.cfg.page_splats, expected, meta, quant)) { return; }
			CHECK(publication.node == node);
			CHECK(publication.metadata_splat_count == h.fixture.source.splats_per_node);
			uint64_t transferred = 0;
			for (uint32_t i = 0; i < atlas_writes; ++i) {
				const auto &write = writes[i];
				CHECK(write.kind == Streamer::TestWrite::ATLAS);
				CHECK(write.node == node);
				CHECK(write.offset == uint64_t(meta.atlas_base) * stride + transferred);
				CHECK(write.bytes > 0);
				CHECK(write.bytes % stride == 0);
				CHECK(write.bytes <= h.cfg.upload_bytes_per_frame);
				CHECK_FALSE(write.shadow);
				CHECK_FALSE(write.during_commit);
				if (transferred > uint64_t(expected.size()) || write.bytes > uint64_t(expected.size()) - transferred ||
						uint64_t(write.payload.size()) != write.bytes) {
					FAIL("D3-4 bounded identity slice escaped the packed golden or lost actual payload capture"); return;
				}
				CHECK(memcmp(write.payload.ptr(), expected.ptr() + transferred, size_t(write.bytes)) == 0);
				if (i > 0) { CHECK(write.frame > writes[i - 1].frame); }
				transferred += write.bytes;
			}
			CHECK(transferred == uint64_t(expected.size()));
			if (quantized) {
				CHECK(writes[atlas_writes].kind == Streamer::TestWrite::QUANTIZATION);
				CHECK(writes[atlas_writes].offset == uint64_t(node) * sizeof(ChunkQuantizationGPU));
				CHECK(writes[atlas_writes].bytes == sizeof(ChunkQuantizationGPU));
				CHECK(memcmp(&writes[atlas_writes].quant, &quant, sizeof(quant)) == 0);
			}
			const auto &last = writes[writes.size() - 1];
			CHECK(last.kind == Streamer::TestWrite::METADATA);
			CHECK(last.offset == uint64_t(node) * sizeof(ChunkMetaGPU));
			CHECK(last.bytes == sizeof(ChunkMetaGPU));
			CHECK(memcmp(&last.meta, &meta, sizeof(meta)) == 0);
			CHECK(last.frame >= writes[atlas_writes - 1].frame);
			total += expected.size();
		}
		CHECK(h.streamer.get_telemetry().uploaded_bytes_total == total);
		CHECK(h.ordinary_frame_ok);
		CHECK_FALSE(h.streamer.is_edit_staged());
		CHECK(h.sample.live_reservations == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] I packing with uploads held does not make an edit ready") {
	Harness h;
	if (!h.init(true, false, false, 1) || !h.probe() || !h.warm()) { return; }
	h.control.hold_uploads = true;
	if (!h.set_control() || !h.stage()) { return; }
	const auto packed = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.sample.shadow_ready_outputs == 1; });
	if (!packed.ready() || !h.observation_ok) { FAIL("D3-3 held shadow never became packed: ", packed.describe()); return; }
	CHECK(h.sample.shadow_ready_bytes > 0);
	CHECK(h.sample.shadow_transfer_count == 0);
	CHECK_FALSE(h.streamer.is_edit_ready());
	CHECK(ready_after_transfer(h.sample, h.streamer.is_edit_ready(), h.commit, h.fixture.leaves.size(), h.total_payload_bytes));
	CHECK(h.revision(false, true) == REVISION_OK);
	h.control.hold_uploads = false;
	if (!h.set_control() || !h.finish()) { return; }
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] J lifecycle exits drain pending worker packed and partial owners") {
	enum Exit { CANCEL, TIMEOUT, BLOCKED, SHUTDOWN, SOURCE_GENERATION, DEVICE_GENERATION };
	enum Owner { PENDING, WORKER, PACKED, PARTIAL };
	for (Exit exit : { CANCEL, TIMEOUT, BLOCKED, SHUTDOWN, SOURCE_GENERATION, DEVICE_GENERATION }) {
		for (Owner owner : { PENDING, WORKER, PACKED, PARTIAL }) {
			INFO("D3-3 lifecycle exit=", int(exit), " owner=", int(owner));
			Harness h;
			if (!h.init(true, false, false, 16, false, true) || !h.probe() || !h.warm()) { return; }
			const uint64_t before = h.sample.reservations_acquired;
			h.control.hold_workers = owner == PENDING || owner == WORKER;
			h.control.hold_uploads = owner != PARTIAL;
			if (!h.set_control() || !h.stage()) { return; }
			if (owner == PENDING) {
				h.sample = h.streamer._test_snapshot();
				CHECK(h.sample.shadow_pending_tickets == h.fixture.leaves.size());
				CHECK(h.sample.reservations_acquired == before);
			} else {
				const auto reached = gs_pump_until([&]() {
					h.step(false);
					if (!h.observation_ok) { return true; }
					if (owner == WORKER) { return h.sample.jobs_in_flight > 0 && h.sample.shadow_live_reservations > 0; }
					if (owner == PACKED) { return h.sample.shadow_ready_outputs > 0; }
					return h.sample.shadow_transfer_count > 0;
				});
				if (!reached.ready() || !h.observation_ok) { FAIL("D3-3 lifecycle never reached the requested owner: ", reached.describe()); return; }
				CHECK(h.sample.reservations_acquired > before);
				if (owner == PARTIAL && h.sample.shadow_transfer_count >= h.fixture.leaves.size()) { FAIL("D3-3 lifecycle requires a partial transfer"); return; }
			}
			if (exit == TIMEOUT) { h.control.hold_uploads = true; }
			h.demand.clear();
			h.drawn.clear();
			if (exit == CANCEL) {
				if (!h.streamer.cancel_staged_edit()) { FAIL("D3-3 lifecycle cancel must kill the staged transaction"); return; }
			} else if (exit == SHUTDOWN) {
				h.streamer.shutdown();
			} else if (exit == SOURCE_GENERATION || exit == DEVICE_GENERATION) {
				const Error err = h.streamer._test_replace_source(h.fixture.source.snapshot, exit == SOURCE_GENERATION ? 22 : 21, exit == DEVICE_GENERATION ? 42 : 41);
				CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: epoch replacement with shadow owners unavailable");
				if (err != OK) { return; }
			} else if (exit == BLOCKED) {
				h.control.fail_validation = true;
			}
			if (exit != SHUTDOWN) {
				// Timeout retains its deliberate holds; all other paths must drain.
				if (exit != TIMEOUT) { h.control.hold_workers = h.control.hold_uploads = false; }
				if (!h.set_control()) { return; }
				h.force_timeout_frames = exit == TIMEOUT;
				const auto drained = gs_pump_until([&]() {
					h.step(false);
					if (exit == TIMEOUT && !h.streamer.is_edit_staged() && (h.control.hold_workers || h.control.hold_uploads)) {
						h.control.hold_workers = h.control.hold_uploads = false;
						if (!h.set_control()) { h.observation_ok = false; }
					}
					return !h.observation_ok || (!h.streamer.is_edit_staged() && h.sample.jobs_in_flight == 0 && retained(h.sample) == 0);
				});
				if (!drained.ready() || !h.observation_ok) { FAIL("D3-3 lifecycle never cancelled/drained: ", drained.describe()); return; }
				CHECK(h.streamer.get_data() == h.fixture.source.snapshot);
				if (exit == TIMEOUT) {
					CHECK(String(h.streamer.get_telemetry().edit_event) == "timeout");
					CHECK(h.streamer.get_telemetry().edit_timeouts_total == 1);
				}
				if (exit == BLOCKED) { CHECK(String(h.streamer.get_telemetry().edit_event) == "blocked"); }
			} else {
				h.sample = h.streamer._test_snapshot();
				CHECK(h.streamer._test_shadow_writes(h.writes) == OK);
			}
			CHECK(accounting_ok(h.sample, true));
			CHECK(h.sample.reservations_acquired == h.sample.reservations_released);
			CHECK(h.revision(false, true) == REVISION_OK);
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] forcing early shadow metadata trips the revision checker") {
	Harness h;
	if (!h.init()) { return; }
	const Error err = h.streamer._test_set_fault(Streamer::TEST_FAULT_EARLY_SHADOW_METADATA);
	CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: early shadow metadata fault unavailable");
	if (err != OK || !h.probe() || !h.warm() || !h.stage()) { return; }
	const auto detected = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.revision(false, true) == EARLY_METADATA; });
	if (!detected.ready() || !h.observation_ok) { FAIL("D3-3 early metadata forcing control was not detected: ", detected.describe()); return; }
	CHECK(h.sample.faults_triggered > 0);
	CHECK(h.revision(false, true) == EARLY_METADATA);
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] forcing ready on pack trips the transfer readiness checker") {
	Harness h;
	if (!h.init(true, false, false, 1)) { return; }
	const Error err = h.streamer._test_set_fault(Streamer::TEST_FAULT_READY_ON_PACK);
	CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: ready on pack fault unavailable");
	if (err != OK || !h.probe() || !h.warm()) { return; }
	h.control.hold_uploads = true;
	// All shadows can pack, but uploads remain held. This control exercises the
	// old finish_shadow->edit_ready mistake without an admission deadlock.
	if (!h.set_control() || !h.stage()) { return; }
	const auto detected = gs_pump_until([&]() {
		h.step(false);
		return !h.observation_ok || !ready_after_transfer(h.sample, h.streamer.is_edit_ready(), h.commit, h.fixture.leaves.size(), h.total_payload_bytes);
	});
	if (!detected.ready() || !h.observation_ok) { FAIL("D3-3 packed-only readiness forcing control was not detected: ", detected.describe()); return; }
	CHECK(h.sample.faults_triggered > 0);
	CHECK_FALSE(ready_after_transfer(h.sample, h.streamer.is_edit_ready(), h.commit, h.fixture.leaves.size(), h.total_payload_bytes));
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] forcing leaked shadow permit trips the accounting checker") {
	Harness h;
	if (!h.init()) { return; }
	const Error err = h.streamer._test_set_fault(Streamer::TEST_FAULT_LEAK_SHADOW_PERMIT);
	CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: leaked shadow permit fault unavailable");
	if (err != OK || !h.probe() || !h.warm() || !h.stage()) { return; }
	const auto transferred = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.sample.shadow_transfer_count > 0; });
	if (!transferred.ready() || !h.observation_ok) { FAIL("D3-3 leak forcing control never transferred a leaf: ", transferred.describe()); return; }
	CHECK(h.streamer.cancel_staged_edit());
	// Wait for actual worker/mailbox storage to drain, independently of permits
	// (the deliberately leaked token must not be the termination condition).
	const auto drained = gs_pump_until([&]() { h.step(false); return !h.observation_ok || (h.sample.jobs_in_flight == 0 && retained(h.sample) == 0); });
	if (!drained.ready() || !h.observation_ok) { FAIL("D3-3 leak forcing control storage never drained: ", drained.describe()); return; }
	CHECK(h.sample.faults_triggered > 0);
	CHECK(h.sample.shadow_live_reservations > 0);
	CHECK_FALSE(accounting_ok(h.sample, true));
}

TEST_CASE("[GaussianSplatting][HLOD][Edit][WorldIO][D3Shadow] forcing partial commit trips the mixed revision checker") {
	Harness h;
	if (!h.init()) { return; }
	const Error err = h.streamer._test_set_fault(Streamer::TEST_FAULT_PARTIAL_COMMIT);
	CHECK_MESSAGE(err == OK, "D3-3 phase-1 stub: partial commit fault unavailable");
	if (err != OK || !h.probe() || !h.warm() || !h.stage()) { return; }
	const auto transferred = gs_pump_until([&]() { h.step(false); return !h.observation_ok || h.streamer.is_edit_ready(); });
	if (!transferred.ready() || !h.observation_ok) { FAIL("D3-3 partial commit control never became ready: ", transferred.describe()); return; }
	h.step();
	CHECK(h.sample.faults_triggered > 0);
	CHECK(h.revision(true) == MIXED_REVISION);
}

} // namespace TestHlodEditShadowTransfer
