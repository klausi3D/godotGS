#include "hlod_node_streamer.h"

#include "../core/gaussian_streaming.h"
#include "../core/gs_vector_alloc.h"
#include "../lod/hlod_edit.h"
#include "../io/gs_payload_page_store.h"

#include <utility>

using gs_hlod_runtime::NodeResidency;

// The node run granularity is the chunk streamer's page size (#1088): the shared allocator and its budget math.
static_assert(HlodNodeStreamer::Config::DEFAULT_PAGE_SPLATS == GaussianStreamingSystem::ATLAS_PAGE_SPLATS, "HLOD node runs use the streaming atlas page size");

HlodNodeStreamer::~HlodNodeStreamer() {
	shutdown();
}

namespace {
// Reuse the runtime's checked CowData allocation accounting, including the
// rounded capacity and both headers (core/templates/cowdata.h).
uint64_t storage_bytes(uint64_t p_count, uint64_t p_stride) {
	uint64_t bytes = 0;
	return gs_hlod_runtime::allocation_bytes(p_count, p_stride, bytes) ? bytes : UINT64_MAX;
}

template <typename T>
uint64_t vector_storage_bytes(const Vector<T> &p_vector) {
	return storage_bytes(uint64_t(p_vector.size()), sizeof(T));
}
} // namespace

void HlodNodeStreamer::ReservationLedger::observe_peak() {
	const uint64_t bytes = stored + decoded + output[0] + output[1] + output[2] + output[3];
	peak_storage = MAX(peak_storage, bytes);
}

HlodNodeStreamer::Reservation::Reservation(const Ref<ReservationLedger> &p_ledger, uint64_t p_charge, bool p_oversized, uint64_t p_transport, bool p_shadow) :
		ledger(p_ledger), charge(p_charge), transport(p_transport), oversized(p_oversized), shadow(p_shadow) {
	MutexLock lock(ledger->mutex);
	ledger->charged += charge;
	ledger->transport_reserved += transport;
	++ledger->acquired;
	++ledger->live;
	++ledger->jobs;
	++ledger->jobs_started;
	ledger->peak_jobs = MAX(ledger->peak_jobs, ledger->jobs);
	if (oversized) { ledger->oversized = charge; }
#ifdef TESTS_ENABLED
	if (shadow) { ++ledger->shadow_live; }
#endif
}

HlodNodeStreamer::Reservation::~Reservation() {
	MutexLock lock(ledger->mutex);
	ledger->stored -= stored;
	ledger->decoded -= decoded;
	ledger->output[owner] -= output;
#ifdef TESTS_ENABLED
	if (shadow) { ledger->shadow_output[owner] -= output; }
#endif
	ledger->transport_reserved -= transport;
	if (running) { --ledger->jobs; }
	if (token_live) {
		ledger->charged -= charge;
		--ledger->live;
		++ledger->released;
#ifdef TESTS_ENABLED
		if (shadow) { --ledger->shadow_live; }
#endif
		if (oversized) { ledger->oversized = 0; }
	}
}

void HlodNodeStreamer::Reservation::storage_changed(uint64_t p_stored, uint64_t p_decoded) {
	MutexLock lock(ledger->mutex);
	ledger->stored = ledger->stored - stored + p_stored;
	ledger->decoded = ledger->decoded - decoded + p_decoded;
	stored = p_stored;
	decoded = p_decoded;
	ledger->observe_peak();
}

void HlodNodeStreamer::Reservation::output_changed(uint64_t p_bytes) {
	MutexLock lock(ledger->mutex);
	ledger->output[owner] = ledger->output[owner] - output + p_bytes;
#ifdef TESTS_ENABLED
	if (shadow) { ledger->shadow_output[owner] = ledger->shadow_output[owner] - output + p_bytes; }
#endif
	output = p_bytes;
	ledger->observe_peak();
}

void HlodNodeStreamer::Reservation::move_output(OutputOwner p_owner) {
	MutexLock lock(ledger->mutex);
	ledger->output[owner] -= output;
	ledger->output[p_owner] += output;
#ifdef TESTS_ENABLED
	if (shadow) {
		ledger->shadow_output[owner] -= output;
		ledger->shadow_output[p_owner] += output;
	}
#endif
	owner = p_owner;
}

void HlodNodeStreamer::Reservation::finish_work() {
	MutexLock lock(ledger->mutex);
	// This runs only after the stored page, decoder and decoded arrays are freed.
	DEV_ASSERT(stored == 0 && decoded == 0 && transport == 0 && output <= charge);
	ledger->charged -= charge - output;
	charge = output;
}

void HlodNodeStreamer::Reservation::finish_transport() {
	MutexLock lock(ledger->mutex);
	ledger->transport_reserved -= transport;
	transport = 0;
}

void HlodNodeStreamer::Reservation::finish_worker() {
	MutexLock lock(ledger->mutex);
	if (running) { --ledger->jobs; running = false; }
}

#ifdef TESTS_ENABLED
void HlodNodeStreamer::Reservation::drop_output_charge() {
	MutexLock lock(ledger->mutex);
	if (token_live) {
		// The real output allocation and observation survive; only its permit is
		// released. This deliberately opens the admission escape under test.
		ledger->charged -= charge;
		charge = 0;
		--ledger->live;
		++ledger->released;
		if (shadow) { --ledger->shadow_live; }
		token_live = false;
		if (oversized) { ledger->oversized = 0; }
	}
}
#endif

void HlodNodeStreamer::ReadObservation::storage_changed(uint64_t p_stored, uint64_t p_decoded, uint64_t p_workspace) {
	if (reservation) { reservation->storage_changed(p_stored, p_decoded + p_workspace); }
}

HlodNodeStreamer::PackResult::PackResult(PackResult &&p_other) noexcept {
	*this = std::move(p_other);
}

HlodNodeStreamer::PackResult &HlodNodeStreamer::PackResult::operator=(PackResult &&p_other) noexcept {
	if (this != &p_other) {
		// LocalVector::remove_at moves over an existing owner. Free its storage
		// BEFORE move-assignment releases the old permit.
		raw.clear();
		quantized.clear();
		reservation.reset();
		ticket = p_other.ticket;
		ok = p_other.ok;
		source_instance_id = p_other.source_instance_id;
		content_generation = p_other.content_generation;
		reservation = std::move(p_other.reservation);
		raw = std::move(p_other.raw);
		quantized = std::move(p_other.quantized);
		meta = p_other.meta;
		quant = p_other.quant;
		uploaded_bytes = p_other.uploaded_bytes;
#ifdef TESTS_ENABLED
		test_first_write = p_other.test_first_write;
#endif
	}
	return *this;
}

Error HlodNodeStreamer::init_with_page_store(const Ref<GaussianSplatHlodRuntimeData> &p_data, const Config &p_config,
		uint64_t p_world_generation, uint64_t p_device_generation, PayloadPageStore &p_store) {
	if (p_data.is_null() || !p_data->is_paged()) { return ERR_INVALID_PARAMETER; }
	const uint64_t next_generation = page_store == &p_store ? store_generation + 1 : 1;
	shutdown();
	if (next_generation == 0) { return ERR_INVALID_PARAMETER; }
	const Error installed = p_data->paged_source->install_in_page_store(p_store, next_generation);
	if (installed != OK) { return installed; }
	if (!init(p_data, p_config, p_world_generation, p_device_generation)) { return ERR_INVALID_DATA; }
	page_store = &p_store;
	store_generation = next_generation;
	const Error budget_error = configure_retained_budget();
	if (budget_error != OK) { shutdown(); return budget_error; }
	return OK;
}

bool HlodNodeStreamer::node_reservation_bytes(const GaussianSplatHlodRuntimeData &p_source, uint32_t p_node,
		bool p_use_store, uint64_t &r_bytes, uint64_t &r_output) const {
	if (p_node >= uint32_t(p_source.nodes.size())) { return false; }
	const uint64_t count = p_source.nodes[p_node].topology.payload_count;
	r_output = storage_bytes(count, record_stride());
	r_bytes = r_output;
	if (r_output == UINT64_MAX) { return false; }
	if (!p_source.is_paged() || p_source.find_edited(p_node)) { return true; }
	const uint64_t read_working = p_source.paged_source->get_node_read_working_set_bytes(p_node);
	const uint64_t gaussians = storage_bytes(count, sizeof(Gaussian));
	const uint64_t sh = storage_bytes(count * p_source.high_order_count, sizeof(Vector3));
	if (gaussians == UINT64_MAX || sh > UINT64_MAX - gaussians || r_output > UINT64_MAX - gaussians - sh) { return false; }
	const uint64_t packing_peak = gaussians + sh + r_output;
	if (p_use_store && p_source.paged_source->get_codec_id() == 0u) {
		// Each raw transport window is freed before packing. Charge the larger
		// actual overlap, never a full stored record beside decoded/output arrays.
		r_bytes = MAX(read_working, packing_peak);
	} else {
		// Keep GSQ's existing conservative estimate and legacy admission intact.
		const uint64_t packed = count * record_stride();
		if (read_working > UINT64_MAX - packed) { return false; }
		r_bytes = MAX(read_working + packed, packing_peak);
	}
	return r_bytes != UINT64_MAX;
}

Error HlodNodeStreamer::configure_retained_budget() {
	retained_byte_cap = cfg.max_inflight_read_bytes;
	if (!page_store || data.is_null() || !data->is_paged() || data->paged_source->get_codec_id() != 0u) { return OK; }
	if (data->paged_source->get_nodes().size() != data->nodes.size()) { return ERR_INVALID_DATA; }
	const uint32_t slots = MIN(cfg.max_jobs_in_flight, uint32_t(data->nodes.size()));
	if (slots == 0) { return OK; }
	Vector<uint64_t> smallest_base;
	if (!gs_resize_or_fail(smallest_base, slots, "HLOD base admission measurements")) { return ERR_OUT_OF_MEMORY; }
	uint32_t used = 0;
	uint64_t largest_charge = 0, largest_output = 0;
	for (uint32_t node = 0; node < uint32_t(data->nodes.size()); ++node) {
		uint64_t charge = 0, output = 0;
		if (!node_reservation_bytes(*data.ptr(), node, true, charge, output)) { return ERR_INVALID_DATA; }
		const uint64_t packed = uint64_t(data->nodes[node].topology.payload_count) * record_stride();
		const uint64_t stored = data->paged_source->get_nodes()[node].stored_bytes;
		if (stored > UINT64_MAX - packed) { return ERR_INVALID_DATA; }
		const uint64_t base = stored + packed;
		// Oversized nodes retain the existing single-node exception, independent
		// of this derived cap. They cannot inflate the normal-work allowance.
		if (charge > cfg.max_inflight_read_bytes || base > cfg.max_inflight_read_bytes) { continue; }
		largest_charge = MAX(largest_charge, charge);
		largest_output = MAX(largest_output, output);
		uint32_t position = used;
		while (position > 0 && smallest_base[position - 1] > base) { --position; }
		if (position >= slots) { continue; }
		if (used < slots) { ++used; }
		for (uint32_t i = used - 1; i > position; --i) { smallest_base.write[i] = smallest_base[i - 1]; }
		smallest_base.write[position] = base;
	}
	uint32_t concurrency = 0;
	uint64_t base_sum = 0;
	for (uint32_t i = 0; i < used; ++i) {
		if (smallest_base[i] > cfg.max_inflight_read_bytes - base_sum) { break; }
		base_sum += smallest_base[i];
		++concurrency;
	}
	// B is the explicit base input (8 MiB by default), J the unchanged job cap.
	// K is the largest <=J count of this world's normal nodes whose base charges
	// (stored + logical packed) fit B. R/O are their largest measured honest job
	// and rounded output charges. Reserve K*R for work plus K*O for one completed
	// batch retained across mailbox/ready/upload: C=max(B,K*(R+O)). No corpus or
	// timing constant enters the formula. R,O<=B and K<=J bound C by max(B,2*J*B);
	// all arithmetic is checked. Further held output still stops admission at C.
	if (largest_output > UINT64_MAX - largest_charge) { return ERR_INVALID_DATA; }
	const uint64_t per_slot = largest_charge + largest_output;
	if (concurrency != 0 && per_slot > UINT64_MAX / concurrency) { return ERR_INVALID_DATA; }
	retained_byte_cap = MAX(retained_byte_cap, uint64_t(concurrency) * per_slot);
	return OK;
}

#ifdef TESTS_ENABLED
Error HlodNodeStreamer::_test_set_control(const TestControl &p_control) {
	if (ledger.is_null() || !active) { return ERR_UNAVAILABLE; }
	if (p_control.cpu_upload_sink && (test_metadata.size() != data->nodes.size() || test_quantization.size() != data->nodes.size())) {
		if (!gs_resize_or_fail(test_metadata, data->nodes.size(), "HLOD test metadata sink") ||
				!gs_resize_or_fail(test_quantization, data->nodes.size(), "HLOD test quantization sink")) { return ERR_OUT_OF_MEMORY; }
		test_metadata.fill(ChunkMetaGPU{});
		test_quantization.fill(ChunkQuantizationGPU{});
	}
	MutexLock lock(ledger->mutex);
	const bool release_workers = ledger->control.hold_workers && !p_control.hold_workers;
	ledger->control = p_control;
	if (release_workers) { ledger->worker_gate.post(ledger->held_workers); }
	return OK;
}

Error HlodNodeStreamer::_test_set_fault(TestFault p_fault) {
	if (ledger.is_null() || !active) { return ERR_UNAVAILABLE; }
	MutexLock lock(ledger->mutex);
	if (ledger->live != 0 || ledger->jobs != 0) { return ERR_BUSY; }
	if (p_fault < TEST_FAULT_NONE || p_fault > TEST_FAULT_PARTIAL_COMMIT) { return ERR_INVALID_PARAMETER; }
	ledger->fault = p_fault;
	ledger->faults_triggered = 0;
	return OK;
}

HlodNodeStreamer::TestSnapshot HlodNodeStreamer::_test_snapshot() const {
	TestSnapshot result;
	if (ledger.is_null()) { return result; }
	MutexLock lock(ledger->mutex);
	result.observation_error = OK;
	result.stored_bytes = ledger->stored;
	result.decode_bytes = ledger->decoded;
	result.worker_output_bytes = ledger->output[OUTPUT_WORKER] - ledger->shadow_output[OUTPUT_WORKER];
	result.mailbox_bytes = ledger->output[OUTPUT_MAILBOX] - ledger->shadow_output[OUTPUT_MAILBOX];
	result.ready_bytes = ledger->output[OUTPUT_READY] - ledger->shadow_output[OUTPUT_READY];
	result.partial_upload_bytes = ledger->output[OUTPUT_UPLOAD] - ledger->shadow_output[OUTPUT_UPLOAD];
	if (ledger->observe_shadows) {
		result.shadow_observation_error = OK;
		result.shadow_worker_output_bytes = ledger->shadow_output[OUTPUT_WORKER];
		result.shadow_mailbox_bytes = ledger->shadow_output[OUTPUT_MAILBOX];
		result.shadow_ready_bytes = ledger->shadow_output[OUTPUT_READY];
		result.shadow_partial_upload_bytes = ledger->shadow_output[OUTPUT_UPLOAD];
		result.shadow_live_reservations = ledger->shadow_live;
		result.shadow_transferred_bytes = shadow_transferred_bytes;
		result.shadow_transfer_count = shadow_transfer_count;
		result.shadow_ready_outputs = shadow_ready.size();
		result.shadow_pending_tickets = shadow_pending.size();
		// shadow_pending contains Tickets; shadow_transferred contains ShadowTransfers.
		// Neither type has a payload owner; the zero fields describe actual storage.
	}
	// V4 leases are transient, so there are no retained idle decoder contexts.
	result.charged_bytes = ledger->charged;
	result.peak_retained_bytes = ledger->peak_storage;
	result.peak_jobs_in_flight = ledger->peak_jobs;
	result.oversized_reservation_bytes = ledger->oversized;
	result.reservations_acquired = ledger->acquired;
	result.reservations_released = ledger->released;
	result.live_reservations = ledger->live;
	result.jobs_started = ledger->jobs_started;
	result.admission_waits = ledger->admission_waits;
	result.jobs_in_flight = ledger->jobs;
	result.pending_tickets = pending.size();
	if (mailbox.is_valid()) {
		for (const PackResult &r : mailbox->done) { if (!r.ticket.shadow) { ++result.mailbox_outputs; } }
	}
	result.ready_outputs = ready.size();
	result.faults_triggered = ledger->faults_triggered;
	return result;
}

Vector<HlodNodeStreamer::TestPublication> HlodNodeStreamer::_test_publications() const {
	if (ledger.is_null()) { return Vector<TestPublication>(); }
	MutexLock lock(ledger->mutex);
	return ledger->publications;
}

Error HlodNodeStreamer::_test_observe_shadow_transfers() {
	if (ledger.is_null() || !active) { return ERR_UNAVAILABLE; }
	MutexLock lock(ledger->mutex);
	ledger->observe_shadows = true;
	return OK;
}

Error HlodNodeStreamer::_test_shadow_writes(Vector<TestWrite> &r_writes) const {
	if (ledger.is_null()) { return ERR_UNAVAILABLE; }
	MutexLock lock(ledger->mutex);
	if (!ledger->observe_shadows) { return ERR_UNAVAILABLE; }
	r_writes = ledger->writes;
	return OK;
}

Error HlodNodeStreamer::_test_shadow_runs(Vector<NodeResidency::Ticket> &r_runs) const {
	if (ledger.is_null() || !active) { return ERR_UNAVAILABLE; }
	MutexLock lock(ledger->mutex);
	if (!ledger->observe_shadows) { return ERR_UNAVAILABLE; }
	r_runs.clear();
	if (staged_data.is_valid()) {
		for (const auto &leaf : staged_data->edited) {
			NodeResidency::Ticket ticket;
			if (residency.shadow_ticket(leaf.node, ticket)) { r_runs.push_back(ticket); }
		}
	}
	return OK;
}

Error HlodNodeStreamer::_test_replace_source(const Ref<GaussianSplatHlodRuntimeData> &p_data, uint64_t p_world, uint64_t p_device) {
	if (!active || !page_store || p_data.is_null() || !p_data->is_paged()) { return ERR_UNAVAILABLE; }
	const bool had_edit = residency.is_edit_staged();
	if (had_edit) { cancel_staged_edit(); }
	{
		MutexLock lock(ledger->mutex);
		ledger->control.hold_workers = false;
		ledger->worker_gate.post(ledger->held_workers);
	}
	// Keep real old outputs in the mailbox/ready queues; only their input-side
	// transport callers must drain before replacing the one store's catalog.
	collect_finished_tasks(true);
	if (had_edit) { release_staged_edit_data(); } // Drop shadow completions that arrived during the join.
	const Error installed = p_data->paged_source->install_in_page_store(*page_store, store_generation + 1);
	if (installed != OK) { return installed; }
	Vector<NodeResidency::NodeInfo> infos;
	if (!gs_hlod_runtime::make_node_infos(*p_data.ptr(), infos)) { return ERR_INVALID_DATA; }
	const NodeResidency::Config rc = residency.config();
	if (!residency.reset(infos, rc, p_world, p_device)) { return ERR_INVALID_DATA; }
	pending.clear();
	if (!had_edit) {
		test_metadata.fill(ChunkMetaGPU{});
		test_quantization.fill(ChunkQuantizationGPU{});
	}
	data = p_data;
	world_generation = p_world;
	device_generation = p_device;
	++store_generation;
	const Error budget_error = configure_retained_budget();
	if (budget_error != OK) { shutdown(); }
	return budget_error;
}

Error HlodNodeStreamer::_test_cancel_node(uint32_t p_node) {
	if (!active || p_node >= residency.node_count()) { return ERR_INVALID_PARAMETER; }
	residency.cancel(p_node);
	return OK;
}

Error HlodNodeStreamer::_test_evict_node(uint32_t p_node) {
	if (!active || p_node >= residency.node_count()) { return ERR_INVALID_PARAMETER; }
	return residency.evict(p_node) ? OK : ERR_BUSY;
}
#endif

bool HlodNodeStreamer::init(const Ref<GaussianSplatHlodRuntimeData> &p_data, const Config &p_config, uint64_t p_world_generation, uint64_t p_device_generation) {
	shutdown();
	if (p_data.is_null() || p_config.atlas_pages == 0 || p_config.page_splats == 0 || p_data->nodes.is_empty()) {
		return false;
	}
	Vector<NodeResidency::NodeInfo> infos;
	if (!gs_hlod_runtime::make_node_infos(*p_data.ptr(), infos)) {
		return false;
	}
	NodeResidency::Config rc;
	rc.page_capacity = p_config.atlas_pages;
	rc.page_splats = p_config.page_splats;
	rc.retire_delay_frames = p_config.retire_delay_frames;
	rc.committed_edit_revision = p_data->edit_revision;
	if (!residency.reset(infos, rc, p_world_generation, p_device_generation)) {
		return false;
	}
	// The root is the complete coarse fallback and the pinned base of every cut: it must fit on its own.
	if (!p_data->is_stale(0) && residency.pages_for(0) > p_config.atlas_pages) {
		return false;
	}
	cfg = p_config;
	retained_byte_cap = p_config.max_inflight_read_bytes;
	data = p_data;
	world_generation = p_world_generation;
	device_generation = p_device_generation;
	mailbox.instantiate();
	read_stats.instantiate();
	ledger.instantiate();
	mailbox->ledger = ledger;
	frame = 0;
	frame_written_bytes = shadow_transferred_bytes = staged_publication_bytes = 0;
	shadow_transfer_count = 0;
	frame_edit_uploads = in_edit_commit = false;
	next_shadow_upload = next_shadow_job = true;
	last_commit_result = pending_commit_result = COMMIT_NOTHING;
#ifdef TESTS_ENABLED
	test_metadata.clear();
	test_quantization.clear();
#endif
	telemetry = Telemetry();
	telemetry.pages_capacity = p_config.atlas_pages;
	telemetry.edit_revision = p_data->edit_revision;
	active = true;
	return true;
}

void HlodNodeStreamer::collect_finished_tasks(bool p_wait_all) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	for (uint32_t i = 0; i < tasks.size();) {
		if (pool && (p_wait_all || pool->is_task_completed(tasks[i]))) {
			pool->wait_for_task_completion(tasks[i]); // Returns at once for a completed task and releases it.
			tasks[i] = tasks[tasks.size() - 1];
			tasks.resize(tasks.size() - 1);
		} else if (!pool) {
			tasks.clear();
		} else {
			++i;
		}
	}
}

void HlodNodeStreamer::shutdown() {
	if (mailbox.is_null() && tasks.is_empty()) {
		return;
	}
	active = false;
	drain_jobs_and_output();
	mailbox.unref();
	data.unref();
	read_stats.unref();
	page_store = nullptr;
}

void HlodNodeStreamer::drain_jobs_and_output() {
#ifdef TESTS_ENABLED
	if (ledger.is_valid()) {
		MutexLock lock(ledger->mutex);
		ledger->control.hold_workers = false;
		ledger->worker_gate.post(ledger->held_workers);
	}
#endif
	// Jobs hold only the snapshot and the mailbox, never this object, but waiting here keeps the last
	// reference to the snapshot (and its world) from being dropped on a worker thread.
	collect_finished_tasks(true);
	residency.cancel_edit();
	for (uint32_t node = 0; node < residency.node_count(); ++node) {
		if (residency.state(node) == NodeResidency::LOADING || residency.state(node) == NodeResidency::UPLOADING) { residency.cancel(node); }
	}
	pending.clear();
	ready.clear();
	release_staged_edit_data();
	pending_metadata_clears.clear();
	if (mailbox.is_valid()) {
		MutexLock lock(ledger->mutex);
		mailbox->done.clear();
	}
#ifdef TESTS_ENABLED
	test_leaked_shadow_permits.clear();
#endif
}

void HlodNodeStreamer::run_job(void *p_userdata) {
	Job *job = static_cast<Job *>(p_userdata);
	PackResult result;
	result.ticket = job->ticket;
	result.reservation = std::move(job->reservation);
	const GaussianSplatHlodRuntimeData &d = *job->data.ptr();
	if (d.is_paged()) {
		result.source_instance_id = uint64_t(d.paged_source->get_instance_id());
		result.content_generation = d.paged_source->get_info().content_generation;
	}
#ifdef TESTS_ENABLED
	// Only this deliberate test hold can wait before I/O. Ordinary admission
	// never starts a worker which must wait for output capacity.
	if (result.reservation) {
		const Ref<ReservationLedger> &accounting = job->mailbox->ledger;
		for (;;) {
			{
				MutexLock lock(accounting->mutex);
				if (!accounting->control.hold_workers) { break; }
				++accounting->held_workers;
			}
			accounting->worker_gate.wait();
			{
				MutexLock lock(accounting->mutex);
				--accounting->held_workers;
			}
		}
	}
#endif
	const uint32_t count = job->ticket.node < uint32_t(d.nodes.size()) ? d.nodes[job->ticket.node].topology.payload_count : 0;
	gs_hlod_pack::Output out;
	SHCompressionMetrics metrics;
	out.meta = &result.meta;
	out.quant = &result.quant;
	out.metrics = &metrics;
	const auto allocate_output = [&]() -> bool {
		bool sized = count != 0;
		if (sized && job->params.quantized) {
			sized = gs_resize_or_fail(result.quantized, count, "HLOD node payload (quantized)");
			out.quantized = result.quantized.ptrw();
		} else if (sized) {
			sized = gs_resize_or_fail(result.raw, count, "HLOD node payload");
			out.raw = result.raw.ptrw();
		}
		if (result.reservation) {
			result.reservation->output_changed(vector_storage_bytes(result.raw) + vector_storage_bytes(result.quantized));
		}
		return sized;
	};
	bool read_ok = false;
	const GaussianSplatHlodRuntimeData::EditedLeaf *edited_leaf = d.find_edited(job->ticket.node);
	if (edited_leaf) {
		// Package F1: an edited leaf is packed from its in-memory replacement payload, never from the file (a V4 package is
		// immutable) and never from the base snapshot's leaf.
		gs_hlod_pack::PayloadView view;
		view.gaussians = edited_leaf->gaussians.ptr();
		view.sh = edited_leaf->sh.is_empty() ? nullptr : edited_leaf->sh.ptr();
		result.ok = allocate_output() && uint64_t(edited_leaf->gaussians.size()) == count &&
				gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out, &view);
	} else if (d.is_paged()) {
		// Defer packed allocation until the stored record/decoder are freed. The
		// conservative estimate covers input + decoded arrays, then decoded +
		// packed output, without three rounded buffers overlapping unnecessarily.
		Vector<Gaussian> gaussians;
		Vector<Vector3> sh;
		String why;
		ReadObservation observation;
		observation.reservation = result.reservation.get();
		const Error read_error = job->store ?
				d.paged_source->read_node_payload_from_store(*job->store, job->ticket.request_generation,
						job->ticket.request_generation, job->ticket.node, gaussians, sh, why, &observation) :
				d.paged_source->read_node_payload(job->ticket.node, gaussians, sh, why);
		if (result.reservation) { result.reservation->finish_transport(); }
		read_ok = read_error == OK;
		if (read_error == OK) {
			if (result.reservation) {
				result.reservation->storage_changed(0, vector_storage_bytes(gaussians) + vector_storage_bytes(sh));
			}
			gs_hlod_pack::PayloadView view;
			view.gaussians = gaussians.ptr();
			view.sh = sh.is_empty() ? nullptr : sh.ptr();
			result.ok = allocate_output() && gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out, &view);
		}
	} else {
		result.ok = allocate_output() && gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out);
	}
	// The scoped decoded arrays above have been destroyed before releasing work.
	if (job->counted) { job->stats->end_read(job->reserved_bytes, job->read_bytes, read_ok); }
	{
		MutexLock lock(job->mailbox->ledger->mutex);
		if (result.reservation) {
			result.reservation->storage_changed(0, 0);
			result.reservation->finish_work();
#ifdef TESTS_ENABLED
			if (job->mailbox->ledger->control.fail_validation) { result.ok = false; }
			if (job->mailbox->ledger->fault == TEST_FAULT_DROP_OUTPUT_CHARGE) {
				result.reservation->drop_output_charge();
				++job->mailbox->ledger->faults_triggered;
			}
#endif
			result.reservation->move_output(OUTPUT_MAILBOX);
			result.reservation->finish_worker();
		}
		job->mailbox->done.push_back(std::move(result));
	}
	memdelete(job);
}

void HlodNodeStreamer::start_jobs() {
	collect_finished_tasks(false);
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	// Starts one worker job. `p_shadow` jobs pack an edited leaf of the STAGED snapshot into its reserved shadow run;
	// ordinary jobs pack from the served snapshot. Returns false when the in-flight read cap defers the job.
	auto start_one = [&](const NodeResidency::Ticket &p_ticket, bool p_shadow) {
		const Ref<GaussianSplatHlodRuntimeData> &source = p_shadow ? staged_data : data;
		uint64_t read_bytes = 0;
		uint64_t reserved = 0;
		bool counted = false;
		const bool use_store = !p_shadow && page_store && source->is_paged();
		// Edited leaves need no file read, but their packed output takes the SAME reservation below.
		if (source->is_paged() && !source->find_edited(p_ticket.node) && p_ticket.node < uint32_t(source->paged_source->get_nodes().size())) {
			read_bytes = source->paged_source->get_nodes()[p_ticket.node].stored_bytes;
			// The offline codec-none reader reads straight into decoded outputs;
			// only the store transport has a simultaneously retained stored buffer.
			const uint64_t read_working = !use_store && source->paged_source->get_codec_id() == 0 ?
					read_bytes : source->paged_source->get_node_read_working_set_bytes(p_ticket.node);
			if (read_working > UINT64_MAX - uint64_t(source->nodes[p_ticket.node].topology.payload_count) * record_stride()) {
				if (p_shadow) { residency.finish_shadow(p_ticket, false); } else { residency.fail_load(p_ticket); }
				return true; // Invalid shape: consumed, with no task or allocation.
			}
			reserved = read_working +
					uint64_t(source->nodes[p_ticket.node].topology.payload_count) * record_stride();
			counted = true;
		}
		std::unique_ptr<Reservation> permit;
		{
			uint64_t reservation_bytes = 0, output_bytes = 0;
			if (!node_reservation_bytes(*source.ptr(), p_ticket.node, use_store, reservation_bytes, output_bytes)) {
				if (p_shadow) { residency.finish_shadow(p_ticket, false); } else { residency.fail_load(p_ticket); }
				return true;
			}
			MutexLock lock(ledger->mutex);
			const bool oversized = reservation_bytes > cfg.max_inflight_read_bytes;
			const uint64_t transport = use_store && source->paged_source->get_codec_id() == 0u ?
					MIN(read_bytes, PayloadReadService::read_slice_bytes) : 0;
			// The larger retained cap must not overbook the unchanged read service.
			// Promise one maximum raw window before starting; return that promise in
			// the worker as soon as read_node_payload_from_store frees its last page.
			const bool transport_full = !oversized && transport != 0 &&
					(transport > cfg.max_inflight_read_bytes || ledger->transport_reserved > cfg.max_inflight_read_bytes - transport);
			if (ledger->oversized != 0 || transport_full ||
					(oversized ? ledger->live != 0 : ledger->charged > retained_byte_cap - reservation_bytes)) {
				++ledger->admission_waits;
				return false; // Pending tickets own no payload storage.
			}
			permit = std::make_unique<Reservation>(ledger, reservation_bytes, oversized, transport, p_shadow);
			if (use_store && source->paged_source->get_codec_id() == 0u) { reserved = reservation_bytes; }
		}
		if (counted) { read_stats->begin_read(reserved); }
		Job *job = memnew(Job);
		job->read_bytes = read_bytes;
		job->reserved_bytes = reserved;
		job->counted = counted;
		job->data = source;
		job->ticket = p_ticket;
		job->params.relative = true;
		job->params.quantized = cfg.quantized;
		job->params.position_bits = cfg.position_bits;
		job->params.scale_bits = cfg.scale_bits;
		job->atlas_base = p_ticket.first_page * cfg.page_splats;
		job->mailbox = mailbox;
		job->stats = read_stats;
		job->store = use_store ? page_store : nullptr;
		job->reservation = std::move(permit);
		const WorkerThreadPool::TaskID task = pool->add_native_task(&HlodNodeStreamer::run_job, job, false, "HlodNodePack");
		if (task == WorkerThreadPool::INVALID_TASK_ID) {
			if (counted) { read_stats->end_read(reserved, read_bytes, false); }
			memdelete(job); // Returns the permit exactly once, without a worker.
			return false;
		}
		tasks.push_back(task);
		return true;
	};
	if (residency.is_edit_staged()) {
		// Alternate admission while both queues have demand. A byte-blocked head
		// can defer without allocating; the other queue may still fit this ledger.
		auto take_one = [&](bool p_shadow) {
			auto &queue = p_shadow ? shadow_pending : pending;
			while (!queue.is_empty()) {
				const NodeResidency::Ticket ticket = queue[0];
				const bool current = p_shadow ? staged_data.is_valid() && residency.is_shadow_pending(ticket) : residency.is_pending(ticket);
				if (!current) { queue.remove_at(0); continue; }
				if (!start_one(ticket, p_shadow)) { return false; }
				queue.remove_at(0);
				return true;
			}
			return false;
		};
		while (pool && tasks.size() < cfg.max_jobs_in_flight) {
			if (take_one(next_shadow_job)) { next_shadow_job = !next_shadow_job; }
			else if (!take_one(!next_shadow_job)) { break; }
		}
		return;
	}
	// Replacement payloads of a staged edit first: they gate the publication boundary.
	uint32_t shadow_consumed = 0;
	while (shadow_consumed < shadow_pending.size() && tasks.size() < cfg.max_jobs_in_flight && pool) {
		const NodeResidency::Ticket ticket = shadow_pending[shadow_consumed];
		if (staged_data.is_null() || !residency.is_shadow_pending(ticket)) {
			++shadow_consumed;
			continue; // Cancelled or superseded before it started.
		}
		if (!start_one(ticket, true)) {
			break;
		}
		++shadow_consumed;
	}
	if (shadow_consumed != 0) {
		for (uint32_t i = shadow_consumed; i < shadow_pending.size(); ++i) {
			shadow_pending[i - shadow_consumed] = shadow_pending[i];
		}
		shadow_pending.resize(shadow_pending.size() - shadow_consumed);
	}
	uint32_t consumed = 0;
	while (consumed < pending.size() && tasks.size() < cfg.max_jobs_in_flight && pool) {
		const NodeResidency::Ticket ticket = pending[consumed];
		if (!residency.is_pending(ticket)) {
			++consumed;
			continue; // Cancelled before it started: nothing to read.
		}
		if (!start_one(ticket, false)) {
			break;
		}
		++consumed;
	}
	if (consumed != 0) {
		for (uint32_t i = consumed; i < pending.size(); ++i) {
			pending[i - consumed] = pending[i];
		}
		pending.resize(pending.size() - consumed);
	}
}

void HlodNodeStreamer::drain_mailbox() {
	MutexLock lock(ledger->mutex);
	for (uint32_t i = 0; i < mailbox->done.size();) {
		PackResult &r = mailbox->done[i];
#ifdef TESTS_ENABLED
		if (ledger->control.hold_mailbox) { ++i; continue; }
#endif
		if (r.ticket.shadow) {
			// A replacement payload of the staged edit. A ticket of a cancelled or superseded edit is dropped (counted).
			if (residency.finish_shadow(r.ticket, r.ok) && r.ok) {
				if (r.reservation) { r.reservation->move_output(OUTPUT_READY); }
#ifdef TESTS_ENABLED
				if (ledger->fault == TEST_FAULT_READY_ON_PACK && residency.finish_shadow_transfer(r.ticket)) {
					++ledger->faults_triggered; // Real premature residency transition, with CPU storage still held.
				}
#endif
				shadow_ready.push_back(std::move(r));
			}
		} else {
#ifdef TESTS_ENABLED
			if (ledger->control.hold_mailbox) { ++i; continue; }
#endif
			if (r.reservation) { r.reservation->move_output(OUTPUT_READY); }
			ready.push_back(std::move(r));
		}
		mailbox->done.remove_at(i);
	}
}

void HlodNodeStreamer::release_staged_edit_data() {
	shadow_pending.clear();
	shadow_ready.clear();
	shadow_transferred.clear();
	staged_stale_nodes.clear();
	staged_publication_bytes = 0;
	staged_data.unref();
	if (mailbox.is_valid()) {
		MutexLock lock(ledger->mutex);
		for (uint32_t i = 0; i < mailbox->done.size();) {
			if (mailbox->done[i].ticket.shadow) {
				residency.finish_shadow(mailbox->done[i].ticket, false); // Count a cancelled completion; never publish it.
				mailbox->done.remove_at(i);
			} else { ++i; }
		}
	}
}

HlodNodeStreamer::EditStageResult HlodNodeStreamer::stage_edit(const Ref<GaussianSplatHlodRuntimeData> &p_edited) {
	if (!active || residency.is_edit_staged()) {
		return EDIT_REJECTED_BUSY;
	}
	if (p_edited.is_valid() && data.is_valid() && p_edited->edit_parent_id != data->get_instance_id()) {
		return EDIT_REJECTED_LINEAGE; // Same revision number is not the same lineage.
	}
	// A direct derivative of the served snapshot: same world and content, same tree, the next edit revision.
	if (p_edited.is_null() || data.is_null() || p_edited->source_world != data->source_world || p_edited->source_epoch != data->source_epoch ||
			p_edited->source_revision != data->source_revision || p_edited->nodes.size() != data->nodes.size() ||
			p_edited->edit_revision != data->edit_revision + 1 || p_edited->is_paged() != data->is_paged() ||
			p_edited->high_order_count != data->high_order_count || (cfg.quantized && !p_edited->quantized_eligible)) {
		return EDIT_REJECTED_DATA;
	}
	LocalVector<uint32_t> edited_nodes;
	if (!gs_hlod_runtime::collect_edited_since(*data.ptr(), *p_edited.ptr(), edited_nodes)) {
		return EDIT_REJECTED_DATA;
	}
	if (edited_nodes.is_empty() || edited_nodes.size() > gs_hlod_runtime::kMaxEditedLeaves) { return EDIT_REJECTED_DATA; }
	LocalVector<NodeResidency::EditedLeaf> leaves;
	for (uint32_t i = 0; i < edited_nodes.size(); ++i) {
		NodeResidency::EditedLeaf leaf;
		leaf.node = edited_nodes[i];
		leaf.splat_count = p_edited->nodes[leaf.node].topology.payload_count;
		leaves.push_back(leaf);
	}
	if (views.is_empty()) {
		telemetry.edit_event = "rejected_no_view";
		return EDIT_REJECTED_NO_VIEW;
	}
	LocalVector<uint32_t> stale_nodes;
	for (uint32_t node = 0; node < uint32_t(p_edited->nodes.size()); ++node) {
		if (p_edited->is_stale(node) && !data->is_stale(node)) { stale_nodes.push_back(node); }
	}
	const uint64_t publication_bytes = uint64_t(edited_nodes.size() + stale_nodes.size()) *
			(sizeof(ChunkMetaGPU) + (cfg.quantized ? sizeof(ChunkQuantizationGPU) : 0));
	if (publication_bytes > cfg.upload_bytes_per_frame || !edit_feasible(p_edited)) {
		++telemetry.edits_rejected_infeasible_total;
		telemetry.edit_event = "rejected_infeasible";
		return EDIT_REJECTED_INFEASIBLE;
	}
	LocalVector<NodeResidency::Ticket> tickets;
	const NodeResidency::EditResult result = residency.stage_edit(p_edited->edit_revision, leaves.ptr(), leaves.size(), tickets);
	if (result != NodeResidency::EDIT_OK) {
		return result == NodeResidency::EDIT_NO_SPACE ? EDIT_REJECTED_NO_SPACE : (result == NodeResidency::EDIT_BUSY ? EDIT_REJECTED_BUSY : EDIT_REJECTED_DATA);
	}
	staged_data = p_edited;
	shadow_pending.clear();
	shadow_ready.clear();
	shadow_transferred.clear();
	staged_stale_nodes = std::move(stale_nodes);
	staged_publication_bytes = publication_bytes;
	shadow_transferred_bytes = shadow_transfer_count = 0;
	frame_edit_uploads = true;
	for (uint32_t i = 0; i < tickets.size(); ++i) {
		shadow_pending.push_back(tickets[i]);
	}
	telemetry.edit_shadow_total = leaves.size();
	telemetry.edit_event = "staged";
	return EDIT_STAGED;
}

bool HlodNodeStreamer::cancel_staged_edit() {
	if (!active || !residency.cancel_edit()) {
		return false;
	}
	release_staged_edit_data();
	telemetry.edit_shadow_total = 0;
	telemetry.edit_event = "cancelled";
	refresh_telemetry();
	return true;
}

HlodNodeStreamer::EditCommitResult HlodNodeStreamer::fail_commit() {
	in_edit_commit = false;
	++telemetry.edit_commit_failures_total;
	telemetry.edit_event = "commit_unusable";
	active = false;
	// Metadata may already name new runs: the owner must discard the bundle.
	// Join outside the ledger lock, releasing every queued/worker permit, while
	// keeping the last served snapshot for the owner's fail-closed rebuild.
	drain_jobs_and_output();
	refresh_telemetry();
	return COMMIT_UNUSABLE;
}

void HlodNodeStreamer::fail_shadow_upload() {
	cancel_staged_edit();
	++telemetry.edit_commit_failures_total;
	telemetry.edit_event = "upload_failed";
	pending_commit_result = COMMIT_UPLOAD_FAILED;
}

HlodNodeStreamer::EditCommitResult HlodNodeStreamer::commit_staged_edit(RenderingDevice *p_device, const Buffers &p_buffers) {
	if (pending_commit_result != COMMIT_NOTHING) {
		const EditCommitResult failure = pending_commit_result;
		pending_commit_result = COMMIT_NOTHING;
		return failure;
	}
	if (!active || !residency.is_edit_staged()) { return COMMIT_NOTHING; }
	if (residency.edit_blocked()) {
		cancel_staged_edit();
		telemetry.edit_event = "blocked";
		return COMMIT_NOTHING;
	}
	if (!data->is_current()) {
		shutdown(); // An obsolete source cannot publish even before this frame's update().
		return COMMIT_NOTHING;
	}
	if (!residency.edit_ready() || staged_data.is_null() || !upload_available(p_device) ||
			!pending_metadata_clears.is_empty() || staged_publication_bytes > upload_remaining()) { return COMMIT_NOTHING; }
	if (!shadow_ready.is_empty() || shadow_transferred.size() != telemetry.edit_shadow_total) { return fail_commit(); }
	// Validate every descriptor against the still-reserved run and edit epoch.
	for (const ShadowTransfer &r : shadow_transferred) {
		NodeResidency::Ticket current;
		if (!residency.shadow_ticket(r.ticket.node, current) || current.request_generation != r.ticket.request_generation ||
				r.ticket.world_generation != world_generation || r.ticket.device_generation != device_generation ||
				current.world_generation != r.ticket.world_generation || current.device_generation != r.ticket.device_generation ||
				current.first_page != r.ticket.first_page || current.page_count != r.ticket.page_count ||
				r.meta.splat_count == 0 || r.meta.atlas_base != current.first_page * cfg.page_splats ||
				r.bytes != uint64_t(r.meta.splat_count) * record_stride()) { return fail_commit(); }
	}
	in_edit_commit = true;
	// Payloads were transferred in update(). Only this bounded metadata batch
	// publishes their runs, in one begin_frame call before any selector observes it.
	for (uint32_t i = 0; i < shadow_transferred.size(); ++i) {
		const ShadowTransfer &r = shadow_transferred[i];
#ifdef TESTS_ENABLED
		if (test_fail_commit_metadata) {
			test_fail_commit_metadata = false;
			return fail_commit();
		}
#endif
		if ((cfg.quantized && write_quant(p_device, p_buffers, r.ticket.node, r.quant, true) != OK) ||
				write_meta(p_device, p_buffers, r.ticket.node, r.meta, true) != OK) { return fail_commit(); }
#ifdef TESTS_ENABLED
		if (ledger->fault == TEST_FAULT_PARTIAL_COMMIT && i == 0 && shadow_transferred.size() > 1) {
			++ledger->faults_triggered; // Publish a real subset, then fail closed.
			return fail_commit();
		}
#endif
	}
	const ChunkMetaGPU empty_meta = {};
	const ChunkQuantizationGPU empty_quant = {};
	for (uint32_t node : staged_stale_nodes) {
		if (write_meta(p_device, p_buffers, node, empty_meta) != OK ||
				(cfg.quantized && write_quant(p_device, p_buffers, node, empty_quant) != OK)) { return fail_commit(); }
	}
	if (!residency.commit_edit()) { return fail_commit(); }
	// The new stale records were zeroed above. Retain any unrelated eviction
	// clears for update(); never clear a just-published leaf a second time.
	residency.take_evicted(evicted_scratch);
	for (uint32_t node : evicted_scratch) {
		bool cleared = false;
		for (uint32_t stale : staged_stale_nodes) { if (stale == node) { cleared = true; break; } }
		if (!cleared) { pending_metadata_clears.push_back(node); }
	}
	data = staged_data;
	telemetry.edit_payload_bytes_committed_total += shadow_transferred_bytes;
	release_staged_edit_data();
	in_edit_commit = false;
	telemetry.edit_revision = data->edit_revision;
	telemetry.edit_shadow_total = 0;
	++telemetry.edits_committed_total;
	telemetry.edit_event = "committed";
	refresh_telemetry();
	return COMMIT_DONE;
}

void HlodNodeStreamer::begin_frame(RenderingDevice *p_device, const Buffers *p_buffers) {
	last_commit_result = pending_commit_result;
	pending_commit_result = COMMIT_NOTHING;
	if (!active) { return; }
	residency.begin_frame(++frame);
	frame_written_bytes = 0;
	frame_edit_uploads = residency.is_edit_staged() || !pending_metadata_clears.is_empty();
	if (last_commit_result == COMMIT_NOTHING && p_buffers && residency.is_edit_staged()) {
		last_commit_result = commit_staged_edit(p_device, *p_buffers);
	}
}

void HlodNodeStreamer::register_view(uint64_t p_view_key, const gs_hlod_runtime::CutRequest &p_request) {
	RegisteredView view;
	view.key = p_view_key;
	view.request = p_request;
	view.request.resident = nullptr;
	view.request.resident_count = 0;
	view.request.tie_rank = nullptr;
	view.request.tie_rank_count = 0;
	for (uint32_t i = 0; i < views.size(); ++i) {
		if (views[i].key == p_view_key) {
			views[i] = view;
			return;
		}
	}
	if (views.size() < 8) {
		views.push_back(view);
	}
}

bool HlodNodeStreamer::unregister_view(uint64_t p_view_key) {
	for (uint32_t i = 0; i < views.size(); ++i) {
		if (views[i].key == p_view_key) {
			views.remove_at(i);
			return true;
		}
	}
	return false;
}

// ADR 14.5: the replacement cut must be reservable before the edit is accepted. Runs the selector on the staged snapshot
// for every registered view, treating every non-stale node as resident (residency is what staging then provides), so
// the answer is about the caps: a node, splat or record budget that cannot hold the cut yields no cut for that view.
bool HlodNodeStreamer::edit_feasible(const Ref<GaussianSplatHlodRuntimeData> &p_edited) const {
	const uint32_t node_total = p_edited->nodes.size();
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(node_total)) {
		return false;
	}
	Vector<uint8_t> all_resident;
	if (!gs_resize_or_fail(all_resident, int64_t(node_total), "HLOD edit feasibility mask")) {
		return false;
	}
	all_resident.fill(1);
	for (uint32_t i = 0; i < views.size(); ++i) {
		gs_hlod_runtime::CutRequest request = views[i].request;
		request.resident = all_resident.ptr();
		request.resident_count = node_total;
		const gs_hlod_runtime::CutHistory::Result result = history.select(*p_edited.ptr(), request);
		if (result.status != gs_hlod_runtime::CutHistory::COMPLETE && result.status != gs_hlod_runtime::CutHistory::EMPTY) {
			return false;
		}
		history.invalidate();
	}
	return true;
}

bool HlodNodeStreamer::upload_available(RenderingDevice *p_device) const {
#ifdef TESTS_ENABLED
	if (ledger.is_valid()) {
		MutexLock lock(ledger->mutex);
		if (ledger->control.cpu_upload_sink) { return true; }
	}
#endif
	return p_device && p_device->get_device_instance_id() == device_generation;
}

uint64_t HlodNodeStreamer::upload_remaining() const {
	return frame_written_bytes < cfg.upload_bytes_per_frame ? cfg.upload_bytes_per_frame - frame_written_bytes : 0;
}

#ifdef TESTS_ENABLED
void HlodNodeStreamer::record_write(TestWrite::Kind p_kind, uint32_t p_node, uint64_t p_offset, uint64_t p_bytes,
		bool p_shadow, const ChunkMetaGPU *p_meta, const ChunkQuantizationGPU *p_quant) {
	MutexLock lock(ledger->mutex);
	if (!ledger->observe_shadows && !ledger->control.cpu_upload_sink) { return; }
	TestWrite write;
	write.kind = p_kind;
	write.node = p_node;
	write.offset = p_offset;
	write.bytes = p_bytes;
	write.frame = frame;
	write.shadow = p_shadow;
	write.during_commit = in_edit_commit;
	if (p_meta) { write.meta = *p_meta; }
	if (p_quant) { write.quant = *p_quant; }
	ledger->writes.push_back(write);
}
#endif

Error HlodNodeStreamer::write_atlas(RenderingDevice *p_device, const Buffers &p_buffers, uint32_t p_node,
		uint64_t p_offset, uint64_t p_bytes, const void *p_payload, bool p_shadow) {
	if (!p_payload || p_bytes == 0 || p_offset > UINT32_MAX || p_bytes > UINT32_MAX ||
			p_offset > atlas_bytes_capacity() || p_bytes > atlas_bytes_capacity() - p_offset) { return ERR_INVALID_DATA; }
	bool cpu_sink = false;
#ifdef TESTS_ENABLED
	{
		MutexLock lock(ledger->mutex);
		cpu_sink = ledger->control.cpu_upload_sink;
		if (ledger->control.fail_upload) { return ERR_CANT_CREATE; }
	}
#endif
	if (!cpu_sink) {
		if (!upload_available(p_device)) { return ERR_UNAVAILABLE; }
		const Error err = p_device->buffer_update(p_buffers.atlas, uint32_t(p_offset), uint32_t(p_bytes), p_payload);
		if (err != OK) { return err; }
	}
	frame_written_bytes += p_bytes;
#ifdef TESTS_ENABLED
	record_write(TestWrite::ATLAS, p_node, p_offset, p_bytes, p_shadow);
#endif
	return OK;
}

Error HlodNodeStreamer::write_meta(RenderingDevice *p_device, const Buffers &p_buffers, uint32_t p_node, const ChunkMetaGPU &p_meta, bool p_shadow) {
	if (p_node >= residency.node_count()) { return ERR_INVALID_DATA; }
	bool cpu_sink = false;
#ifdef TESTS_ENABLED
	{
		MutexLock lock(ledger->mutex);
		cpu_sink = ledger->control.cpu_upload_sink;
		if (cpu_sink) {
			if (p_node >= uint32_t(test_metadata.size())) { return ERR_INVALID_DATA; }
			test_metadata.write[p_node] = p_meta;
		}
	}
#endif
	if (!cpu_sink) {
		if (!upload_available(p_device)) { return ERR_UNAVAILABLE; }
		const Error err = p_device->buffer_update(p_buffers.chunk_meta, p_node * sizeof(ChunkMetaGPU), sizeof(ChunkMetaGPU), &p_meta);
		if (err != OK) { return err; }
	}
	frame_written_bytes += sizeof(ChunkMetaGPU);
#ifdef TESTS_ENABLED
	record_write(TestWrite::METADATA, p_node, uint64_t(p_node) * sizeof(ChunkMetaGPU), sizeof(ChunkMetaGPU), p_shadow, &p_meta);
#endif
	return OK;
}

Error HlodNodeStreamer::write_quant(RenderingDevice *p_device, const Buffers &p_buffers, uint32_t p_node, const ChunkQuantizationGPU &p_quant, bool p_shadow) {
	if (p_node >= residency.node_count()) { return ERR_INVALID_DATA; }
	bool cpu_sink = false;
#ifdef TESTS_ENABLED
	{
		MutexLock lock(ledger->mutex);
		cpu_sink = ledger->control.cpu_upload_sink;
		if (cpu_sink) {
			if (p_node >= uint32_t(test_quantization.size())) { return ERR_INVALID_DATA; }
			test_quantization.write[p_node] = p_quant;
		}
	}
#endif
	if (!cpu_sink) {
		if (!upload_available(p_device)) { return ERR_UNAVAILABLE; }
		const Error err = p_device->buffer_update(p_buffers.quantization, p_node * sizeof(ChunkQuantizationGPU), sizeof(ChunkQuantizationGPU), &p_quant);
		if (err != OK) { return err; }
	}
	frame_written_bytes += sizeof(ChunkQuantizationGPU);
#ifdef TESTS_ENABLED
	record_write(TestWrite::QUANTIZATION, p_node, uint64_t(p_node) * sizeof(ChunkQuantizationGPU), sizeof(ChunkQuantizationGPU), p_shadow, nullptr, &p_quant);
#endif
	return OK;
}

bool HlodNodeStreamer::upload_shadow_ready(RenderingDevice *p_device, const Buffers &p_buffers) {
	MutexLock lock(ledger->mutex);
	if (shadow_ready.is_empty()) { return false; }
	PackResult &r = shadow_ready[0];
	const bool source_current = staged_data.is_valid() && data->is_current() &&
			(!staged_data->is_paged() || (r.source_instance_id == uint64_t(staged_data->paged_source->get_instance_id()) &&
					r.content_generation == staged_data->paged_source->get_info().content_generation));
	if (!source_current || r.ticket.world_generation != world_generation || r.ticket.device_generation != device_generation || !residency.is_shadow_packed(r.ticket)) {
		residency.finish_shadow_transfer(r.ticket, false);
		shadow_ready.remove_at(0);
		return true; // Stale/cancelled outputs never reach a device write.
	}
#ifdef TESTS_ENABLED
	if (ledger->control.hold_uploads) { return false; }
#endif
	if (!upload_available(p_device)) { return false; }
	const uint64_t stride = record_stride();
	const uint32_t count = cfg.quantized ? uint32_t(r.quantized.size()) : uint32_t(r.raw.size());
	const uint64_t bytes = uint64_t(count) * stride;
	const uint64_t offset = uint64_t(r.ticket.first_page) * cfg.page_splats * stride;
	if (!r.ok || count == 0 || count != r.meta.splat_count || count > uint64_t(r.ticket.page_count) * cfg.page_splats ||
			offset > atlas_bytes_capacity() || bytes > atlas_bytes_capacity() - offset || r.uploaded_bytes > bytes) {
		fail_shadow_upload(); return true;
	}
	const uint64_t slice = MIN(bytes - r.uploaded_bytes, upload_remaining()) & ~uint64_t(3);
	if (slice == 0) { return false; }
	if (r.reservation) { r.reservation->move_output(OUTPUT_UPLOAD); }
	const uint8_t *payload = cfg.quantized ? reinterpret_cast<const uint8_t *>(r.quantized.ptr()) : reinterpret_cast<const uint8_t *>(r.raw.ptr());
	if (write_atlas(p_device, p_buffers, r.ticket.node, offset + r.uploaded_bytes, slice, payload + r.uploaded_bytes, true) != OK) {
		fail_shadow_upload(); return true;
	}
	r.uploaded_bytes += slice;
	telemetry.uploaded_bytes_this_frame += slice;
	telemetry.uploaded_bytes_total += slice;
	if (r.uploaded_bytes == bytes) {
		if (!residency.finish_shadow_transfer(r.ticket)) { fail_shadow_upload(); return true; }
#ifdef TESTS_ENABLED
		if (ledger->fault == TEST_FAULT_EARLY_SHADOW_METADATA) {
			// Actually expose this invisible run before commit; the same revision
			// checker as the positive case must reject the device/CPU-sink write.
			if (write_meta(p_device, p_buffers, r.ticket.node, r.meta, true) == OK) { ++ledger->faults_triggered; }
		}
#endif
		ShadowTransfer transfer;
		transfer.ticket = r.ticket;
		transfer.meta = r.meta;
		transfer.quant = r.quant;
		transfer.bytes = bytes;
		shadow_transferred.push_back(transfer);
		shadow_transferred_bytes += bytes;
		++shadow_transfer_count;
		++telemetry.uploaded_this_frame;
		// Release actual allocations BEFORE returning the output charge. A
		// transferred transaction keeps only the descriptor above.
		r.raw.clear();
		r.quantized.clear();
		if (r.reservation) { r.reservation->output_changed(0); }
#ifdef TESTS_ENABLED
		if (ledger->fault == TEST_FAULT_LEAK_SHADOW_PERMIT && r.reservation) {
			test_leaked_shadow_permits.push_back(std::move(r.reservation));
			++ledger->faults_triggered; // Retain the real charged permit after freeing the bytes.
		}
#endif
		shadow_ready.remove_at(0);
	}
	return true;
}

bool HlodNodeStreamer::upload_edit_ordinary(RenderingDevice *p_device, const Buffers &p_buffers) {
	MutexLock lock(ledger->mutex);
	if (ready.is_empty()) { return false; }
	PackResult &r = ready[0];
	const bool source_current = !page_store || (data->is_current() && data->is_paged() &&
			r.source_instance_id == uint64_t(data->paged_source->get_instance_id()) && r.content_generation == data->paged_source->get_info().content_generation);
	if (!r.ok || !source_current || r.ticket.world_generation != world_generation || r.ticket.device_generation != device_generation || !residency.is_pending(r.ticket)) {
		if (!r.ok) { residency.fail_load(r.ticket); }
		else if (residency.is_pending(r.ticket)) { residency.cancel(r.ticket.node); }
		else { residency.begin_upload(r.ticket); }
		ready.remove_at(0);
		return true;
	}
#ifdef TESTS_ENABLED
	if (ledger->control.hold_uploads) { return false; }
#endif
	if (!upload_available(p_device)) { return false; }
	const uint64_t stride = record_stride();
	const uint32_t count = cfg.quantized ? uint32_t(r.quantized.size()) : uint32_t(r.raw.size());
	const uint64_t bytes = uint64_t(count) * stride;
	const uint64_t offset = uint64_t(r.ticket.first_page) * cfg.page_splats * stride;
	const uint64_t metadata_bytes = sizeof(ChunkMetaGPU) + (cfg.quantized ? sizeof(ChunkQuantizationGPU) : 0);
	bool ok = count != 0 && offset <= atlas_bytes_capacity() && bytes <= atlas_bytes_capacity() - offset &&
			count <= uint64_t(r.ticket.page_count) * cfg.page_splats && r.uploaded_bytes <= bytes;
	bool progressed = false;
#ifdef TESTS_ENABLED
	if (r.test_first_write == UINT32_MAX) { r.test_first_write = ledger->writes.size(); }
#endif
	if (ok && r.uploaded_bytes < bytes) {
		const uint64_t slice = MIN(bytes - r.uploaded_bytes, upload_remaining()) & ~uint64_t(3);
		if (slice == 0) { return false; }
		if (r.reservation) { r.reservation->move_output(OUTPUT_UPLOAD); }
		const uint8_t *payload = cfg.quantized ? reinterpret_cast<const uint8_t *>(r.quantized.ptr()) : reinterpret_cast<const uint8_t *>(r.raw.ptr());
		ok = write_atlas(p_device, p_buffers, r.ticket.node, offset + r.uploaded_bytes, slice, payload + r.uploaded_bytes, false) == OK;
		if (ok) {
			r.uploaded_bytes += slice;
			telemetry.uploaded_bytes_this_frame += slice;
			telemetry.uploaded_bytes_total += slice;
			progressed = true;
		}
	}
	if (ok && (r.uploaded_bytes != bytes || metadata_bytes > upload_remaining())) { return progressed; }
	if (!residency.begin_upload(r.ticket)) { ready.remove_at(0); return true; }
	if (ok) {
		ok = (!cfg.quantized || write_quant(p_device, p_buffers, r.ticket.node, r.quant) == OK) &&
				write_meta(p_device, p_buffers, r.ticket.node, r.meta) == OK;
	}
	const bool finished = residency.finish_upload(r.ticket, ok);
	if (ok && finished) {
		++telemetry.uploaded_this_frame;
#ifdef TESTS_ENABLED
		TestPublication publication;
		publication.node = r.ticket.node;
		publication.source_instance_id = r.source_instance_id;
		publication.content_generation = r.content_generation;
		publication.world_generation = r.ticket.world_generation;
		publication.device_generation = r.ticket.device_generation;
		publication.request_generation = r.ticket.request_generation;
		publication.metadata_splat_count = r.meta.splat_count;
		for (uint32_t w = r.test_first_write; w < uint32_t(ledger->writes.size()); ++w) {
			const TestWrite &write = ledger->writes[w];
			if (write.node == r.ticket.node && !write.shadow) { publication.writes.push_back(write); }
		}
		ledger->publications.push_back(publication);
#endif
	}
	ready.remove_at(0);
	return true;
}

void HlodNodeStreamer::upload_edit_ready(RenderingDevice *p_device, const Buffers &p_buffers) {
	if (!pending_metadata_clears.is_empty()) { return; }
	// A slice can consume the remaining allowance; the other queue then gets
	// first service next frame. Empty or blocked queues cannot starve the other.
	while (upload_remaining() != 0) {
		const bool shadow_first = next_shadow_upload;
		const bool first_progress = shadow_first ? upload_shadow_ready(p_device, p_buffers) : upload_edit_ordinary(p_device, p_buffers);
		if (first_progress) { next_shadow_upload = !shadow_first; }
		else {
			const bool second_progress = shadow_first ? upload_edit_ordinary(p_device, p_buffers) : upload_shadow_ready(p_device, p_buffers);
			if (!second_progress) { break; }
			next_shadow_upload = shadow_first;
		}
	}
}

void HlodNodeStreamer::upload_ready(RenderingDevice *p_device, const Buffers &p_buffers) {
	MutexLock lock(ledger->mutex);
	const uint64_t stride = record_stride();
	const uint64_t atlas_capacity = atlas_bytes_capacity();
	uint64_t spent = 0;
	uint32_t i = 0;
	while (i < ready.size()) {
		PackResult &r = ready[i];
		if (!r.ok) {
			residency.fail_load(r.ticket); // A stale ticket is only counted as dropped.
			ready.remove_at(i);
			continue;
		}
		NodeResidency::Ticket publication_ticket = r.ticket;
		const bool source_current = !page_store || (data->is_current() && data->is_paged() &&
				r.source_instance_id == uint64_t(data->paged_source->get_instance_id()) &&
				r.content_generation == data->paged_source->get_info().content_generation);
		bool current = source_current && r.ticket.world_generation == world_generation &&
				r.ticket.device_generation == device_generation && residency.is_pending(r.ticket);
#ifdef TESTS_ENABLED
		if (!current && ledger->fault == TEST_FAULT_SKIP_PUBLICATION_GENERATION && r.ticket.node < residency.node_count()) {
			// Accepting stale output also has to bypass the model's independent
			// ticket gates. Reticket the REAL old payload onto a current run, without
			// altering its recorded source/world/device provenance or observations.
			LocalVector<NodeResidency::Ticket> current_tickets;
			const uint32_t node = r.ticket.node;
			residency.request(&node, 1, current_tickets);
			for (const NodeResidency::Ticket &ticket : current_tickets) {
				if (ticket.node == node) {
					publication_ticket = ticket;
					current = true;
					r.meta.atlas_base = ticket.first_page * cfg.page_splats;
					r.quant.start_index = r.meta.atlas_base;
					++ledger->faults_triggered;
					break;
				}
			}
		}
#endif
		if (!current) {
			if (residency.is_pending(r.ticket)) {
				// Source freshness can fail even while the model's own ticket is
				// current. Cancel it; begin_upload would incorrectly accept it.
				residency.cancel(r.ticket.node);
			} else {
				residency.begin_upload(r.ticket); // Rejected and counted: cancelled or another epoch.
			}
			ready.remove_at(i);
			continue;
		}
#ifdef TESTS_ENABLED
		if (ledger->control.hold_uploads) { ++i; continue; }
#endif
		const uint32_t count = r.quantized.is_empty() ? uint32_t(r.raw.size()) : uint32_t(r.quantized.size());
		const uint64_t bytes = uint64_t(count) * stride;
		if (spent != 0 && spent + bytes > cfg.upload_bytes_per_frame) {
			break; // Keep the rest for the next frame; they stay LOADING (protected, not drawable).
		}
		if (!residency.begin_upload(publication_ticket)) {
			ready.remove_at(i);
			continue;
		}
		const uint64_t offset = uint64_t(publication_ticket.first_page) * cfg.page_splats * stride;
		const bool fits = count != 0 && offset + bytes <= atlas_capacity && count <= publication_ticket.page_count * cfg.page_splats;
		if (r.reservation) { r.reservation->move_output(OUTPUT_UPLOAD); }
		bool ok = fits && upload_available(p_device);
#ifdef TESTS_ENABLED
		const bool cpu_sink = ledger->control.cpu_upload_sink;
		const uint32_t first_write = ledger->writes.size();
#endif
		if (ok) {
			const void *payload = cfg.quantized ? static_cast<const void *>(r.quantized.ptr()) : static_cast<const void *>(r.raw.ptr());
			// Keep the legacy no-edit command order and whole-node admission rule.
			ok = write_atlas(p_device, p_buffers, publication_ticket.node, offset, bytes, payload, false) == OK &&
					(!cfg.quantized || write_quant(p_device, p_buffers, publication_ticket.node, r.quant) == OK) &&
					write_meta(p_device, p_buffers, publication_ticket.node, r.meta) == OK;
		}
		const bool finished = residency.finish_upload(publication_ticket, ok);
		ok = ok && finished;
#ifdef TESTS_ENABLED
		if (ok) {
			TestPublication publication;
			publication.node = r.ticket.node;
			publication.source_instance_id = r.source_instance_id;
			publication.content_generation = r.content_generation;
			publication.world_generation = r.ticket.world_generation;
			publication.device_generation = r.ticket.device_generation;
			publication.request_generation = r.ticket.request_generation;
			publication.metadata_splat_count = cpu_sink ? test_metadata[publication_ticket.node].splat_count : r.meta.splat_count;
			for (uint32_t w = first_write; w < uint32_t(ledger->writes.size()); ++w) { publication.writes.push_back(ledger->writes[w]); }
			ledger->publications.push_back(publication);
		}
#endif
		if (ok) {
			spent += bytes;
			telemetry.uploaded_bytes_this_frame += bytes;
			telemetry.uploaded_bytes_total += bytes;
			++telemetry.uploaded_this_frame;
		}
		ready.remove_at(i);
	}
}

void HlodNodeStreamer::clear_evicted(RenderingDevice *p_device, const Buffers &p_buffers) {
	residency.take_evicted(evicted_scratch);
	const ChunkMetaGPU empty_meta = {};
	const ChunkQuantizationGPU empty_quant = {};
	if (!frame_edit_uploads && pending_metadata_clears.is_empty()) {
		// No edit: retain the original immediate clear and command order.
		for (uint32_t node : evicted_scratch) {
			write_meta(p_device, p_buffers, node, empty_meta);
			if (cfg.quantized) { write_quant(p_device, p_buffers, node, empty_quant); }
		}
		if (upload_available(p_device)) { telemetry.evicted_this_frame += evicted_scratch.size(); }
		return;
	}
	for (uint32_t node : evicted_scratch) { pending_metadata_clears.push_back(node); }
	const uint64_t bytes = sizeof(ChunkMetaGPU) + (cfg.quantized ? sizeof(ChunkQuantizationGPU) : 0);
	while (!pending_metadata_clears.is_empty() && bytes <= upload_remaining() && upload_available(p_device)) {
		const uint32_t node = pending_metadata_clears[0];
		if (write_meta(p_device, p_buffers, node, empty_meta) != OK ||
				(cfg.quantized && write_quant(p_device, p_buffers, node, empty_quant) != OK)) {
			fail_commit(); return;
		}
		pending_metadata_clears.remove_at(0);
		++telemetry.evicted_this_frame;
	}
	// Pending clears block all run uploads and new evictions. Even if the CPU
	// allocator retires a run first, its reused payload cannot land before the
	// old metadata has been zeroed in the same device command stream.
}

void HlodNodeStreamer::update(RenderingDevice *p_device, const Buffers &p_buffers, const gs_hlod_runtime::DesiredRecord *p_desired,
		uint32_t p_desired_count, const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count) {
	if (!active) {
		return;
	}
	if (p_device && p_device->get_device_instance_id() != device_generation) {
		shutdown(); // Never submit old payloads to a different device's buffers.
		return;
	}
	if ((page_store || residency.is_edit_staged()) && !data->is_current()) {
		shutdown(); // The owner must install a fresh snapshot/bundle before work resumes.
		return;
	}
	telemetry.requested_this_frame = telemetry.uploaded_this_frame = telemetry.skipped_groups_this_frame = telemetry.evicted_this_frame = 0;
	telemetry.uploaded_bytes_this_frame = 0;
	for (const PackResult &r : ready) {
		if (r.uploaded_bytes != 0) { frame_edit_uploads = true; break; }
	}
	split_protection_and_demand(p_desired, p_desired_count, p_drawn, p_drawn_count, drawn_scratch, demand_scratch);
	if (residency.is_edit_staged()) {
		// The replacement cut of a staged edit is ordinary demand (whole transitions, same admission), ahead of the view's.
		prefetch_scratch.clear();
		residency.edit_prefetch(prefetch_scratch);
		for (uint32_t i = 0; i < demand_scratch.size(); ++i) {
			prefetch_scratch.push_back(demand_scratch[i]);
		}
		demand_scratch = prefetch_scratch;
		if (residency.edit_blocked()) {
			cancel_staged_edit();
			telemetry.edit_event = "blocked";
		}
	}
	// Protection first (ADR 14.1): drawn versions and their ancestors cannot be evicted by this frame's requests.
	residency.protect_drawn(drawn_scratch.ptr(), drawn_scratch.size());
	tickets_scratch.clear();
	const bool draining_metadata = !pending_metadata_clears.is_empty();
	if (residency.is_edit_staged()) {
		// After the frame's protection is known: allocate (evicting if needed) the shadow runs of a waiting edit.
		LocalVector<NodeResidency::Ticket> shadow_tickets;
		if (!draining_metadata) { residency.advance_edit(shadow_tickets); }
		for (uint32_t i = 0; i < shadow_tickets.size(); ++i) {
			shadow_pending.push_back(shadow_tickets[i]);
		}
		if (residency.staged_edit_age_frames() > cfg.edit_timeout_frames) {
			cancel_staged_edit();
			++telemetry.edit_timeouts_total;
			telemetry.edit_event = "timeout";
		}
	}
	const NodeResidency::RequestResult requested = draining_metadata ? NodeResidency::RequestResult() :
			residency.request(demand_scratch.ptr(), demand_scratch.size(), tickets_scratch);
	// Metadata of nodes evicted by this request is cleared before any upload of this update: a node evicted and
	// requested again is not in the log (NodeResidency), and a payload that lands below is never zeroed after.
	clear_evicted(p_device, p_buffers);
	if (!active) { return; }
	for (uint32_t i = 0; i < tickets_scratch.size(); ++i) {
		pending.push_back(tickets_scratch[i]);
	}
	telemetry.requested_this_frame = requested.requested;
	telemetry.skipped_groups_this_frame = requested.skipped_groups;
	telemetry.blocked_groups_this_frame = requested.blocked_groups;
	telemetry.fragmented_groups_this_frame = requested.fragmented_groups;
	bool partial_ordinary = false;
	for (const PackResult &r : ready) { if (r.uploaded_bytes != 0) { partial_ordinary = true; break; } }
	if (residency.is_edit_staged() || frame_edit_uploads || partial_ordinary) {
		// Drain before admission: transferred output returns capacity for the
		// next pending job in this update, so a transaction can exceed the window.
		drain_mailbox();
		upload_edit_ready(p_device, p_buffers);
		start_jobs();
	} else {
		drain_mailbox();
		upload_ready(p_device, p_buffers);
		// Completed output keeps its permit through upload/discard. Reuse the
		// capacity returned above for pending jobs in this update, not next frame.
		start_jobs();
	}
	const NodeResidency::Stats stats = residency.stats();
	const bool nothing_moving = stats.loading + stats.uploading == 0 && pending.is_empty() && ready.is_empty();
	telemetry.stall_frames = (p_desired_count != 0 && nothing_moving && requested.requested == 0) ? telemetry.stall_frames + 1 : 0;
	telemetry.desired = demand_scratch.size();
	telemetry.drawn = p_drawn_count;
	refresh_telemetry();
}

void HlodNodeStreamer::split_protection_and_demand(const gs_hlod_runtime::DesiredRecord *p_desired, uint32_t p_desired_count,
		const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count, LocalVector<uint32_t> &r_protected, LocalVector<uint32_t> &r_demand) {
	r_protected.clear();
	r_demand.clear();
	for (uint32_t i = 0; i < p_drawn_count; ++i) {
		r_protected.push_back(p_drawn[i].node);
	}
	for (uint32_t i = 0; i < p_desired_count; ++i) {
		(p_desired[i].keep ? r_protected : r_demand).push_back(p_desired[i].node);
	}
}

void HlodNodeStreamer::refresh_telemetry() {
	const NodeResidency::Stats s = residency.stats();
	telemetry.resident = s.resident;
	telemetry.edit_staged = s.edit_staged;
	telemetry.edit_waiting_for_space = s.edit_waiting_for_space;
	telemetry.edit_shadow_ready = shadow_ready.size();
	telemetry.edit_shadow_transferred = shadow_transferred.size();
	telemetry.edits_cancelled_total = s.edits_cancelled_total;
	telemetry.edits_rejected_total = s.edits_rejected_total;
	telemetry.stale_nodes = s.stale_nodes;
	telemetry.loading = s.loading + s.uploading;
	telemetry.failed = s.failed;
	telemetry.permanently_failed = s.permanently_failed;
	telemetry.pages_resident = s.pages_resident;
	telemetry.pages_in_flight = s.pages_in_flight;
	telemetry.pages_retiring = s.pages_retiring;
	telemetry.late_dropped_total = s.late_dropped_total;
	telemetry.late_dropped_cancelled_total = s.late_dropped_cancelled_total;
	telemetry.cancelled_total = s.cancelled_total;
	telemetry.evicted_total = s.evicted_total;
	telemetry.failed_total = s.failed_total;
	telemetry.skipped_groups_total = s.skipped_groups_total;
	uint64_t ready_bytes = 0;
	for (uint32_t i = 0; i < ready.size(); ++i) {
		ready_bytes += uint64_t(ready[i].raw.size()) * sizeof(PackedGaussian) + uint64_t(ready[i].quantized.size()) * sizeof(PackedGaussianQuantized);
	}
	telemetry.payload_ready_bytes_peak = MAX(telemetry.payload_ready_bytes_peak, ready_bytes);
	if (read_stats.is_valid()) {
		telemetry.payload_reads_total = read_stats->reads.load();
		telemetry.payload_bytes_read_total = read_stats->bytes.load();
		telemetry.payload_read_failures_total = read_stats->failures.load();
		telemetry.payload_reads_in_flight = read_stats->in_flight.load();
		telemetry.payload_in_flight_bytes_peak = read_stats->in_flight_bytes_peak.load();
		telemetry.payload_largest_read_bytes = read_stats->largest.load();
	}
}
