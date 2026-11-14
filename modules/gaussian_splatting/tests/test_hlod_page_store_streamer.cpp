extern "C" int test_hlod_page_store_streamer_cpp_force_link() { return 0; }

#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gs_payload_page_store.h"
#include "../renderer/hlod_node_streamer.h"

#include <cstring>

namespace TestHlodPageStoreStreamer {

using Streamer = HlodNodeStreamer;
using Residency = gs_hlod_runtime::NodeResidency;
using TestGaussianSplatting::gs_pump_until;

static Dictionary exported_queue(const Streamer &p_streamer, const char *p_owner) {
	const Variant payload = p_streamer.get_payload_telemetry();
	if (payload.get_type() != Variant::DICTIONARY) { FAIL("D3-6 real queue export is absent"); return Dictionary(); }
	const Dictionary block = payload;
	const Dictionary queues = block.get("queue_observations", Dictionary());
	return queues.get(p_owner, Dictionary());
}

// D3 design 3.3 / 4.2 D3-2. This checker is deliberately outside production
// code. It measures every payload owner, not just read-service or ready bytes.
// The same checker runs under the healthy and drop-output-charge controls.
static uint64_t retained(const Streamer::TestSnapshot &p_sample) {
	return p_sample.stored_bytes + p_sample.decode_bytes + p_sample.idle_decoder_bytes +
			p_sample.worker_output_bytes + p_sample.mailbox_bytes + p_sample.ready_bytes + p_sample.partial_upload_bytes;
}

static bool bounded(const Streamer::TestSnapshot &p_sample, const Streamer::Config &p_config,
		uint64_t p_retained_cap, bool p_allow_oversized = false) {
	if (p_sample.observation_error != OK || p_sample.jobs_in_flight > p_config.max_jobs_in_flight ||
			p_sample.peak_jobs_in_flight > p_config.max_jobs_in_flight ||
			p_sample.reservations_released > p_sample.reservations_acquired ||
			p_sample.live_reservations != p_sample.reservations_acquired - p_sample.reservations_released ||
			retained(p_sample) > p_sample.charged_bytes) {
		return false;
	}
	if (p_sample.oversized_reservation_bytes != 0) {
		return p_allow_oversized && p_sample.oversized_reservation_bytes > p_config.max_inflight_read_bytes &&
				p_sample.live_reservations == 1 && p_sample.jobs_in_flight <= 1 &&
				p_sample.charged_bytes <= p_sample.oversized_reservation_bytes &&
				retained(p_sample) <= p_sample.oversized_reservation_bytes;
	}
	return p_sample.charged_bytes <= p_retained_cap && retained(p_sample) <= p_retained_cap &&
			(p_allow_oversized || p_sample.peak_retained_bytes <= p_retained_cap);
}

struct Harness {
	// Reverse destruction order is intentional: streamer -> store -> source file.
	TestHlodPageStore::Fixture fixture;
	PayloadPageStore store;
	Streamer streamer;
	Streamer::Config config;
	Streamer::TestControl control;
	LocalVector<gs_hlod_runtime::DesiredRecord> demand;
	Streamer::TestSnapshot sample;
	uint64_t largest_reservation = 0;
	uint64_t retained_cap = 0;
	bool observations_ok = true;
	bool bound_ok = true;
	bool byte_overrun = false;
	uint64_t world = 21;
	uint64_t device = 41;

	~Harness() {
		// Even an assertion failure releases a deliberate worker hold before join.
		if (streamer.is_active()) {
			streamer._test_set_control(Streamer::TestControl());
		}
		streamer.shutdown();
		store.shutdown();
	}

	bool node_reservation(uint32_t p_node, uint64_t &r_reservation, uint64_t &r_output) const {
		if (fixture.source.is_null() || p_node >= uint32_t(fixture.source->get_nodes().size())) {
			FAIL("D3-2 reservation oracle requires a valid source node");
			return false;
		}
		const GaussianSplatHlodPagedSource::NodeRecord &node = fixture.source->get_nodes()[p_node];
		const uint64_t count = node.topology.payload_count;
		uint64_t gaussians = 0, sh = 0, window = 0;
		if (!gs_hlod_runtime::allocation_bytes(count, sizeof(Gaussian), gaussians) ||
				!gs_hlod_runtime::allocation_bytes(count * fixture.high_order, sizeof(Vector3), sh) ||
				!gs_hlod_runtime::allocation_bytes(count, sizeof(PackedGaussian), r_output)) {
			FAIL("D3-2 reservation oracle cannot represent decoded/packed allocation capacities");
			return false;
		}
		const uint64_t packing_peak = gaussians + sh + r_output;
		if (!fixture.gsq) {
			// Independently count the complete decoded arrays plus ONE existing
			// read-service slice. That window is gone before packing starts.
			const uint64_t window_bytes = node.stored_bytes < PayloadReadService::read_slice_bytes ?
					node.stored_bytes : PayloadReadService::read_slice_bytes;
			if (!gs_hlod_runtime::allocation_bytes(window_bytes, 1, window)) {
				FAIL("D3-2 reservation oracle cannot represent the codec-none read window");
				return false;
			}
			const uint64_t reading_peak = window + gaussians + sh;
			r_reservation = reading_peak > packing_peak ? reading_peak : packing_peak;
		} else {
			const uint64_t conservative = fixture.source->get_node_read_working_set_bytes(p_node) + count * sizeof(PackedGaussian);
			r_reservation = conservative > packing_peak ? conservative : packing_peak;
		}
		return true;
	}

	bool expected_retained_cap(uint64_t &r_cap) const {
		r_cap = config.max_inflight_read_bytes;
		if (fixture.gsq) {
			return true; // The GSQ route keeps the original input envelope.
		}
		uint64_t largest_normal = 0, largest_output = 0, uniform_base_cost = 0;
		uint32_t normal_nodes = 0;
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			uint64_t reservation = 0, output = 0;
			if (!node_reservation(node, reservation, output)) {
				return false;
			}
			const uint64_t base_cost = fixture.source->get_nodes()[node].stored_bytes +
					uint64_t(fixture.splats_per_node) * sizeof(PackedGaussian);
			if (reservation > config.max_inflight_read_bytes || base_cost > config.max_inflight_read_bytes) {
				continue; // Oversized nodes remain alone; they cannot expand the normal envelope.
			}
			// Production sorts arbitrary nodes by base cost. These writer fixtures
			// have equal-sized codec-none nodes, so division independently gives
			// the number of smallest costs that fit, capped by jobs and node count.
			if (uniform_base_cost != 0 && uniform_base_cost != base_cost) {
				FAIL("D3-2 retained-cap oracle requires its uniform production-written codec-none fixture");
				return false;
			}
			uniform_base_cost = base_cost;
			++normal_nodes;
			largest_normal = reservation > largest_normal ? reservation : largest_normal;
			largest_output = output > largest_output ? output : largest_output;
		}
		if (normal_nodes != 0 && uniform_base_cost != 0) {
			uint64_t jobs = config.max_inflight_read_bytes / uniform_base_cost;
			jobs = jobs < config.max_jobs_in_flight ? jobs : config.max_jobs_in_flight;
			jobs = jobs < normal_nodes ? jobs : normal_nodes;
			const uint64_t derived = jobs * (largest_normal + largest_output);
			r_cap = derived > r_cap ? derived : r_cap;
		}
		return true;
	}

	bool init(bool p_oversized = false, bool p_gsq = false, bool p_base_parallelism = false) {
		// The larger SH3 case reproduces the pre-migration per-node input size;
		// the small fixture keeps saturation/cancellation cases cheap.
		if (!fixture.build(p_gsq, p_base_parallelism ? 4 : 5,
				p_base_parallelism ? 4206 : 256, p_base_parallelism ? 3 : 0)) {
			return false;
		}
		if (fixture.source.is_null() || fixture.snapshot.is_null() ||
				fixture.source->get_nodes().size() != fixture.node_count || !WorkerThreadPool::get_singleton()) {
			FAIL("D3-2 streamer fixture requires a paged source, complete topology and worker pool");
			return false;
		}
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			uint64_t reservation = 0, output = 0;
			if (!node_reservation(node, reservation, output)) {
				return false;
			}
			if (reservation > largest_reservation) {
				largest_reservation = reservation;
			}
		}
		if (largest_reservation < 2) {
			FAIL("D3-2 fixture must require a nonzero stored/decode/output reservation");
			return false;
		}
		// Small per-instance envelope makes saturation cheap. Frozen runtime
		// defaults (16 jobs, 8 MiB, 32 MiB/frame) are never changed.
		if (!p_base_parallelism) {
			config.max_inflight_read_bytes = p_oversized ? largest_reservation - 1 : 2 * largest_reservation;
		}
		config.atlas_pages = fixture.node_count *
				((fixture.splats_per_node + config.page_splats - 1u) / config.page_splats);
		PayloadPageStore::Config store_config;
		store_config.reader_count = 1; // Deterministic test reader, not runtime sizing policy.
		store_config.max_bytes_in_flight = config.max_inflight_read_bytes;
		store_config.max_idle_handles = 0;
		store_config.decoder_retention_cap = config.max_jobs_in_flight;
		store_config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		if (store.start(store_config) != OK) {
			FAIL("D3-2 streamer fixture requires a running retained-byte page store");
			return false;
		}
		if (streamer.init_with_page_store(fixture.snapshot, config, world, device, store) != OK) {
			FAIL("D3-2 bounded ordinary streamer must initialize with the common store (phase-1 stub refuses)");
			return false;
		}
		retained_cap = streamer.get_retained_byte_cap();
		uint64_t expected_cap = 0;
		if (!expected_retained_cap(expected_cap)) {
			return false;
		}
		if (retained_cap != expected_cap || retained_cap < config.max_inflight_read_bytes) {
			FAIL("D3-2 retained cap must equal the measured base-concurrency/output derivation");
			return false;
		}
		if (uint64_t(fixture.node_count) * fixture.splats_per_node * sizeof(PackedGaussian) <= retained_cap) {
			FAIL("D3-2 saturation fixture must demand more packed output than its entire byte envelope");
			return false;
		}
		control.cpu_upload_sink = true;
		control.hold_uploads = true;
		if (!set_control()) {
			return false;
		}
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			gs_hlod_runtime::DesiredRecord wanted;
			wanted.node = node;
			demand.push_back(wanted);
		}
		return true;
	}

	bool set_control() {
		if (streamer._test_set_control(control) != OK) {
			FAIL("D3-2 test controls must affect the real ordinary streaming path");
			return false;
		}
		return true;
	}

	void step(bool p_oversized = false) {
		streamer.begin_frame();
		// The sink replaces only device writes; actual request/admission, workers,
		// mailbox, ready, validation and finish_upload remain under test.
		streamer.update(nullptr, Streamer::Buffers(), demand.ptr(), demand.size(), nullptr, 0);
		sample = streamer._test_snapshot();
		observations_ok = observations_ok && sample.observation_error == OK;
		bound_ok = bound_ok && bounded(sample, config, retained_cap, p_oversized);
		if (p_oversized) {
			// High-water measurements catch exceptions that overlap entirely
			// between frame snapshots, too. Every fixture node has this bound.
			bound_ok = bound_ok && sample.peak_jobs_in_flight <= 1 && sample.peak_retained_bytes <= largest_reservation;
		}
		byte_overrun = byte_overrun || retained(sample) > retained_cap;
	}

	bool wait_held(bool p_oversized = false) {
		const auto outcome = gs_pump_until([&]() {
			step(p_oversized);
			// A worker can release its working-set part after start_jobs() last
			// refused admission. Historical admission_waits + jobs==0 therefore
			// does not prove saturation. These fixtures start nodes in BFS order;
			// independently require that the NEXT node cannot fit right now.
			bool capacity_full = sample.oversized_reservation_bytes != 0;
			if (sample.jobs_started < fixture.node_count) {
				uint64_t next = 0, output = 0;
				if (!node_reservation(uint32_t(sample.jobs_started), next, output)) {
					observations_ok = false;
					return true;
				}
				capacity_full = capacity_full || next > config.max_inflight_read_bytes ||
						sample.charged_bytes > retained_cap - next;
			}
			return !observations_ok || (sample.ready_outputs > 0 && sample.pending_tickets > 0 &&
					sample.jobs_in_flight == 0 && sample.mailbox_outputs == 0 && sample.admission_waits > 0 && capacity_full);
		});
		if (!outcome.ready() || !observations_ok) {
			FAIL("D3-2 held uploads never reached bounded ready output with pending demand and no blocked workers");
			return false;
		}
		return true;
	}

	bool drain() {
		const auto outcome = gs_pump_until([&]() {
			step();
			return !observations_ok || (sample.jobs_in_flight == 0 && sample.pending_tickets == 0 &&
					sample.mailbox_outputs == 0 && sample.ready_outputs == 0 && sample.live_reservations == 0);
		});
		if (!outcome.ready() || !observations_ok) {
			FAIL("D3-2 cancelled or stale owners never drained their jobs, output and reservations");
			return false;
		}
		return true;
	}

	bool finish_demand(bool p_oversized = false) {
		control.hold_uploads = false;
		control.hold_mailbox = false;
		control.hold_workers = false;
		if (!set_control()) {
			return false;
		}
		const auto outcome = gs_pump_until([&]() {
			step(p_oversized);
			return !observations_ok || streamer.get_residency().stats().resident == fixture.node_count;
		});
		if (!outcome.ready() || !observations_ok) {
			FAIL("D3-2 releasing held uploads never satisfied all ordinary demand");
			return false;
		}
		const Vector<Streamer::TestPublication> publications = streamer._test_publications();
		if (publications.size() != fixture.node_count || store.stats().reads.completed_ok == 0) {
			FAIL("D3-2 successful progress must include real store reads and one metadata publication per node");
			return false;
		}
		return true;
	}

	bool check_released() {
		sample = streamer._test_snapshot();
		const PayloadPageStore::Stats stats = store.stats();
		if (sample.observation_error != OK || sample.reservations_acquired == 0 ||
				sample.reservations_acquired != sample.reservations_released || sample.live_reservations != 0 ||
				sample.charged_bytes != 0 || retained(sample) != 0 || stats.live_reservations != 0 ||
				stats.reservations_acquired != stats.reservations_released || stats.retained_bytes != 0 ||
				stats.decoders_leased != 0 || stats.reads.bytes_in_flight != 0) {
			FAIL("D3-2 drain must release every streamer/store reservation exactly once with zero retained payload");
			return false;
		}
		return true;
	}
};

enum Holder { WORKER, MAILBOX, READY };

static bool reach_holder(Harness &p_harness, Holder p_holder) {
	p_harness.control.hold_workers = p_holder == WORKER;
	p_harness.control.hold_mailbox = p_holder == MAILBOX;
	if (!p_harness.set_control()) {
		return false;
	}
	const auto outcome = gs_pump_until([&]() {
		p_harness.step();
		return !p_harness.observations_ok ||
				(p_holder == WORKER && p_harness.sample.jobs_in_flight > 0) ||
				(p_holder == MAILBOX && p_harness.sample.mailbox_outputs > 0) ||
				(p_holder == READY && p_harness.sample.ready_outputs > 0);
	});
	if (!outcome.ready() || !p_harness.observations_ok) {
		FAIL("D3-2 ordinary payload never reached the requested worker/mailbox/ready owner");
		return false;
	}
	return true;
}

static bool no_residents(const Harness &p_harness) {
	const Vector<uint8_t> &mask = p_harness.streamer.resident_mask();
	if (mask.size() != p_harness.fixture.node_count) {
		return false;
	}
	for (uint32_t node = 0; node < p_harness.fixture.node_count; ++node) {
		if (mask[node] != 0 || p_harness.streamer.get_residency().state(node) == Residency::RESIDENT) {
			return false;
		}
	}
	return true;
}

// Uses provenance recorded by real publication, plus model state and the
// metadata actually written by that path. A counter claiming "stale dropped"
// cannot substitute for this check. Also reused by the generation mutation.
static bool publications_current(const Harness &p_harness, uint64_t p_source, uint64_t p_content, uint64_t p_world,
		uint64_t p_device, bool &r_stale_published) {
	bool valid = true;
	r_stale_published = false;
	for (const Streamer::TestPublication &entry : p_harness.streamer._test_publications()) {
		if (entry.node >= p_harness.fixture.node_count || entry.request_generation == 0 ||
				entry.source_instance_id != p_source || entry.content_generation != p_content ||
				entry.world_generation != p_world || entry.device_generation != p_device) {
			valid = false;
			if (entry.node < p_harness.fixture.node_count && entry.metadata_splat_count > 0 &&
					p_harness.streamer.get_residency().state(entry.node) == Residency::RESIDENT) {
				r_stale_published = true;
			}
		}
	}
	return valid;
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] codec-none SH3 preserves base read concurrency within the retained envelope") {
	Harness h;
	if (!h.init(false, false, true)) {
		return;
	}
	if (h.fixture.node_count < 2 || h.fixture.high_order == 0) {
		FAIL("D3-2 SH3 cross-window identity requires an interior node, a leaf and high-order SH");
		return;
	}
	// Exercise bytes on BOTH sides of the 1 MiB transport boundary before any
	// streamer job starts. The oracle regenerates source attributes independently;
	// successful envelope/metric checks alone cannot prove the SH copy is exact.
	for (uint32_t node : { 0u, h.fixture.node_count - 1u }) {
		CAPTURE(node);
		if (node >= uint32_t(h.fixture.source->get_nodes().size()) ||
				h.fixture.source->get_nodes()[node].stored_bytes <= PayloadReadService::read_slice_bytes) {
			FAIL("D3-2 SH3 identity target must exist and span more than one transport window");
			return;
		}
		Vector<Gaussian> actual, expected;
		Vector<Vector3> actual_sh, expected_sh;
		String reason;
		if (!h.fixture.expected_payload(node, expected, expected_sh)) {
			return;
		}
		if (h.fixture.source->read_node_payload_from_store(h.store, uint64_t(0xd3204206u) + node, uint64_t(node) + 1u,
					node, actual, actual_sh, reason) != OK) {
			FAIL("D3-2 SH3 cross-window common-store read failed: ", reason.utf8().get_data());
			return;
		}
		const uint64_t high_count = uint64_t(h.fixture.splats_per_node) * h.fixture.high_order;
		if (uint64_t(actual.size()) != h.fixture.splats_per_node || uint64_t(expected.size()) != h.fixture.splats_per_node ||
				uint64_t(actual_sh.size()) != high_count || uint64_t(expected_sh.size()) != high_count) {
			FAIL("D3-2 SH3 cross-window identity requires complete Gaussian and SH arrays before byte comparison");
			return;
		}
		CHECK_MESSAGE(std::memcmp(actual.ptr(), expected.ptr(), size_t(h.fixture.splats_per_node) * sizeof(Gaussian)) == 0,
				"D3-2 SH3 cross-window Gaussian bytes differ from the independent codec-none oracle");
		CHECK_MESSAGE(std::memcmp(actual_sh.ptr(), expected_sh.ptr(), size_t(high_count) * sizeof(Vector3)) == 0,
				"D3-2 SH3 cross-window SH bytes differ from the independent codec-none oracle");
	}
	CHECK_MESSAGE(h.store.stats().peak_retained_bytes <= PayloadReadService::read_slice_bytes,
			"D3-2 codec-none transport must retain at most one bounded window per direct read");
	// Replay 48c49582803 admission independently: ready output had no charge;
	// a concurrent read charged only its stored record plus logical packed copy.
	// These frozen default inputs are the comparison, not a test-specific cap.
	if (h.config.max_inflight_read_bytes != 8ull * 1024 * 1024 || h.config.max_jobs_in_flight != 16) {
		FAIL("D3-2 base concurrency comparison requires unchanged 8 MiB and 16-job defaults");
		return;
	}
	uint64_t base_bytes = 0;
	uint32_t base_jobs = 0;
	for (const GaussianSplatHlodPagedSource::NodeRecord &node : h.fixture.source->get_nodes()) {
		const uint64_t cost = node.stored_bytes + uint64_t(node.topology.payload_count) * sizeof(PackedGaussian);
		if (base_jobs >= h.config.max_jobs_in_flight ||
				(base_jobs != 0 && base_bytes + cost > h.config.max_inflight_read_bytes)) {
			break;
		}
		base_bytes += cost;
		++base_jobs;
	}
	if (base_jobs <= 1 || base_jobs >= h.fixture.node_count) {
		FAIL("D3-2 SH3 fixture must expose concurrent base admission with excess pending demand");
		return;
	}
	// Hold before reads so worker speed cannot hide the number of jobs really
	// admitted. Uploads remain enabled. Releasing this seam must then execute
	// real common-store reads, packing and publication for every node.
	h.control.hold_uploads = false;
	h.control.hold_workers = true;
	if (!h.set_control()) {
		return;
	}
	const auto admitted = gs_pump_until([&]() {
		h.step();
		return !h.observations_ok || (h.sample.pending_tickets > 0 &&
				(h.sample.admission_waits > 0 || h.sample.jobs_in_flight == h.config.max_jobs_in_flight));
	});
	if (!admitted.ready() || !h.observations_ok) {
		FAIL("D3-2 SH3 workers never reached observable concurrent admission with pending demand");
		return;
	}
	const uint32_t new_jobs = h.sample.peak_jobs_in_flight;
	MESSAGE("D3-2 codec-none SH3 base concurrent jobs=", base_jobs, ", retained-path concurrent jobs=", new_jobs,
			", derived retained cap=", h.retained_cap);
	CHECK_MESSAGE(new_jobs >= base_jobs, "D3-2 retained admission reduced codec-none SH3 concurrency below base");
	if (!h.finish_demand()) {
		return;
	}
	MESSAGE("D3-2 codec-none SH3 observed retained peak=", h.sample.peak_retained_bytes, ", derived retained cap=", h.retained_cap);
	CHECK_MESSAGE(h.bound_ok, "D3-2 codec-none SH3 storage exceeded its retained byte/job envelope");
	CHECK_MESSAGE(h.sample.peak_retained_bytes <= h.retained_cap, "D3-2 codec-none SH3 actual retained storage exceeded its derived cap");
	CHECK_MESSAGE(h.sample.peak_jobs_in_flight >= base_jobs, "D3-2 codec-none SH3 never sustained base concurrent job admission");
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] held uploads bound every ordinary owner and resume pending demand") {
	for (bool gsq : { false, true }) {
		CAPTURE(gsq);
		Harness h;
		if (!h.init(false, gsq) || !h.wait_held()) {
			return;
		}
		// A time FLOOR observes sustained backpressure, without a worker-speed
		// dependent frame ceiling. Every pumped frame runs the same checker.
		const OS *os = OS::get_singleton();
		if (!os) {
			FAIL("D3-2 saturation requires the wall clock for sustained held-upload observation");
			return;
		}
		const uint64_t started = os->get_ticks_usec();
		const uint64_t started_jobs = h.sample.jobs_started;
		const auto held = gs_pump_until([&]() {
			h.step();
			return !h.observations_ok || os->get_ticks_usec() - started >= 50000;
		});
		if (!held.ready() || !h.observations_ok) {
			FAIL("D3-2 saturation never completed the held-upload observation window");
			return;
		}
		CHECK_MESSAGE(h.bound_ok, "D3-2 ordinary retained bytes/jobs exceeded the continuous reservation envelope");
		CHECK_MESSAGE(h.sample.jobs_started == started_jobs, "D3-2 held output must stop further job admission");
		CHECK_MESSAGE(h.sample.pending_tickets > 0, "D3-2 excess demand must stay pending without payload allocation");
		CHECK_MESSAGE(no_residents(h), "D3-2 held uploads must never publish RESIDENT nodes");
		if (!h.finish_demand()) {
			return;
		}
		CHECK_MESSAGE(h.bound_ok, "D3-2 releasing uploads must preserve the retained-byte/job envelope");
		h.streamer.shutdown();
		h.store.shutdown();
		h.check_released();
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] oversized single node keeps a visible reservation and loads alone") {
	Harness h;
	if (!h.init(true) || !h.wait_held(true)) {
		return;
	}
	CHECK_MESSAGE(h.bound_ok, "D3-2 oversized exception must retain one full conservative reservation");
	CHECK_MESSAGE(h.sample.live_reservations == 1, "D3-2 oversized output must be the only live node reservation");
	CHECK_MESSAGE(h.sample.jobs_started == 1, "D3-2 oversized output must block every other job until upload/discard");
	CHECK_MESSAGE(h.sample.oversized_reservation_bytes == h.largest_reservation,
			"D3-2 oversized reservation must be visible at its full size");
	const Dictionary held = exported_queue(h.streamer, "retained");
	CHECK(int64_t(held.get("byte_bound", -1)) == int64_t(h.largest_reservation));
	CHECK(int64_t(held.get("jobs", -1)) == 1);
	CHECK(int64_t(held.get("bound_violations", -1)) == 0);
	if (!h.finish_demand(true)) {
		return;
	}
	CHECK_MESSAGE(h.bound_ok, "D3-2 oversized progress must remain one-node-alone throughout transfer");
	const Dictionary drained = exported_queue(h.streamer, "retained");
	CHECK(int64_t(drained.get("byte_bound", -1)) == int64_t(h.retained_cap));
	CHECK(int64_t(drained.get("peak_byte_bound", -1)) == int64_t(h.largest_reservation));
	CHECK(int64_t(drained.get("peak_bytes", -1)) <= int64_t(drained.get("peak_byte_bound", -2)));
	CHECK(int64_t(drained.get("bound_violations", -1)) == 0);
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] cancellation retarget and eviction release each ordinary owner once") {
	for (Holder holder : { WORKER, MAILBOX, READY }) {
		CAPTURE(holder);
		Harness h;
		if (!h.init() || !reach_holder(h, holder)) {
			return;
		}
		for (uint32_t node = 0; node < h.fixture.node_count; ++node) {
			if (h.streamer._test_cancel_node(node) != OK || h.streamer._test_cancel_node(node) != OK) {
				FAIL("D3-2 repeated cancellation must use the real idempotent node-cancel path");
				return;
			}
		}
		h.demand.clear(); // Retarget to no ordinary demand while late jobs complete.
		h.control.hold_workers = h.control.hold_mailbox = h.control.hold_uploads = false;
		if (!h.set_control() || !h.drain() || !h.check_released()) {
			return;
		}
		CHECK_MESSAGE(no_residents(h), "D3-2 cancelled worker/mailbox/ready output became RESIDENT");
		CHECK_MESSAGE(h.streamer._test_publications().is_empty(), "D3-2 cancelled output wrote GPU metadata");
		CHECK_MESSAGE(h.bound_ok, "D3-2 cancellation must preserve continuous accounting");
	}
	for (Holder holder : { WORKER, MAILBOX, READY }) {
		CAPTURE(holder);
		Harness h;
		if (!h.init() || !reach_holder(h, holder)) {
			return;
		}
		// Withdraw demand with the owners still present. This independently
		// exercises cancellation after the real residency grace period; removing
		// that wiring cannot be hidden by an explicit _test_cancel_node call.
		h.demand.clear();
		const auto retargeted = gs_pump_until([&]() {
			h.step();
			const Residency::Stats stats = h.streamer.get_residency().stats();
			return !h.observations_ok || (stats.loading == 0 && stats.uploading == 0 && stats.cancelled_total > 0);
		});
		if (!retargeted.ready() || !h.observations_ok) {
			FAIL("D3-2 withdrawn demand never cancelled live worker/mailbox/ready nodes after the grace period");
			return;
		}
		h.control.hold_workers = h.control.hold_mailbox = h.control.hold_uploads = false;
		if (!h.set_control() || !h.drain() || !h.check_released()) {
			return;
		}
		CHECK_MESSAGE(no_residents(h), "D3-2 retargeted output became RESIDENT after demand cancellation");
		CHECK_MESSAGE(h.streamer._test_publications().is_empty(), "D3-2 retargeted cancelled output wrote metadata");
	}
	Harness h;
	if (!h.init() || !h.finish_demand()) {
		return;
	}
	const uint32_t leaf = h.fixture.node_count - 1;
	h.demand.clear();
	h.step(); // Clears demand/protection before eviction.
	if (h.streamer._test_evict_node(leaf) != OK) {
		FAIL("D3-2 completed ordinary nodes must remain evictable through the real eviction path");
		return;
	}
	h.step();
	CHECK_MESSAGE(h.streamer.get_residency().state(leaf) != Residency::RESIDENT,
			"D3-2 evicted ordinary node remained RESIDENT");
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

enum GenerationChange { SOURCE, WORLD, DEVICE };

static void generation_contract(bool p_fault, GenerationChange p_change, Holder p_holder) {
	TestHlodPageStore::Fixture replacement;
	Harness h; // Drains before the replacement file is removed, even on failure.
	if (!h.init() || !replacement.build(false, 5, 256, 0)) {
		return;
	}
	if (h.streamer._test_set_fault(p_fault ? Streamer::TEST_FAULT_SKIP_PUBLICATION_GENERATION : Streamer::TEST_FAULT_NONE) != OK) {
		FAIL("D3-2 generation fault must be installed on the real publication path");
		return;
	}
	if (!reach_holder(h, p_holder)) {
		return;
	}
	const uint64_t new_world = p_change == WORLD ? h.world + 1 : h.world;
	const uint64_t new_device = p_change == DEVICE ? h.device + 1 : h.device;
	const Ref<GaussianSplatHlodRuntimeData> next = p_change == SOURCE ? replacement.snapshot : h.fixture.snapshot;
	const Ref<GaussianSplatHlodPagedSource> expected_source = p_change == SOURCE ? replacement.source : h.fixture.source;
	const uint64_t expected_source_id = uint64_t(expected_source->get_instance_id());
	const uint64_t expected_content = expected_source->get_info().content_generation;
	// Source-only replacement deliberately uses identical content and unchanged
	// world/device epochs. A source-identity check must stand on its own.
	if (h.streamer._test_replace_source(next, new_world, new_device) != OK) {
		FAIL("D3-2 source/device replacement must invalidate live generations before draining old output");
		return;
	}
	h.demand.clear(); // Only OLD work may arrive during this drain.
	h.control.hold_uploads = h.control.hold_mailbox = h.control.hold_workers = false;
	if (!h.set_control() || !h.drain()) {
		return;
	}
	bool stale_published = false;
	const bool current = publications_current(h, expected_source_id, expected_content, new_world, new_device, stale_published);
	const Variant observed = h.streamer.get_payload_telemetry();
	if (observed.get_type() != Variant::DICTIONARY) {
		FAIL("D3-6 publication counter must reach the real route-labelled export"); return;
	}
	const Dictionary telemetry = observed;
	if (!telemetry.has("stale_generation_publications") || telemetry["stale_generation_publications"].get_type() != Variant::INT) {
		FAIL("D3-6 actual stale-publication counter export was removed"); return;
	}
	if (p_fault) {
		CHECK_MESSAGE(int64_t(telemetry["stale_generation_publications"]) > 0,
				"D3-6 counter must detect actual stale publication when the real generation guards are bypassed");
		CHECK_MESSAGE(h.sample.faults_triggered > 0,
				"D3-2 skip-generation fault never fired on a real stale completion (wiring removal must fail)");
		CHECK_MESSAGE(!current,
				"D3-2 stale-publication checker must detect actual stale metadata and RESIDENT publication");
		CHECK_MESSAGE(stale_published,
				"D3-2 stale-publication checker must detect actual stale metadata and RESIDENT publication");
	} else {
		CHECK(int64_t(telemetry["stale_generation_publications"]) == 0);
		CHECK_MESSAGE(current, "D3-2 stale source/world/device completion was published");
		CHECK_MESSAGE(!stale_published, "D3-2 stale source/world/device completion was published");
		CHECK_MESSAGE(no_residents(h), "D3-2 generation replacement must not make old output RESIDENT");
		CHECK_MESSAGE(h.streamer._test_publications().is_empty(), "D3-2 stale generation wrote metadata");
		CHECK_MESSAGE(h.sample.faults_triggered == 0, "D3-2 healthy generation path unexpectedly ran a fault");
	}
	if (!h.check_released()) {
		return;
	}
	if (!p_fault) {
		// Prove the checker is not accepting an eternally idle replacement.
		gs_hlod_runtime::DesiredRecord root;
		h.demand.push_back(root);
		const auto resumed = gs_pump_until([&]() {
			h.step();
			return !h.observations_ok || h.streamer.get_residency().state(0) == Residency::RESIDENT;
		});
		if (!resumed.ready() || !h.observations_ok) {
			FAIL("D3-2 current source/device never resumed publication after stale work drained");
			return;
		}
		const Vector<Streamer::TestPublication> published = h.streamer._test_publications();
		CHECK_MESSAGE(!published.is_empty(), "D3-2 replacement progress requires an actual metadata publication");
		CHECK_MESSAGE(publications_current(h, expected_source_id, expected_content, new_world, new_device, stale_published),
				"D3-2 resumed publication carries an obsolete generation");
	}
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] source and world replacement discard stale worker mailbox and ready output") {
	for (Holder holder : { WORKER, MAILBOX, READY }) {
		CAPTURE(holder);
		generation_contract(false, SOURCE, holder);
		generation_contract(false, WORLD, holder);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] device generation change discards stale worker mailbox and ready output") {
	for (Holder holder : { WORKER, MAILBOX, READY }) {
		CAPTURE(holder);
		generation_contract(false, DEVICE, holder);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] dropping the real output charge is detected by the saturation checker") {
	Harness h;
	if (!h.init()) {
		return;
	}
	if (h.streamer._test_set_fault(Streamer::TEST_FAULT_DROP_OUTPUT_CHARGE) != OK) {
		FAIL("D3-2 drop-output-charge fault must be installed on the real ownership handoff");
		return;
	}
	const auto overrun = gs_pump_until([&]() {
		h.step();
		return !h.observations_ok || (h.byte_overrun && h.sample.jobs_in_flight == 0);
	});
	if (!overrun.ready() || !h.observations_ok) {
		FAIL("D3-2 dropped output charge never produced the required retained-byte overrun");
		return;
	}
	CHECK_MESSAGE(h.sample.faults_triggered > 0,
			"D3-2 drop-output-charge fault never fired on real output (wiring removal must fail)");
	CHECK_MESSAGE(!h.bound_ok,
			"D3-2 saturation checker must detect the real retained-byte overrun after output charge is dropped");
	CHECK_MESSAGE(h.byte_overrun,
			"D3-2 saturation checker must detect the real retained-byte overrun after output charge is dropped");
	const Dictionary escaped = exported_queue(h.streamer, "retained");
	CHECK_MESSAGE(int64_t(escaped.get("bound_violations", -1)) > 0,
			"D3-6 export must retain the actual ownership/bound escape after the real output charge is dropped");
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] skipping real publication generation validation exposes stale residency") {
	generation_contract(true, SOURCE, READY);
	generation_contract(true, WORLD, READY);
	generation_contract(true, DEVICE, READY);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] validation upload failure and shutdown release ordinary reservations") {
	for (bool fail_validation : { true, false }) {
		CAPTURE(fail_validation);
		Harness h;
		if (!h.init()) {
			return;
		}
		h.control.fail_validation = fail_validation;
		h.control.fail_upload = !fail_validation;
		h.control.hold_uploads = false;
		// Set the fault BEFORE jobs start, and retain demand until a real failure
		// is observed. Cancellation must not masquerade as failed validation/IO.
		h.demand.clear();
		h.demand.push_back(gs_hlod_runtime::DesiredRecord());
		if (!h.set_control()) {
			return;
		}
		const auto failed = gs_pump_until([&]() {
			h.step();
			return !h.observations_ok || h.streamer.get_residency().stats().failed_total > 0;
		});
		if (!failed.ready() || !h.observations_ok) {
			FAIL("D3-2 injected validation/upload failure never failed a real ordinary load");
			return;
		}
		h.demand.clear();
		if (!h.drain() || !h.check_released()) {
			return;
		}
		CHECK_MESSAGE(no_residents(h), "D3-2 failed validation/upload must never publish RESIDENT nodes");
		CHECK_MESSAGE(h.streamer._test_publications().is_empty(), "D3-2 failed validation/upload wrote metadata");
	}
	for (Holder holder : { WORKER, MAILBOX, READY }) {
		CAPTURE(holder);
		Harness h;
		if (!h.init() || !reach_holder(h, holder)) {
			return;
		}
		// Shutdown must itself release any intentional hold before joining.
		h.streamer.shutdown();
		h.store.shutdown();
		if (!h.check_released()) {
			return;
		}
		const uint64_t released = h.sample.reservations_released;
		h.streamer.shutdown();
		h.store.shutdown();
		if (!h.check_released()) {
			return;
		}
		CHECK_MESSAGE(h.sample.reservations_released == released, "D3-2 repeated shutdown released an owner twice");
	}
}

} // namespace TestHlodPageStoreStreamer
