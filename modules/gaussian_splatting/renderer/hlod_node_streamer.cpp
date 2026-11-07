#include "hlod_node_streamer.h"

#include "../core/gaussian_streaming.h"
#include "../core/gs_vector_alloc.h"
#include "../lod/hlod_edit.h"

using gs_hlod_runtime::NodeResidency;

// The node run granularity is the chunk streamer's page size (#1088): the shared allocator and its budget math.
static_assert(HlodNodeStreamer::Config::DEFAULT_PAGE_SPLATS == GaussianStreamingSystem::ATLAS_PAGE_SPLATS, "HLOD node runs use the streaming atlas page size");

HlodNodeStreamer::~HlodNodeStreamer() {
	shutdown();
}

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
	data = p_data;
	world_generation = p_world_generation;
	device_generation = p_device_generation;
	mailbox.instantiate();
	read_stats.instantiate();
	frame = 0;
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
	if (!active && tasks.is_empty()) {
		return;
	}
	active = false;
	// Jobs hold only the snapshot and the mailbox, never this object, but waiting here keeps the last
	// reference to the snapshot (and its world) from being dropped on a worker thread.
	collect_finished_tasks(true);
	pending.clear();
	ready.clear();
	shadow_pending.clear();
	shadow_ready.clear();
	staged_data.unref();
	if (mailbox.is_valid()) {
		MutexLock lock(mailbox->mutex);
		mailbox->done.clear();
	}
	mailbox.unref();
	data.unref();
	read_stats.unref();
}

void HlodNodeStreamer::run_job(void *p_userdata) {
	Job *job = static_cast<Job *>(p_userdata);
	PackResult result;
	result.ticket = job->ticket;
	const GaussianSplatHlodRuntimeData &d = *job->data.ptr();
	const uint32_t count = job->ticket.node < uint32_t(d.nodes.size()) ? d.nodes[job->ticket.node].topology.payload_count : 0;
	gs_hlod_pack::Output out;
	SHCompressionMetrics metrics;
	out.meta = &result.meta;
	out.quant = &result.quant;
	out.metrics = &metrics;
	bool sized = count != 0;
	if (sized) {
		if (job->params.quantized) {
			sized = gs_resize_or_fail(result.quantized, count, "HLOD node payload (quantized)");
			out.quantized = result.quantized.ptrw();
		} else {
			sized = gs_resize_or_fail(result.raw, count, "HLOD node payload");
			out.raw = result.raw.ptrw();
		}
	}
	if (!sized && d.is_paged() && job->counted) {
		job->stats->end_read(job->reserved_bytes, job->read_bytes, false); // Allocation failure: release the reservation.
	}
	const GaussianSplatHlodRuntimeData::EditedLeaf *edited_leaf = d.find_edited(job->ticket.node);
	if (sized && edited_leaf) {
		// Package F1: an edited leaf is packed from its in-memory replacement payload, never from the file (a V4 package is
		// immutable) and never from the base snapshot's leaf.
		gs_hlod_pack::PayloadView view;
		view.gaussians = edited_leaf->gaussians.ptr();
		view.sh = edited_leaf->sh.is_empty() ? nullptr : edited_leaf->sh.ptr();
		result.ok = uint64_t(edited_leaf->gaussians.size()) == count &&
				gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out, &view);
	} else if (sized && d.is_paged()) {
		// Package E1: the payload is read from the V4 file now, on this worker, bounded to this one node's record.
		// Nothing of it outlives the job: the packed copy below is all that travels to the upload.
		Vector<Gaussian> gaussians;
		Vector<Vector3> sh;
		String why;
		const Error read_error = d.paged_source->read_node_payload(job->ticket.node, gaussians, sh, why);
		if (read_error == OK) {
			gs_hlod_pack::PayloadView view;
			view.gaussians = gaussians.ptr();
			view.sh = sh.is_empty() ? nullptr : sh.ptr();
			result.ok = gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out, &view);
		} else {
			result.ok = false;
		}
		if (job->counted) { job->stats->end_read(job->reserved_bytes, job->read_bytes, read_error == OK); }
	} else {
		result.ok = sized && gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out);
	}
	{
		MutexLock lock(job->mailbox->mutex);
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
		// An edited leaf is packed from memory (package F1): no file read, so no read reservation.
		if (source->is_paged() && !source->find_edited(p_ticket.node) && p_ticket.node < uint32_t(source->paged_source->get_nodes().size())) {
			read_bytes = source->paged_source->get_nodes()[p_ticket.node].stored_bytes;
			reserved = read_bytes + uint64_t(source->nodes[p_ticket.node].topology.payload_count) * record_stride();
			// Reserve against the read cap; the rest stay pending (LOADING, protected) and start as reads finish.
			if (read_stats->in_flight.load() != 0 && read_stats->in_flight_bytes.load() + reserved > cfg.max_inflight_read_bytes) {
				return false;
			}
			read_stats->begin_read(reserved);
			counted = true;
		}
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
		tasks.push_back(pool->add_native_task(&HlodNodeStreamer::run_job, job, false, "HlodNodePack"));
		return true;
	};
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
	MutexLock lock(mailbox->mutex);
	for (uint32_t i = 0; i < mailbox->done.size(); ++i) {
		PackResult &r = mailbox->done[i];
		if (r.ticket.shadow) {
			// A replacement payload of the staged edit. A ticket of a cancelled or superseded edit is dropped (counted).
			if (residency.finish_shadow(r.ticket, r.ok) && r.ok) {
				shadow_ready.push_back(std::move(r));
			}
		} else {
			ready.push_back(std::move(r));
		}
	}
	mailbox->done.clear();
}

void HlodNodeStreamer::release_staged_edit_data() {
	shadow_pending.clear();
	shadow_ready.clear();
	staged_data.unref();
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
	if (!edit_feasible(p_edited)) {
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
	return true;
}

HlodNodeStreamer::EditCommitResult HlodNodeStreamer::commit_staged_edit(RenderingDevice *p_device, const Buffers &p_buffers) {
	if (!active || !residency.is_edit_staged()) {
		return COMMIT_NOTHING;
	}
	if (residency.edit_blocked()) {
		cancel_staged_edit(); // A failed payload or a replacement child that can never load: give the reservation back.
		telemetry.edit_event = "blocked";
		return COMMIT_NOTHING;
	}
	if (!residency.edit_ready() || staged_data.is_null() || !p_device || p_device->get_device_instance_id() != device_generation) {
		return COMMIT_NOTHING;
	}
	const uint64_t stride = record_stride();
	const uint64_t atlas_capacity = atlas_bytes_capacity();
	// Every staged leaf must have its packed payload in hand before anything is written.
	for (uint32_t i = 0; i < shadow_ready.size(); ++i) {
		const PackResult &r = shadow_ready[i];
		const uint32_t count = r.quantized.is_empty() ? uint32_t(r.raw.size()) : uint32_t(r.quantized.size());
		const uint64_t offset = uint64_t(r.ticket.first_page) * cfg.page_splats * stride;
		if (count == 0 || offset + uint64_t(count) * stride > atlas_capacity || count > r.ticket.page_count * cfg.page_splats) {
			cancel_staged_edit();
			++telemetry.edit_commit_failures_total;
			telemetry.edit_event = "upload_failed";
			return COMMIT_UPLOAD_FAILED;
		}
	}
	// Phase 1: payloads into the reserved shadow runs. No node refers to them yet, so a failure here changes nothing a
	// draw can see.
	uint64_t payload_bytes = 0;
	for (uint32_t i = 0; i < shadow_ready.size(); ++i) {
		const PackResult &r = shadow_ready[i];
		const uint32_t count = r.quantized.is_empty() ? uint32_t(r.raw.size()) : uint32_t(r.quantized.size());
		const uint64_t bytes = uint64_t(count) * stride;
		const uint64_t offset = uint64_t(r.ticket.first_page) * cfg.page_splats * stride;
		const void *payload = cfg.quantized ? static_cast<const void *>(r.quantized.ptr()) : static_cast<const void *>(r.raw.ptr());
		if (p_device->buffer_update(p_buffers.atlas, uint32_t(offset), uint32_t(bytes), payload) != OK) {
			cancel_staged_edit();
			++telemetry.edit_commit_failures_total;
			telemetry.edit_event = "upload_failed";
			return COMMIT_UPLOAD_FAILED;
		}
		payload_bytes += bytes;
	}
	// Phase 2: quantization and metadata, last (a node becomes visible to the GPU only after its splats are queued).
	for (uint32_t i = 0; i < shadow_ready.size(); ++i) {
		const PackResult &r = shadow_ready[i];
#ifdef TESTS_ENABLED
		if (test_fail_commit_metadata) {
			++telemetry.edit_commit_failures_total;
			telemetry.edit_event = "commit_unusable";
			active = false;
			return COMMIT_UNUSABLE;
		}
#endif
		const bool ok = (!cfg.quantized || p_device->buffer_update(p_buffers.quantization, r.ticket.node * sizeof(ChunkQuantizationGPU),
											   sizeof(ChunkQuantizationGPU), &r.quant) == OK) &&
				p_device->buffer_update(p_buffers.chunk_meta, r.ticket.node * sizeof(ChunkMetaGPU), sizeof(ChunkMetaGPU), &r.meta) == OK;
		if (!ok) {
			// Part of the metadata may already name the new runs while the residency model still names the old ones:
			// the bundle cannot be trusted. Fail closed; the owner rebuilds it from the (unchanged) world.
			++telemetry.edit_commit_failures_total;
			telemetry.edit_event = "commit_unusable";
			active = false;
			return COMMIT_UNUSABLE;
		}
	}
	if (!residency.commit_edit()) {
		++telemetry.edit_commit_failures_total;
		telemetry.edit_event = "commit_unusable";
		active = false;
		return COMMIT_UNUSABLE;
	}
	data = staged_data;
	release_staged_edit_data();
	// Stale ancestors: their GPU metadata is cleared so no later record can draw them.
	clear_evicted(p_device, p_buffers);
	telemetry.edit_revision = data->edit_revision;
	telemetry.edit_shadow_total = 0;
	++telemetry.edits_committed_total;
	telemetry.edit_event = "committed";
	telemetry.edit_payload_bytes_committed_total += payload_bytes;
	refresh_telemetry();
	return COMMIT_DONE;
}

void HlodNodeStreamer::begin_frame(RenderingDevice *p_device, const Buffers *p_buffers) {
	last_commit_result = COMMIT_NOTHING;
	if (!active) {
		return;
	}
	residency.begin_frame(++frame);
	if (p_device && p_buffers && residency.is_edit_staged()) {
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

void HlodNodeStreamer::upload_ready(RenderingDevice *p_device, const Buffers &p_buffers) {
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
		if (!residency.is_pending(r.ticket)) {
			residency.begin_upload(r.ticket); // Rejected and counted: cancelled, unloaded or another device.
			ready.remove_at(i);
			continue;
		}
		const uint32_t count = r.quantized.is_empty() ? uint32_t(r.raw.size()) : uint32_t(r.quantized.size());
		const uint64_t bytes = uint64_t(count) * stride;
		if (spent != 0 && spent + bytes > cfg.upload_bytes_per_frame) {
			break; // Keep the rest for the next frame; they stay LOADING (protected, not drawable).
		}
		if (!residency.begin_upload(r.ticket)) {
			ready.remove_at(i);
			continue;
		}
		const uint64_t offset = uint64_t(r.ticket.first_page) * cfg.page_splats * stride;
		const bool fits = count != 0 && offset + bytes <= atlas_capacity && count <= r.ticket.page_count * cfg.page_splats;
		bool ok = fits && p_device && p_device->get_device_instance_id() == device_generation;
		if (ok) {
			const void *payload = cfg.quantized ? static_cast<const void *>(r.quantized.ptr()) : static_cast<const void *>(r.raw.ptr());
			// Payload first, metadata last: a node becomes visible to the GPU only after its splats are queued.
			ok = p_device->buffer_update(p_buffers.atlas, uint32_t(offset), uint32_t(bytes), payload) == OK &&
					(!cfg.quantized || p_device->buffer_update(p_buffers.quantization, r.ticket.node * sizeof(ChunkQuantizationGPU),
												sizeof(ChunkQuantizationGPU), &r.quant) == OK) &&
					p_device->buffer_update(p_buffers.chunk_meta, r.ticket.node * sizeof(ChunkMetaGPU), sizeof(ChunkMetaGPU), &r.meta) == OK;
		}
		residency.finish_upload(r.ticket, ok);
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
	if (!p_device || p_device->get_device_instance_id() != device_generation) {
		return;
	}
	const ChunkMetaGPU empty_meta = {};
	const ChunkQuantizationGPU empty_quant = {};
	for (uint32_t i = 0; i < evicted_scratch.size(); ++i) {
		const uint32_t node = evicted_scratch[i];
		p_device->buffer_update(p_buffers.chunk_meta, node * sizeof(ChunkMetaGPU), sizeof(ChunkMetaGPU), &empty_meta);
		if (cfg.quantized) {
			p_device->buffer_update(p_buffers.quantization, node * sizeof(ChunkQuantizationGPU), sizeof(ChunkQuantizationGPU), &empty_quant);
		}
	}
	telemetry.evicted_this_frame += evicted_scratch.size();
}

void HlodNodeStreamer::update(RenderingDevice *p_device, const Buffers &p_buffers, const gs_hlod_runtime::DesiredRecord *p_desired,
		uint32_t p_desired_count, const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count) {
	if (!active) {
		return;
	}
	telemetry.requested_this_frame = telemetry.uploaded_this_frame = telemetry.skipped_groups_this_frame = telemetry.evicted_this_frame = 0;
	telemetry.uploaded_bytes_this_frame = 0;
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
		}
	}
	// Protection first (ADR 14.1): drawn versions and their ancestors cannot be evicted by this frame's requests.
	residency.protect_drawn(drawn_scratch.ptr(), drawn_scratch.size());
	tickets_scratch.clear();
	if (residency.is_edit_staged()) {
		// After the frame's protection is known: allocate (evicting if needed) the shadow runs of a waiting edit.
		LocalVector<NodeResidency::Ticket> shadow_tickets;
		residency.advance_edit(shadow_tickets);
		for (uint32_t i = 0; i < shadow_tickets.size(); ++i) {
			shadow_pending.push_back(shadow_tickets[i]);
		}
		if (residency.staged_edit_age_frames() > cfg.edit_timeout_frames) {
			cancel_staged_edit();
			++telemetry.edit_timeouts_total;
			telemetry.edit_event = "timeout";
		}
	}
	const NodeResidency::RequestResult requested = residency.request(demand_scratch.ptr(), demand_scratch.size(), tickets_scratch);
	// Metadata of nodes evicted by this request is cleared before any upload of this update: a node evicted and
	// requested again is not in the log (NodeResidency), and a payload that lands below is never zeroed after.
	clear_evicted(p_device, p_buffers);
	for (uint32_t i = 0; i < tickets_scratch.size(); ++i) {
		pending.push_back(tickets_scratch[i]);
	}
	telemetry.requested_this_frame = requested.requested;
	telemetry.skipped_groups_this_frame = requested.skipped_groups;
	telemetry.blocked_groups_this_frame = requested.blocked_groups;
	telemetry.fragmented_groups_this_frame = requested.fragmented_groups;
	start_jobs();
	drain_mailbox();
	upload_ready(p_device, p_buffers);
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
