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
// Package F1 (ADR 14.5, editable regions): stage_edit() takes an edited snapshot (lod/hlod_edit.*), reserves shadow
// runs for the edited leaves, loads the replacement cut and packs the new leaf payloads on the same worker path;
// commit_staged_edit() is the atomic publication boundary. Until it commits, nothing drawable changes: the old
// snapshot, its resident mask and its GPU metadata keep serving a consistent old cut. A paged (V4) file is never
// written: unedited nodes keep reading from it, edited leaves come from the in-memory overlay of the edited snapshot.
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
#include <memory>
#ifdef TESTS_ENABLED
#include "core/os/semaphore.h"
#endif

class PayloadPageStore;

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
		// Ordinary stored/decode/packed-output envelope. Reservations survive the
		// mailbox, ready and upload handoffs. An oversized node is admitted alone
		// and excludes further jobs until its output is uploaded or discarded.
		uint64_t max_inflight_read_bytes = 8ull * 1024 * 1024;
		uint32_t retire_delay_frames = 3;
		// Package F1: a staged edit that has not committed after this many frames is cancelled and reported
		// (edit_event "timeout"), so a stuck edit cannot hold its reservation forever.
		uint32_t edit_timeout_frames = 1800;
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
		// Package F1.
		bool edit_staged = false;
		uint32_t edit_shadow_ready = 0; // Packed replacement payloads waiting for the publication boundary.
		uint32_t edit_shadow_total = 0;
		uint64_t edit_revision = 0; // The revision of the snapshot currently served (0 = unedited).
		uint64_t edits_committed_total = 0;
		uint64_t edits_cancelled_total = 0;
		uint64_t edits_rejected_total = 0;
		uint64_t edit_commit_failures_total = 0;
		uint64_t edit_payload_bytes_committed_total = 0;
		uint32_t stale_nodes = 0;
		uint64_t edits_rejected_infeasible_total = 0;
		uint64_t edit_timeouts_total = 0;
		bool edit_waiting_for_space = false;
		// The last edit lifecycle event, for diagnostics: "" (none), "staged", "committed", "cancelled", "timeout",
		// "blocked", "upload_failed", "commit_unusable".
		const char *edit_event = "";
	};

	// Package F1: a view whose caps the replacement cut must fit (ADR 14.5: the full replacement cut is reserved across the
	// admitted views). The owner registers the request it selects with each frame; stage_edit() runs the selector on the
	// staged snapshot against every registered view. Pointers in the request are ignored.
	void register_view(uint64_t p_view_key, const gs_hlod_runtime::CutRequest &p_request);
	// A view that expired (its render buffers were freed or reconfigured) no longer constrains an edit.
	bool unregister_view(uint64_t p_view_key);
	uint32_t registered_view_count() const { return views.size(); }
	uint64_t registered_view_key(uint32_t p_index) const { return views[p_index].key; }

	enum EditStageResult : uint8_t {
		EDIT_STAGED = 0,
		EDIT_REJECTED_BUSY, // Inactive streamer, or another edit is staged.
		EDIT_REJECTED_DATA, // Not a direct derivative of the served snapshot, or unusable on this route.
		EDIT_REJECTED_NO_SPACE, // The shadow runs do not fit: nothing was changed.
		EDIT_REJECTED_INFEASIBLE, // The replacement cut does not fit a registered view's caps: nothing was changed.
		EDIT_REJECTED_NO_VIEW, // No view is registered, so feasibility cannot be shown: nothing was changed.
		EDIT_REJECTED_LINEAGE, // Not derived from the served snapshot.
	};
	enum EditCommitResult : uint8_t {
		COMMIT_NOTHING = 0, // No edit staged, not ready yet, or no usable device this frame: nothing changed.
		COMMIT_DONE,
		COMMIT_UPLOAD_FAILED, // A payload upload failed before any metadata changed: the edit was cancelled, nothing changed.
		COMMIT_UNUSABLE, // Metadata writes failed part-way: the streamer deactivated itself; the bundle must be rebuilt.
	};

	~HlodNodeStreamer();

	// Sizes the residency model for `p_data` and the page budget. False if the tree or budget is unusable
	// (root run larger than the atlas, empty tree). No GPU resource is created here.
	bool init(const Ref<GaussianSplatHlodRuntimeData> &p_data, const Config &p_config, uint64_t p_world_generation, uint64_t p_device_generation);
	// D3-2 runtime-store entry point. The renderer owns p_store through worker
	// drain. Admission uses this streamer's Config, without chunk defaults.
	Error init_with_page_store(const Ref<GaussianSplatHlodRuntimeData> &p_data, const Config &p_config,
			uint64_t p_world_generation, uint64_t p_device_generation, PayloadPageStore &p_store);
	// Waits for outstanding worker jobs (bounded: at most max_jobs_in_flight, each packs one node) and drops
	// every pending payload. Idempotent. After this nothing refers to the snapshot but the caller.
	void shutdown();
	bool is_active() const { return active; }
	// Render-thread ownership check; the caller owns p_store through worker drain.
	bool uses_page_store(const PayloadPageStore *p_store) const { return active && p_store != nullptr && page_store == p_store; }

	// Retires freed runs; call once per frame before the selector reads resident_mask() or get_data(). With a device, this
	// is also the publication boundary of a staged edit (package F1): when the replacement cut is complete and ready the
	// edit commits HERE, so the order "commit, then select" is structural and cannot be skipped by a caller.
	void begin_frame(RenderingDevice *p_device = nullptr, const Buffers *p_buffers = nullptr);
	EditCommitResult get_last_commit_result() const { return last_commit_result; }

	// --- Package F1 ---
	// The snapshot the selector must read. It changes only inside commit_staged_edit().
	const Ref<GaussianSplatHlodRuntimeData> &get_data() const { return data; }
	// Admits an edit whole or not at all. `p_edited` must be the direct derivative (edit revision + 1) of get_data().
	EditStageResult stage_edit(const Ref<GaussianSplatHlodRuntimeData> &p_edited);
	bool is_edit_staged() const { return active && residency.is_edit_staged(); }
	bool is_edit_ready() const { return active && residency.edit_ready(); }
	// The publication boundary (called by begin_frame(device, buffers)): when the replacement cut is complete and ready it
	// uploads the new leaf payloads, flips the served snapshot and drops the stale ancestors in one step, so no draw built
	// after this can name a stale node or an old leaf version. Frames already submitted may still present (ADR 14.5).
	EditCommitResult commit_staged_edit(RenderingDevice *p_device, const Buffers &p_buffers);
	// Gives the reservation back; the served snapshot and residency are exactly as before stage_edit().
	bool cancel_staged_edit();
#ifdef TESTS_ENABLED
	// Ordinary-path observations only; shadow_ready is deliberately excluded.
	// Sample actual storage under the mailbox/accounting locks, independently
	// of token counters: service/stash, worker arrays/scratch, cached scratch,
	// and packed output in every owner (including an unfinished upload).
	struct TestSnapshot {
		Error observation_error = ERR_UNAVAILABLE;
		uint64_t stored_bytes = 0;
		uint64_t decode_bytes = 0;
		uint64_t idle_decoder_bytes = 0;
		uint64_t worker_output_bytes = 0;
		uint64_t mailbox_bytes = 0;
		uint64_t ready_bytes = 0;
		uint64_t partial_upload_bytes = 0;
		uint64_t charged_bytes = 0;
		uint64_t peak_retained_bytes = 0; // Real storage high-water, not token peak.
		uint32_t peak_jobs_in_flight = 0;
		uint64_t oversized_reservation_bytes = 0;
		uint64_t reservations_acquired = 0;
		uint64_t reservations_released = 0;
		uint64_t live_reservations = 0;
		uint64_t jobs_started = 0;
		uint64_t admission_waits = 0;
		uint32_t jobs_in_flight = 0;
		uint32_t pending_tickets = 0;
		uint32_t mailbox_outputs = 0;
		uint32_t ready_outputs = 0;
		uint64_t faults_triggered = 0;
	};
	// Append at the REAL metadata write / finish_upload boundary. The CPU sink
	// substitutes device writes only; jobs, admission, validation and residency
	// stay on the real path. Epochs come from the completed payload, never from
	// the current source (tests independently know the expected epoch).
	struct TestPublication {
		uint32_t node = UINT32_MAX;
		uint64_t source_instance_id = 0; // Immutable paged source owning this job.
		uint64_t content_generation = 0; // V4 record stamp of the completed payload.
		uint64_t world_generation = 0;
		uint64_t device_generation = 0;
		uint64_t request_generation = 0;
		uint32_t metadata_splat_count = 0;
	};
	struct TestControl {
		bool hold_uploads = false;
		bool hold_mailbox = false;
		bool hold_workers = false; // Before read/decode; teardown releases it.
		bool cpu_upload_sink = false;
		bool fail_validation = false;
		bool fail_upload = false;
	};
	enum TestFault {
		TEST_FAULT_NONE,
		TEST_FAULT_DROP_OUTPUT_CHARGE,
		TEST_FAULT_SKIP_PUBLICATION_GENERATION,
	};
	// Faults alter the REAL handoff/publication, never stats or a checker.
	// These seam methods belong to the render thread, like update()/shutdown().
	// The ledger lock serializes worker-visible fields; pending/ready and the
	// CPU device-write sink additionally rely on that render-thread confinement.
	// Drop-output-charge releases the token at mailbox/ready while keeping the
	// packed allocation. Skip-generation bypasses every publication epoch
	// rejection (including NodeResidency's ticket gates) needed to write stale
	// metadata AND finish as RESIDENT, while preserving the old provenance.
	Error _test_set_control(const TestControl &p_control);
	Error _test_set_fault(TestFault p_fault);
	// Cumulative owner/peak counters survive epoch replacement and shutdown.
	TestSnapshot _test_snapshot() const;
	Vector<TestPublication> _test_publications() const;
	// Invalidates the live epoch while retaining old in-flight/queued output:
	// the real publication guard, rather than test queue deletion, must reject it.
	Error _test_replace_source(const Ref<GaussianSplatHlodRuntimeData> &p_data,
			uint64_t p_world_generation, uint64_t p_device_generation);
	Error _test_cancel_node(uint32_t p_node); // Idempotent, uses real cancel path.
	Error _test_evict_node(uint32_t p_node);

	// Test seam: makes the metadata writes of the NEXT commit fail, which is COMMIT_UNUSABLE (the fail-closed path).
	bool test_fail_commit_metadata = false;
	// Test seam: the packed replacement payload of a staged leaf that is waiting for the publication boundary (raw route).
	bool test_get_shadow_payload(uint32_t p_node, Vector<PackedGaussian> &r_raw, ChunkMetaGPU &r_meta) const {
		for (uint32_t i = 0; i < shadow_ready.size(); ++i) {
			if (shadow_ready[i].ticket.node == p_node) {
				r_raw = shadow_ready[i].raw;
				r_meta = shadow_ready[i].meta;
				return true;
			}
		}
		return false;
	}
#endif
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
	enum OutputOwner { OUTPUT_WORKER, OUTPUT_MAILBOX, OUTPUT_READY, OUTPUT_UPLOAD };
	// The existing mailbox lock also guards admission, live storage observations
	// and test controls. It is recursive so destruction during a locked handoff
	// can release its token. No store call or task join runs under this lock.
	class ReservationLedger : public RefCounted {
	public:
		mutable Mutex mutex;
		uint64_t charged = 0, acquired = 0, released = 0, live = 0, oversized = 0;
		uint64_t jobs_started = 0, admission_waits = 0;
		uint32_t jobs = 0, peak_jobs = 0;
		// Storage observations are changed by allocation/free/handoff events,
		// independently of token charges (including under mutation controls).
		uint64_t stored = 0, decoded = 0, output[4] = {}, peak_storage = 0;
		void observe_peak(); // Caller holds mutex.
#ifdef TESTS_ENABLED
		TestControl control;
		TestFault fault = TEST_FAULT_NONE;
		uint64_t faults_triggered = 0;
		uint32_t held_workers = 0;
		Semaphore worker_gate; // Test-only wait; never a ready-slot wait.
		Vector<TestPublication> publications;
#endif
	};
	class Reservation {
		Ref<ReservationLedger> ledger;
		uint64_t charge = 0, stored = 0, decoded = 0, output = 0;
		bool oversized = false, running = true, token_live = true;
		OutputOwner owner = OUTPUT_WORKER;

	public:
		Reservation(const Ref<ReservationLedger> &p_ledger, uint64_t p_charge, bool p_oversized);
		~Reservation();
		Reservation(const Reservation &) = delete;
		Reservation &operator=(const Reservation &) = delete;
		void storage_changed(uint64_t p_stored, uint64_t p_decoded);
		void output_changed(uint64_t p_bytes);
		void move_output(OutputOwner p_owner);
		void finish_work(); // Input storage freed; keeps actual output capacity charged.
		void finish_worker();
#ifdef TESTS_ENABLED
		void drop_output_charge(); // Only called by the real TESTS_ENABLED fault.
#endif
	};
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
		uint64_t source_instance_id = 0, content_generation = 0;
		// Declared before the vectors: storage is destroyed before the permit.
		std::unique_ptr<Reservation> reservation;
		Vector<PackedGaussian> raw;
		Vector<PackedGaussianQuantized> quantized;
		ChunkMetaGPU meta = {};
		ChunkQuantizationGPU quant = {};
		PackResult() = default;
		PackResult(const PackResult &) = delete;
		PackResult &operator=(const PackResult &) = delete;
		PackResult(PackResult &&p_other) noexcept;
		PackResult &operator=(PackResult &&p_other) noexcept;
	};
	class Mailbox : public RefCounted {
	public:
		Ref<ReservationLedger> ledger;
		LocalVector<PackResult> done; // Guarded by ledger->mutex.
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
		PayloadPageStore *store = nullptr; // Borrowed; renderer drains jobs before store shutdown.
		std::unique_ptr<Reservation> reservation;
	};
	struct ReadObservation : GaussianSplatHlodPagedSource::ReadStorageObserver {
		Reservation *reservation = nullptr;
		void storage_changed(uint64_t p_stored, uint64_t p_decoded, uint64_t p_workspace) override;
	};

	static void run_job(void *p_userdata);
	void start_jobs();
	void collect_finished_tasks(bool p_wait_all);
	void drain_mailbox();
	void release_staged_edit_data();
	void upload_ready(RenderingDevice *p_device, const Buffers &p_buffers);
	void clear_evicted(RenderingDevice *p_device, const Buffers &p_buffers);
	void refresh_telemetry();

	bool active = false;
	Config cfg;
	Ref<GaussianSplatHlodRuntimeData> data;
	gs_hlod_runtime::NodeResidency residency;
	Ref<Mailbox> mailbox;
	Ref<ReadStats> read_stats;
	Ref<ReservationLedger> ledger; // Kept after shutdown for cumulative observations.
	PayloadPageStore *page_store = nullptr; // Render-thread field; jobs borrow an immutable copy.
	uint64_t store_generation = 0;
#ifdef TESTS_ENABLED
	Vector<ChunkMetaGPU> test_metadata; // Render-thread CPU device-write sink only.
#endif
	LocalVector<WorkerThreadPool::TaskID> tasks;
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> pending; // Requested, job not started.
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> shadow_pending; // Package F1: staged-edit tickets, job not started.
	LocalVector<PackResult> shadow_ready; // Package F1: packed replacement payloads, uploaded at the publication boundary.
	Ref<GaussianSplatHlodRuntimeData> staged_data; // Package F1: the edited snapshot, served after commit.
	LocalVector<PackResult> ready; // Packed, waiting for the per-frame upload budget (still LOADING).
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> tickets_scratch;
	LocalVector<uint32_t> demand_scratch;
	LocalVector<uint32_t> prefetch_scratch;
	struct RegisteredView {
		uint64_t key = 0;
		gs_hlod_runtime::CutRequest request;
	};
	LocalVector<RegisteredView> views;
	EditCommitResult last_commit_result = COMMIT_NOTHING;
	bool edit_feasible(const Ref<GaussianSplatHlodRuntimeData> &p_edited) const;
	LocalVector<uint32_t> drawn_scratch;
	LocalVector<uint32_t> evicted_scratch;
	uint64_t world_generation = 0;
	uint64_t device_generation = 0;
	uint64_t frame = 0;
	Telemetry telemetry;
};

#endif
