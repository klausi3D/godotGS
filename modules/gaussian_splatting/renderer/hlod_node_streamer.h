#ifndef GS_HLOD_NODE_STREAMER_H
#define GS_HLOD_NODE_STREAMER_H

// ADR docs/architecture/adr-hlod-streaming.md sections 6.6, 14.1, 14.2 (package D1): glue between the CPU
// residency model (lod/hlod_node_residency.*) and the HLOD relative bundle's GPU buffers.
//
// One streamer per prepared HLOD bundle. It is not a second streaming system: placement, generations,
// retirement and eviction are the shared NodeResidency model on top of GaussianAtlasAllocator (#1088); this
// class only (a) packs requested nodes on worker threads with the bundle's single packing path
// (renderer/hlod_node_pack.*), (b) uploads completed payloads, bounded per frame, and (c) hands the selector
// the resident mask. The atlas starts empty and every ChunkMetaGPU starts zeroed (splat_count 0), so a node
// that is not resident-ready draws nothing even if a stale record named it.
//
// Threading: update()/begin_frame()/shutdown() belong to the render thread. Workers only read the immutable
// runtime snapshot and write into a mailbox they co-own; they never touch the residency model or the device.

#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../lod/hlod_node_residency.h"
#include "../lod/hlod_view_set.h"
#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "gaussian_gpu_layout.h"
#include "hlod_node_pack.h"
#include "servers/rendering/rendering_device.h"

#include <atomic>

class HlodNodeStreamer : public RefCounted {
public:
	struct Config {
		uint32_t atlas_pages = 0;
		static constexpr uint32_t DEFAULT_PAGE_SPLATS = 1024;
		uint32_t page_splats = DEFAULT_PAGE_SPLATS;
		bool quantized = false;
		uint32_t position_bits = 0;
		uint32_t scale_bits = 0;
		uint64_t upload_bytes_per_frame = 32ull * 1024 * 1024;
		uint32_t max_jobs_in_flight = 16;
		// Package E1 (V4 paged source): the CPU working set of payload reads. A job is started only while the reserved
		// bytes (file record + packed copy) of all reads in flight plus its own stay within this cap (one read is always
		// allowed, so a record larger than the cap still loads, alone). It bounds the memory of reads in flight. Packed
		// payloads waiting for the per-frame upload budget (the `ready` queue) are OUTSIDE this cap and are measured
		// separately (payload_ready_bytes_peak).
		uint64_t max_inflight_read_bytes = 8ull * 1024 * 1024;
		uint32_t retire_delay_frames = 3;
	};
	struct Buffers {
		RID atlas;
		RID chunk_meta;
		RID quantization; // Invalid when not quantized.
	};
	// Per frame and cumulative, for diagnostics. Loading, ready and failed are reported separately (ADR 14.2).
	struct Telemetry {
		uint32_t desired = 0; // Nodes the view wants and does not have.
		uint32_t resident = 0;
		uint32_t loading = 0; // LOADING + UPLOADING
		uint32_t failed = 0;
		uint32_t permanently_failed = 0;
		uint32_t drawn = 0;
		uint32_t requested_this_frame = 0;
		uint32_t uploaded_this_frame = 0;
		uint64_t uploaded_bytes_this_frame = 0;
		uint32_t skipped_groups_this_frame = 0;
		uint32_t evicted_this_frame = 0;
		uint32_t pages_capacity = 0;
		uint32_t pages_resident = 0;
		uint32_t pages_in_flight = 0;
		uint32_t pages_retiring = 0;
		uint64_t uploaded_bytes_total = 0;
		uint64_t late_dropped_total = 0; // Real loss: rejected completion of a request that was not cancelled.
		uint64_t late_dropped_cancelled_total = 0; // Harmless: completion of a request already cancelled.
		uint64_t cancelled_total = 0;
		uint64_t evicted_total = 0;
		uint64_t failed_total = 0;
		uint64_t skipped_groups_total = 0;
		uint32_t stall_frames = 0; // Consecutive frames with demand, nothing in flight and nothing placeable.
		// Why the stall: a failed child blocks a transition; enough free pages without a contiguous run.
		uint32_t blocked_groups_this_frame = 0;
		uint32_t fragmented_groups_this_frame = 0;
		// Package E1 (V4 paged source only; all 0 for a resident snapshot): node payloads read from the file.
		uint64_t payload_reads_total = 0;
		uint64_t payload_bytes_read_total = 0;
		uint64_t payload_read_failures_total = 0;
		uint32_t payload_reads_in_flight = 0;
		// Peak sum of the working sets (file record + packed copy) of reads running at the same moment: the CPU payload
		// working set. Bounded by Config::max_inflight_read_bytes (or one record, if one alone is larger), whatever
		// the tree size.
		uint64_t payload_in_flight_bytes_peak = 0;
		uint64_t payload_largest_read_bytes = 0;
		// Peak bytes of packed payloads held on the CPU after their read, waiting for the per-frame upload budget.
		uint64_t payload_ready_bytes_peak = 0;
	};

	~HlodNodeStreamer();

	// Sizes the residency model for `p_data` and the page budget. False if the tree or budget is unusable
	// (root run larger than the atlas, empty tree). No GPU resource is created here.
	bool init(const Ref<GaussianSplatHlodRuntimeData> &p_data, const Config &p_config, uint64_t p_world_generation, uint64_t p_device_generation);
	// Waits for outstanding worker jobs (bounded: at most max_jobs_in_flight, each packs one node) and drops
	// every pending payload. Idempotent. After this nothing refers to the snapshot but the caller.
	void shutdown();
	bool is_active() const { return active; }

	// Retires freed runs; call once per frame before the selector reads resident_mask().
	void begin_frame();
	const Vector<uint8_t> &resident_mask() const { return residency.resident_mask(); }
	const gs_hlod_runtime::NodeResidency &get_residency() const { return residency; }

	// After the view's selection: protects the drawn set and its fallback coverage, requests the demand,
	// starts worker jobs, uploads completed payloads (bounded) and clears the metadata of evicted nodes.
	// `p_device` must be the device the buffers belong to; a different device generation drops every payload.
	void update(RenderingDevice *p_device, const Buffers &p_buffers, const gs_hlod_runtime::DesiredRecord *p_desired,
			uint32_t p_desired_count, const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count);

	// Splits a view's Desired records into what must be protected and what must be requested: drawn nodes and the
	// resident "keep" part of an incomplete bootstrap frontier are protected (nothing is drawn until the frontier
	// is complete, but evicting it would undo the work every frame); only the other records are demand.
	static void split_protection_and_demand(const gs_hlod_runtime::DesiredRecord *p_desired, uint32_t p_desired_count,
			const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count,
			LocalVector<uint32_t> &r_protected, LocalVector<uint32_t> &r_demand);

	const Telemetry &get_telemetry() const { return telemetry; }
	bool root_failed() const { return residency.root_permanently_failed(); }
	uint64_t atlas_bytes_capacity() const { return uint64_t(cfg.atlas_pages) * cfg.page_splats * record_stride(); }
	uint64_t record_stride() const { return cfg.quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian); }

private:
	// Counters the worker jobs update (atomics, co-owned like the mailbox so a late job never touches freed memory).
	class ReadStats : public RefCounted {
	public:
		std::atomic<uint64_t> reads{ 0 };
		std::atomic<uint64_t> bytes{ 0 };
		std::atomic<uint64_t> failures{ 0 };
		std::atomic<uint32_t> in_flight{ 0 };
		std::atomic<uint64_t> in_flight_bytes{ 0 };
		std::atomic<uint64_t> in_flight_bytes_peak{ 0 };
		std::atomic<uint64_t> largest{ 0 };
		// `p_reserved` is the job's whole CPU working set while it runs: the file record plus the packed copy.
		void begin_read(uint64_t p_reserved) {
			in_flight.fetch_add(1);
			const uint64_t now = in_flight_bytes.fetch_add(p_reserved) + p_reserved;
			uint64_t peak = in_flight_bytes_peak.load();
			while (now > peak && !in_flight_bytes_peak.compare_exchange_weak(peak, now)) {
			}
		}
		void end_read(uint64_t p_reserved, uint64_t p_file_bytes, bool p_ok) {
			in_flight_bytes.fetch_sub(p_reserved);
			in_flight.fetch_sub(1);
			if (p_ok) {
				reads.fetch_add(1);
				bytes.fetch_add(p_file_bytes);
				uint64_t seen = largest.load();
				while (p_file_bytes > seen && !largest.compare_exchange_weak(seen, p_file_bytes)) {
				}
			} else {
				failures.fetch_add(1);
			}
		}
	};
	struct PackResult {
		gs_hlod_runtime::NodeResidency::Ticket ticket;
		bool ok = false;
		Vector<PackedGaussian> raw;
		Vector<PackedGaussianQuantized> quantized;
		ChunkMetaGPU meta;
		ChunkQuantizationGPU quant;
	};
	class Mailbox : public RefCounted {
	public:
		Mutex mutex;
		LocalVector<PackResult> done; // Guarded by `mutex`.
	};
	struct Job {
		Ref<GaussianSplatHlodRuntimeData> data;
		gs_hlod_runtime::NodeResidency::Ticket ticket;
		gs_hlod_pack::Params params;
		uint32_t atlas_base = 0;
		Ref<Mailbox> mailbox;
		Ref<ReadStats> stats;
		uint64_t read_bytes = 0; // The record's stored bytes (paged source only).
		uint64_t reserved_bytes = 0; // Record + packed copy, reserved against max_inflight_read_bytes at start.
		bool counted = false; // begin_read() was called for this job, so end_read() must be.
	};

	static void run_job(void *p_userdata);
	void start_jobs();
	void collect_finished_tasks(bool p_wait_all);
	void drain_mailbox();
	void upload_ready(RenderingDevice *p_device, const Buffers &p_buffers);
	void clear_evicted(RenderingDevice *p_device, const Buffers &p_buffers);
	void refresh_telemetry();

	bool active = false;
	Config cfg;
	Ref<GaussianSplatHlodRuntimeData> data;
	gs_hlod_runtime::NodeResidency residency;
	Ref<Mailbox> mailbox;
	Ref<ReadStats> read_stats;
	LocalVector<WorkerThreadPool::TaskID> tasks;
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> pending; // Requested, job not started.
	LocalVector<PackResult> ready; // Packed, waiting for the per-frame upload budget (still LOADING).
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> tickets_scratch;
	LocalVector<uint32_t> demand_scratch;
	LocalVector<uint32_t> drawn_scratch;
	LocalVector<uint32_t> evicted_scratch;
	uint64_t world_generation = 0;
	uint64_t device_generation = 0;
	uint64_t frame = 0;
	Telemetry telemetry;
};

#endif
