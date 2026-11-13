extern "C" int test_hlod_page_store_streamer_cpp_force_link() { return 0; }

#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gs_payload_page_store.h"
#include "../renderer/hlod_node_streamer.h"

namespace TestHlodPageStoreStreamer {

using Streamer = HlodNodeStreamer;
using Residency = gs_hlod_runtime::NodeResidency;
using TestGaussianSplatting::gs_pump_until;

// D3 design 3.3 / 4.2 D3-2. This checker is deliberately outside production
// code. It measures every payload owner, not just read-service or ready bytes.
// The same checker runs under the healthy and drop-output-charge controls.
static uint64_t retained(const Streamer::TestSnapshot &p_sample) {
	return p_sample.stored_bytes + p_sample.decode_bytes + p_sample.idle_decoder_bytes +
			p_sample.worker_output_bytes + p_sample.mailbox_bytes + p_sample.ready_bytes + p_sample.partial_upload_bytes;
}

static bool bounded(const Streamer::TestSnapshot &p_sample, const Streamer::Config &p_config, bool p_allow_oversized = false) {
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
	return p_sample.charged_bytes <= p_config.max_inflight_read_bytes &&
			retained(p_sample) <= p_config.max_inflight_read_bytes &&
			(p_allow_oversized || p_sample.peak_retained_bytes <= p_config.max_inflight_read_bytes);
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

	bool init(bool p_oversized = false, bool p_gsq = false) {
		if (!fixture.build(p_gsq, 5, 256, 0)) {
			return false;
		}
		if (fixture.source.is_null() || fixture.snapshot.is_null() ||
				fixture.source->get_nodes().size() != fixture.node_count || !WorkerThreadPool::get_singleton()) {
			FAIL("D3-2 streamer fixture requires a paged source, complete topology and worker pool");
			return false;
		}
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			const uint64_t reservation = fixture.source->get_node_read_working_set_bytes(node) +
					uint64_t(fixture.splats_per_node) * sizeof(PackedGaussian);
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
		config.max_inflight_read_bytes = p_oversized ? largest_reservation - 1 : 2 * largest_reservation;
		if (uint64_t(fixture.node_count) * fixture.splats_per_node * sizeof(PackedGaussian) <= config.max_inflight_read_bytes) {
			FAIL("D3-2 saturation fixture must demand more packed output than its entire byte envelope");
			return false;
		}
		config.atlas_pages = fixture.node_count; // One 256-splat node per default 1024-splat page.
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
		bound_ok = bound_ok && bounded(sample, config, p_oversized);
		if (p_oversized) {
			// High-water measurements catch exceptions that overlap entirely
			// between frame snapshots, too. Every fixture node has this bound.
			bound_ok = bound_ok && sample.peak_jobs_in_flight <= 1 && sample.peak_retained_bytes <= largest_reservation;
		}
		byte_overrun = byte_overrun || retained(sample) > config.max_inflight_read_bytes;
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
				const uint64_t next = fixture.source->get_node_read_working_set_bytes(uint32_t(sample.jobs_started)) +
						uint64_t(fixture.splats_per_node) * sizeof(PackedGaussian);
				capacity_full = capacity_full || next > config.max_inflight_read_bytes ||
						sample.charged_bytes > config.max_inflight_read_bytes - next;
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
	if (!h.finish_demand(true)) {
		return;
	}
	CHECK_MESSAGE(h.bound_ok, "D3-2 oversized progress must remain one-node-alone throughout transfer");
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
	if (p_fault) {
		CHECK_MESSAGE(h.sample.faults_triggered > 0,
				"D3-2 skip-generation fault never fired on a real stale completion (wiring removal must fail)");
		CHECK_MESSAGE(!current,
				"D3-2 stale-publication checker must detect actual stale metadata and RESIDENT publication");
		CHECK_MESSAGE(stale_published,
				"D3-2 stale-publication checker must detect actual stale metadata and RESIDENT publication");
	} else {
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
