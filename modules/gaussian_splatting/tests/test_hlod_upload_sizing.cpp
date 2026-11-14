extern "C" int test_hlod_upload_sizing_cpp_force_link() { return 0; }

#include "gs_hlod_edit_shadow_test_fixture.h"
#include "../io/gs_payload_page_store.h"
#include "core/os/thread.h"

#include <cstring>
#include <initializer_list>
#include <map>

namespace TestHlodUploadSizing {

using Streamer = HlodNodeStreamer;
using Residency = gs_hlod_runtime::NodeResidency;
using TestGaussianSplatting::gs_pump_until;

enum Violation { CLEAN, FRAME_ALLOWANCE, EARLY_PUBLICATION, OUTSIDE_RUN, UNALIGNED, PAYLOAD_IDENTITY, DRAWN_EARLY };

// Independent oracle for the REAL write trace. Both healthy transfers and the
// two production-path fault controls use this checker, with identical inputs.
struct Checker {
	uint64_t allowance = 0, stride = 0, base = 0, payload_bytes = 0;
	uint32_t observed = 0, atlas_writes = 0, metadata_writes = 0, quant_writes = 0;
	bool quantized = false;
	Vector<uint8_t> atlas, golden;
	ChunkMetaGPU expected_meta = {};
	ChunkQuantizationGPU expected_quant = {};
	std::map<uint64_t, uint64_t> frame_bytes;
	Violation violation = CLEAN;

	Violation consume(const Vector<Streamer::TestWrite> &p_writes) {
		if (violation != CLEAN) { return violation; }
		for (; observed < uint32_t(p_writes.size()); ++observed) {
			const auto &w = p_writes[observed];
			uint64_t &spent = frame_bytes[w.frame];
			if (w.bytes > allowance || spent > allowance - w.bytes) { return violation = FRAME_ALLOWANCE; }
			spent += w.bytes; // Counts payload AND quantization/metadata.
			if (w.node != 0 || w.shadow) { return violation = OUTSIDE_RUN; }
			if (w.kind == Streamer::TestWrite::ATLAS) {
				if (w.offset != base + payload_bytes || payload_bytes > uint64_t(golden.size()) ||
						w.bytes > uint64_t(golden.size()) - payload_bytes) { return violation = OUTSIDE_RUN; }
				if (w.bytes == 0 || w.bytes % stride != 0 || (w.offset - base) % stride != 0) { return violation = UNALIGNED; }
				if (uint64_t(w.payload.size()) != w.bytes) { return violation = PAYLOAD_IDENTITY; }
				memcpy(atlas.ptrw() + payload_bytes, w.payload.ptr(), size_t(w.bytes));
				payload_bytes += w.bytes;
				++atlas_writes;
			} else {
				if (payload_bytes != uint64_t(golden.size())) { return violation = EARLY_PUBLICATION; }
				if (w.kind == Streamer::TestWrite::QUANTIZATION) {
					if (!quantized || metadata_writes != 0 || ++quant_writes != 1 ||
							memcmp(&w.quant, &expected_quant, sizeof(expected_quant)) != 0) { return violation = PAYLOAD_IDENTITY; }
				} else if (++metadata_writes != 1 || (quantized && quant_writes != 1) ||
						memcmp(&w.meta, &expected_meta, sizeof(expected_meta)) != 0) { return violation = PAYLOAD_IDENTITY; }
			}
		}
		return violation;
	}
};

struct Harness {
	TestHlodEditShadowTransfer::Fixture fixture;
	PayloadPageStore store;
	Streamer streamer;
	Streamer::Config cfg;
	Streamer::TestControl control;
	Streamer::TestSnapshot sample;
	Vector<Streamer::TestWrite> writes;
	gs_hlod_runtime::CutHistory history;
	gs_hlod_runtime::DesiredRecord demand;
	Checker checker;
	bool observations_ok = true, advance_frame = true, request_node = true;
	uint32_t selected_count = 0;
	uint32_t first_page = 0, page_count = 0;

	~Harness() { streamer.shutdown(); store.shutdown(); }

	bool init(bool p_quantized, uint64_t p_limit, uint64_t p_staging = 0,
			Streamer::TestFault p_fault = Streamer::TEST_FAULT_NONE) {
		if (!WorkerThreadPool::get_singleton() || !fixture.build(2, 257, 1)) {
			FAIL("D3-4 requires the real worker pool and production-written corpus"); return false;
		}
		cfg.quantized = p_quantized;
		cfg.position_bits = p_quantized ? 16 : 0;
		cfg.scale_bits = p_quantized ? 16 : 0;
		cfg.atlas_pages = fixture.source.node_count;
		cfg.upload_bytes_per_frame = p_limit;
		cfg.staging_upload_clamp_bytes = p_staging;
		PayloadPageStore::Config sc;
		sc.reader_count = 1; // Test IO capacity stays independent of pack-worker sizing.
		sc.max_bytes_in_flight = cfg.max_inflight_read_bytes;
		sc.max_idle_handles = 0;
		sc.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		if (store.start(sc) != OK || streamer.init_with_page_store(fixture.source.snapshot, cfg, 21, 41, store) != OK ||
				!history.reserve(fixture.source.node_count)) { FAIL("D3-4 real streamer/cut initialization failed"); return false; }
		control.cpu_upload_sink = true;
		control.hold_uploads = true;
		if (streamer._test_set_control(control) != OK || streamer._test_observe_shadow_transfers() != OK) {
			FAIL("D3-4 CPU sink and actual-write observation must be available"); return false;
		}
		if (p_fault != Streamer::TEST_FAULT_NONE && streamer._test_set_fault(p_fault) != OK) {
			FAIL("D3-4 phase-1 stub: fault must install on the REAL sizing/upload path"); return false;
		}
		checker.allowance = p_staging != 0 && p_staging < p_limit ? p_staging : p_limit;
		checker.stride = streamer.record_stride();
		checker.quantized = p_quantized;
		demand.node = 0;
		return true;
	}

	void step(bool p_check = true) {
		// Poll workers inside the current logical frame. Only actual transfer or
		// run retirement needs another frame/allowance; polling never ages timeouts.
		if (advance_frame || streamer.get_telemetry().pages_retiring != 0) { streamer.begin_frame(); }
		else {
#ifdef THREADS_ENABLED
			Thread::yield();
#endif
		}
		const uint32_t before = writes.size();
		streamer.update(nullptr, Streamer::Buffers(), request_node ? &demand : nullptr, request_node ? 1 : 0, nullptr, 0);
		sample = streamer._test_snapshot();
		observations_ok = observations_ok && sample.observation_error == OK && streamer._test_shadow_writes(writes) == OK;
		if (streamer.resident_mask().size() != fixture.source.node_count) {
			FAIL("D3-4 requires a complete residency mask before selecting or indexing a node"); observations_ok = false; return;
		}
		advance_frame = uint32_t(writes.size()) != before;
		gs_hlod_runtime::CutRequest view;
		view.frustum_enabled = false;
		view.focal_pixels = 100;
		view.camera = Vector3(0, 0, 1000);
		view.tau_pixels = 1.0e9;
		view.record_budget = 1.0e9;
		view.splat_budget = 1u << 30;
		view.node_budget = fixture.source.node_count;
		view.resident = streamer.resident_mask().ptr();
		view.resident_count = streamer.resident_mask().size();
		selected_count = history.select(*fixture.source.snapshot.ptr(), view).selected_count;
		if (!p_check) { return; }
		checker.consume(writes);
		if (checker.violation == CLEAN && checker.metadata_writes == 0) {
			const auto state = streamer.get_residency().state(0);
			if (streamer.resident_mask()[0] != 0 || selected_count != 0 ||
					(state != Residency::LOADING && state != Residency::UPLOADING)) { checker.violation = DRAWN_EARLY; }
		}
	}

	bool packed() {
		const auto ready = gs_pump_until([&]() { step(false); return !observations_ok || sample.ready_outputs != 0; });
		if (!ready.ready() || !observations_ok || sample.ready_outputs != 1 ||
				!streamer.get_residency().run(0, first_page, page_count)) {
			FAIL("D3-4 node never reached held, reserved ordinary ready output: ", ready.describe().utf8().get_data()); return false;
		}
		checker.base = uint64_t(first_page) * cfg.page_splats * checker.stride;
		if (!fixture.pack(0, false, cfg, first_page * cfg.page_splats, checker.golden, checker.expected_meta, checker.expected_quant) ||
				checker.atlas.resize(checker.golden.size()) != OK) { FAIL("D3-4 independent packed golden failed"); return false; }
		checker.atlas.fill(0);
		if (uint64_t(checker.golden.size()) > uint64_t(page_count) * cfg.page_splats * checker.stride) {
			FAIL("D3-4 fixture payload must fit its reserved run"); return false;
		}
		return true;
	}

	bool release() {
		control.hold_uploads = false;
		if (streamer._test_set_control(control) != OK) { FAIL("D3-4 cannot release real uploads"); return false; }
		return true;
	}

	bool finish() {
		const auto done = gs_pump_until([&]() {
			step(); return !observations_ok || checker.violation != CLEAN || checker.metadata_writes == 1;
		});
		if (!done.ready()) { FAIL("D3-4 final ordinary slice never published: ", done.describe().utf8().get_data()); return false; }
		CHECK(observations_ok);
		CHECK_MESSAGE(checker.violation == CLEAN, "D3-4 actual write checker violation=", int(checker.violation));
		if (!observations_ok || checker.violation != CLEAN) { return false; }
		CHECK(checker.payload_bytes == uint64_t(checker.golden.size()));
		CHECK(memcmp(checker.atlas.ptr(), checker.golden.ptr(), size_t(checker.golden.size())) == 0);
		CHECK(checker.metadata_writes == 1);
		CHECK(checker.quant_writes == (cfg.quantized ? 1 : 0));
		CHECK(streamer.resident_mask()[0] == 1);
		CHECK(selected_count == 1);
		CHECK(sample.live_reservations == 0);
		CHECK(sample.reservations_acquired == sample.reservations_released);
		// Demand remains unchanged after residency. Another deterministic update
		// must not rewrite the completed node or publish its metadata again.
		const uint32_t completed_writes = writes.size();
		step();
		CHECK(checker.violation == CLEAN);
		CHECK(uint32_t(writes.size()) == completed_writes);
		return true;
	}
};

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] ordinary oversized nodes publish only after bounded final slices") {
	for (bool quantized : { false, true }) {
		const uint64_t stride = quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
		Harness h;
		if (!h.init(quantized, 64 * stride + sizeof(ChunkMetaGPU) + sizeof(ChunkQuantizationGPU)) || !h.packed() || !h.release()) {
			FAIL("D3-4 constrained ordinary upload preconditions failed"); return;
		}
		if (!h.finish()) { return; }
		CHECK(h.checker.atlas_writes >= 3);
		CHECK(h.checker.frame_bytes.size() >= 3);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] subrecord allowance makes no progress or publication") {
	Harness h;
	if (!h.init(false, sizeof(PackedGaussian) - 1) || !h.packed() || !h.release()) {
		FAIL("D3-4 subrecord upload preconditions failed"); return;
	}
	// All async work has completed. One real update is enough to exercise the
	// zero-progress slice; this is not an asynchronous fixed-frame wait.
	h.step();
	CHECK(h.writes.is_empty());
	CHECK(h.checker.violation == CLEAN);
	CHECK(h.streamer.resident_mask()[0] == 0);
	CHECK(h.selected_count == 0);
	CHECK(h.sample.live_reservations == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] complete payload waits nonresident for next frame metadata allowance") {
	Harness h;
	const uint64_t bytes = 257 * sizeof(PackedGaussianQuantized);
	if (!h.init(true, bytes) || !h.packed() || !h.release()) {
		FAIL("D3-4 deferred metadata preconditions failed"); return;
	}
	// All worker output is held ready already. This update fills the frame with
	// one whole payload; quantization and metadata must wait for fresh allowance.
	h.step();
	if (!h.observations_ok || h.writes.size() != 1) { FAIL("D3-4 exact-payload allowance must issue one atlas write only"); return; }
	CHECK(h.writes[0].kind == Streamer::TestWrite::ATLAS);
	CHECK(h.writes[0].bytes == bytes);
	CHECK(h.checker.violation == CLEAN);
	CHECK(h.checker.metadata_writes == 0);
	CHECK(h.checker.quant_writes == 0);
	CHECK(h.streamer.resident_mask()[0] == 0);
	CHECK(h.selected_count == 0);
	CHECK(h.sample.live_reservations == 1);
	CHECK(h.sample.partial_upload_bytes > 0);
	if (!h.finish()) { return; }
	CHECK(h.checker.atlas_writes == 1); // The publication-only frame must not reupload.
	CHECK(h.checker.frame_bytes.size() == 2);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] nodes fitting the full allowance defer whole when only the remainder is small") {
	Harness h;
	const uint64_t bytes = 257 * sizeof(PackedGaussianQuantized);
	const uint64_t publication = sizeof(ChunkMetaGPU) + sizeof(ChunkQuantizationGPU);
	if (!h.init(true, bytes + publication + bytes / 2)) { FAIL("D3-4 whole-node deferral setup failed"); return; }
	LocalVector<gs_hlod_runtime::DesiredRecord> demand;
	for (uint32_t node = 0; node < h.fixture.source.node_count; ++node) {
		gs_hlod_runtime::DesiredRecord wanted;
		wanted.node = node;
		demand.push_back(wanted);
	}
	// Demand all nodes while the upload sink is held, removing completion order
	// from the remaining-allowance test. The ready queue has actual packed data.
	h.streamer.begin_frame();
	const auto packed = gs_pump_until([&]() {
		h.streamer.update(nullptr, Streamer::Buffers(), demand.ptr(), demand.size(), nullptr, 0);
		return h.streamer._test_snapshot().ready_outputs == h.fixture.source.node_count;
	});
	if (!packed.ready() || !h.release()) { FAIL("D3-4 all ordinary nodes never reached held ready state"); return; }
	Vector<uint32_t> counts;
	if (counts.resize(h.fixture.source.node_count) != OK) { FAIL("D3-4 publication count allocation failed"); return; }
	counts.fill(0);
	uint32_t seen = 0, published = 0;
	bool valid = true;
	const auto done = gs_pump_until([&]() {
		h.streamer.begin_frame();
		h.streamer.update(nullptr, Streamer::Buffers(), demand.ptr(), demand.size(), nullptr, 0);
		if (h.streamer._test_shadow_writes(h.writes) != OK) { valid = false; return true; }
		uint64_t spent = 0;
		uint32_t payloads = 0;
		for (; seen < uint32_t(h.writes.size()); ++seen) {
			const auto &w = h.writes[seen];
			if (w.node >= uint32_t(counts.size()) || spent > h.cfg.upload_bytes_per_frame ||
					w.bytes > h.cfg.upload_bytes_per_frame - spent) { valid = false; return true; }
			spent += w.bytes;
			if (w.kind == Streamer::TestWrite::ATLAS) {
				if (w.bytes != bytes || ++payloads != 1) { valid = false; return true; }
			} else if (w.kind == Streamer::TestWrite::METADATA) {
				++counts.write[w.node];
				++published;
			}
		}
		return !valid || published == h.fixture.source.node_count;
	});
	if (!done.ready()) { FAIL("D3-4 whole nodes never drained through subsequent frame allowances"); return; }
	CHECK(valid);
	CHECK(published == h.fixture.source.node_count);
	for (uint32_t count : counts) { CHECK(count == 1); }
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] partial ordinary cancellation and shutdown drain without metadata") {
	for (bool shutdown : { false, true }) {
		Harness h;
		if (!h.init(true, 64 * sizeof(PackedGaussianQuantized)) || !h.packed() || !h.release()) {
			FAIL("D3-4 cancellation preconditions failed"); return;
		}
		const auto partial = gs_pump_until([&]() {
			h.step(); return !h.observations_ok || h.checker.violation != CLEAN || h.checker.payload_bytes != 0;
		});
		if (!partial.ready() || !h.observations_ok || h.checker.violation != CLEAN ||
				h.checker.payload_bytes == 0 || h.checker.payload_bytes >= uint64_t(h.checker.golden.size())) {
			FAIL("D3-4 expected a bounded, invisible partial node before cancellation"); return;
		}
		CHECK(h.sample.partial_upload_bytes > 0);
		CHECK(h.sample.live_reservations == 1);
		h.request_node = false;
		const uint32_t before = h.writes.size();
		if (shutdown) { h.streamer.shutdown(); }
		else {
			if (h.streamer._test_cancel_node(0) != OK) { FAIL("D3-4 real partial-node cancel refused"); return; }
			const auto drained = gs_pump_until([&]() {
				h.step(false); return !h.observations_ok || (h.sample.live_reservations == 0 && h.streamer.get_telemetry().pages_retiring == 0);
			});
			if (!drained.ready() || !h.observations_ok) { FAIL("D3-4 cancelled partial run/permit never drained"); return; }
		}
		const auto sample = h.streamer._test_snapshot();
		if (h.streamer._test_shadow_writes(h.writes) != OK) { FAIL("D3-4 drain observations unavailable"); return; }
		CHECK(sample.live_reservations == 0);
		CHECK(sample.charged_bytes == 0);
		CHECK(sample.partial_upload_bytes == 0);
		CHECK(sample.reservations_acquired == sample.reservations_released);
		// Clearing metadata is observable too: an invisible partial run should
		// need no publication/clear and must issue no post-cancel payload writes.
		for (const auto &w : h.writes) { CHECK(w.kind == Streamer::TestWrite::ATLAS); }
		CHECK(uint32_t(h.writes.size()) == before);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] staging clamp bounds actual ordinary frame writes") {
	Harness h;
	if (!h.init(true, 32ull * 1024 * 1024, 64 * sizeof(PackedGaussianQuantized)) || !h.packed() || !h.release()) {
		FAIL("D3-4 staging-clamp preconditions failed"); return;
	}
	if (!h.finish()) { return; }
	CHECK(h.checker.atlas_writes >= 3);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] unavailable ordinary device fails packed nodes and returns serialized read permits") {
	TestHlodPageStore::Fixture fixture;
	if (!WorkerThreadPool::get_singleton() || !fixture.build(false, 2, 257, 0)) {
		FAIL("D3-4 deviceless admission requires the real worker pool and production-written paged corpus"); return;
	}
	LocalVector<gs_hlod_runtime::DesiredRecord> demand;
	for (uint32_t node = 0; node < fixture.node_count; ++node) {
		if (node != 0 && fixture.snapshot->nodes[node].topology.parent != 0) { continue; }
		gs_hlod_runtime::DesiredRecord wanted;
		wanted.node = node;
		demand.push_back(wanted);
	}
	if (demand.size() < 3) { FAIL("D3-4 deviceless corpus needs a root and at least two children"); return; }
	Streamer streamer;
	Streamer::Config cfg;
	cfg.atlas_pages = fixture.node_count;
	cfg.max_inflight_read_bytes = 1; // Below one record: each oversize reservation must run alone.
	// Use the original paged init/read path, as the regressed PagedV4 case does.
	// No CPU sink is installed: the real unavailable-device branch must discard.
	if (!streamer.init(fixture.snapshot, cfg, 21, 41) || streamer._test_observe_shadow_transfers() != OK) {
		FAIL("D3-4 deviceless streamer or real write observations failed to initialize"); return;
	}
	Streamer::TestSnapshot sample;
	Streamer::Telemetry telemetry;
	Vector<Streamer::TestWrite> writes;
	bool valid = true;
	streamer.begin_frame(); // Polling must not age FAILED retry cooldowns or request timeouts.
	const auto drained = gs_pump_until([&]() {
#ifdef THREADS_ENABLED
		Thread::yield();
#endif
		streamer.update(nullptr, Streamer::Buffers(), demand.ptr(), demand.size(), nullptr, 0);
		sample = streamer._test_snapshot();
		telemetry = streamer.get_telemetry();
		valid = valid && sample.observation_error == OK && streamer._test_shadow_writes(writes) == OK &&
				sample.jobs_in_flight <= 1 && sample.peak_jobs_in_flight <= 1 &&
				telemetry.payload_reads_in_flight <= 1 && sample.live_reservations <= 1 &&
				sample.reservations_released <= sample.reservations_acquired && writes.is_empty();
		return !valid || (telemetry.payload_reads_total == demand.size() && sample.live_reservations == 0 &&
				sample.jobs_in_flight == 0 && sample.pending_tickets == 0 && sample.ready_outputs == 0 && sample.mailbox_outputs == 0);
	});
	if (!drained.ready() || !valid) {
		FAIL("D3-4 unavailable device never failed each packed node and released its serialized read permit: reads=",
				telemetry.payload_reads_total, " wanted=", demand.size(), " live=", sample.live_reservations,
				" ", drained.describe().utf8().get_data()); return;
	}
	CHECK(telemetry.payload_reads_total == demand.size());
	CHECK(telemetry.failed_total == demand.size());
	CHECK(telemetry.resident == 0);
	CHECK(telemetry.uploaded_bytes_total == 0);
	CHECK(sample.jobs_started == demand.size());
	CHECK(sample.reservations_acquired == demand.size());
	CHECK(sample.reservations_released == sample.reservations_acquired);
	CHECK(sample.live_reservations == 0);
	CHECK(sample.charged_bytes == 0);
	CHECK(sample.partial_upload_bytes == 0);
	CHECK(sample.ready_bytes == 0);
	CHECK(sample.mailbox_bytes == 0);
	CHECK(sample.worker_output_bytes == 0);
	CHECK(sample.pending_tickets == 0);
	CHECK(sample.jobs_in_flight == 0);
	CHECK(sample.ready_outputs == 0);
	CHECK(sample.mailbox_outputs == 0);
	CHECK(writes.is_empty()); // Includes atlas, quantization and metadata writes.
	CHECK(streamer._test_publications().is_empty());
	for (const auto &wanted : demand) { CHECK(streamer.get_residency().state(wanted.node) == Residency::FAILED); }
	// The same demand cannot retry inside this logical frame or release a permit twice.
	streamer.update(nullptr, Streamer::Buffers(), demand.ptr(), demand.size(), nullptr, 0);
	const auto repeated = streamer._test_snapshot();
	CHECK(repeated.reservations_acquired == sample.reservations_acquired);
	CHECK(repeated.reservations_released == sample.reservations_released);
	CHECK(repeated.live_reservations == 0);
	streamer.shutdown();

	// Losing the sink after a bounded partial write must fail the ordinary node
	// too, without publishing it or issuing another write into its reserved run.
	Harness h;
	if (!h.init(true, 64 * sizeof(PackedGaussianQuantized)) || !h.packed() || !h.release()) {
		FAIL("D3-4 partial deviceless failure preconditions failed"); return;
	}
	h.step(); // Worker output is held ready already; this is one synchronous slice.
	if (!h.observations_ok || h.checker.violation != CLEAN || h.checker.payload_bytes == 0 ||
			h.checker.payload_bytes >= uint64_t(h.checker.golden.size()) || h.sample.live_reservations != 1) {
		FAIL("D3-4 unavailable-device control needs a real invisible partial upload with its permit"); return;
	}
	const uint32_t writes_before = h.writes.size();
	h.control.cpu_upload_sink = false;
	if (h.streamer._test_set_control(h.control) != OK) { FAIL("D3-4 could not withdraw the CPU device sink"); return; }
	const auto partial_drained = gs_pump_until([&]() {
		h.streamer.update(nullptr, Streamer::Buffers(), &h.demand, 1, nullptr, 0);
		h.sample = h.streamer._test_snapshot();
		h.observations_ok = h.observations_ok && h.sample.observation_error == OK && h.streamer._test_shadow_writes(h.writes) == OK;
		return !h.observations_ok || (h.sample.live_reservations == 0 && h.sample.ready_outputs == 0);
	});
	if (!partial_drained.ready() || !h.observations_ok) {
		FAIL("D3-4 partial ordinary node kept its permit after losing the device: ", partial_drained.describe().utf8().get_data()); return;
	}
	CHECK(h.streamer.get_residency().state(0) == Residency::FAILED);
	CHECK(h.streamer.resident_mask()[0] == 0);
	CHECK(h.sample.reservations_acquired == 1);
	CHECK(h.sample.reservations_released == 1);
	CHECK(h.sample.charged_bytes == 0);
	CHECK(h.sample.partial_upload_bytes == 0);
	CHECK(uint32_t(h.writes.size()) == writes_before);
	for (const auto &write : h.writes) { CHECK(write.kind == Streamer::TestWrite::ATLAS); }
	CHECK(h.streamer._test_publications().is_empty());
	h.streamer.update(nullptr, Streamer::Buffers(), &h.demand, 1, nullptr, 0);
	CHECK(h.streamer._test_snapshot().reservations_released == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] unknown staging and publisher defaults preserve whole-node uploads") {
	for (uint64_t clamp : { uint64_t(0), uint64_t(128ull * 1024 * 1024 / 2) }) {
		Harness h;
		if (!h.init(true, Streamer::Config().upload_bytes_per_frame, clamp) || !h.packed() || !h.release()) {
			FAIL("D3-4 default allowance preconditions failed"); return;
		}
		CHECK(h.cfg.upload_bytes_per_frame == 32ull * 1024 * 1024);
		CHECK(h.cfg.max_jobs_in_flight == 16);
		CHECK(h.cfg.max_inflight_read_bytes == 8ull * 1024 * 1024);
		CHECK(h.checker.allowance == 32ull * 1024 * 1024);
		if (!h.finish()) { return; }
		CHECK(h.checker.atlas_writes == 1);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] zero configured HLOD allowance is rejected observably") {
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(false, 1, 16, 0)) { FAIL("D3-4 zero-allowance corpus unavailable"); return; }
	Streamer streamer;
	Streamer::Config cfg;
	cfg.atlas_pages = fixture.node_count;
	cfg.upload_bytes_per_frame = 0;
	CHECK_FALSE(streamer.init(fixture.snapshot, cfg, 21, 41));
	CHECK_FALSE(streamer.is_active());
	CHECK(streamer._test_snapshot().jobs_started == 0);
}

// Frozen D3-2 codec-none cap oracle for uniform writer nodes. CPU/pool inputs
// must NEVER replace J=16 in K or change B=8 MiB in C=max(B,K*(R+O)).
static bool retained_cap(const TestHlodPageStore::Fixture &p_fixture, const Streamer::Config &p_cfg, uint64_t &r_cap) {
	uint64_t largest_r = 0, largest_o = 0, base = 0;
	for (uint32_t node = 0; node < p_fixture.node_count; ++node) {
		uint64_t decoded = 0, sh = 0, output = 0, window = 0;
		const auto &record = p_fixture.source->get_nodes()[node];
		if (!gs_hlod_runtime::allocation_bytes(p_fixture.splats_per_node, sizeof(Gaussian), decoded) ||
				!gs_hlod_runtime::allocation_bytes(uint64_t(p_fixture.splats_per_node) * p_fixture.high_order, sizeof(Vector3), sh) ||
				!gs_hlod_runtime::allocation_bytes(p_fixture.splats_per_node, sizeof(PackedGaussian), output) ||
				!gs_hlod_runtime::allocation_bytes(MIN(record.stored_bytes, PayloadReadService::read_slice_bytes), 1, window)) {
			FAIL("D3-4 retained-cap oracle arithmetic failed"); return false;
		}
		const uint64_t next_base = record.stored_bytes + uint64_t(p_fixture.splats_per_node) * sizeof(PackedGaussian);
		if (base != 0 && base != next_base) { FAIL("D3-4 cap oracle requires uniform production-written nodes"); return false; }
		base = next_base;
		largest_r = MAX(largest_r, decoded + sh + MAX(window, output));
		largest_o = MAX(largest_o, output);
	}
	if (base == 0 || largest_r > p_cfg.max_inflight_read_bytes || base > p_cfg.max_inflight_read_bytes) {
		FAIL("D3-4 cap fixture must exercise normal concurrent admission"); return false;
	}
	const uint64_t k = MIN(uint64_t(p_cfg.max_jobs_in_flight), MIN(uint64_t(p_fixture.node_count), p_cfg.max_inflight_read_bytes / base));
	r_cap = MAX(p_cfg.max_inflight_read_bytes, k * (largest_r + largest_o));
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] CPU and pool slots bound started jobs without changing D3-2 retained cap") {
	TestHlodPageStore::Fixture fixture;
	if (!WorkerThreadPool::get_singleton() || !fixture.build(false, 4, 4206, 3)) {
		FAIL("D3-4 concurrency requires the real pool and D3-2 SH3 corpus"); return;
	}
	struct Input { uint32_t cpus, pool, reserved, expected; };
	const Input inputs[] = { { 4, 0, 0, 2 }, { 32, 1, 0, 1 }, { 0, 3, 1, 2 } };
	uint64_t frozen_cap = 0;
	for (const auto &input : inputs) {
		PayloadPageStore store;
		Streamer streamer; // Join held workers before store/source destruction.
		Streamer::Config cfg;
		cfg.atlas_pages = fixture.node_count * ((fixture.splats_per_node + cfg.page_splats - 1) / cfg.page_splats);
		cfg.logical_cpus = input.cpus;
		cfg.pool_threads = input.pool;
		cfg.reserved_pool_threads = input.reserved;
		PayloadPageStore::Config sc;
		sc.reader_count = 2; // Existing publisher IO input, unaffected by injected pack slots.
		sc.max_bytes_in_flight = cfg.max_inflight_read_bytes;
		sc.max_idle_handles = 0;
		sc.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		if (store.start(sc) != OK || streamer.init_with_page_store(fixture.snapshot, cfg, 21, 41, store) != OK ||
				!retained_cap(fixture, cfg, frozen_cap)) { FAIL("D3-4 concurrent streamer/cap oracle init failed"); return; }
		CHECK(frozen_cap > cfg.max_inflight_read_bytes); // Non-vacuous: replacing K with effective slots shrinks C.
		CHECK(streamer.get_retained_byte_cap() == frozen_cap);
		Streamer::TestControl control;
		control.cpu_upload_sink = true;
		control.hold_uploads = true;
		control.hold_workers = true;
		if (streamer._test_set_control(control) != OK) { FAIL("D3-4 held real workers unavailable"); return; }
		LocalVector<gs_hlod_runtime::DesiredRecord> demand;
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			gs_hlod_runtime::DesiredRecord wanted;
			wanted.node = node;
			demand.push_back(wanted);
		}
		Streamer::TestSnapshot sample;
		streamer.begin_frame();
		const auto started = gs_pump_until([&]() {
			streamer.update(nullptr, Streamer::Buffers(), demand.ptr(), demand.size(), nullptr, 0);
			sample = streamer._test_snapshot();
			return sample.observation_error != OK || sample.jobs_started >= input.expected;
		});
		if (!started.ready() || sample.observation_error != OK) { FAIL("D3-4 effective job slots never started"); return; }
		CHECK(sample.jobs_started == input.expected);
		CHECK(sample.jobs_in_flight <= input.expected);
		CHECK(sample.peak_jobs_in_flight <= input.expected);
		CHECK(streamer.get_retained_byte_cap() == frozen_cap);
		streamer.shutdown(); // Releases the worker gate, drains, and returns all permits.
		CHECK(streamer._test_snapshot().live_reservations == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] forcing ignored staging clamp trips the same actual-write checker") {
	Harness h;
	if (!h.init(true, 32ull * 1024 * 1024, 64 * sizeof(PackedGaussianQuantized), Streamer::TEST_FAULT_IGNORE_STAGING_CLAMP) ||
			!h.packed() || !h.release()) { FAIL("D3-4 ignored-clamp fault installation/preconditions failed"); return; }
	const auto detected = gs_pump_until([&]() { h.step(); return !h.observations_ok || h.checker.violation != CLEAN; });
	if (!detected.ready() || !h.observations_ok) { FAIL("D3-4 ignored clamp never produced an observable violation"); return; }
	CHECK(h.sample.faults_triggered > 0);
	CHECK(h.checker.violation == FRAME_ALLOWANCE);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Sizing] forcing early final slice trips the same publication checker") {
	Harness h;
	if (!h.init(true, 64 * sizeof(PackedGaussianQuantized) + sizeof(ChunkMetaGPU) + sizeof(ChunkQuantizationGPU), 0,
				Streamer::TEST_FAULT_EARLY_FINAL_SLICE) || !h.packed() || !h.release()) {
		FAIL("D3-4 early-final fault installation/preconditions failed"); return;
	}
	const auto detected = gs_pump_until([&]() { h.step(); return !h.observations_ok || h.checker.violation != CLEAN; });
	if (!detected.ready() || !h.observations_ok) { FAIL("D3-4 early final slice never produced an observable violation"); return; }
	CHECK(h.sample.faults_triggered > 0);
	CHECK(h.checker.violation == EARLY_PUBLICATION);
}

} // namespace TestHlodUploadSizing
