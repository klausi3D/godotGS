#ifndef STREAMING_UPLOAD_PIPELINE_H
#define STREAMING_UPLOAD_PIPELINE_H

#include "gaussian_data.h"
#include "streaming_chunk_payload_source.h"
#include "streaming_latency_histogram.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device.h"
#include "../renderer/gaussian_gpu_layout.h"
#include <atomic>

class GaussianStreamingSystem;
namespace GaussianStreamingTypes {
struct StreamingChunk;
} // namespace GaussianStreamingTypes

class StreamingUploadPipeline {
public:
    struct PackSnapshotScratch {
        LocalVector<Gaussian> gaussian_snapshot;
        LocalVector<Vector3> sh_high_order_snapshot;
    };

    struct UploadCoalescingCandidate {
        uint32_t buffer_slot = UINT32_MAX; // #1088: first page of the chunk's run
        uint32_t page_count = 0;           // #1088: pages in the chunk's run
        uint32_t packed_count = 0;
        uint32_t bytes_uploaded = 0;
    };

    struct UploadCoalescingPlan {
        uint32_t coalesced_job_count = 1;
        uint64_t total_bytes = 0;
    };

    struct PackJob {
        uint32_t asset_id = 0;
        uint32_t chunk_idx = UINT32_MAX;
        uint32_t buffer_slot = UINT32_MAX;
        uint32_t asset_generation = 0;
        uint32_t upload_sequence = 0; // #1177: StreamingChunk::upload_sequence at queue time
        uint64_t enqueue_usec = 0;
        uint32_t chunk_start = 0;
        uint32_t chunk_count = 0;
        bool uses_explicit_source_indices = false;
        LocalVector<uint32_t> source_indices;
        Ref<GaussianData> data_ref;
        Ref<ChunkPayloadSource> payload_source;
    };

    struct PendingChunkUpload {
        uint32_t asset_id = 0;
        uint32_t chunk_idx = UINT32_MAX;
        uint32_t buffer_slot = UINT32_MAX;
        uint32_t asset_generation = 0;
        uint32_t upload_sequence = 0; // #1177: copied from the PackJob
        uint64_t enqueue_usec = 0;
        Vector<PackedGaussian> packed_data;
        // #766 (extends #757): the atlas byte stride the async payload in `packed_data` is
        // actually written at. build_pending_upload_from_pack_job() packs on a worker thread and
        // ALWAYS emits the 128 B PackedGaussian layout (pack_gaussians_range + sizeof(PackedGaussian)
        // sizing/offsets); the 80 B quantized layout only ships through the SYNC path. Because
        // packing and staging are decoupled for async jobs (staged later in finalize_upload_job),
        // the EFFECTIVE stride can flip 128->80 in between (a mixed-DC asset becomes DC-uniform ->
        // _refresh_quantization_dc_compatibility). Recording the true pack-time stride HERE, at pack
        // time, lets finalize_upload_job hand it to _stage_chunk_upload_retirement so the #757
        // retirement guard compares the stride the payload was really written at (not the post-flip
        // stage-time stride) and fail-closes symmetrically for the async path. 0 = not set by the
        // async packer (staging then falls back to the effective stride, matching pre-#766).
        uint64_t packed_stride_bytes = 0;
        uint32_t payload_checksum = 0;
        bool payload_checksum_valid = false;
        // #1178: the payload source could not deliver the chunk (capture_*_chunk_snapshot
        // failed or came back short). packed_data is empty; the main thread records the
        // failure on the chunk instead of silently re-queueing it every frame.
        bool payload_read_failed = false;
        uint64_t queue_allocation_bytes = 0; // Observation only, charged while this output is queued (pack_mutex).
        SHCompressionMetrics metrics;
        uint32_t bytes_uploaded = 0;
    };

    struct PackThreadContext {
        GaussianStreamingSystem *system = nullptr;
        uint32_t thread_index = 0;
        // BV-LAT: most pack jobs one wakeup of this worker may claim (compute_pack_dequeue_batch()).
        uint32_t dequeue_batch = 1;
    };

    // BV-04a (ADR adr-beyond-vram-worlds.md D-5): every counter here is a lock-free atomic in
    // EVERY build flavour. They were stubs without DEV_ENABLED, and the upload counters were
    // further gated on debug frame logging, so `upload_chunks_this_frame` read 0 in optimized
    // builds and fed a constant 0 into the upload-stall predicate and the broad forced-sync
    // guard. Recording is per job, per lock or per upload slice, never per splat.
    //
    // The per-frame fields are drained by exchange_and_reset() once per frame on the main thread
    // (end_frame). The latency histograms are cumulative over the pipeline lifetime.
    struct alignas(64) PackTelemetry {
        SafeNumeric<uint64_t> pack_time_usec_total;
        SafeNumeric<uint64_t> pack_time_usec_max;
        SafeNumeric<uint32_t> pack_jobs_completed;
        SafeNumeric<uint64_t> upload_bytes_total;
        SafeNumeric<uint32_t> upload_chunks_completed;
        // Bytes the chunk payload source delivered into the pack snapshot (gaussians + SH).
        SafeNumeric<uint64_t> read_bytes_total;
        // Bytes produced by a payload decode stage. The uncompressed (PLAIN) chunk route has no
        // decode stage; a GSQ source (BV-C5b) records its decoded snapshot here (BV-K9).
        SafeNumeric<uint64_t> decode_bytes_total;
        SafeNumeric<uint64_t> pack_queue_latency_usec_total;
        SafeNumeric<uint64_t> pack_queue_latency_usec_max;
        SafeNumeric<uint32_t> pack_queue_latency_samples;
        SafeNumeric<uint64_t> upload_queue_latency_usec_total;
        SafeNumeric<uint64_t> upload_queue_latency_usec_max;
        SafeNumeric<uint32_t> upload_queue_latency_samples;
        SafeNumeric<uint64_t> pack_mutex_wait_usec_total;
        SafeNumeric<uint64_t> pack_mutex_wait_usec_max;
        SafeNumeric<uint32_t> pack_mutex_wait_samples;
        SafeNumeric<uint32_t> pack_thread_wake_count;
        SafeNumeric<uint32_t> pack_thread_dequeue_count;
        SafeNumeric<uint32_t> pack_thread_snapshot_count;
        SafeNumeric<uint32_t> pack_thread_enqueue_upload_count;

        // Cumulative stage latency histograms (see StreamingLatencyHistogram):
        //  read   - payload source capture (capture_*_chunk_snapshot) wall time, successful reads,
        //           minus the source's decode share;
        //  decode - payload decode wall time inside the capture (GSQ; none on the uncompressed route);
        //  pack   - pack_gaussians_range wall time;
        //  upload - pack completion to dequeue for the GPU write (upload queue latency).
        StreamingLatencyHistogram read_latency;
        StreamingLatencyHistogram decode_latency;
        StreamingLatencyHistogram pack_latency;
        StreamingLatencyHistogram upload_latency;

        _ALWAYS_INLINE_ void add_pack_time(uint64_t p_usec) {
            pack_time_usec_total.add(p_usec);
            pack_jobs_completed.increment();
            pack_time_usec_max.exchange_if_greater(p_usec);
            pack_latency.record(p_usec);
        }

        _ALWAYS_INLINE_ void add_read(uint64_t p_bytes, uint64_t p_usec) {
            read_bytes_total.add(p_bytes);
            read_latency.record(p_usec);
        }

        _ALWAYS_INLINE_ void add_decode(uint64_t p_bytes, uint64_t p_usec) {
            decode_bytes_total.add(p_bytes);
            decode_latency.record(p_usec);
        }

        _ALWAYS_INLINE_ void add_upload_bytes(uint64_t p_bytes) {
            upload_bytes_total.add(p_bytes);
        }

        _ALWAYS_INLINE_ void add_upload_chunk() {
            upload_chunks_completed.increment();
        }

        _ALWAYS_INLINE_ void add_pack_queue_latency(uint64_t p_usec) {
            pack_queue_latency_usec_total.add(p_usec);
            pack_queue_latency_samples.increment();
            pack_queue_latency_usec_max.exchange_if_greater(p_usec);
        }

        _ALWAYS_INLINE_ void add_upload_queue_latency(uint64_t p_usec) {
            upload_queue_latency_usec_total.add(p_usec);
            upload_queue_latency_samples.increment();
            upload_queue_latency_usec_max.exchange_if_greater(p_usec);
            upload_latency.record(p_usec);
        }

        _ALWAYS_INLINE_ void add_mutex_wait(uint64_t p_usec) {
            pack_mutex_wait_usec_total.add(p_usec);
            pack_mutex_wait_samples.increment();
            if (p_usec > 0) {
                pack_mutex_wait_usec_max.exchange_if_greater(p_usec);
            }
        }

        _ALWAYS_INLINE_ void record_pack_thread_wake() {
            pack_thread_wake_count.increment();
        }

        _ALWAYS_INLINE_ void record_pack_thread_dequeue(uint32_t p_count) {
            pack_thread_dequeue_count.add(p_count);
        }

        _ALWAYS_INLINE_ void record_pack_thread_snapshot() {
            pack_thread_snapshot_count.increment();
        }

        _ALWAYS_INLINE_ void record_pack_thread_enqueue_upload() {
            pack_thread_enqueue_upload_count.increment();
        }

        struct Snapshot {
            uint64_t pack_time_total = 0;
            uint64_t pack_time_max = 0;
            uint32_t pack_jobs = 0;
            uint64_t upload_bytes = 0;
            uint32_t upload_chunks = 0;
            uint64_t read_bytes = 0;
            uint64_t decode_bytes = 0;
            uint64_t pack_queue_lat_total = 0;
            uint64_t pack_queue_lat_max = 0;
            uint32_t pack_queue_lat_samples = 0;
            uint64_t upload_queue_lat_total = 0;
            uint64_t upload_queue_lat_max = 0;
            uint32_t upload_queue_lat_samples = 0;
            uint64_t mutex_wait_total = 0;
            uint64_t mutex_wait_max = 0;
            uint32_t mutex_wait_samples = 0;
            uint32_t thread_wakes = 0;
            uint32_t thread_dequeues = 0;
            uint32_t thread_snapshots = 0;
            uint32_t thread_enqueue_uploads = 0;
        };

        // Drains the per-frame fields. Does not touch the cumulative histograms.
        Snapshot exchange_and_reset();
        Snapshot read_current() const;
    };

    struct UploadBudgetState {
        uint64_t upload_budget = 0;
        uint64_t slice_limit = 0;
        uint32_t completed_chunks = 0;
        uint32_t chunk_limit = 0;
    };

    static constexpr uint32_t QUEUE_COMPACT_MIN_PREFIX = 128;

    bool async_pack_enabled = true;
    // BV-K9b: matches the GLOBAL_DEF defaults (8 and 8) so every job of the default in-flight
    // window has its own worker; the started count is compute_pack_worker_count().
    uint32_t pack_worker_threads = 8;
    uint32_t max_pack_jobs_in_flight = 8;
    uint32_t max_chunk_loads_per_frame = 16;
    uint64_t max_upload_bytes_per_frame = 128 * 1024 * 1024;
    uint64_t max_upload_bytes_per_slice = 16 * 1024 * 1024;
    uint64_t max_upload_bytes_per_second = 0;
    // BV-08c (ADR 3.4 step 4, 7.1): the per-frame upload never exceeds staging ring / frames in
    // flight, whatever max_upload_mb_per_frame asks for. 0 = no staging limit known.
    uint64_t upload_staging_clamp_bytes_per_frame = 0;
    // BV-08c: packed payloads waiting for upload, counted with the jobs still packing. Twice the
    // clamped frame of full-size chunks, so the queue never holds more than two frames of work.
    // 0 = unbounded.
    uint32_t max_ready_upload_jobs = 0;
    uint64_t upload_budget_tokens = 0;
    uint64_t upload_budget_last_update_usec = 0;
    uint32_t queued_chunk_loads_this_frame = 0;
    std::atomic<uint32_t> pack_jobs_in_flight{0};
    PackTelemetry telemetry;

    double last_pack_avg_ms = 0.0;
    double last_pack_max_ms = 0.0;
    uint32_t last_pack_jobs = 0;
    double last_upload_mb = 0.0;
    uint32_t last_upload_chunks = 0;
    // BV-04a: exact per-frame byte counts drained in end_frame, and their cumulative totals
    // (main thread only, never reset, so a reader that samples some frames diffs two totals).
    uint64_t last_upload_bytes = 0;
    uint64_t last_read_bytes = 0;
    uint64_t last_decode_bytes = 0;
    uint64_t upload_bytes_cumulative = 0;
    uint64_t upload_chunks_cumulative = 0;
    uint64_t read_bytes_cumulative = 0;
    uint64_t decode_bytes_cumulative = 0;
    String cap_tier_preset = "custom";
    bool cap_tier_active = false;
    String cap_source_upload_mb_per_frame = "project_default";
    String cap_source_upload_mb_per_slice = "project_default";
    String cap_source_upload_mb_per_second = "project_default";
    uint32_t effective_upload_cap_mb_per_frame = 128;
    uint32_t effective_upload_cap_mb_per_slice = 16;
    uint32_t effective_upload_cap_mb_per_second = 0;
    bool upload_frame_cap_hit_this_frame = false;
    bool upload_slice_cap_hit_this_frame = false;
    bool upload_bandwidth_cap_hit_this_frame = false;
    bool chunk_load_cap_hit_this_frame = false;
    std::atomic<bool> validate_upload_payload_checksums{false};
    bool queue_pressure_active = false;
    String queue_pressure_source = "none";
    String queue_pressure_reason = "none";
    double last_pack_queue_latency_avg_ms = 0.0;
    double last_pack_queue_latency_max_ms = 0.0;
    double last_upload_queue_latency_avg_ms = 0.0;
    double last_upload_queue_latency_max_ms = 0.0;
    double last_pack_mutex_wait_avg_ms = 0.0;
    double last_pack_mutex_wait_max_ms = 0.0;
    uint64_t sync_promoted_pack_jobs_total = 0;
    uint32_t last_sync_promoted_pack_jobs = 0;
    uint32_t last_thread_wakes = 0;
    uint32_t last_thread_dequeues = 0;
    uint32_t last_thread_snapshots = 0;
    uint32_t last_thread_enqueue_uploads = 0;

    Mutex pack_mutex;
    Semaphore pack_semaphore;
    LocalVector<Thread *> pack_threads;
    LocalVector<PackThreadContext> pack_thread_contexts;
    std::atomic<bool> pack_thread_running{false};
    std::atomic<bool> pack_thread_exit{false};
    LocalVector<PackJob> pack_queue;
    uint32_t pack_queue_read_idx = 0;
    LocalVector<PendingChunkUpload *> upload_queue;
    uint32_t upload_queue_read_idx = 0;
    PackSnapshotScratch sync_pack_scratch;
    Vector<PackedGaussian> upload_coalescing_scratch;
    std::atomic<uint32_t> pack_queue_depth_cached{0};
    std::atomic<uint32_t> upload_queue_depth_cached{0};
    // Protected by pack_mutex, sampled at every queue mutation, including worker enqueue.
    uint64_t ready_bytes = 0, ready_bytes_peak = 0;
    uint32_t ready_jobs_peak = 0;
    uint64_t admission_waits = 0; // Owner thread, like queue admission itself.
    Dictionary get_queue_observations();
    void reset_queue_observations(); // Owner thread; established pack mutex covers queue samples.

    void load_streaming_tuning_config_from_project_settings(GaussianStreamingSystem &system);
    void start_pack_threads(GaussianStreamingSystem &system);
    void stop_pack_threads(GaussianStreamingSystem &system);
    static void pack_thread_entry(void *p_userdata);
    void pack_thread_func(GaussianStreamingSystem &system, uint32_t p_thread_index, uint32_t p_dequeue_batch);
    // #1176: p_allow_visible_eviction = false (predictive prefetch) never takes the
    // visible-eviction fallback when the atlas has no run for the chunk; the load is skipped.
    bool queue_chunk_load(GaussianStreamingSystem &system, uint32_t asset_id, uint32_t chunk_idx,
            bool p_allow_visible_eviction = true);
    void process_upload_queue(GaussianStreamingSystem &system);
    void clear_pending_uploads(GaussianStreamingSystem &system);
    void cancel_asset_jobs(GaussianStreamingSystem &system, uint32_t asset_id);
    void cancel_chunk_jobs(GaussianStreamingSystem &system, uint32_t asset_id, uint32_t chunk_idx, uint32_t buffer_slot);
    bool has_pending_uploads();
    uint32_t get_pack_queue_depth_cached() const;
    uint32_t get_upload_queue_depth_cached() const;
    // BV-08c: pack_jobs_in_flight as admission must read it. Reports saturation while the ready
    // queue is at max_ready_upload_jobs, so a scan stops before it evicts for a load that could
    // not be queued. A max_pack_jobs_in_flight of 0 asks for no backpressure and gets none.
    uint32_t get_pack_jobs_in_flight_for_admission() const;
    static uint64_t compute_staging_upload_clamp_bytes(uint32_t p_staging_max_mb, uint32_t p_frames_in_flight);
    static uint32_t compute_max_ready_upload_jobs(uint64_t p_frame_upload_limit_bytes, uint32_t p_max_pack_jobs_in_flight);
    // BV-LAT: most jobs one pack-worker wakeup may claim: its fair share of the in-flight window
    // (max_pack_jobs_in_flight / workers, at least 1, at most PACK_DEQUEUE_BATCH_MAX). A fixed batch
    // of 4 with 4 jobs in flight let the first worker take every job and run them serially while
    // the others slept: beyond-VRAM refill ran at one worker's rate (~32 chunks/s on tier A).
    static constexpr uint32_t PACK_DEQUEUE_BATCH_MAX = 4;
    static uint32_t compute_pack_dequeue_batch(uint32_t p_worker_threads, uint32_t p_max_pack_jobs_in_flight);
    // BV-K9b: pack workers actually started for the pack_worker_threads setting: never more than
    // max(2, logical CPUs / 2), so the decode/pack burst of a refill leaves at least half of the
    // CPU to the game's main, render and job threads (16 logical CPUs -> 8, 8 -> 4, 4 -> 2).
    // 0 stays 0 (async packing off).
    static uint32_t compute_pack_worker_count(uint32_t p_setting, uint32_t p_logical_cpus);
    uint64_t get_frame_upload_limit_bytes() const;
    void get_pending_queue_depths_cached(uint32_t &r_pack_queue_depth, uint32_t &r_upload_queue_depth) const;
#if defined(TESTS_ENABLED)
    uint64_t _test_frame_upload_budget() { return prepare_upload_budget_state().upload_budget; }
    void _test_set_async_pack_queue_owner(bool p_has_owner) {
        async_pack_enabled = true;
        pack_thread_running.store(p_has_owner, std::memory_order_release);
    }
    void _test_enqueue_dummy_pack_job() {
        MutexLock lock(pack_mutex);
        pack_queue.push_back(PackJob());
        sync_cached_queue_depths_locked();
    }
    uint32_t _test_promote_pack_jobs_sync(uint32_t p_max_jobs) { return promote_pack_jobs_sync(p_max_jobs); }
    bool _test_has_async_pack_queue_owner() const { return has_async_pack_queue_owner(); }
    uint32_t _test_get_sync_snapshot_gaussian_size() const { return sync_pack_scratch.gaussian_snapshot.size(); }
    uint32_t _test_get_sync_snapshot_gaussian_capacity() const { return sync_pack_scratch.gaussian_snapshot.get_capacity(); }
    void _test_set_upload_payload_checksum_validation_enabled(bool p_enabled) { validate_upload_payload_checksums.store(p_enabled, std::memory_order_release); }
    bool _test_is_upload_payload_checksum_validation_enabled() const { return validate_upload_payload_checksums.load(std::memory_order_acquire); }
    static void _test_reset_payload_checksum_hash_calls();
    static uint64_t _test_get_payload_checksum_hash_calls();
    static UploadCoalescingPlan _test_plan_coalesced_upload_batch(
            const LocalVector<UploadCoalescingCandidate> &p_candidates, uint64_t p_byte_limit) {
        return _plan_coalesced_upload_batch(p_candidates, p_byte_limit);
    }
    // #1177: drive the pack/upload lifecycle without worker threads or a device.
    // _test_pop_pack_job() is a worker's dequeue; _test_complete_pack_job() is what the
    // worker then does with the job (pack_thread_func: build, enqueue, release the in-flight
    // slot). _test_finalize_completed_uploads_without_gpu_write() is process_upload_queue()
    // minus the buffer write: every completed upload goes through the same
    // accept_completed_upload() and finalize_completed_upload() steps. Returns the number of
    // retirement tickets staged.
    bool _test_pop_pack_job(PackJob &r_job) { return pop_pack_job(r_job); }
    void _test_complete_pack_job(const PackJob &p_job);
    uint32_t _test_complete_pack_jobs();
    uint32_t _test_finalize_completed_uploads_without_gpu_write(GaussianStreamingSystem &system);
#endif

private:
    bool has_async_pack_queue_owner() const;
    bool pop_pack_job(PackJob &r_job);
    PendingChunkUpload *build_pending_upload_from_pack_job(const PackJob &p_job, PackSnapshotScratch &r_scratch);
    // Main-thread validation of one completed upload before any buffer write (chunk, slot,
    // generation, #1177 upload sequence, payload size, checksum, #766 stride). On false the job
    // was dropped (deleted) and the chunk handled; on true r_chunk/r_total_bytes describe it.
    bool accept_completed_upload(GaussianStreamingSystem &system, PendingChunkUpload *p_job,
            GaussianStreamingTypes::StreamingChunk *&r_chunk, uint64_t &r_total_bytes,
            bool p_checksum_validation_enabled);
    // After the payload is written: stage the retirement ticket. Always consumes p_job.
    // Returns whether a ticket was staged.
    bool finalize_completed_upload(GaussianStreamingSystem &system, PendingChunkUpload *p_job,
            GaussianStreamingTypes::StreamingChunk &r_chunk, RenderingDevice *p_submission_rd);
    void enqueue_upload_job(PendingChunkUpload *p_job);
    uint32_t promote_pack_jobs_sync(uint32_t p_max_jobs);
    bool pop_upload_job(PendingChunkUpload *&job);
    uint32_t peek_upload_jobs(uint32_t p_max_jobs, LocalVector<PendingChunkUpload *> &r_jobs);
    uint32_t consume_upload_jobs(uint32_t p_count);
    UploadBudgetState prepare_upload_budget_state();
    uint32_t get_pack_queue_depth_unsafe() const;
    uint32_t get_upload_queue_depth_unsafe() const;
    static UploadCoalescingPlan _plan_coalesced_upload_batch(
            const LocalVector<UploadCoalescingCandidate> &p_candidates, uint64_t p_byte_limit);
    void compact_queues_locked();
    void sync_cached_queue_depths_locked();
    void record_pack_mutex_wait(uint64_t wait_start_usec);
    void record_upload_queue_latency(uint64_t enqueue_usec);
    void requeue_upload_job(PendingChunkUpload *job);
};

#endif // STREAMING_UPLOAD_PIPELINE_H
