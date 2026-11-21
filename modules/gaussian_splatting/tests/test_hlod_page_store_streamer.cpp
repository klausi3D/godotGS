extern "C" int test_hlod_page_store_streamer_cpp_force_link() { return 0; }

#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gaussian_splat_world_paged.h"
#include "../io/gs_payload_page_store.h"
#include "../lod/hlod_edit.h"
#include "../renderer/hlod_node_streamer.h"
#include "../renderer/hlod_contract_lifecycle.h"
#include "../core/gs_upload_sizing.h"

#include <cstring>
#include <memory>

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
			// The complete GSQ record and transient decoder coexist with both
			// decoded arrays. They are freed before packed output is allocated.
			if (!gs_hlod_runtime::allocation_bytes(node.stored_bytes, 1, window)) {
				FAIL("GSQ charge oracle cannot represent the complete stored allocation");
				return false;
			}
			const uint64_t reading_peak = window + gaussians + sh + gs_world_paged::gsq_decode_workspace_bytes();
			r_reservation = reading_peak > packing_peak ? reading_peak : packing_peak;
		}
		return true;
	}

	bool expected_retained_cap(uint64_t &r_cap) const {
		r_cap = config.max_inflight_read_bytes;
		uint64_t largest_normal = 0, largest_output = 0;
		Vector<uint64_t> base_costs;
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			uint64_t reservation = 0, output = 0;
			if (!node_reservation(node, reservation, output)) {
				return false;
			}
			const auto &record = fixture.source->get_nodes()[node];
			const uint64_t base_cost = record.stored_bytes + uint64_t(record.topology.payload_count) * sizeof(PackedGaussian);
			if (reservation > config.max_inflight_read_bytes || base_cost > config.max_inflight_read_bytes) {
				continue; // Oversized nodes cannot expand the normal envelope.
			}
			base_costs.push_back(base_cost);
			largest_normal = reservation > largest_normal ? reservation : largest_normal;
			largest_output = output > largest_output ? output : largest_output;
		}
		// Sort the complete independently measured corpus, rather than repeat
		// production's bounded insertion algorithm. Compressed sizes may differ.
		base_costs.sort();
		uint64_t base_sum = 0;
		uint32_t jobs = 0;
		for (uint64_t cost : base_costs) {
			if (jobs == config.max_jobs_in_flight || cost > config.max_inflight_read_bytes - base_sum) {
				break;
			}
			base_sum += cost;
			++jobs;
		}
		const uint64_t derived = uint64_t(jobs) * (largest_normal + largest_output);
		r_cap = derived > r_cap ? derived : r_cap;
		return true;
	}

	bool init(bool p_oversized = false, bool p_gsq = false, bool p_base_parallelism = false) {
		// The larger SH3 case reproduces the pre-migration per-node input size;
		// the small fixture keeps saturation/cancellation cases cheap. GSQ needs
		// enough output to saturate C despite its fixed decoder workspace.
		if (!fixture.build(p_gsq, p_base_parallelism ? 4 : 5,
				p_base_parallelism ? 4206 : (p_gsq ? 4096 : 256), p_base_parallelism ? 3 : 0)) {
			return false;
		}
		return start(p_oversized, p_base_parallelism);
	}

	bool init_gsq_sh3(bool p_oversized = false, bool p_single_node = false) {
		// 11000 splats fit B with the honest rounded phase maximum, but the
		// old double-stored/decoded-plus-output estimate exceeds B.
		// 16384 splats exceed B even with the honest charge.
		if (!fixture.build(true, p_single_node ? 0 : 2, p_oversized ? 16384 : 11000, 3)) {
			return false;
		}
		// Each new case asserts its own contract after initialization, so a cap
		// mismatch cannot prevent the charge/concurrency assertions from running.
		return start(p_oversized, true, true);
	}

	bool start(bool p_oversized, bool p_base_parallelism, bool p_gsq_contract = false) {
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
		for (const auto &record : fixture.source->get_nodes()) {
			if (record.stored_bytes > store_config.max_bytes_in_flight) {
				store_config.max_bytes_in_flight = record.stored_bytes;
			}
		}
		store_config.max_idle_handles = 0;
		store_config.decoder_retention_cap = 0; // Runtime HLOD uses transient leases.
		store_config.collect_content_digests = false;
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
		if (!p_gsq_contract && (retained_cap != expected_cap || retained_cap < config.max_inflight_read_bytes)) {
			FAIL("D3-2 retained cap must equal the measured base-concurrency/output derivation");
			return false;
		}
		if (!p_gsq_contract && uint64_t(fixture.node_count) * fixture.splats_per_node * sizeof(PackedGaussian) <= retained_cap) {
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
				sample.charged_bytes != 0 || sample.transport_reserved_bytes != 0 || retained(sample) != 0 || stats.live_reservations != 0 ||
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

// Freeze only the owner's inputs, never a measured charge/cap. All expected
// byte counts below come from this production-written fixture's actual records.
static bool gsq_sh3_contract_fixture(const Harness &p_harness) {
	if (p_harness.fixture.source.is_null() || p_harness.fixture.source->get_codec_id() != gs_gsq::kCodecId ||
			p_harness.fixture.degree != 3 || p_harness.fixture.high_order != 12 ||
			p_harness.config.max_inflight_read_bytes != 8ull * 1024 * 1024 ||
			p_harness.config.max_jobs_in_flight != 16) {
		FAIL("GSQ working-set cases require a GSQ SH3 V4 fixture and unchanged 8 MiB / 16-job inputs");
		return false;
	}
	return true;
}

static bool hold_gsq_workers(Harness &p_harness) {
	p_harness.control.hold_workers = true;
	if (!p_harness.set_control()) {
		return false;
	}
	const auto admitted = gs_pump_until([&]() {
		p_harness.step();
		return !p_harness.observations_ok || (p_harness.sample.jobs_started > 0 &&
				(p_harness.sample.pending_tickets == 0 || p_harness.sample.admission_waits > 0 ||
						p_harness.sample.jobs_in_flight == p_harness.config.max_jobs_in_flight));
	});
	if (!admitted.ready() || !p_harness.observations_ok) {
		FAIL("GSQ workers never reached observable admission before the read/decode hold");
		return false;
	}
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] unrepresentable payload allocation sizes are permanent") {
	// Valid V4 records bound every dimension before allocation. Exercise the
	// shared read/pack size check directly, without attempting huge allocations.
	uint64_t bytes = 0;
	CHECK(gs_hlod_runtime::checked_allocation_bytes(1024, 1, bytes, 1040) == OK);
	CHECK(bytes == 1040);
	const Error errors[] = {
		gs_hlod_runtime::checked_allocation_bytes(UINT64_MAX, 2, bytes), // Multiplication overflow.
		gs_hlod_runtime::checked_allocation_bytes(UINT64_MAX, 1, bytes), // Power-of-two rounding overflow.
		gs_hlod_runtime::checked_allocation_bytes(1024, 1, bytes, 1039), // Headers exceed the size limit.
		gs_hlod_runtime::checked_allocation_bytes(1025, 1, bytes, 1040), // Rounded payload exceeds the size limit.
	};
	for (Error error : errors) {
		CHECK(error == ERR_PARAMETER_RANGE_ERROR);
		CHECK(gs_hlod_runtime::classify_load_error(error) == gs_hlod_runtime::FAILURE_PERMANENT);
	}
	CHECK(bytes == 0);
}

static bool failure_root_descriptor(const TestHlodPageStore::Fixture &p_fixture, PayloadPageStore::PageDescriptor &r_page) {
	if (p_fixture.source.is_null() || p_fixture.source->get_nodes().size() != 1) {
		FAIL("failure-policy fixture requires one real V4 source record"); return false;
	}
	const auto &record = p_fixture.source->get_nodes()[0];
	if (record.stored_bytes < 2 || record.stored_bytes > PayloadReadService::read_slice_bytes) {
		FAIL("failure-policy fixture requires one complete codec-none read window"); return false;
	}
	r_page.id = 0;
	r_page.offset = record.offset;
	r_page.stored_bytes = uint32_t(record.stored_bytes);
	r_page.splat_count = record.topology.payload_count;
	r_page.sh_degree = p_fixture.degree;
	r_page.codec_params.codec_id = gs_world_writer::kCodecNone;
	memcpy(r_page.stored_digest, record.digest, sizeof(r_page.stored_digest));
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] page store refusal errors distinguish invalid requests from temporary unavailability") {
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(false, 0, 32, 0)) { return; }
	PayloadPageStore::PageDescriptor descriptor;
	if (!failure_root_descriptor(fixture, descriptor)) { return; }
	const PayloadReadService::SubmitResult refusals[] = {
		PayloadReadService::SUBMIT_TOO_LARGE, PayloadReadService::SUBMIT_INVALID,
		PayloadReadService::SUBMIT_NOT_RUNNING, PayloadReadService::SUBMIT_DUPLICATE_TICKET
	};
	for (auto refusal : refusals) {
		INFO("native refusal: ", int(refusal));
		PayloadPageStore store;
		PayloadPageStore::Config config;
		config.reader_count = 1;
		config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		config.max_bytes_in_flight = descriptor.stored_bytes - (refusal == PayloadReadService::SUBMIT_TOO_LARGE ? 1u : 0u);
		if (store.start(config) != OK || fixture.source->install_in_page_store(store, 1) != OK) {
			FAIL("refusal fixture could not install the real V4 catalog"); return;
		}
		if (refusal == PayloadReadService::SUBMIT_INVALID) {
			// Install a different descriptor, so the adapter's real request cannot match.
			LocalVector<PayloadPageStore::PageDescriptor> changed;
			changed.push_back(descriptor);
			++changed[0].offset;
			if (store.replace_source(fixture.path, 2, changed) != OK) { FAIL("could not install mismatched catalog"); return; }
		} else if (refusal == PayloadReadService::SUBMIT_NOT_RUNNING) {
			store.shutdown();
		} else if (refusal == PayloadReadService::SUBMIT_DUPLICATE_TICKET) {
			if (store.request(17, 1, descriptor) != PayloadReadService::SUBMIT_OK) { FAIL("could not retain a live ticket"); return; }
		}
		if (store.request(17, 1, descriptor) != refusal) { FAIL("native page store did not produce the intended refusal"); return; }
		Vector<Gaussian> gaussians;
		Vector<Vector3> sh;
		String why;
		const Error error = fixture.source->read_node_payload_from_store(store, 17, 1, 0, gaussians, sh, why);
		const bool permanent = refusal == PayloadReadService::SUBMIT_TOO_LARGE || refusal == PayloadReadService::SUBMIT_INVALID;
		const Error expected_error = refusal == PayloadReadService::SUBMIT_TOO_LARGE ? ERR_PARAMETER_RANGE_ERROR :
				(permanent ? ERR_INVALID_DATA : ERR_UNAVAILABLE);
		CHECK(error == expected_error);
		CHECK(gs_hlod_runtime::classify_load_error(error) ==
				(permanent ? gs_hlod_runtime::FAILURE_PERMANENT : gs_hlod_runtime::FAILURE_RETRY_LATER));
		CHECK(why == "v4_page_store_request_refused");
		CHECK(gaussians.is_empty());
		CHECK(sh.is_empty());
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] a real busy page store reaches residency as retry later and recovers") {
	// Keep one completed real page charged to a one-page store. This provokes
	// SUBMIT_BUSY independently of worker timing, without a fault-injection hook.
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(false, 0, 32, 0)) { return; }
	PayloadPageStore::PageDescriptor descriptor;
	if (!failure_root_descriptor(fixture, descriptor) || !WorkerThreadPool::get_singleton()) {
		FAIL("busy-streamer fixture requires its V4 descriptor and worker pool"); return;
	}
	PayloadPageStore store;
	PayloadPageStore::Config store_config;
	store_config.reader_count = 1;
	store_config.max_bytes_in_flight = descriptor.stored_bytes;
	store_config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
	store_config.decoder_retention_cap = 0;
	if (store.start(store_config) != OK) { FAIL("busy fixture could not start its page store"); return; }
	Streamer streamer;
	Streamer::Config config;
	config.atlas_pages = 1;
	config.max_jobs_in_flight = 1;
	if (streamer.init_with_page_store(fixture.snapshot, config, 21, 41, store) != OK) {
		FAIL("busy fixture could not initialize the real streamer"); return;
	}
	Streamer::TestControl control;
	control.cpu_upload_sink = true;
	if (streamer._test_set_control(control) != OK) { FAIL("busy fixture could not enable the CPU upload sink"); return; }
	PayloadPageStore::StoredPage held;
	if (store.request(UINT64_MAX, 1, descriptor) != PayloadReadService::SUBMIT_OK ||
			!store.wait(UINT64_MAX, 1, held) || held.get_error() != OK) {
		FAIL("busy fixture could not retain its real completed page"); return;
	}
	CHECK(store.stats().caller_bytes == descriptor.stored_bytes);
	Vector<Gaussian> gaussians;
	Vector<Vector3> sh;
	String why;
	CHECK(fixture.source->read_node_payload_from_store(store, UINT64_MAX - 1, 1, 0, gaussians, sh, why) == ERR_BUSY);
	gs_hlod_runtime::DesiredRecord demand;
	demand.node = 0;
	const auto step = [&]() {
		streamer.begin_frame();
		streamer.update(nullptr, Streamer::Buffers(), &demand, 1, nullptr, 0);
	};
	const auto refused = gs_pump_until([&]() {
		step();
		return streamer.get_residency().stats().failed_total != 0;
	});
	if (!refused.ready()) { FAIL("real busy store never reached the residency failure path"); return; }
	const auto stats = streamer.get_residency().stats();
	const auto telemetry = streamer.get_telemetry();
	CHECK(store.stats().refused_retained_busy >= 2);
	CHECK(stats.failed_total == 1);
	CHECK(stats.failed_retry_later_total == 1);
	CHECK(stats.failed_permanent_total == 0);
	CHECK(stats.retry_later_waiting == 1);
	CHECK(stats.permanently_failed == 0);
	CHECK_FALSE(streamer.root_failed());
	CHECK(telemetry.failed_total == stats.failed_total);
	CHECK(telemetry.failed_retry_later_total == 1);
	CHECK(telemetry.failed_permanent_total == 0);
	CHECK(telemetry.retry_later_waiting == 1);
	held.reset();
	const auto recovered = gs_pump_until([&]() {
		step();
		return streamer.get_residency().state(0) == Residency::RESIDENT;
	});
	if (!recovered.ready()) { FAIL("releasing real store pressure never made the root resident"); return; }
	CHECK(streamer.get_telemetry().retry_later_waiting == 0);
	CHECK(streamer.get_telemetry().failed_retry_later_total == 1);
	CHECK(streamer.get_telemetry().failed_permanent_total == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] read admission telemetry names the unchanged predicate order") {
	CHECK(Streamer::classify_admission_refusal(false, false, false, false) == Streamer::REFUSING_NONE);
	CHECK(Streamer::classify_admission_refusal(true, true, true, true) == Streamer::REFUSING_OVERSIZED_LATCH);
	CHECK(Streamer::classify_admission_refusal(false, true, true, true) == Streamer::REFUSING_TRANSPORT_BUDGET);
	CHECK(Streamer::classify_admission_refusal(false, false, true, true) == Streamer::REFUSING_OVERSIZED_WAIT_LIVE);
	CHECK(Streamer::classify_admission_refusal(false, false, false, true) == Streamer::REFUSING_RETAINED_BUDGET);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] pressure telemetry distinguishes measured idle zero from absent") {
	Harness h;
	if (!h.init()) { return; }
	h.demand.clear();
	h.step();
	const Streamer::Telemetry &t = h.streamer.get_telemetry();
	CHECK(t.pressure_measured);
	CHECK(t.gate_measured);
	CHECK(t.pages_free == h.config.atlas_pages);
	CHECK(t.largest_free_run == h.config.atlas_pages);
	CHECK(t.allocation_refused_total == 0);
	CHECK(t.live_reservations == 0);
	CHECK(t.pending_count == 0);
	CHECK(t.ready_count == 0);
	CHECK(t.refusing_gate == Streamer::REFUSING_NONE);
	CHECK(t.gate_frames_total[Streamer::REFUSING_NONE] == 1);
	for (uint32_t gate = 1; gate < Streamer::REFUSING_GATE_COUNT; ++gate) { CHECK(t.gate_frames_total[gate] == 0); }
	CHECK_FALSE(t.cancelled_running_jobs_measured);
	CHECK(h.streamer.get_pressure_eviction_events().is_empty());
	// Multiple views do not manufacture extra frame counts.
	h.streamer.update(nullptr, Streamer::Buffers(), nullptr, 0, nullptr, 0);
	CHECK(t.gate_frames_total[Streamer::REFUSING_NONE] == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] transition telemetry preserves absence measured idle and lifecycle reset (#1483 B)") {
	Harness h;
	CHECK_FALSE(h.streamer.get_telemetry().transitions_measured);
	CHECK(h.streamer.get_telemetry().transition_state == Residency::TRANSITIONS_ABSENT);
	if (!h.init()) { return; }
	h.demand.clear();
	h.step();
	const auto idle = h.streamer.get_telemetry();
	CHECK(idle.transitions_measured);
	CHECK(idle.transition_state == Residency::NORMAL);
	CHECK(idle.protected_sibling_nodes == 0);
	CHECK(idle.abandoned_transitions_total == 0);
	CHECK(idle.abandoned_transitions_this_frame == 0);
	CHECK(h.streamer.transition_workspace_bytes() >= h.streamer.get_residency().transition_workspace_bytes());
	CHECK(h.streamer.transition_workspace_bytes() > 0);
	const uint64_t source_generation = h.store.stats().source_generation;
	h.streamer.shutdown();
	CHECK_FALSE(h.streamer.get_telemetry().transitions_measured);
	CHECK(h.streamer.get_telemetry().transition_state == Residency::TRANSITIONS_ABSENT);
	if (h.streamer.init_with_page_store(h.fixture.snapshot, h.config, h.world, h.device, h.store) != OK || !h.set_control()) {
		FAIL("could not reinitialize transition observations"); return;
	}
	CHECK(h.store.stats().source_generation == source_generation + 1);
	h.step();
	CHECK(h.streamer.get_telemetry().transitions_measured);
	CHECK(h.streamer.get_telemetry().transition_state == Residency::NORMAL);
	CHECK(h.streamer.get_telemetry().abandoned_transitions_total == 0);
	CHECK(h.streamer.get_telemetry().abandoned_transitions_this_frame == 0);
	CHECK(idle.transitions_measured); // A copied checkpoint cannot alias the lifecycle reset.
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] shared owners fail closed without cancelling another owners work (#1483 B)") {
	for (bool reverse : { false, true }) {
		CAPTURE(reverse);
		Harness h;
		if (!h.init()) { return; }
		h.demand.clear();
		gs_hlod_runtime::DesiredRecord lower;
		lower.node = 4;
		h.demand.push_back(lower);
		if (!reach_holder(h, WORKER)) { return; }
		const auto before = h.streamer.get_residency().stats();
		if (before.loading != 1) { FAIL("shared-owner test requires a real held ordinary request"); return; }
		gs_hlod_runtime::CutRequest view;
		h.streamer.register_view(reverse ? 202 : 101, view);
		h.streamer.register_view(reverse ? 101 : 202, view);
		for (uint64_t owner : { 101u, 202u, 101u }) {
			h.streamer.update(nullptr, Streamer::Buffers(), h.demand.ptr(), h.demand.size(), nullptr, 0, owner);
			CHECK(h.streamer.get_telemetry().transition_state == Residency::UNSUPPORTED_OWNERS);
			CHECK(h.streamer.get_telemetry().transitions_measured);
			CHECK(h.streamer.get_telemetry().desired > 0);
			CHECK(h.streamer.get_residency().stats().cancelled_total == before.cancelled_total);
			CHECK(h.streamer.get_residency().stats().requested_total == before.requested_total);
			CHECK(h.streamer.get_residency().state(4) == Residency::LOADING);
		}
		CHECK(h.streamer.unregister_view(202));
		// Multiple instances are also owners, even when there is only one registered view.
		gs_hlod_runtime::DesiredRecord instances[2];
		// Both owners refer to the already-held request; this probe adds no new legacy work.
		instances[0].instance_key = 1; instances[0].node = 4;
		instances[1].instance_key = 2; instances[1].node = 4;
		h.streamer.update(nullptr, Streamer::Buffers(), instances, 2, nullptr, 0, 101);
		CHECK(h.streamer.get_telemetry().transition_state == Residency::UNSUPPORTED_OWNERS);
		CHECK(h.streamer.get_residency().stats().cancelled_total == 0);
		CHECK(h.streamer.get_residency().state(4) == Residency::LOADING);
		// Once only one owner remains, replacing the old single owner explicitly withdraws its work.
		// Rebind that owner after legacy admission, then expire its frame-only want stamp.
		h.streamer.update(nullptr, Streamer::Buffers(), h.demand.ptr(), h.demand.size(), nullptr, 0, 0);
		h.streamer.begin_frame();
		h.demand.clear();
		h.streamer.update(nullptr, Streamer::Buffers(), nullptr, 0, nullptr, 0, 101);
		CHECK(h.streamer.get_residency().stats().cancelled_total == 1);
		h.control.hold_workers = h.control.hold_mailbox = h.control.hold_uploads = false;
		if (!h.set_control() || !h.drain() || !h.check_released()) { return; }
		CHECK(h.streamer._test_publications().is_empty());
	}
}

TEST_CASE("[GaussianSplatting][HLOD] shared views admit missing demand and evict through the legacy path (#1483 B)") {
	Harness h;
	if (!h.init()) { return; }
	h.streamer.shutdown();
	h.config.atlas_pages = 3; // Synthetic pressure: pinned root plus two served nodes.
	if (h.streamer.init_with_page_store(h.fixture.snapshot, h.config, h.world, h.device, h.store) != OK) {
		FAIL("shared-view legacy fixture could not initialize its three-page atlas"); return;
	}
	h.retained_cap = h.streamer.get_retained_byte_cap();
	h.demand.clear();
	h.control.hold_uploads = false;
	auto step_owner = [&](uint64_t p_owner) {
		h.streamer.begin_frame();
		h.streamer.update(nullptr, Streamer::Buffers(), h.demand.ptr(), h.demand.size(), nullptr, 0, p_owner);
		h.sample = h.streamer._test_snapshot();
		h.observations_ok = h.observations_ok && h.sample.observation_error == OK;
		h.bound_ok = h.bound_ok && bounded(h.sample, h.config, h.retained_cap);
		const auto stats = h.streamer.get_residency().stats();
		CHECK(stats.pages_resident + stats.pages_in_flight + stats.pages_retiring <= 3);
	};
	auto load_missing = [&](uint32_t p_node, uint64_t p_owner, bool p_shared, bool p_evict) {
		CAPTURE(p_node);
		CAPTURE(p_owner);
		if (h.streamer.get_residency().state(p_node) != Residency::ABSENT) { FAIL("phase must offer only a missing node"); return false; }
		h.demand.clear();
		gs_hlod_runtime::DesiredRecord wanted;
		wanted.node = p_node;
		h.demand.push_back(wanted);
		h.control.hold_workers = true;
		if (!h.set_control()) { return false; }
		const auto before = h.streamer.get_residency().stats();
		step_owner(p_owner);
		const auto first = h.streamer.get_residency().stats();
		// Admission and eviction are synchronous model actions: the frozen path fails before an async wait.
		if ((!p_evict && first.requested_total != before.requested_total + 1) ||
				(p_evict && first.evicted_total != before.evicted_total + 1)) {
			FAIL("shared-view legacy admission did not request or evict for fresh demand"); return false;
		}
		if (p_evict) {
			const auto admitted = gs_pump_until([&]() {
				step_owner(p_owner);
				return !h.observations_ok || h.streamer.get_residency().state(p_node) == Residency::LOADING;
			});
			if (!admitted.ready() || !h.observations_ok) { FAIL("legacy eviction did not admit the missing node after retirement: ", admitted.describe()); return false; }
		}
		CHECK(h.streamer.get_residency().stats().requested_total == before.requested_total + 1);
		h.control.hold_workers = false;
		if (!h.set_control()) { return false; }
		const auto resident = gs_pump_until([&]() {
			step_owner(p_owner);
			return !h.observations_ok || (h.streamer.get_residency().state(p_node) == Residency::RESIDENT &&
					h.sample.tasks_outstanding == 0 && h.sample.live_reservations == 0);
		});
		if (!resident.ready() || !h.observations_ok) { FAIL("view's missing node never became actually resident: ", resident.describe()); return false; }
		CHECK(h.bound_ok);
		CHECK(h.streamer.get_residency().stats().cancelled_total == before.cancelled_total);
		CHECK(h.streamer.get_residency().stats().late_dropped_total == 0);
		CHECK(h.streamer.get_telemetry().protected_sibling_nodes == 0);
		CHECK(h.streamer.get_telemetry().abandoned_transitions_total == 0);
		if (p_shared) { CHECK(h.streamer.get_telemetry().transition_state == Residency::UNSUPPORTED_OWNERS); }
		return true;
	};
	if (!load_missing(0, 0, false, false)) { return; }
	gs_hlod_runtime::CutRequest view;
	view.frustum_enabled = false;
	view.focal_pixels = 100;
	view.tau_pixels = 1.0e9;
	view.node_budget = h.fixture.node_count;
	view.splat_budget = 1u << 30;
	view.record_budget = 1.0e9;
	h.streamer.register_view(101, view);
	h.streamer.register_view(202, view);
	if (!load_missing(1, 101, true, false) || !load_missing(2, 202, true, false)) { return; }
	CHECK(h.streamer.get_residency().state(1) == Residency::RESIDENT);
	CHECK(h.streamer.get_residency().stats().pages_free == 0);
	// Parent 1 and pinned root 0 survive node 3's allocation; the actual served node 2 pays for it.
	if (!load_missing(3, 101, true, true)) { return; }
	CHECK(h.streamer.get_residency().state(0) == Residency::RESIDENT);
	CHECK(h.streamer.get_residency().state(1) == Residency::RESIDENT);
	CHECK(h.streamer.get_residency().state(2) == Residency::ABSENT);
	CHECK(h.streamer.get_residency().state(3) == Residency::RESIDENT);
	CHECK(h.streamer.get_residency().stats().evicted_total == 1);
	CHECK(h.streamer.get_residency().stats().requested_total == 4);
	const auto publications = h.streamer._test_publications();
	if (publications.size() != 4) { FAIL("legacy views must publish all four actual node payloads"); return; }
	for (uint32_t node = 0; node < 4; ++node) {
		CHECK(publications[node].node == node);
		CHECK(publications[node].metadata_splat_count == h.fixture.splats_per_node);
	}
	CHECK(h.store.stats().reads.completed_ok == 4);
	if (!h.check_released()) { return; }
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] placement reserve capture preserves zero and clears absent frames and lifecycle resets (#1483)") {
	Harness h;
	CHECK_FALSE(h.streamer.get_telemetry().placement_reserve_measured);
	CHECK(h.streamer.get_telemetry().placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_ABSENT);
	if (!h.init()) { return; }
	const Streamer::Telemetry &t = h.streamer.get_telemetry();
	CHECK_FALSE(t.placement_reserve_measured);
	gs_hlod_runtime::CutHistory::Result cut;
	cut.status = gs_hlod_runtime::CutHistory::COMPLETE;
	cut.placement_reserve_state = gs_hlod_runtime::CutHistory::RESERVE_MEASURED;
	cut.placement_reserve_measured = true;
	cut.placement_reserve_pages = 3;
	h.streamer.capture_placement_reserve(cut);
	CHECK(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 3);
	CHECK(t.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_MEASURED);
	cut.placement_reserve_pages = 0;
	h.streamer.capture_placement_reserve(cut);
	CHECK(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0); // A measured zero survives independently of pressure counters.
	CHECK(t.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_MEASURED);
	cut.placement_reserve_measured = false;
	cut.placement_reserve_state = gs_hlod_runtime::CutHistory::RESERVE_UNPREPARED;
	cut.placement_reserve_pages = 99;
	h.streamer.capture_placement_reserve(cut);
	CHECK_FALSE(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0);
	CHECK(t.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_UNPREPARED);
	cut.placement_reserve_measured = true;
	cut.placement_reserve_state = gs_hlod_runtime::CutHistory::RESERVE_MEASURED;
	cut.placement_reserve_pages = 3;
	h.streamer.capture_placement_reserve(cut);
	cut.status = gs_hlod_runtime::CutHistory::INVALID;
	h.streamer.capture_placement_reserve(cut);
	CHECK_FALSE(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0);
	CHECK(t.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_ABSENT);
	cut.status = gs_hlod_runtime::CutHistory::UNAVAILABLE; // A loading selection still measures headroom.
	h.streamer.capture_placement_reserve(cut);
	CHECK(t.placement_reserve_measured);
	h.streamer.begin_frame(); // No selection follows this frame.
	CHECK_FALSE(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0);
	CHECK(t.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_ABSENT);
	h.streamer.capture_placement_reserve(cut);
	h.streamer.shutdown();
	CHECK_FALSE(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0);
	CHECK(t.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_ABSENT);
	h.streamer.capture_placement_reserve(cut); // An inactive streamer cannot resurrect the observation.
	CHECK_FALSE(t.placement_reserve_measured);
	if (!h.streamer.init(h.fixture.snapshot, h.config, h.world, h.device)) { FAIL("streamer reinitialization failed"); return; }
	CHECK_FALSE(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0);
	h.streamer.capture_placement_reserve(cut);
	if (!h.streamer.init(h.fixture.snapshot, h.config, h.world, h.device)) { FAIL("active streamer reset failed"); return; }
	CHECK_FALSE(t.placement_reserve_measured);
	CHECK(t.placement_reserve_pages == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][Edit] placement reserve is refreshed after a growing edit rebase and shrink (#1483)") {
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(false, 3, 256, 0)) { return; } // Whole tree stays larger than the pressure-test atlas.
	CHECK(fixture.snapshot->placement_reserve_prepared);
	CHECK(fixture.snapshot->max_node_payload_count == 256);
	CHECK(fixture.snapshot->placement_node_pages == 15);
	CHECK(fixture.snapshot->placement_page_splats == 1024);
	gs_hlod_runtime::LeafEdit edit;
	edit.node = 7;
	Vector<Gaussian> samples;
	if (!fixture.expected_payload(edit.node, samples, edit.sh) || samples.is_empty() ||
			!gs_resize_or_fail(edit.gaussians, 2049, "reserve growing edit fixture")) {
		FAIL("growing edit fixture payload unavailable"); return;
	}
	edit.gaussians.fill(samples[0]); // Duplicate validated, in-bounds samples; no geometry or node-cap change.
	Ref<GaussianSplatHlodRuntimeData> edited;
	String reason;
	if (gs_hlod_runtime::make_edited_snapshot(fixture.snapshot, 0, &edit, 1, edited, reason) != gs_hlod_runtime::EditStatus::OK || edited.is_null()) {
		FAIL("growing edit failed: ", reason.utf8().get_data()); return;
	}
	CHECK(edited->placement_reserve_prepared);
	CHECK(edited->max_node_payload_count == 2049);
	CHECK(edited->placement_node_pages == 14); // Three stale ancestors excluded; the grown leaf costs three pages.
	CHECK(fixture.snapshot->max_node_payload_count == 256);
	Ref<GaussianSplatHlodRuntimeData> fresh, rebased;
	if (fixture.world->capture_hlod_runtime_snapshot(fresh, reason) != OK || fresh.is_null() ||
			!gs_hlod_runtime::rebase_edited_snapshot(edited, fresh, rebased) || rebased.is_null()) {
		FAIL("edited reserve rebase failed: ", reason.utf8().get_data()); return;
	}
	CHECK(fresh->placement_reserve_prepared);
	CHECK(fresh->max_node_payload_count == 256);
	CHECK(fresh->placement_node_pages == 15);
	CHECK(rebased->placement_reserve_prepared);
	CHECK(rebased->max_node_payload_count == 2049);
	CHECK(rebased->placement_node_pages == 14);
	gs_hlod_runtime::CutRequest request;
	request.frustum_enabled = false;
	request.focal_pixels = 1;
	request.tau_pixels = 1e9;
	request.record_budget = 1e9;
	request.splat_budget = 10000;
	request.node_budget = 15;
	request.page_splats = 1024;
	request.page_budget = 9; // Forced frontier {2,4,7,8} costs 1+1+3+1, with three pages of headroom.
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(15)) { FAIL("cut workspace unavailable"); return; }
	const auto cut = history.select(*rebased.ptr(), request);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.placement_reserve_pages == 3);
	CHECK(cut.demanded_closure_pages == 6);
	edit.gaussians = samples;
	Ref<GaussianSplatHlodRuntimeData> shrunk;
	if (gs_hlod_runtime::make_edited_snapshot(rebased, 1, &edit, 1, shrunk, reason) != gs_hlod_runtime::EditStatus::OK || shrunk.is_null()) {
		FAIL("shrinking edit failed: ", reason.utf8().get_data()); return;
	}
	CHECK(shrunk->placement_reserve_prepared);
	CHECK(shrunk->max_node_payload_count == 256);
	CHECK(shrunk->placement_node_pages == 12);
	CHECK(rebased->max_node_payload_count == 2049);
	const auto smaller = history.select(*shrunk.ptr(), request);
	CHECK(smaller.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(smaller.placement_reserve_pages == 1);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] GSQ SH3 charge equals the rounded decode or pack phase maximum") {
	Harness h;
	if (!h.init_gsq_sh3(false, true) || !gsq_sh3_contract_fixture(h)) {
		return;
	}
	if (h.fixture.source->get_nodes().size() != 1 || h.fixture.node_count != 1) {
		FAIL("GSQ per-job peak requires exactly one source node and one reservation");
		return;
	}
	const auto &record = h.fixture.source->get_nodes()[0];
	const uint64_t count = record.topology.payload_count;
	uint64_t stored = 0, gaussians = 0, sh = 0, output = 0;
	if (!gs_hlod_runtime::allocation_bytes(record.stored_bytes, 1, stored) ||
			!gs_hlod_runtime::allocation_bytes(count, sizeof(Gaussian), gaussians) ||
			!gs_hlod_runtime::allocation_bytes(count * h.fixture.high_order, sizeof(Vector3), sh) ||
			!gs_hlod_runtime::allocation_bytes(count, sizeof(PackedGaussian), output)) {
		FAIL("GSQ exact-charge oracle cannot represent every independently rounded allocation");
		return;
	}
	const uint64_t workspace = gs_world_paged::gsq_decode_workspace_bytes();
	const uint64_t phase_a = stored + gaussians + sh + workspace;
	const uint64_t phase_b = gaussians + sh + output;
	const uint64_t expected = phase_a > phase_b ? phase_a : phase_b;
	const uint64_t decoded_logical = count * (sizeof(Gaussian) + h.fixture.high_order * sizeof(Vector3));
	const uint64_t doubled_read = 2 * (record.stored_bytes + decoded_logical) + workspace;
	const uint64_t old_read = doubled_read > phase_a ? doubled_read : phase_a;
	const uint64_t old_with_output = old_read + count * sizeof(PackedGaussian);
	const uint64_t old_charge = old_with_output > phase_b ? old_with_output : phase_b;
	if (expected > h.config.max_inflight_read_bytes || old_charge <= h.config.max_inflight_read_bytes) {
		FAIL("GSQ exact-charge fixture must change from oversized to normal solely through honest accounting");
		return;
	}
	if (!hold_gsq_workers(h)) {
		return;
	}
	if (h.sample.live_reservations != 1 || h.sample.jobs_in_flight != 1) {
		FAIL("GSQ exact-charge observation must contain one complete, unshrunk job reservation");
		return;
	}
	const uint64_t admitted_charge = h.sample.charged_bytes;
	CHECK_MESSAGE(admitted_charge == expected, "GSQ store charge must equal MAX(stored+decoded+workspace, decoded+rounded output)");
	CHECK_MESSAGE(admitted_charge < old_charge, "GSQ store charge still uses the old conservative expression");
	CHECK_MESSAGE(h.sample.oversized_reservation_bytes == 0, "An honest normal GSQ node must not take the oversized exception");
	h.control.hold_workers = false;
	if (!h.set_control()) {
		return;
	}
	const auto packed = gs_pump_until([&]() {
		h.step();
		return !h.observations_ok || (h.sample.ready_outputs == 1 && h.sample.jobs_in_flight == 0 &&
				h.sample.mailbox_outputs == 0);
	});
	if (!packed.ready() || !h.observations_ok) {
		FAIL("GSQ exact-charge job never completed a real store read and packed its held output");
		return;
	}
	CHECK_MESSAGE(h.store.stats().reads.completed_ok == 1, "Per-job peak requires a successful real stored-record read");
	CHECK_MESSAGE(h.sample.peak_retained_bytes > output, "Per-job peak must observe decoded storage, not only retained output");
	CHECK_MESSAGE(h.sample.peak_retained_bytes <= admitted_charge, "Observed per-job storage exceeds its actual reservation");
	CHECK_MESSAGE(h.sample.peak_retained_bytes <= expected, "Phase maximum undercharges observed stored/decoded/output storage");
	CHECK_MESSAGE(int64_t(exported_queue(h.streamer, "retained").get("bound_violations", -1)) == 0,
			"GSQ exact charge must remain an upper bound at every allocation event");
	if (!h.finish_demand()) {
		return;
	}
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] normal GSQ SH3 nodes load concurrently within retained and transport bounds") {
	Harness h;
	if (!h.init_gsq_sh3() || !gsq_sh3_contract_fixture(h)) {
		return;
	}
	uint64_t expected_cap = 0;
	if (!h.expected_retained_cap(expected_cap)) {
		return;
	}
	if (h.largest_reservation > h.config.max_inflight_read_bytes || expected_cap <= h.config.max_inflight_read_bytes) {
		FAIL("GSQ concurrency fixture must contain normal nodes and derive a retained cap above B");
		return;
	}
	if (!hold_gsq_workers(h)) {
		return;
	}
	CHECK_MESSAGE(h.sample.peak_jobs_in_flight > 1, "Normal GSQ nodes still serialize at the 8 MiB input budget");
	const Dictionary admitted = exported_queue(h.streamer, "retained");
	CHECK_MESSAGE(int64_t(admitted.get("peak_jobs", -1)) > 1, "GSQ must have multiple simultaneously live reservations");
	CHECK_MESSAGE(h.sample.oversized_reservation_bytes == 0, "Normal GSQ jobs must not acquire oversized reservations");
	if (h.sample.jobs_started == 0 || h.sample.jobs_started > h.fixture.node_count) {
		FAIL("GSQ transport oracle requires the admitted BFS prefix of existing fixture nodes");
		return;
	}
	uint64_t promised_stored = 0;
	for (uint32_t node = 0; node < uint32_t(h.sample.jobs_started); ++node) {
		promised_stored += h.fixture.source->get_nodes()[node].stored_bytes;
	}
	CHECK_MESSAGE(h.sample.transport_reserved_bytes == promised_stored, "Each GSQ job must promise its whole stored record before starting");
	CHECK_MESSAGE(h.sample.transport_reserved_bytes <= h.config.max_inflight_read_bytes, "GSQ promises overbook the unchanged read-service budget");
	CHECK_MESSAGE(h.sample.charged_bytes <= expected_cap, "Admitted GSQ jobs exceed the independently derived retained cap");
	if (!h.finish_demand()) {
		return;
	}
	const auto stats = h.store.stats();
	CHECK_MESSAGE(h.bound_ok, "GSQ charged or live storage escaped the retained cap during loading or upload");
	CHECK_MESSAGE(h.sample.peak_retained_bytes <= expected_cap, "GSQ actual storage peak exceeded the derived cap");
	CHECK_MESSAGE(int64_t(exported_queue(h.streamer, "retained").get("bound_violations", -1)) == 0,
			"GSQ retained referee observed an undercharge or cap escape");
	CHECK_MESSAGE(h.sample.jobs_started == h.fixture.node_count, "Every GSQ node must start exactly once");
	CHECK_MESSAGE(stats.reads.completed_ok == h.fixture.node_count, "Every GSQ node must complete its whole-record read");
	CHECK_MESSAGE(stats.refused_retained_busy == 0, "Retained page-store admission must never reject an admitted GSQ job");
	CHECK_MESSAGE(stats.reads.refused_busy == 0, "Read-service BUSY must be prevented by GSQ transport promises");
	CHECK_MESSAGE(stats.reads.completed_failed == 0, "GSQ concurrent reads must not fail");
	CHECK_MESSAGE(h.streamer.get_telemetry().payload_read_failures_total == 0, "Every admitted GSQ node must load without ERR_BUSY or another read failure");
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] GSQ SH3 retained cap derives from base concurrency and honest job plus output charges") {
	Harness h;
	if (!h.init_gsq_sh3() || !gsq_sh3_contract_fixture(h)) {
		return;
	}
	uint64_t expected_cap = 0;
	if (!h.expected_retained_cap(expected_cap)) {
		return;
	}
	if (expected_cap <= h.config.max_inflight_read_bytes) {
		FAIL("GSQ cap fixture must independently derive K*(R+O) above B");
		return;
	}
	CHECK_MESSAGE(h.retained_cap == expected_cap, "GSQ normal cap must equal max(B, K*(largest honest charge + largest rounded output))");
	const Variant payload = h.streamer.get_payload_telemetry();
	if (payload.get_type() != Variant::DICTIONARY) {
		FAIL("GSQ normal cap must be exported by the actual measured page-store route");
		return;
	}
	const Dictionary block = payload;
	CHECK_MESSAGE(int64_t(block.get("normal_retained_byte_bound", -1)) == int64_t(expected_cap),
			"GSQ telemetry must expose the independently derived normal retained bound");
	CHECK_MESSAGE(int64_t(exported_queue(h.streamer, "retained").get("byte_bound", -1)) == int64_t(expected_cap),
			"GSQ ledger referee must use the derived bound before jobs start");
	CHECK_MESSAGE(h.retained_cap > h.config.max_inflight_read_bytes, "GSQ still skips the D3-2 retained-cap derivation");
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] oversized GSQ SH3 nodes retain the single-flight exception") {
	Harness h;
	if (!h.init_gsq_sh3(true) || !gsq_sh3_contract_fixture(h)) {
		return;
	}
	if (h.largest_reservation <= h.config.max_inflight_read_bytes) {
		FAIL("Oversized GSQ fixture must exceed B with its independently computed honest charge");
		return;
	}
	if (!h.wait_held(true)) {
		return;
	}
	CHECK_MESSAGE(h.sample.live_reservations == 1, "An oversized GSQ node must be the only live reservation through upload");
	CHECK_MESSAGE(h.sample.jobs_started == 1, "Held oversized GSQ output must block every subsequent node");
	CHECK_MESSAGE(h.sample.oversized_reservation_bytes == h.largest_reservation, "Oversized GSQ must charge its full honest phase maximum");
	CHECK_MESSAGE(int64_t(exported_queue(h.streamer, "retained").get("peak_jobs", -1)) == 1,
			"Oversized GSQ reservations must never overlap between frame observations");
	const Variant payload = h.streamer.get_payload_telemetry();
	if (payload.get_type() != Variant::DICTIONARY) {
		FAIL("Oversized GSQ events must be reported by the measured store route");
		return;
	}
	const Dictionary block = payload;
	CHECK_MESSAGE(int64_t(block.get("oversized_node_events", -1)) > 0, "Oversized GSQ admission must retain its explicit event");
	if (!h.finish_demand(true)) {
		return;
	}
	CHECK_MESSAGE(h.bound_ok, "Oversized GSQ must remain one node alone throughout decode, packing and transfer");
	CHECK_MESSAGE(h.sample.peak_jobs_in_flight == 1, "Oversized GSQ worker jobs must remain single-flight");
	const Dictionary drained = exported_queue(h.streamer, "retained");
	CHECK_MESSAGE(int64_t(drained.get("peak_jobs", -1)) == 1, "Oversized GSQ live reservations must remain single-flight");
	CHECK_MESSAGE(int64_t(drained.get("bound_violations", -1)) == 0, "Oversized GSQ must never undercharge its real storage");
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
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
	const Streamer::Telemetry &pressure = h.streamer.get_telemetry();
	CHECK(pressure.gate_measured);
	CHECK(pressure.pending_count == h.sample.pending_tickets);
	CHECK(pressure.ready_count == h.sample.ready_outputs);
	CHECK(pressure.live_reservations == h.sample.live_reservations);
	CHECK(pressure.oversized_reservation_bytes == h.sample.oversized_reservation_bytes);
	CHECK(pressure.oversized_latch);
	CHECK(pressure.oversized_wait_live);
	CHECK(pressure.refusing_gate == Streamer::REFUSING_OVERSIZED_LATCH);
	CHECK(pressure.gate_frames_total[Streamer::REFUSING_OVERSIZED_LATCH] > 0);
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

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3Store] priority abandonment drains real output owners without publishing the abandoned child (#1483 B)") {
	// Use only existing streamer APIs. The CPU sink replaces device writes; request arbitration, store
	// reads, worker/mailbox/ready ownership, partial cursors and publication gates all remain real.
	for (uint32_t phase = 0; phase < 4; ++phase) {
		CAPTURE(phase);
		Harness h;
		if (!h.init()) { return; }
		h.config.atlas_pages = 3; // Root + resident sibling + the admitted missing child fill the atlas.
		h.config.upload_bytes_per_frame = 64 * sizeof(PackedGaussian); // Four slices for a 256-splat node.
		if (h.streamer.init_with_page_store(h.fixture.snapshot, h.config, h.world, h.device, h.store) != OK) {
			FAIL("could not initialize the three-page abandonment fixture"); return;
		}
		h.retained_cap = h.streamer.get_retained_byte_cap();
		h.control.hold_uploads = false;
		if (!h.set_control()) { return; }
		h.demand.clear();
		for (uint32_t node : { 0u, 3u }) {
			gs_hlod_runtime::DesiredRecord wanted;
			wanted.node = node;
			h.demand.push_back(wanted);
		}
		const auto seeded = gs_pump_until([&]() {
			h.step();
			return !h.observations_ok || (h.streamer.get_residency().state(0) == Residency::RESIDENT &&
					h.streamer.get_residency().state(3) == Residency::RESIDENT && h.sample.live_reservations == 0);
		});
		if (!seeded.ready() || !h.observations_ok) {
			FAIL("the real root and resident sibling never finished loading"); return;
		}
		h.demand.clear();
		gs_hlod_runtime::DesiredRecord lower;
		lower.node = 4;
		h.demand.push_back(lower);
		h.control.hold_uploads = true;
		const Holder holder = phase == 0 ? WORKER : (phase == 1 ? MAILBOX : READY);
		if (!reach_holder(h, holder)) { return; }
		if (phase == 3) {
			h.control.hold_uploads = false;
			if (!h.set_control()) { return; }
			h.step(); // One actual atlas slice, with the metadata-last boundary still ahead.
			if (h.sample.partial_upload_bytes == 0 || h.streamer.get_residency().state(4) != Residency::LOADING) {
				FAIL("the lower child never reached a real partial-upload owner"); return;
			}
		}
		CHECK(h.streamer.get_residency().stats().pages_free == 0);
		CHECK(h.streamer.get_telemetry().protected_sibling_nodes == 1);
		h.demand.clear();
		for (uint32_t node : { 5u, 6u, 4u }) {
			gs_hlod_runtime::DesiredRecord wanted;
			wanted.node = node;
			h.demand.push_back(wanted);
		}
		h.step(); // Higher-priority arrival must cause cancellation; the test never calls a cancel seam.
		CHECK(h.streamer.get_residency().stats().cancelled_total == 1);
		CHECK(h.streamer.get_residency().state(4) == Residency::ABSENT);
		CHECK(h.streamer.get_telemetry().desired == 3); // Unmet selection demand BEFORE abandonment filtering.
		CHECK(h.streamer.get_telemetry().abandoned_transitions_total == 1);
		CHECK(h.streamer.get_telemetry().abandoned_transitions_this_frame == 1);
		CHECK(h.streamer.get_telemetry().transition_state == Residency::RETIREMENT_WAIT);
		h.streamer.update(nullptr, Streamer::Buffers(), h.demand.ptr(), h.demand.size(), nullptr, 0);
		CHECK(h.streamer.get_telemetry().abandoned_transitions_total == 1);
		CHECK(h.streamer.get_telemetry().abandoned_transitions_this_frame == 1);
		h.control.hold_workers = h.control.hold_mailbox = h.control.hold_uploads = false;
		if (!h.set_control()) { return; }
		const auto progressed = gs_pump_until([&]() {
			h.step(); // Keep offering the abandoned demand; it must not steal the head's released capacity.
			return !h.observations_ok || (h.streamer.get_residency().state(5) == Residency::RESIDENT &&
					h.streamer.get_residency().state(6) == Residency::RESIDENT && h.sample.jobs_in_flight == 0 &&
					h.sample.pending_tickets == 0 && h.sample.mailbox_outputs == 0 && h.sample.ready_outputs == 0 &&
					h.sample.live_reservations == 0);
		});
		if (!progressed.ready() || !h.observations_ok) {
			FAIL("the higher-priority transition never completed after abandonment and owner drain"); return;
		}
		CHECK(h.streamer.get_residency().state(0) == Residency::RESIDENT);
		CHECK(h.streamer.get_residency().state(4) == Residency::ABSENT);
		CHECK(h.streamer.get_telemetry().late_dropped_total == 0);
		const Vector<Streamer::TestPublication> publications = h.streamer._test_publications();
		CHECK(publications.size() == 4); // Exactly the two seed nodes and the two head children.
		for (const auto &publication : publications) {
			CHECK(publication.node != 4); // A counter cannot substitute for absence at the real metadata write.
		}
		CHECK(h.bound_ok);
		if (!h.check_released()) { return; }
	}
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
	h.streamer.update(nullptr, Streamer::Buffers(), nullptr, 0, nullptr, 0);
	const Array victims = h.streamer.get_pressure_eviction_events();
	if (victims.size() != 1) {
		FAIL("The current-frame pressure export must contain the actual ordinary eviction");
		return;
	}
	const Dictionary victim = victims[0];
	CHECK(int64_t(victim.get("node", -1)) == int64_t(leaf));
	CHECK(int64_t(victim.get("class", -1)) == Residency::EVICTION_NEITHER);
	CHECK(int64_t(victim.get("frame", -1)) == int64_t(h.streamer.get_residency().frame()));
	CHECK(int64_t(victim.get("sequence", -1)) == int64_t(h.streamer.get_telemetry().evicted_total));
	CHECK(h.streamer.get_telemetry().evicted_neither_total == 1);
	CHECK(h.streamer.get_telemetry().evicted_in_cut_total == 0);
	h.step();
	CHECK(h.streamer.get_pressure_eviction_events().is_empty());
	CHECK(victims.size() == 1); // A later metrics query must not mutate a retained prior report.
	CHECK_MESSAGE(h.streamer.get_residency().state(leaf) != Residency::RESIDENT,
			"D3-2 evicted ordinary node remained RESIDENT");
	const uint64_t first_epoch = h.streamer.get_telemetry().pressure_epoch;
	CHECK(first_epoch > 0);
	h.streamer.shutdown();
	h.store.shutdown();
	h.check_released();
	// The same owner, source, world and device can be reinitialized. A distinct
	// report epoch must prevent the old event sequence from hiding new victims.
	if (!h.streamer.init(h.fixture.snapshot, h.config, h.world, h.device)) {
		FAIL("The same snapshot must reinitialize for the pressure epoch contract");
		return;
	}
	CHECK(h.streamer.get_telemetry().pressure_epoch > first_epoch);
	CHECK(h.streamer.get_telemetry().evicted_total == 0);
	CHECK(h.streamer.get_residency().eviction_event_count() == 0);
	CHECK(h.streamer.get_pressure_eviction_events().is_empty());
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

// A finite slow read which yields when the real service cancels its ticket.
// This models cancellation at a slice boundary, not interruption of an OS
// syscall. Against base, no cancellation arrives and the hook finishes late.
struct SlowRead {
	PayloadPageStore *store = nullptr;
	std::atomic<bool> entered{ false }, released{ false }, saw_cancel{ false };
	std::atomic<uint64_t> deadline_usec{ 0 };
	static constexpr uint64_t duration_usec = 4 * 1000 * 1000;

	static void hook(void *p_user, uint64_t p_ticket, uint64_t p_offset) {
		SlowRead &slow = *static_cast<SlowRead *>(p_user);
		uint64_t expected = 0;
		slow.deadline_usec.compare_exchange_strong(expected, OS::get_singleton()->get_ticks_usec() + duration_usec);
		slow.entered.store(true);
		while (!slow.released.load() && OS::get_singleton()->get_ticks_usec() < slow.deadline_usec.load()) {
			if (slow.store->_test_read_cancelled(p_ticket)) {
				slow.saw_cancel.store(true);
				break;
			}
			OS::get_singleton()->delay_usec(1000); // Independent reader; frames keep pumping on the owner.
		}
	}
};

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] partial object tail upload tears down the shared world owner (#1473)") {
	Harness h;
	if (!h.init() || !reach_holder(h, WORKER)) { return; }
	if (h.sample.pending_tickets == 0 || h.sample.jobs_in_flight == 0) {
		FAIL("#1473 upload failure requires a real admitted world request held before IO"); return;
	}
	uint8_t tail[] = { 1, 2 };
	bool committed = false, discarded = false;
	const bool published = gs_hlod_bundle::publish_instance_tail([&]() {
		tail[0] = 7; // Model RD accepting a first slice before a later slice fails.
		return false;
	}, [&]() { committed = true; }, [&]() {
		discarded = true;
		CHECK(h.streamer.is_active()); // Candidate disposal must not free the borrowed atlas/world owner.
		CHECK(h.streamer.uses_page_store(&h.store));
	}, [&]() {
		h.streamer.shutdown(); // Same production teardown order, including cancel-before-join.
		h.store.shutdown();
	});
	CHECK_FALSE(published);
	CHECK_FALSE(committed);
	CHECK(discarded);
	CHECK(tail[0] == 7);
	CHECK(tail[1] == 2);
	CHECK_FALSE(h.streamer.is_active());
	CHECK_FALSE(h.streamer.uses_page_store(&h.store));
	CHECK(h.streamer._test_snapshot().tasks_outstanding == 0);
	CHECK(h.streamer._test_snapshot().pending_tickets == 0);
	CHECK(h.streamer._test_snapshot().live_reservations == 0);
	CHECK(h.store.stats().live_reservations == 0);
	CHECK(h.store.stats().reads.bytes_in_flight == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] successful tail publication retains the held world request (#1473)") {
	Harness h;
	if (!h.init() || !reach_holder(h, WORKER)) { return; }
	const auto before = h.streamer._test_snapshot();
	const uint64_t identity = h.streamer.get_telemetry().pressure_epoch;
	bool committed = false, discarded = false, torn_down = false;
	CHECK(gs_hlod_bundle::publish_instance_tail([]() { return true; }, [&]() { committed = true; },
			[&]() { discarded = true; }, [&]() { torn_down = true; }));
	CHECK(committed);
	CHECK_FALSE(discarded);
	CHECK_FALSE(torn_down);
	CHECK(h.streamer.is_active());
	CHECK(h.streamer.uses_page_store(&h.store));
	CHECK(h.streamer.get_telemetry().pressure_epoch == identity);
	CHECK(h.streamer._test_snapshot().pending_tickets == before.pending_tickets);
	CHECK(h.streamer._test_snapshot().live_reservations == before.live_reservations);
}

// The publisher requires RD/sort resources. Replay its production lifecycle decision
// against real host owners; generation/page inputs model the director's four edits.
// This covers host residency/request lifetime, not publisher wiring or GPU RID identity.
TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] object add add remove remove-last preserves world identity and a live read (#1473)") {
	using namespace gs_hlod_bundle;
	SlowRead slow; // Outlives the store readers on every early return.
	auto owner = std::make_unique<Harness>();
	slow.store = &owner->store;
	slow.released.store(true);
	owner->store._test_set_read_hook(&SlowRead::hook, &slow);
	if (!owner->init()) { return; }
	owner->control.hold_uploads = false;
	if (!owner->set_control()) { return; }
	owner->demand.clear();
	for (uint32_t node : { 0u, 3u }) {
		gs_hlod_runtime::DesiredRecord wanted;
		wanted.node = node;
		owner->demand.push_back(wanted);
	}
	const auto seeded = gs_pump_until([&]() {
		owner->step();
		return !owner->observations_ok || (owner->streamer.get_residency().stats().resident == 2 &&
				owner->sample.live_reservations == 0 && owner->sample.jobs_in_flight == 0);
	});
	if (!seeded.ready() || !owner->observations_ok || owner->store.stats().reads.completed_ok != 2) {
		FAIL("#1473 requires two actually uploaded world nodes backed by real store reads"); return;
	}
	const auto resident_mask = owner->streamer.resident_mask();
	if (resident_mask.size() != owner->fixture.node_count) {
		FAIL("#1473 resident mask must cover the complete fixture topology"); return;
	}
	Vector<Vector<uint32_t>> page_lists;
	LocalVector<uint32_t> pt_bases;
	for (uint32_t node = 0; node < owner->fixture.node_count; ++node) {
		Vector<uint32_t> pages;
		uint32_t pt_base = 0;
		if (resident_mask[node] && !owner->streamer.get_residency().page_allocation(node, pages, pt_base)) {
			FAIL("#1473 resident world node must have a page run"); return;
		}
		page_lists.push_back(pages);
		pt_bases.push_back(pt_base);
	}
	slow.entered.store(false);
	slow.deadline_usec.store(0);
	slow.released.store(false);
	gs_hlod_runtime::DesiredRecord held;
	held.node = 4;
	owner->demand.push_back(held);
	const auto reading = gs_pump_until([&]() {
		owner->step();
		return !owner->observations_ok || (slow.entered.load() && owner->store.stats().reads.reading == 1 &&
				owner->sample.jobs_in_flight == 1 && owner->streamer.get_residency().state(4) == Residency::LOADING);
	});
	if (!reading.ready() || !owner->observations_ok || slow.saw_cancel.load() ||
			OS::get_singleton()->get_ticks_usec() + 2 * 1000 * 1000 >= slow.deadline_usec.load()) {
		FAIL("#1473 requires an uncancelled world read held at a real store slice boundary"); return;
	}
	const uint64_t init_id = owner->streamer.get_telemetry().pressure_epoch;
	const auto store_before = owner->store.stats();
	if (init_id == 0 || store_before.source_generation == 0 || store_before.live_reservations == 0) {
		FAIL("#1473 requires measured initialization/source identities and a live store reservation"); return;
	}
	ContractChange change;
	change.reserved_object_pages = 10; // Synthetic input only; phase A allocates no tail.
	change.prepared_asset_generation = change.asset_generation = 4;
	change.prepared_row_generation = change.row_generation = 9;
	const char *edits[] = { "asset add", "instance add", "instance remove", "remove-last asset" };
	for (uint32_t edit = 0; edit < 4; ++edit) {
		CAPTURE(edits[edit]);
		++change.asset_generation; // Registration/removal currently bumps both counters, including shared assets.
		++change.row_generation;
		change.object_pages = edit == 3 ? 0 : 1;
		const auto decision = hlod_contract_change_action(change);
		if (decision.action == ContractChangeAction::FULL_REBUILD) {
			// Same owner ordering as release_hlod_resources(), then host reinitialization.
			owner->streamer.shutdown();
			owner->store.shutdown();
			owner = std::make_unique<Harness>();
			slow.store = &owner->store;
			owner->store._test_set_read_hook(&SlowRead::hook, &slow);
			if (!owner->init()) { return; }
		}
		CHECK(decision.action == ContractChangeAction::INSTANCE_REGION_ONLY);
		CHECK(owner->streamer.is_active());
		CHECK(owner->streamer.uses_page_store(&owner->store));
		CHECK(owner->streamer.get_telemetry().pressure_epoch == init_id);
		CHECK(owner->store.stats().source_generation == store_before.source_generation);
		CHECK(owner->store.stats().collection_epoch == store_before.collection_epoch);
		CHECK(owner->streamer.get_residency().world_generation() == owner->world);
		CHECK(owner->streamer.get_residency().device_generation() == owner->device);
		for (uint32_t node = 0; node < owner->fixture.node_count; ++node) {
			CHECK(bool(owner->streamer.get_residency().state(node) == Residency::RESIDENT) == bool(resident_mask[node]));
			if (!resident_mask[node]) { continue; }
			Vector<uint32_t> pages;
			uint32_t pt_base = 0;
			CHECK(owner->streamer.get_residency().page_allocation(node, pages, pt_base));
			CHECK(pages == page_lists[node]);
			CHECK(pt_base == pt_bases[node]);
		}
		CHECK_FALSE(slow.saw_cancel.load());
		CHECK(owner->store.stats().reads.submitted == store_before.reads.submitted);
		CHECK(owner->store.stats().reads.reading == 1);
		CHECK(owner->store.stats().live_reservations == store_before.live_reservations);
		CHECK(owner->streamer.get_residency().state(4) == Residency::LOADING);
		change.prepared_asset_generation = change.asset_generation;
		change.prepared_row_generation = change.row_generation;
	}
	slow.released.store(true);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] an abandoned in-flight read counts as cancellation without capture or payload read failures (#1483 B)") {
	SlowRead slow; // Outlives readers even when a precondition fails.
	Harness h;
	slow.store = &h.store;
	slow.released.store(true); // Let the root and sibling seed reads finish normally.
	h.store._test_set_read_hook(&SlowRead::hook, &slow);
	if (!h.init()) { return; }
	h.config.atlas_pages = 3; // Root + resident sibling + in-flight child fill the atlas.
	if (h.streamer.init_with_page_store(h.fixture.snapshot, h.config, h.world, h.device, h.store) != OK) {
		FAIL("cancelled-read accounting fixture could not initialize its three-page atlas"); return;
	}
	h.retained_cap = h.streamer.get_retained_byte_cap();
	h.control.hold_uploads = false;
	if (!h.set_control()) { return; }
	h.demand.clear();
	for (uint32_t node : { 0u, 3u }) {
		gs_hlod_runtime::DesiredRecord wanted;
		wanted.node = node;
		h.demand.push_back(wanted);
	}
	const auto seeded = gs_pump_until([&]() {
		h.step();
		return !h.observations_ok || (h.streamer.get_residency().state(0) == Residency::RESIDENT &&
				h.streamer.get_residency().state(3) == Residency::RESIDENT && h.sample.jobs_in_flight == 0 &&
				h.sample.live_reservations == 0);
	});
	if (!seeded.ready() || !h.observations_ok) {
		FAIL("cancelled-read accounting requires a real resident root and sibling"); return;
	}
	const auto before = h.store.stats();
	CHECK(before.captures_failed == 0);
	CHECK(h.streamer.get_telemetry().payload_read_failures_total == 0);
	slow.entered.store(false);
	slow.saw_cancel.store(false);
	slow.deadline_usec.store(0);
	slow.released.store(false);
	h.demand.clear();
	gs_hlod_runtime::DesiredRecord lower;
	lower.node = 4;
	h.demand.push_back(lower);
	const auto reading = gs_pump_until([&]() {
		h.step();
		return !h.observations_ok || (slow.entered.load() && h.store.stats().reads.reading == 1 &&
				h.sample.jobs_in_flight == 1 && h.streamer.get_residency().state(4) == Residency::LOADING);
	});
	if (!reading.ready() || !h.observations_ok || slow.saw_cancel.load() ||
			OS::get_singleton()->get_ticks_usec() + 2 * 1000 * 1000 >= slow.deadline_usec.load()) {
		FAIL("abandonment requires an uncancelled real read still held at its slice boundary"); return;
	}
	CHECK(h.streamer.get_residency().stats().pages_free == 0);
	CHECK(h.streamer.get_telemetry().protected_sibling_nodes == 1);
	h.demand.clear();
	for (uint32_t node : { 5u, 6u, 4u }) {
		gs_hlod_runtime::DesiredRecord wanted;
		wanted.node = node;
		h.demand.push_back(wanted);
	}
	h.step(); // Only priority arbitration may cancel the read; never call a cancel seam.
	CHECK(h.streamer.get_telemetry().abandoned_transitions_total == 1);
	CHECK(h.streamer.get_residency().state(4) == Residency::ABSENT);
	CHECK(slow.saw_cancel.load());
	slow.released.store(true);
	h.demand.clear(); // Drain the cancelled capture without admitting another read.
	if (!h.drain()) { return; }
	const auto after = h.store.stats();
	const Dictionary capture = h.store.get_telemetry("hlod");
	CHECK(after.captures == before.captures + 1);
	CHECK(after.reads.submitted == before.reads.submitted + 1);
	CHECK(after.reads.cancelled_reading >= 1);
	CHECK(int64_t(capture.get("cancellations", -1)) >= 1);
	CHECK(after.reads.completed_failed == 0);
	CHECK(after.captures_failed == 0);
	CHECK(int64_t(capture.get("captures_failed", -1)) == 0);
	CHECK(after.last_failure_reason.is_empty());
	CHECK(String(capture.get("last_failure_reason", "missing")).is_empty());
	CHECK(h.streamer.get_telemetry().payload_read_failures_total == 0);
	CHECK(h.streamer.get_residency().stats().failed_permanent_total == 0);
	CHECK(h.bound_ok);
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] a real reader IO error still counts exactly one failed capture and payload read (#1483 B)") {
	Harness h;
	if (!h.init()) { return; }
	h.demand.clear();
	gs_hlod_runtime::DesiredRecord root;
	root.node = 0;
	h.demand.push_back(root);
	// The snapshot/catalog are already open, but no service read or handle exists yet.
	// Removing only this fixture's file injects a real reader open error, not a pack failure.
	if (h.store.stats().reads.submitted != 0 || DirAccess::remove_absolute(h.fixture.path) != OK) {
		FAIL("IO-error control could not remove its unread fixture payload"); return;
	}
	const auto failed = gs_pump_until([&]() {
		h.step();
		return !h.observations_ok || h.streamer.get_residency().state(0) == Residency::FAILED;
	});
	if (!failed.ready() || !h.observations_ok) {
		FAIL("the real reader IO error never reached the streamer's failed-read path"); return;
	}
	h.demand.clear(); // Stop after the first failed attempt; do not exercise retry policy here.
	if (!h.drain()) { return; }
	const auto stats = h.store.stats();
	const Dictionary capture = h.store.get_telemetry("hlod");
	CHECK(stats.captures == 1);
	CHECK(stats.reads.submitted == 1);
	CHECK(stats.reads.completed_failed == 1);
	CHECK(stats.captures_failed == 1);
	CHECK(int64_t(capture.get("captures_failed", -1)) == 1);
	CHECK(int64_t(capture.get("read_failures", -1)) == 1);
	CHECK(int64_t(capture.get("cancellations", -1)) == 0);
	CHECK_FALSE(stats.last_failure_reason.is_empty());
	CHECK(stats.last_failure_reason != "v4_page_store_request_cancelled");
	CHECK(String(capture.get("last_failure_reason", "")) == stats.last_failure_reason);
	CHECK(h.streamer.get_telemetry().payload_read_failures_total == 1);
	CHECK(h.streamer.get_residency().stats().failed_permanent_total == 1);
	CHECK(h.streamer._test_publications().is_empty());
	CHECK(h.bound_ok);
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] shutdown cancels slow IO before joining and never publishes cancelled jobs") {
	SlowRead slow; // Outlives the harness/readers, including on an early return.
	Harness h;
	slow.store = &h.store;
	h.store._test_set_read_hook(&SlowRead::hook, &slow);
	if (!h.init()) { return; }
	const auto started = gs_pump_until([&]() {
		h.step();
		return slow.entered.load() && h.sample.jobs_in_flight > 0;
	});
	if (!started.ready()) { FAIL("slow read never entered the real store with a live streamer job"); return; }
	if (OS::get_singleton()->get_ticks_usec() + 2 * 1000 * 1000 >= slow.deadline_usec.load()) {
		FAIL("slow-read setup exhausted the timing margin before shutdown"); return;
	}
	if (!h.observations_ok || h.sample.tasks_outstanding == 0 || h.sample.mailbox_publications != 0 ||
			h.store.stats().reads.reading == 0 || slow.saw_cancel.load()) {
		FAIL("shutdown timing requires a real outstanding uncancelled read with no published output"); return;
	}
	const uint64_t before = OS::get_singleton()->get_ticks_usec();
	h.streamer.shutdown();
	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - before;
	slow.released.store(true); // Also release hooks on the old (late-returning) path.
	CHECK_MESSAGE(elapsed < 1000 * 1000, "shutdown waited for slow IO instead of cancelling before join; usec=", elapsed);
	CHECK_MESSAGE(slow.saw_cancel.load(), "the slow read never saw cancellation from shutdown");
	h.sample = h.streamer._test_snapshot();
	CHECK(h.sample.tasks_outstanding == 0);
	CHECK(h.sample.jobs_in_flight == 0);
	CHECK(h.sample.mailbox_publications == 0); // A mailbox clear must not hide a late handoff.
	CHECK(h.sample.mailbox_outputs == 0);
	CHECK(h.sample.ready_outputs == 0);
	CHECK(no_residents(h));
	CHECK(h.streamer._test_publications().is_empty());
	CHECK(h.store.stats().reads.cancelled_reading > 0);
	if (!h.check_released()) { return; }
	const uint64_t released = h.sample.reservations_released;
	h.streamer.shutdown();
	CHECK(h.streamer._test_snapshot().reservations_released == released);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] shutdown cancels jobs held before request without publishing or reading") {
	Harness h;
	if (!h.init() || !reach_holder(h, WORKER)) { return; }
	if (h.sample.tasks_outstanding == 0 || h.sample.jobs_started == 0 ||
			h.sample.mailbox_publications != 0 || h.store.stats().reads.submitted != 0) {
		FAIL("pre-request cancellation requires an admitted held task with no read or mailbox handoff"); return;
	}
	h.streamer.shutdown(); // Must mark cancellation BEFORE releasing the deliberate worker hold.
	h.sample = h.streamer._test_snapshot();
	CHECK(h.sample.tasks_outstanding == 0);
	CHECK(h.sample.jobs_in_flight == 0);
	CHECK(h.sample.mailbox_publications == 0);
	CHECK(h.store.stats().reads.submitted == 0);
	CHECK(no_residents(h));
	CHECK(h.streamer._test_publications().is_empty());
	h.check_released();
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] read cancellation at the submission boundary stops all later raw windows") {
	TestHlodPageStore::Fixture fixture;
	PayloadPageStore store;
	// Multiple raw read windows make a lost cancellation resubmit the same key.
	if (!fixture.build(false, 0, 4206, 3)) { return; }
	PayloadPageStore::Config config;
	config.reader_count = 1;
	config.max_bytes_in_flight = fixture.source->get_max_record_bytes();
	config.max_idle_handles = 0;
	// install_in_page_store refuses a store without the runtime retained-byte policy.
	config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
	if (store.start(config) != OK || fixture.source->install_in_page_store(store, 1) != OK) {
		FAIL("cancel-race fixture could not install its real paged source"); return;
	}
	struct CancelAtSubmit : GaussianSplatHlodPagedSource::ReadStorageObserver {
		PayloadPageStore &store;
		bool cancelled = false;
		explicit CancelAtSubmit(PayloadPageStore &p_store) : store(p_store) {}
		void storage_changed(uint64_t p_stored, uint64_t p_decoded, uint64_t p_workspace) override {
			if (p_stored != 0 && !cancelled) {
				cancelled = true;
				store.cancel_requester(987); // No request exists yet: cancellation alone is not sticky.
			}
		}
		bool is_cancelled() const override { return cancelled; }
	} observer(store);
	Vector<Gaussian> gaussians;
	Vector<Vector3> sh;
	String why;
	const Error error = fixture.source->read_node_payload_from_store(store, 987, 987, 0, gaussians, sh, why, &observer);
	CHECK(observer.cancelled);
	CHECK(error == ERR_UNAVAILABLE);
	CHECK(gaussians.is_empty());
	CHECK(sh.is_empty());
	CHECK(store.stats().reads.submitted <= 1); // Never start another window after the cancelled submit.
	CHECK(store.stats().live_reservations == 0);
	CHECK(store.stats().reservations_acquired == store.stats().reservations_released);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Store] publisher pool reservation and store readers follow actual worker slots") {
	struct Case { uint32_t cpus, pool, slots, reserved; };
	const Case cases[] = { { 0, 0, 16, 0 }, { 32, 32, 16, 16 }, { 64, 16, 15, 1 }, { 8, 8, 4, 1 },
		{ 0, 4, 3, 1 }, { 2, 2, 1, 1 }, { 1, 1, 1, 0 }, { 0, 1, 1, 0 } };
	for (const Case &entry : cases) {
		CAPTURE(entry.cpus);
		CAPTURE(entry.pool);
		Streamer::Config config;
		config.set_worker_pool(entry.cpus, entry.pool);
		CHECK(config.logical_cpus == entry.cpus);
		CHECK(config.pool_threads == entry.pool);
		CHECK(config.reserved_pool_threads == entry.reserved);
		const uint32_t slots = GSUploadSizing::worker_count(config.max_jobs_in_flight, config.logical_cpus,
				config.pool_threads, config.reserved_pool_threads);
		CHECK(slots == entry.slots);
		CHECK(config.page_store_reader_count() == (entry.slots == 0 ? 1 : entry.slots));
		config.max_jobs_in_flight = 0;
		CHECK(config.page_store_reader_count() == 1);
	}
}

} // namespace TestHlodPageStoreStreamer
