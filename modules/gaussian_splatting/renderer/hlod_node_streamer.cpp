#include "hlod_node_streamer.h"

#include "../core/gaussian_streaming.h"
#include "../core/gs_vector_alloc.h"

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
	if (!residency.reset(infos, rc, p_world_generation, p_device_generation)) {
		return false;
	}
	// The root is the complete coarse fallback and the pinned base of every cut: it must fit on its own.
	if (residency.pages_for(0) > p_config.atlas_pages) {
		return false;
	}
	cfg = p_config;
	data = p_data;
	world_generation = p_world_generation;
	device_generation = p_device_generation;
	mailbox.instantiate();
	frame = 0;
	telemetry = Telemetry();
	telemetry.pages_capacity = p_config.atlas_pages;
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
	if (mailbox.is_valid()) {
		MutexLock lock(mailbox->mutex);
		mailbox->done.clear();
	}
	mailbox.unref();
	data.unref();
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
	result.ok = sized && gs_hlod_pack::pack_node(d, job->ticket.node, job->params, job->atlas_base, out);
	{
		MutexLock lock(job->mailbox->mutex);
		job->mailbox->done.push_back(std::move(result));
	}
	memdelete(job);
}

void HlodNodeStreamer::start_jobs() {
	collect_finished_tasks(false);
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	uint32_t consumed = 0;
	while (consumed < pending.size() && tasks.size() < cfg.max_jobs_in_flight && pool) {
		const NodeResidency::Ticket ticket = pending[consumed++];
		if (!residency.is_pending(ticket)) {
			continue; // Cancelled before it started: nothing to read.
		}
		Job *job = memnew(Job);
		job->data = data;
		job->ticket = ticket;
		job->params.relative = true;
		job->params.quantized = cfg.quantized;
		job->params.position_bits = cfg.position_bits;
		job->params.scale_bits = cfg.scale_bits;
		job->atlas_base = ticket.first_page * cfg.page_splats;
		job->mailbox = mailbox;
		tasks.push_back(pool->add_native_task(&HlodNodeStreamer::run_job, job, false, "HlodNodePack"));
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
		ready.push_back(std::move(mailbox->done[i]));
	}
	mailbox->done.clear();
}

void HlodNodeStreamer::begin_frame() {
	if (!active) {
		return;
	}
	residency.begin_frame(++frame);
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
	drawn_scratch.clear();
	for (uint32_t i = 0; i < p_drawn_count; ++i) {
		drawn_scratch.push_back(p_drawn[i].node);
	}
	// The resident part of an incomplete bootstrap frontier is protected like a drawn node: it is the work the
	// view is building toward, and nothing is drawn until it is complete.
	for (uint32_t i = 0; i < p_desired_count; ++i) {
		if (p_desired[i].keep) { drawn_scratch.push_back(p_desired[i].node); }
	}
	// Protection first (ADR 14.1): drawn versions and their ancestors cannot be evicted by this frame's requests.
	residency.protect_drawn(drawn_scratch.ptr(), drawn_scratch.size());
	demand_scratch.clear();
	for (uint32_t i = 0; i < p_desired_count; ++i) {
		if (!p_desired[i].keep) { demand_scratch.push_back(p_desired[i].node); }
	}
	tickets_scratch.clear();
	const NodeResidency::RequestResult requested = residency.request(demand_scratch.ptr(), demand_scratch.size(), tickets_scratch);
	for (uint32_t i = 0; i < tickets_scratch.size(); ++i) {
		pending.push_back(tickets_scratch[i]);
	}
	telemetry.requested_this_frame = requested.requested;
	telemetry.skipped_groups_this_frame = requested.skipped_groups;
	start_jobs();
	drain_mailbox();
	upload_ready(p_device, p_buffers);
	clear_evicted(p_device, p_buffers);
	const NodeResidency::Stats stats = residency.stats();
	const bool nothing_moving = stats.loading + stats.uploading == 0 && pending.is_empty() && ready.is_empty();
	telemetry.stall_frames = (p_desired_count != 0 && nothing_moving && requested.requested == 0) ? telemetry.stall_frames + 1 : 0;
	telemetry.desired = demand_scratch.size();
	telemetry.drawn = p_drawn_count;
	refresh_telemetry();
}

void HlodNodeStreamer::refresh_telemetry() {
	const NodeResidency::Stats s = residency.stats();
	telemetry.resident = s.resident;
	telemetry.loading = s.loading + s.uploading;
	telemetry.failed = s.failed;
	telemetry.permanently_failed = s.permanently_failed;
	telemetry.pages_resident = s.pages_resident;
	telemetry.pages_in_flight = s.pages_in_flight;
	telemetry.pages_retiring = s.pages_retiring;
	telemetry.late_dropped_total = s.late_dropped_total;
	telemetry.cancelled_total = s.cancelled_total;
	telemetry.evicted_total = s.evicted_total;
	telemetry.failed_total = s.failed_total;
	telemetry.skipped_groups_total = s.skipped_groups_total;
}
