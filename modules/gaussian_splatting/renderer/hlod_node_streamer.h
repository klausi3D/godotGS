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
// Ordinary nodes that fit the full staging-clamped frame allowance keep whole-payload admission.
// Larger nodes transfer record-aligned slices while LOADING; their CPU allocation stays charged.
// Quantization then metadata publish only after the final payload slice, in a later frame if needed.
//
// Package F1 (ADR 14.5, editable regions): stage_edit() takes an edited snapshot (lod/hlod_edit.*), reserves shadow
// runs for the edited leaves, loads the replacement cut and packs with the ordinary reservation ledger. update()
// transfers payload slices into invisible shadow runs under the shared frame allowance, returning each CPU permit
// after transfer and retaining only a per-leaf descriptor. commit_staged_edit() atomically publishes metadata at
// begin_frame(), after every leaf is transferred. Until it commits, nothing drawable changes: the old
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
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

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
		static_assert(DEFAULT_PAGE_SPLATS == GaussianSplatHlodRuntimeData::DEFAULT_PAGE_SPLATS, "Snapshot placement cache must use the streamer's page geometry.");
		uint32_t page_splats = DEFAULT_PAGE_SPLATS;
		bool quantized = false;
		uint32_t position_bits = 0;
		uint32_t scale_bits = 0;
		uint64_t upload_bytes_per_frame = 32ull * 1024 * 1024;
		// Caller inputs to shared sizing; upload_bytes_per_frame must be nonzero.
		// Zero staging/CPU/pool means unknown, hence no clamp from that input.
		// Publisher-like 128 MiB / 2 frames = 64 MiB leaves 32 MiB/frame;
		// 32 logical CPUs and >=16 available pool threads leave the 16-job cap.
		uint64_t staging_upload_clamp_bytes = 0;
		uint32_t logical_cpus = 0;
		uint32_t pool_threads = 0;
		uint32_t reserved_pool_threads = 0;
		uint32_t max_jobs_in_flight = 16;
		// Pure publisher policy; also used by host tests with small/unknown pools.
		void set_worker_pool(uint32_t p_logical_cpus, uint32_t p_pool_threads);
		uint32_t page_store_reader_count() const;
		// Base read-admission input. Codec-none and GSQ store streams derive their
		// retained cap from node allocations and base-equivalent concurrency; other
		// paths retain this cap. A node above this input still runs alone.
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
	// Report-only ordinal contract for the flythrough trace. Admission reasons
	// preserve the order of the existing short-circuit predicate in start_jobs().
	enum RefusingGate : uint8_t {
		REFUSING_NONE = 0,
		REFUSING_ALLOCATION = 1,
		REFUSING_OVERSIZED_LATCH = 2,
		REFUSING_TRANSPORT_BUDGET = 3,
		REFUSING_OVERSIZED_WAIT_LIVE = 4,
		REFUSING_RETAINED_BUDGET = 5,
		REFUSING_WORKER_PERMIT = 6,
		REFUSING_METADATA_CLEAR = 7,
		REFUSING_REQUEST_BUDGET = 8,
		REFUSING_FAILED_DEPENDENCY = 9,
		REFUSING_GATE_COUNT,
	};
	static RefusingGate classify_admission_refusal(bool p_oversized_latch, bool p_transport_full,
			bool p_oversized_wait_live, bool p_retained_full);
	// Per frame and cumulative, for diagnostics. Loading, ready and failed are reported separately (ADR 14.2).
	struct Telemetry {
		uint32_t desired = 0; // Nodes the view wants and does not have.
		uint32_t resident = 0;
		uint32_t loading = 0; // LOADING + UPLOADING
		uint32_t failed = 0;
		uint32_t permanently_failed = 0;
		uint32_t retry_later_waiting = 0;
		uint32_t drawn = 0;
		uint32_t requested_this_frame = 0;
		uint32_t uploaded_this_frame = 0;
		uint64_t uploaded_bytes_this_frame = 0; // Payload bytes; metadata also consumes the shared frame allowance.
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
		uint64_t failed_permanent_total = 0;
		uint64_t failed_retry_later_total = 0;
		uint64_t skipped_groups_total = 0;
		uint32_t stall_frames = 0; // Consecutive frames with demand, nothing in flight and nothing placeable.
		// Why the stall: a failed child blocks a transition; enough free pages without a contiguous run.
		uint32_t blocked_groups_this_frame = 0;
		uint32_t fragmented_groups_this_frame = 0;
		// Render-thread snapshots only; workers never access these fields. Worker ledger samples below
		// still use ReservationLedger::mutex, the existing admission/mailbox lock.
		bool transitions_measured = false;
		uint32_t protected_sibling_nodes = 0, abandoned_transitions_this_frame = 0;
		uint64_t abandoned_transitions_total = 0;
		gs_hlod_runtime::NodeResidency::TransitionState transition_state = gs_hlod_runtime::NodeResidency::TRANSITIONS_ABSENT;
		// Selection observation, render-thread-owned. No selection is absent; an unprepared selection uses no reserve.
		gs_hlod_runtime::CutHistory::PlacementReserveState placement_reserve_state = gs_hlod_runtime::CutHistory::RESERVE_ABSENT;
		bool placement_reserve_measured = false;
		uint64_t placement_reserve_pages = 0;
		// Owner-thread snapshots; worker-visible ledger values are sampled under
		// ReservationLedger::mutex, the existing admission/mailbox lock.
		bool pressure_measured = false;
		uint64_t pressure_epoch = 0; // Process-unique successful init identity; report-only, independent of reused world/device IDs.
		uint32_t pages_free = 0, largest_free_run = 0;
		uint64_t allocation_refused_total = 0;
		uint64_t allocation_no_contiguous_run_total = 0, allocation_protected_capacity_total = 0;
		uint64_t allocation_over_page_budget_total = 0, allocation_pending_retire_total = 0;
		uint64_t allocation_eviction_limit_total = 0, allocation_other_total = 0;
		uint64_t evicted_in_cut_total = 0, evicted_in_transition_total = 0, evicted_neither_total = 0;
		uint64_t eviction_events_dropped_total = 0, eviction_events_truncated_total = 0;
		bool gate_measured = false;
		uint32_t pending_count = 0, ready_count = 0;
		uint64_t live_reservations = 0, oversized_reservation_bytes = 0;
		bool oversized_latch = false, oversized_wait_live = false;
		// TaskIDs do not expose whether their request was cancelled. There is no
		// separate cancellation latch; unavailable must remain distinct from zero.
		bool cancelled_running_jobs_measured = false;
		RefusingGate refusing_gate = REFUSING_NONE;
		uint64_t gate_frames_total[REFUSING_GATE_COUNT] = {};
		// Package E1 (V4 paged source only; all 0 for a resident snapshot): node payloads read from the file.
		uint64_t payload_reads_total = 0;
		uint64_t payload_bytes_read_total = 0;
		uint64_t payload_read_failures_total = 0;
		uint32_t payload_reads_in_flight = 0;
		// Peak sum of the admitted working sets of reads running at the same moment. Both store codecs use
		// rounded read/decode/pack phase peaks and the derived retained cap; legacy reads retain record + packed accounting,
		// bounded by Config::max_inflight_read_bytes (or one record, if one alone is larger), whatever
		// the tree size.
		uint64_t payload_in_flight_bytes_peak = 0;
		uint64_t payload_largest_read_bytes = 0;
		// Peak bytes of packed payloads held on the CPU after their read, waiting for the per-frame upload budget.
		uint64_t payload_ready_bytes_peak = 0;
		// Package F1.
		bool edit_staged = false;
		uint32_t edit_shadow_ready = 0; // Packed payloads waiting for (or part-way through) transfer in update().
		uint32_t edit_shadow_transferred = 0; // Payload-free descriptors waiting for atomic publication.
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
		EDIT_REJECTED_INFEASIBLE, // Replacement cut or final metadata publication cannot fit: nothing was changed.
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
	// Cancels outstanding jobs/store requesters before joining workers and dropping
	// pending payloads. Active IO drains at its next read-slice boundary. Idempotent.
	void shutdown();
	bool is_active() const { return active; }
	// Render-thread ownership check; the caller owns p_store through worker drain.
	bool uses_page_store(const PayloadPageStore *p_store) const { return active && p_store != nullptr && page_store == p_store; }
	// Render-thread derived shared cap; ordinary and shadow reservations include queued output.
	uint64_t get_retained_byte_cap() const { return retained_byte_cap; }

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
	// publishes the already transferred leaves' metadata, flips the snapshot and drops stale ancestors in one call, so no draw built
	// after this can name a stale node or an old leaf version. Frames already submitted may still present (ADR 14.5).
	EditCommitResult commit_staged_edit(RenderingDevice *p_device, const Buffers &p_buffers);
	// Gives the reservation back; the served snapshot and residency are exactly as before stage_edit().
	bool cancel_staged_edit();
#ifdef TESTS_ENABLED
	// Ordinary and shadow payload owners are disjoint; the reservation ledger totals are shared.
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
		uint64_t transport_reserved_bytes = 0; // Promised stored bytes, under ledger->mutex.
		uint64_t peak_retained_bytes = 0; // Real storage high-water, not token peak.
		uint32_t peak_jobs_in_flight = 0;
		uint64_t oversized_reservation_bytes = 0;
		uint64_t reservations_acquired = 0;
		uint64_t reservations_released = 0;
		uint64_t live_reservations = 0;
		uint64_t jobs_started = 0;
		uint64_t admission_waits = 0;
		uint32_t jobs_in_flight = 0;
		uint32_t tasks_outstanding = 0; // Real WorkerThreadPool handles, including completed/uncollected tasks.
		uint64_t mailbox_publications = 0; // Cumulative actual handoffs; survives shutdown's mailbox clear.
		uint32_t pending_tickets = 0;
		uint32_t mailbox_outputs = 0;
		uint32_t ready_outputs = 0;
		uint64_t faults_triggered = 0;
		// Owner byte fields above remain ordinary-only; ledger totals are shared.
		Error shadow_observation_error = ERR_UNAVAILABLE;
		uint64_t shadow_worker_output_bytes = 0;
		uint64_t shadow_mailbox_bytes = 0;
		uint64_t shadow_ready_bytes = 0;
		uint64_t shadow_partial_upload_bytes = 0;
		uint64_t shadow_live_reservations = 0;
		uint64_t shadow_transferred_bytes = 0; // Current/last transaction, survives commit/cancel.
		uint64_t shadow_descriptor_payload_bytes = 0;
		uint64_t shadow_pending_payload_bytes = 0;
		uint32_t shadow_transfer_count = 0;
		uint32_t shadow_ready_outputs = 0;
		uint32_t shadow_pending_tickets = 0;
	};
	struct TestWrite {
		enum Kind { ATLAS, QUANTIZATION, METADATA };
		Kind kind = ATLAS;
		uint32_t node = UINT32_MAX;
		uint64_t offset = 0;
		uint64_t bytes = 0;
		uint64_t frame = 0;
		bool shadow = false;
		bool during_commit = false;
		ChunkMetaGPU meta = {};
		ChunkQuantizationGPU quant = {};
		// CPU sink only: capture the actual write arguments, so sizing tests can
		// reconstruct the atlas independently of packing and telemetry.
		Vector<uint8_t> payload;
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
		// Ordered writes made by the ordinary CPU sink, for preservation evidence.
		Vector<TestWrite> writes;
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
		// D3-3 controls alter actual transfer/readiness/publication/permit ownership.
		TEST_FAULT_EARLY_SHADOW_METADATA,
		TEST_FAULT_READY_ON_PACK,
		TEST_FAULT_LEAK_SHADOW_PERMIT,
		TEST_FAULT_PARTIAL_COMMIT,
		// Controls change the real allowance/final-slice publication path.
		TEST_FAULT_IGNORE_STAGING_CLAMP,
		TEST_FAULT_EARLY_FINAL_SLICE,
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
	Error _test_set_upload_hold(bool p_hold); // Changes only the existing test upload hold.
	Error _test_set_fault(TestFault p_fault);
	// Cumulative owner/peak counters survive epoch replacement and shutdown.
	TestSnapshot _test_snapshot() const;
	Vector<TestPublication> _test_publications() const;
	// Observation capability only, never an opt-in to production transfer behavior.
	// The trace records CPU-sink or device writes cumulatively, at the
	// actual write boundary, including commit and eviction metadata/quantization.
	Error _test_observe_shadow_transfers();
	Error _test_shadow_writes(Vector<TestWrite> &r_writes) const;
	Error _test_shadow_runs(Vector<gs_hlod_runtime::NodeResidency::Ticket> &r_runs) const;
	// Invalidates the live epoch while retaining old in-flight/queued output:
	// the real publication guard, rather than test queue deletion, must reject it.
	Error _test_replace_source(const Ref<GaussianSplatHlodRuntimeData> &p_data,
			uint64_t p_world_generation, uint64_t p_device_generation);
	Error _test_cancel_node(uint32_t p_node); // Idempotent, uses real cancel path.
	Error _test_evict_node(uint32_t p_node);

	// Test seam: makes the metadata writes of the NEXT commit fail, which is COMMIT_UNUSABLE (the fail-closed path).
	bool test_fail_commit_metadata = false;
	// Test seam: inspect a packed leaf BEFORE transfer frees its payload (raw route).
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
			uint32_t p_desired_count, const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count, uint64_t p_owner = 0);

	// Splits a view's Desired records into what must be protected and what must be requested: drawn nodes and the
	// resident "keep" part of an incomplete bootstrap frontier are protected (nothing is drawn until the frontier
	// is complete, but evicting it would undo the work every frame); only the other records are demand.
	// Runtime callers supply r_kept/r_groups: bootstrap keeps then become revocable plan dependencies,
	// separate from r_protected's genuinely drawn nodes. The six-argument form retains legacy splitting.
	static void split_protection_and_demand(const gs_hlod_runtime::DesiredRecord *p_desired, uint32_t p_desired_count,
			const gs_hlod_runtime::DrawRecord *p_drawn, uint32_t p_drawn_count,
			LocalVector<uint32_t> &r_protected, LocalVector<uint32_t> &r_demand,
			LocalVector<uint32_t> *r_kept = nullptr, LocalVector<gs_hlod_runtime::TransitionGroup> *r_groups = nullptr);
	uint64_t transition_workspace_bytes() const;

	// Render-thread handoff after selection; begin_frame clears it if this frame never selects.
	void capture_placement_reserve(const gs_hlod_runtime::CutHistory::Result &p_cut);
	const Telemetry &get_telemetry() const { return telemetry; }
	Array get_pressure_eviction_events() const; // Current-frame batch only; probe retains its capped history.
	Variant get_payload_telemetry() const; // Null when this route has no measured page store.
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
		uint64_t charged = 0, acquired = 0, released = 0, live = 0, oversized = 0, peak_live = 0;
		uint64_t transport_reserved = 0; // Raw windows or whole GSQ records promised against the unchanged base read cap.
		uint64_t jobs_started = 0, admission_waits = 0;
		uint64_t collection_admission_waits_base = 0;
		uint32_t jobs = 0, peak_jobs = 0;
		// Storage observations are changed by allocation/free/handoff events,
		// independently of token charges (including under mutation controls).
		uint64_t stored = 0, decoded = 0, output[4] = {}, peak_storage = 0;
		// All measurement fields, including worker pack sums, use this same mutex.
		uint64_t shadow_output[4] = {};
		uint64_t owner_peak[4] = {}, stored_peak = 0, active_peak = 0, packed_peak = 0, shadow_peak = 0;
		uint32_t output_jobs[4] = {}, ready_jobs_peak = 0;
		uint64_t collection_storage_peak = 0, ready_output_peak = 0, stale_generation_publications = 0, upload_hold_frames = 0, oversized_events = 0;
		// Actual D3 admission policy: normal cap, or one oversized reservation alone.
		// Peak bounds are captured with their peaks; an old oversized node cannot hide a later escape.
		uint64_t normal_byte_bound = 0, storage_peak_bound = 0, ready_peak_bound = 0, bound_violations = 0;
		uint64_t pack_calls = 0, pack_nsec = 0;
		uint64_t queue_residence_nsec = 0, queue_residence_samples = 0;
		uint64_t upload_bytes = 0, upload_slices = 0, upload_completions = 0, upload_completion_nsec = 0;
		void reset_measurements(); // Caller holds mutex; ownership counters survive.
		void observe_peak(); // Caller holds mutex.
#ifdef TESTS_ENABLED
		TestControl control;
		TestFault fault = TEST_FAULT_NONE;
		uint64_t faults_triggered = 0;
		uint32_t held_workers = 0;
		uint64_t mailbox_publications = 0;
		Semaphore worker_gate; // Test-only wait; never a ready-slot wait.
		Vector<TestPublication> publications;
		bool observe_shadows = false;
		uint64_t shadow_live = 0;
		Vector<TestWrite> writes;
#endif
	};
	class Reservation {
		Ref<ReservationLedger> ledger;
		uint64_t charge = 0, stored = 0, decoded = 0, output = 0, transport = 0;
		bool oversized = false, running = true, token_live = true;
		bool shadow = false;
		OutputOwner owner = OUTPUT_WORKER;

	public:
		Reservation(const Ref<ReservationLedger> &p_ledger, uint64_t p_charge, bool p_oversized, uint64_t p_transport, bool p_shadow = false);
		~Reservation();
		Reservation(const Reservation &) = delete;
		Reservation &operator=(const Reservation &) = delete;
		void storage_changed(uint64_t p_stored, uint64_t p_decoded);
		void output_changed(uint64_t p_bytes);
		void move_output(OutputOwner p_owner);
		void finish_transport(); // Last stored window freed; decoder/packing may still be running.
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
		// `p_reserved` is the admitted job working set (the legacy path uses record + packed copy).
		void begin_read(uint64_t p_reserved) {
			in_flight.fetch_add(1);
			const uint64_t now = in_flight_bytes.fetch_add(p_reserved) + p_reserved;
			uint64_t peak = in_flight_bytes_peak.load();
			while (now > peak && !in_flight_bytes_peak.compare_exchange_weak(peak, now)) {
			}
		}
		void end_read(uint64_t p_reserved, uint64_t p_file_bytes, bool p_ok, bool p_cancelled = false) {
			in_flight_bytes.fetch_sub(p_reserved);
			in_flight.fetch_sub(1);
			if (p_ok) {
				reads.fetch_add(1);
				bytes.fetch_add(p_file_bytes);
				uint64_t seen = largest.load();
				while (p_file_bytes > seen && !largest.compare_exchange_weak(seen, p_file_bytes)) {
				}
			} else if (!p_cancelled) {
				failures.fetch_add(1);
			}
		}
	};
	class JobCancellation;
	struct PackResult {
		gs_hlod_runtime::NodeResidency::Ticket ticket;
		bool ok = false;
		gs_hlod_runtime::FailureClass failure_class = gs_hlod_runtime::FAILURE_PERMANENT;
		uint64_t source_instance_id = 0, content_generation = 0;
		Ref<JobCancellation> cancellation; // Exact token outlives task retirement and the bounded residency report history.
		// Declared before the vectors: storage is destroyed before the permit.
		std::unique_ptr<Reservation> reservation;
		Vector<PackedGaussian> raw;
		Vector<PackedGaussianQuantized> quantized;
		ChunkMetaGPU meta = {};
		ChunkQuantizationGPU quant = {};
		uint64_t uploaded_bytes = 0; // Cursor only; the entire remaining allocation stays charged until completion.
		uint64_t queue_enter_nsec = 0, upload_start_nsec = 0;
#ifdef TESTS_ENABLED
		uint32_t test_first_write = UINT32_MAX;
#endif
		PackResult() = default;
		PackResult(const PackResult &) = delete;
		PackResult &operator=(const PackResult &) = delete;
		PackResult(PackResult &&p_other) noexcept;
		PackResult &operator=(PackResult &&p_other) noexcept;
	};
	struct ShadowTransfer {
		gs_hlod_runtime::NodeResidency::Ticket ticket;
		ChunkMetaGPU meta = {};
		ChunkQuantizationGPU quant = {};
		uint64_t bytes = 0; // No packed vector or permit survives successful transfer.
	};
	class Mailbox : public RefCounted {
	public:
		Ref<ReservationLedger> ledger;
		LocalVector<PackResult> done; // Guarded by ledger->mutex.
	};
	class JobCancellation : public RefCounted {
	public:
		// Set under ledger->mutex to serialize cancellation with mailbox publication;
		// workers sample atomically at stage boundaries, outside that lock.
		std::atomic<bool> cancelled{ false };
		PayloadPageStore *store = nullptr; // Immutable; owner drains tasks before freeing it.
		uint64_t requester = 0;
	};
	struct Job {
		Ref<GaussianSplatHlodRuntimeData> data;
		gs_hlod_runtime::NodeResidency::Ticket ticket;
		gs_hlod_pack::Params params;
		uint32_t atlas_base = 0;
		Ref<Mailbox> mailbox;
		Ref<ReadStats> stats;
		uint64_t read_bytes = 0; // The record's stored bytes (paged source only).
		uint64_t reserved_bytes = 0; // Honest store working set; non-store reads retain conservative read + packed accounting.
		bool counted = false; // begin_read() was called for this job, so end_read() must be.
		PayloadPageStore *store = nullptr; // Borrowed; renderer drains jobs before store shutdown.
		Ref<JobCancellation> cancellation;
		std::unique_ptr<Reservation> reservation;
	};
	struct ReadObservation : GaussianSplatHlodPagedSource::ReadStorageObserver {
		Reservation *reservation = nullptr;
		Ref<JobCancellation> cancellation;
		bool is_cancelled() const override { return cancellation.is_valid() && cancellation->cancelled.load(); }
		void storage_changed(uint64_t p_stored, uint64_t p_decoded, uint64_t p_workspace) override;
	};

	static void run_job(void *p_userdata);
	bool node_reservation_bytes(const GaussianSplatHlodRuntimeData &p_source, uint32_t p_node, bool p_use_store,
			uint64_t &r_bytes, uint64_t &r_output) const;
	Error configure_retained_budget();
	void start_jobs();
	void cancel_outstanding_jobs(); // Teardown only; marks all tokens before draining store requesters.
	void cancel_withdrawn_jobs(); // Ledger mutex serializes cancellation against worker publication.
	void collect_finished_tasks(bool p_wait_all);
	void drain_mailbox();
	void release_staged_edit_data();
	void drain_jobs_and_output(); // Teardown/fail-closed drain, preserving the last served snapshot.
	EditCommitResult fail_commit();
	void fail_shadow_upload();
	bool upload_shadow_ready(RenderingDevice *p_device, const Buffers &p_buffers);
	bool upload_edit_ordinary(RenderingDevice *p_device, const Buffers &p_buffers);
	bool upload_ordinary_ready(RenderingDevice *p_device, const Buffers &p_buffers, bool p_preserve_whole_nodes,
			uint32_t p_ready_index, bool p_fail_without_device);
	void upload_edit_ready(RenderingDevice *p_device, const Buffers &p_buffers);
	void upload_ready(RenderingDevice *p_device, const Buffers &p_buffers);
	void clear_evicted(RenderingDevice *p_device, const Buffers &p_buffers);
	bool upload_available(RenderingDevice *p_device) const;
	uint64_t upload_allowance() const;
	uint64_t upload_remaining() const;
	Error write_atlas(RenderingDevice *p_device, const Buffers &p_buffers, uint32_t p_node, uint64_t p_offset,
			uint64_t p_bytes, const void *p_payload, bool p_shadow);
	Error write_meta(RenderingDevice *p_device, const Buffers &p_buffers, uint32_t p_node, const ChunkMetaGPU &p_meta, bool p_shadow = false);
	Error write_quant(RenderingDevice *p_device, const Buffers &p_buffers, uint32_t p_node, const ChunkQuantizationGPU &p_quant, bool p_shadow = false);
#ifdef TESTS_ENABLED
	void record_write(TestWrite::Kind p_kind, uint32_t p_node, uint64_t p_offset, uint64_t p_bytes,
			bool p_shadow, const ChunkMetaGPU *p_meta = nullptr, const ChunkQuantizationGPU *p_quant = nullptr,
			const void *p_payload = nullptr);
#endif
	void refresh_telemetry();
	void record_upload_slice(PackResult &p_result, uint64_t p_started_nsec, uint64_t p_bytes);
	void record_upload_completion(const PackResult &p_result);

	bool active = false;
	Config cfg;
	uint64_t retained_byte_cap = 0; // Render-thread owned, immutable between source installations.
	// Derivation inputs share retained_byte_cap's render-thread ownership.
	uint32_t normal_retained_concurrency = 0;
	uint64_t normal_retained_job_charge = 0;
	uint64_t normal_retained_output_charge = 0;
	Ref<GaussianSplatHlodRuntimeData> data;
	gs_hlod_runtime::NodeResidency residency;
	Ref<Mailbox> mailbox;
	Ref<ReadStats> read_stats;
	Ref<ReservationLedger> ledger; // Kept after shutdown for cumulative observations.
	PayloadPageStore *page_store = nullptr; // Render-thread field; jobs borrow an immutable copy.
	uint64_t store_generation = 0;
#ifdef TESTS_ENABLED
	Vector<ChunkMetaGPU> test_metadata; // Render-thread CPU device-write sink only.
	Vector<ChunkQuantizationGPU> test_quantization;
	LocalVector<std::unique_ptr<Reservation>> test_leaked_shadow_permits; // Real leak control, reclaimed on teardown.
#endif
	struct Task {
		WorkerThreadPool::TaskID id = WorkerThreadPool::INVALID_TASK_ID;
		Ref<JobCancellation> cancellation; // Co-owned with Job, valid even after run_job deletes Job.
		gs_hlod_runtime::NodeResidency::Ticket ticket; // Render-thread only; immutable after submission.
	};
	LocalVector<Task> tasks; // Render-thread owned; workers never mutate the task list.
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> pending; // Requested, job not started.
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> shadow_pending; // Tickets only; permits are acquired at worker admission.
	LocalVector<PackResult> shadow_ready; // Charged packed/partial output, drained incrementally by update(), never by commit.
	LocalVector<ShadowTransfer> shadow_transferred; // Bounded by kMaxEditedLeaves; contains no CPU payload storage.
	LocalVector<uint32_t> staged_stale_nodes; // Ancestors whose metadata this transaction zeros at publication.
	LocalVector<uint32_t> pending_metadata_clears; // Budgeted edit-era evictions, cleared before any run can be uploaded again.
	uint64_t shadow_transferred_bytes = 0;
	uint32_t shadow_transfer_count = 0; // Current/last edit; observations survive commit/cancel.
	uint64_t staged_publication_bytes = 0;
	uint64_t frame_written_bytes = 0; // Includes publication bytes before update() in this same frame.
	bool frame_edit_uploads = false, in_edit_commit = false;
	bool next_shadow_upload = true, next_shadow_job = true;
	Ref<GaussianSplatHlodRuntimeData> staged_data; // Package F1: the edited snapshot, served after commit.
	LocalVector<PackResult> ready; // Packed, waiting for the per-frame upload budget (still LOADING).
	LocalVector<gs_hlod_runtime::NodeResidency::Ticket> tickets_scratch;
	LocalVector<uint32_t> demand_scratch;
	LocalVector<uint32_t> kept_scratch;
	LocalVector<gs_hlod_runtime::TransitionGroup> transition_scratch;
	// Render-thread confined; workers never read ownership or reconciliation scratch.
	uint64_t transition_owner = 0;
	uint64_t transition_instance = 0;
	bool transition_owner_bound = false;
	LocalVector<uint32_t> prefetch_scratch;
	struct RegisteredView {
		uint64_t key = 0;
		gs_hlod_runtime::CutRequest request;
	};
	LocalVector<RegisteredView> views;
	EditCommitResult last_commit_result = COMMIT_NOTHING;
	EditCommitResult pending_commit_result = COMMIT_NOTHING; // Transfer failure reported at the next begin_frame().
	bool edit_feasible(const Ref<GaussianSplatHlodRuntimeData> &p_edited) const;
	LocalVector<uint32_t> drawn_scratch;
	LocalVector<uint32_t> evicted_scratch;
	uint64_t world_generation = 0;
	uint64_t device_generation = 0;
	uint64_t frame = 0;
	uint64_t frame_eviction_start_total = 0, frame_eviction_truncated = 0;
	RefusingGate frame_admission_refusing_gate = REFUSING_NONE;
	uint64_t gate_counted_frame = UINT64_MAX;
	mutable uint64_t eviction_export_frame = UINT64_MAX, eviction_export_total = UINT64_MAX;
	mutable Array eviction_export; // Cached bounded current-frame report batch, never worker-owned.
	Telemetry telemetry;
};

#endif
