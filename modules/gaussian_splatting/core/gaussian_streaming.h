#ifndef GAUSSIAN_STREAMING_H
#define GAUSSIAN_STREAMING_H

#include "gaussian_data.h"
#include "gaussian_splat_manager.h"
#include "core/error/error_list.h"
#include "core/math/plane.h"
#include "core/math/transform_3d.h"
#include "core/string/string_name.h"
#include "servers/rendering/rendering_device.h"

class GaussianMemoryStream;
class OS;

// Extracted subsystem headers (ISSUE-006 split)
#include "streaming_config_overrides.h"
#include "residency_budget_controller.h"
#include "streaming_quantization.h"
#include "streaming_vram_regulator.h"
#include "streaming_asset_types.h"
#include "streaming_runtime_state.h"
#include "streaming_visibility_controller.h"
#include "streaming_atlas.h"
#include "streaming_global_atlas_registry.h"
#include "streaming_eviction_controller.h"
#include "streaming_upload_pipeline.h"

// GPU memory streaming system with ring buffer for large datasets
class GaussianStreamingSystem : public RefCounted {
    GDCLASS(GaussianStreamingSystem, RefCounted);
    friend class StreamingUploadPipeline;
    friend class StreamingEvictionController;
    friend class StreamingVisibilityController;
    friend class StreamingGlobalAtlasRegistry;

public:
    static constexpr uint32_t CHUNK_SIZE = 65536;  // 64K splats per chunk
    static constexpr uint32_t MAX_CHUNKS_IN_VRAM = 32;  // ~2GB @ 64K splats/chunk
    static constexpr uint32_t RING_BUFFER_FRAMES = 3;  // Triple buffering

    using ConfigOverrides = GaussianStreamingTypes::ConfigOverrides;
    using ChunkLayoutHint = GaussianStreamingTypes::ChunkLayoutHint;

private:
    RenderingDevice *primary_device_override = nullptr;
    static constexpr uint32_t PRIMARY_ASSET_ID = 0;
    static constexpr uint32_t INVALID_ASSET_ID = UINT32_MAX;
    static constexpr uint32_t MAX_REQUESTED_LOD = GS_MAX_ASSET_LODS - 1;

    using StreamingChunk = GaussianStreamingTypes::StreamingChunk;
    using RequestedChunkState = GaussianStreamingTypes::RequestedChunkState;
    using AtlasAssetState = GaussianStreamingTypes::AtlasAssetState;
    using FrameData = GaussianStreamingTypes::FrameData;

    using EvictionResult = StreamingEvictionController::EvictionResult;

    using ChunkCullingStats = StreamingVisibilityController::ChunkCullingStats;
    using CameraVelocityTracker = StreamingVisibilityController::CameraVelocityTracker;
    using VisibilityState = StreamingVisibilityController;
    using ZeroVisibleRecoveryMode = StreamingVisibilityController::ZeroVisibleRecoveryMode;
    using ZeroVisibleRecoveryState = StreamingVisibilityController::ZeroVisibleRecoveryState;

    using BudgetState = GaussianStreamingTypes::BudgetState;
    using StreamingUploadCompletionMode = GaussianStreamingTypes::StreamingUploadCompletionMode;

    // Ring buffer for frame synchronization
    FrameData frame_data[RING_BUFFER_FRAMES];
    uint32_t current_frame_idx = 0;
    uint64_t total_frame_count = 0;
    bool debug_logging_enabled = false;
    int debug_frame_log_frequency = 0;

    VisibilityState visibility;
    StreamingEvictionController eviction_controller;
    StreamingUploadPipeline upload_pipeline;
    BudgetState budget;

    using AssetRegistryState = GaussianStreamingTypes::AssetRegistryState;

    AssetRegistryState asset_registry;

    GaussianAtlasAllocator atlas_allocator;
    StreamingGlobalAtlasRegistry global_atlas_registry;

    // Chunk management
    LocalVector<StreamingChunk> chunks;
    uint32_t total_splat_count = 0;

    // GPU resources
    RID persistent_buffer;  // Main GPU buffer (persistent mapped)
    uint32_t persistent_buffer_size = 0;

    // Persistent buffer sizing (Phase 3 right-sized init + growth path)
    uint32_t streaming_initial_capacity = 0;     // Slots allocated at init (asset-scaled).
    uint32_t streaming_current_capacity = 0;     // Current allocator/buffer slot count.
    uint32_t streaming_max_capacity = 0;         // Growth ceiling (effective_max_chunks at init).
    uint32_t streaming_grow_count = 0;           // Number of successful grow operations.

    // Source data reference
    Ref<::GaussianData> source_data;

    // Compression tracking
    SHCompressionMetrics total_sh_metrics;

    // Configuration reload (triggered on ProjectSettings changes)
    static constexpr uint64_t BYTES_PER_MB = 1024u * 1024u;
    static constexpr float STREAMING_LOAD_DISTANCE_BASE = 100000.0f;
    static constexpr float ESTIMATED_FRAME_DELTA_60FPS = 1.0f / 60.0f;
    static constexpr double USEC_PER_MS = 1000.0;
    uint64_t last_config_reload_frame = 0;
    bool config_dirty = true;  // Force initial load
    bool project_settings_connected = false;
    bool streaming_initialized = false;

    using SchedulerState = GaussianStreamingTypes::SchedulerState;
    using DiagnosticsState = GaussianStreamingTypes::DiagnosticsState;
    using PrimaryChunkLayoutMetrics = GaussianStreamingTypes::PrimaryChunkLayoutMetrics;

    SchedulerState scheduler;
    DiagnosticsState diagnostics;
    PrimaryChunkLayoutMetrics primary_chunk_layout_metrics;

    uint64_t last_streaming_update_usec = 0;
    float last_streaming_frame_delta_seconds = ESTIMATED_FRAME_DELTA_60FPS;
    bool per_frame_counters_reset_for_streaming_update = false;
    bool effective_max_guard_warning_emitted = false;
    uint32_t effective_max_guard_warning_regulated = 0;
    uint32_t effective_max_guard_warning_capacity = 0;
    bool runtime_capacity_guard_logged = false;
    uint32_t runtime_capacity_guard_effective_max = UINT32_MAX;
    uint32_t runtime_capacity_guard_runtime_capacity = UINT32_MAX;
    bool runtime_capacity_guard_buffer_valid = true;
    bool runtime_capacity_guard_initialized = true;
    // PR #352: warned-once latch for failed-init cascades. Set on the first
    // ERR_PRINT emitted by initialize()/update_streaming()/_load_visible_chunks()
    // when the streaming runtime is unavailable, then suppresses identical
    // ERRs on every subsequent frame so a headless-no-device run does not
    // emit one ERR per frame (which previously snowballed into 602 SEH
    // CrashHandlerException dumps in run_module_tests.py). Cleared inside
    // initialize()/initialize_empty() so a successful re-init re-arms the
    // warning for a subsequent failure.
    bool failed_init_warning_emitted = false;
    uint64_t invalid_camera_input_events = 0;
    uint64_t last_invalid_camera_log_frame = UINT64_MAX;
    uint32_t invalid_camera_log_interval_frames = 120;
    ConfigOverrides config_overrides;
    bool config_overrides_active = false;

    struct PendingUploadRetirement {
        uint64_t ticket_id = 0;
        uint32_t asset_id = 0;
        uint32_t chunk_idx = UINT32_MAX;
        uint32_t buffer_slot = UINT32_MAX;
        uint32_t asset_generation = 0;
        uint64_t submit_frame = 0;
        uint64_t retire_frame = 0;
        uint64_t bytes = 0;
        // #757 (extends #513): the EFFECTIVE atlas byte stride (_atlas_gaussian_stride_bytes(),
        // 80 B quantized vs 144 B raw) that this chunk was packed/uploaded/staged at. The effective
        // stride can flip while the ticket sits in the frame-delay queue (a mixed-DC asset
        // registration toggles per_chunk_quantization_dc_compatible via
        // _refresh_quantization_dc_compatibility). #513 only evicts *resident* chunks, so an
        // upload_pending chunk survives the flip; _process_upload_retirements() compares this
        // against the current stride and fail-closes (drops the ticket) on a mismatch instead of
        // reinterpreting the old-stride payload at the new stride.
        uint64_t packed_stride_bytes = 0;
        uint8_t completion_mode = GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_NONE;
        SHCompressionMetrics metrics;
    };
    LocalVector<PendingUploadRetirement> pending_upload_retirements;
    uint64_t next_upload_ticket_id = 1;
    uint64_t last_completed_upload_ticket_id = 0;
    String last_upload_completion_mode = "none";

    // Streaming orchestration hooks
    Ref<GaussianMemoryStream> memory_stream_proxy;
    Dictionary analytics_snapshot;
    RenderingDevice *last_upload_device = nullptr;

    // Per-chunk quantization (Unity technique for 4x compression)
    bool per_chunk_quantization_enabled = false;
    bool per_chunk_quantization_dc_compatible = true;
    RID quantization_buffer;  // GPU buffer for ChunkQuantizationGPU data
    uint32_t quantization_buffer_size = 0;
    LocalVector<ChunkQuantizationGPU> quantization_gpu_data;  // CPU-side copy for upload
    bool quantization_release_deferred_logged = false;
    uint32_t quantization_position_bits = 16;
    uint32_t quantization_scale_bits = 12;
    bool quantization_scales_enabled = false;
    bool quantization_cpu_cache_valid = false;

    // Global atlas metadata (instance pipeline)
    bool quantization_dirty = false;
#if defined(TESTS_ENABLED)
    bool test_force_next_chunk_upload_failure = false;
#endif

    void _connect_project_settings();
    void _on_project_settings_changed();

protected:
    static void _bind_methods();

public:
    GaussianStreamingSystem();
    ~GaussianStreamingSystem();

    // Initialize streaming system with source data
    void initialize(Ref<::GaussianData> p_data);
    void initialize_with_device(Ref<::GaussianData> p_data, RenderingDevice *p_device);
    void initialize_empty(RenderingDevice *p_device);
    void update_primary_asset_data(Ref<::GaussianData> p_data);
    void attach_memory_stream(const Ref<GaussianMemoryStream> &p_stream);
    void set_config_overrides(const ConfigOverrides &p_overrides);
    void clear_config_overrides();
    void set_io_chunk_layout_hints(const Vector<ChunkLayoutHint> &p_hints, uint32_t p_asset_id = INVALID_ASSET_ID);
    void set_primary_chunk_layout(const Vector<ChunkLayoutHint> &p_hints, const Vector<uint32_t> &p_source_indices);
    bool has_config_overrides() const { return config_overrides_active; }
    const ConfigOverrides &get_config_overrides() const { return config_overrides; }

    // Update visibility and load/unload chunks based on camera
    // Note: camera_transform is in the same space as the Gaussian data (camera-to-world or camera-to-local).
    void update_streaming(const Transform3D &camera_transform, const Projection &projection, float frame_delta_seconds = -1.0f);

    // Global atlas residency requests (instance pipeline).
    void begin_residency_requests();
    Error request_chunk_residency(uint32_t asset_id, uint32_t chunk_id, uint32_t lod_level);
    Error request_asset_residency(uint32_t asset_id, uint32_t lod_level);
    void finalize_residency_requests();
    // `requested` is current-generation only. The request state/result fields
    // expose the latest recorded explicit-request outcome, and
    // `request_status_current_generation` tells callers whether that outcome
    // still belongs to the active request generation.
    Dictionary get_residency_request_status(uint32_t asset_id, uint32_t chunk_id) const;

    // Get GPU buffer for current frame's visible splats
    RID get_frame_buffer() const;
    uint32_t get_visible_count() const;
    LocalVector<Gaussian> get_visible_gaussians() const;
    LocalVector<uint32_t> get_visible_indices() const;

    // Frame management
    void begin_frame();
    void end_frame();

    // Memory management
    uint64_t get_vram_usage() const;
    uint32_t get_loaded_chunks() const { return budget.loaded_chunks_count; }
    uint32_t get_pending_pack_jobs();
    uint32_t get_pending_upload_jobs();
    uint32_t get_pending_upload_retirement_slots() const;
    uint64_t get_pending_upload_retirement_bytes() const;
    SHCompressionMetrics get_total_sh_metrics() const { return total_sh_metrics; }
    Dictionary get_task_debug_state() const;
    Dictionary get_streaming_analytics() const;
    bool is_runtime_ready(String *r_reason = nullptr) const;
    // PR #352 helper: cheap probe used by tests/macros to skip work when no
    // streaming runtime is loadable. Wraps is_runtime_ready() so the call
    // site does not need to construct a reason string.
    bool is_streaming_capable() const { return is_runtime_ready(nullptr); }
    bool is_runtime_capacity_zero() const;
    bool is_persistent_buffer_invalid() const;
    uint32_t get_registered_asset_count_with_data() const;

    // Chunk frustum culling configuration
    void set_chunk_frustum_culling_enabled(bool p_enabled) { visibility.chunk_frustum_culling_enabled = p_enabled; }
    bool is_chunk_frustum_culling_enabled() const { return visibility.chunk_frustum_culling_enabled; }
    void set_chunk_frustum_padding(float p_padding) { visibility.chunk_frustum_padding = MAX(1.0f, p_padding); }
    float get_chunk_frustum_padding() const { return visibility.chunk_frustum_padding; }
    void set_chunk_radius_multiplier(float p_multiplier);
    float get_chunk_radius_multiplier() const { return visibility.chunk_radius_multiplier; }

    // Debug statistics for chunk culling
    Dictionary get_chunk_culling_stats() const;

    // VRAM budget regulation
    Ref<VRAMBudgetRegulator> get_vram_regulator() const { return budget.vram_regulator; }
    Dictionary get_vram_debug_stats() const;
    bool is_vram_budget_warning_active() const;
    uint32_t get_effective_max_chunks() const;
    // Returns the regulated max chunks (VRAM-budget cap), NOT clamped by the
    // current runtime/buffer capacity. Use this when computing a growth
    // ceiling for the persistent buffer; passing the runtime-clamped value
    // from get_effective_max_chunks() would silently prevent expansion.
    uint32_t get_regulated_max_chunks() const;
    uint32_t get_max_chunk_count_per_asset() const { return global_atlas_registry.get_max_chunk_count_per_asset(); }
    uint32_t get_max_chunk_splats() const { return global_atlas_registry.get_max_chunk_splats(); }

    float get_global_lod_blend_factor() const { return visibility.current_lod_blend_factor; }
    void set_lod_blend_enabled(bool p_enabled) { visibility.lod_blend_config.blend_enabled = p_enabled; }
    bool is_lod_blend_enabled() const { return visibility.lod_blend_config.blend_enabled; }
    void set_lod_blend_distance(float p_distance) { visibility.lod_blend_config.blend_distance = MAX(0.1f, p_distance); }
    float get_lod_blend_distance() const { return visibility.lod_blend_config.blend_distance; }
    float get_lod_hysteresis_zone() const { return visibility.lod_blend_config.hysteresis_zone; }
    const LODConfig &get_lod_config() const { return _get_lod_config(); }

    // SH band level and visibility change tracking
    int get_global_sh_band_level() const { return visibility.global_sh_band_level; }
    void set_global_sh_band_level(int p_level) { visibility.global_sh_band_level = CLAMP(p_level, 0, 3); }
    float get_visible_count_change_ratio() const;

    // Streaming change tracking
    uint32_t get_chunks_loaded_this_frame() const { return budget.chunks_loaded_this_frame; }
    uint32_t get_chunks_evicted_this_frame() const { return eviction_controller.get_chunks_evicted_this_frame(); }
    uint32_t get_visible_chunks_evicted_this_frame() const { return eviction_controller.get_visible_chunks_evicted_this_frame(); }
    float get_visible_chunk_change_ratio() const;
    float get_effective_count_change_ratio() const;
    uint32_t get_buffer_capacity_splats() const;
    void build_visible_indices(LocalVector<uint32_t> &out_buffer_indices, LocalVector<uint32_t> &out_source_indices) const;

    // Buffer index mapping for streaming
    bool map_buffer_index_to_source(uint32_t buffer_index, uint32_t &out_source_index) const;
    bool is_asset_registered(uint32_t asset_id) const { return asset_registry.atlas_assets.has(asset_id); }

    // Per-chunk quantization (Unity technique for 4x compression)
    bool is_per_chunk_quantization_enabled() const { return per_chunk_quantization_enabled && per_chunk_quantization_dc_compatible; }
    RID get_quantization_buffer() const { return quantization_buffer; }
    uint32_t get_quantization_position_bits() const { return quantization_position_bits; }
    uint32_t get_quantization_scale_bits() const { return quantization_scale_bits; }
    bool is_quantization_scales_enabled() const { return quantization_scales_enabled; }
    Dictionary get_quantization_stats() const;

    // Multi-asset registration (scaffolding).
    void register_asset(uint32_t asset_id, const Ref<GaussianData> &p_data);
    void unregister_asset(uint32_t asset_id);
    void set_chunk_payload_source(uint32_t asset_id, const Ref<ChunkPayloadSource> &p_source);
    void detach_source_data(uint32_t asset_id);
    bool has_asset(uint32_t asset_id) const { return asset_registry.atlas_assets.has(asset_id); }
    uint32_t get_dense_asset_id(uint32_t asset_id) const;
    bool remap_instance_asset_ids(LocalVector<InstanceDataGPU> &p_instances, bool p_warn_on_missing = true) const;

    // Global atlas (instance pipeline) accessors
    const GlobalAtlasState &get_global_atlas_state() const { return global_atlas_registry.get_global_atlas_state(); }
    RID get_atlas_gaussian_buffer() const { return global_atlas_registry.get_global_atlas_state().atlas_gaussian_buffer; }
    uint32_t get_atlas_gaussian_count() const { return global_atlas_registry.get_global_atlas_state().atlas_gaussian_count; }
    RID get_asset_meta_buffer() const { return global_atlas_registry.get_global_atlas_state().asset_meta_buffer; }
    RID get_chunk_meta_buffer() const { return global_atlas_registry.get_global_atlas_state().chunk_meta_buffer; }
    RID get_asset_chunk_index_buffer() const { return global_atlas_registry.get_global_atlas_state().asset_chunk_index_buffer; }
    RID get_atlas_quantization_buffer() const { return global_atlas_registry.get_global_atlas_state().quantization_buffer; }
    uint64_t get_atlas_generation() const { return global_atlas_registry.get_atlas_generation(); }
#if defined(TESTS_ENABLED)
    void _test_sync_global_atlas_state(RenderingDevice *p_rd) { global_atlas_registry.sync_to_gpu(*this, p_rd); }
    void _test_build_global_atlas_cpu_state() { global_atlas_registry.build_cpu_state(*this); }
    StreamingGlobalAtlasRegistry::ChunkMetaUploadPlan _test_plan_chunk_meta_sync() {
        return global_atlas_registry._test_plan_chunk_meta_sync(*this);
    }
    StreamingGlobalAtlasRegistry::SyncDiagnostics _test_get_atlas_sync_diagnostics() const {
        return global_atlas_registry.get_last_sync_diagnostics();
    }
    void _test_clear_atlas_cpu_dirty_state() { global_atlas_registry._test_clear_cpu_dirty_state(); }
    void _test_mark_chunk_meta_dirty(uint32_t asset_id, uint32_t chunk_idx) {
        global_atlas_registry.mark_chunk_meta_dirty(*this, asset_id, chunk_idx);
    }
    void _test_force_next_chunk_upload_failure() { test_force_next_chunk_upload_failure = true; }

    // Test-only access to atlas-state internals. Replaced the
    // `#define private public` macro that test_gpu_streaming.cpp used to reach
    // these private members directly; the macro leaks into transitively-included
    // core templates and triggers GCC ODR errors.
    AtlasAssetState *_test_get_asset_state(uint32_t p_asset_id) { return _get_asset_state(p_asset_id); }
    LocalVector<StreamingChunk> &_test_get_asset_chunks(AtlasAssetState &p_asset) { return _get_asset_chunks(p_asset); }
    LocalVector<StreamingChunk> &_test_get_primary_chunks() { return chunks; }
    StreamingVisibilityController &_test_get_visibility_controller() { return visibility; }
    uint64_t _test_make_chunk_key(uint32_t p_asset_id, uint32_t p_chunk_id) const {
        return _make_chunk_key(p_asset_id, p_chunk_id);
    }
    GaussianAtlasAllocator &_test_atlas_allocator() { return atlas_allocator; }
    bool _test_begin_chunk_upload(uint32_t p_asset_id, uint32_t p_chunk_idx, StreamingChunk &p_chunk, uint32_t p_buffer_slot) {
        return _begin_chunk_upload(p_asset_id, p_chunk_idx, p_chunk, p_buffer_slot);
    }
    uint32_t _test_get_reserved_chunk_count() const { return _get_reserved_chunk_count(); }
    // Persistent storage-buffer allocation size in bytes. Test-only window onto the
    // single largest streaming VRAM allocation so the VRAM-accounting tests can
    // assert get_vram_usage() folds it in, without the `#define private public`
    // hack that breaks the GCC ODR build.
    uint32_t _test_get_persistent_buffer_size() const { return persistent_buffer_size; }
    // Q80A/Q80B: the atlas byte stride must track the EFFECTIVE quantization state
    // (enabled && dc_compatible), so a mixed-DC fallback packs 144 B, not 80 B.
    uint64_t _test_atlas_gaussian_stride_bytes() const { return _atlas_gaussian_stride_bytes(); }
    void _test_set_quantization_state(bool p_enabled, bool p_dc_compatible) {
        per_chunk_quantization_enabled = p_enabled;
        per_chunk_quantization_dc_compatible = p_dc_compatible;
    }
    uint32_t _test_get_retired_upload_slots_this_frame() const { return budget.retired_upload_slots_this_frame; }
    uint64_t _test_get_retired_upload_bytes_this_frame() const { return budget.retired_upload_bytes_this_frame; }
    uint64_t _test_get_failed_upload_retirements() const { return budget.failed_upload_retirements; }
    // #757: pending retirements dropped fail-closed because the effective atlas stride flipped
    // between staging and retirement (see BudgetState::stride_flip_dropped_upload_retirements).
    uint64_t _test_get_stride_flip_dropped_upload_retirements() const { return budget.stride_flip_dropped_upload_retirements; }
    bool _test_stage_chunk_upload_retirement(uint32_t p_asset_id, uint32_t p_chunk_idx,
            StreamingChunk &p_chunk, uint32_t p_buffer_slot, uint64_t p_bytes,
            uint32_t p_retire_after_frames, StreamingUploadCompletionMode p_mode) {
        return _stage_chunk_upload_retirement(p_asset_id, p_chunk_idx, p_chunk, p_buffer_slot,
                p_bytes, SHCompressionMetrics(), nullptr, p_retire_after_frames, p_mode);
    }
    void _test_process_upload_retirements() { _process_upload_retirements(); }
    void _test_rollback_pending_chunk(uint32_t p_asset_id, uint32_t p_chunk_idx, StreamingChunk &p_chunk, bool p_release_slot) {
        _rollback_pending_chunk(p_asset_id, p_chunk_idx, p_chunk, p_release_slot);
    }
    bool _test_enqueue_sync_fallback_chunk_load(uint32_t p_asset_id, uint32_t p_chunk_idx, bool p_prioritize = false) {
        return _enqueue_sync_fallback_chunk_load(p_asset_id, p_chunk_idx, p_prioritize);
    }
    uint32_t _test_drain_sync_fallback_chunk_loads(uint32_t p_effective_max, uint32_t &r_evictions_left, bool &r_eviction_blocked) {
        return _drain_sync_fallback_chunk_loads(p_effective_max, r_evictions_left, r_eviction_blocked);
    }
    void _test_evict_unrequested_chunks(uint32_t p_asset_id, AtlasAssetState &p_asset, LocalVector<StreamingChunk> &p_asset_chunks) {
        _evict_unrequested_chunks(p_asset_id, p_asset, p_asset_chunks);
    }
    void _test_register_primary_asset_for_chunks() { _register_primary_asset(); }
	void _test_mark_chunk_loaded_for_eviction(uint32_t p_asset_id, uint32_t p_chunk_id, bool p_visible,
			uint64_t p_last_loaded_frame, uint64_t p_last_used_frame, float p_distance);
	void _test_reset_atlas_allocator(uint32_t p_capacity) { atlas_allocator.reset(p_capacity); }
	EvictionResult _test_evict_least_recently_used(bool p_allow_visible_eviction) {
		return _evict_least_recently_used(p_allow_visible_eviction);
	}
    EvictionResult _test_evict_non_primary_lru() { return _evict_non_primary_lru(); }
    uint32_t _test_get_primary_eviction_scan_count() const { return scheduler.last_primary_eviction_scan_count; }
    uint32_t _test_get_primary_eviction_candidate_count() const { return scheduler.last_primary_eviction_candidate_count; }
    uint32_t _test_get_non_primary_eviction_scan_count() const { return scheduler.last_non_primary_scan_count; }
    uint32_t _test_get_non_primary_eviction_candidate_count() const { return scheduler.last_non_primary_eviction_candidate_count; }
    // Field-level accessors for the global atlas registry. Returning the
    // registry by reference would expose private fields the registry's
    // friendship with this class doesn't grant onward — these forward only
    // the specific fields tests need, through this class's own friendship.
    bool _test_get_atlas_registry_dirty() const { return global_atlas_registry.asset_registry_dirty; }
    bool _test_get_chunk_meta_dirty_all() const { return global_atlas_registry.chunk_meta_dirty_all; }
    const LocalVector<uint32_t> &_test_get_chunk_meta_dirty_indices() const { return global_atlas_registry.chunk_meta_dirty_indices; }
    RID _test_get_registry_asset_meta_buffer() const { return global_atlas_registry.asset_meta_buffer; }
    RID _test_get_registry_chunk_meta_buffer() const { return global_atlas_registry.chunk_meta_buffer; }
    RID _test_get_registry_asset_chunk_index_buffer() const { return global_atlas_registry.asset_chunk_index_buffer; }
#endif

    // Distance-based LOD (Octree-GS) - chunk-level LOD selection and reduction
    Dictionary get_lod_debug_stats() const;
    uint32_t get_effective_splat_count() const;  // Total splats after LOD reduction

    // Internal upload scheduler access (not user-facing API).
    StreamingUploadPipeline &_internal_get_upload_pipeline() { return upload_pipeline; }

private:
    bool _create_chunks();
    void _build_chunks_for_data(const Ref<GaussianData> &p_data, LocalVector<StreamingChunk> &out_chunks);
    bool _populate_chunks_from_bake(const Ref<GaussianData> &p_data, LocalVector<StreamingChunk> &out_chunks, bool p_build_primary_spatial);
    void _build_chunks_for_payload_source(const Ref<ChunkPayloadSource> &p_source, LocalVector<StreamingChunk> &out_chunks);
    bool _build_primary_chunks_from_layout_hints(const Ref<GaussianData> &p_data, const Vector<ChunkLayoutHint> &p_hints,
            const LocalVector<uint32_t> &p_source_indices, LocalVector<StreamingChunk> &out_chunks);
    bool _build_chunks_from_layout_hints(const Ref<GaussianData> &p_data, const Vector<ChunkLayoutHint> &p_hints, LocalVector<StreamingChunk> &out_chunks);
    bool _resolve_primary_chunk_source_index(const StreamingChunk &chunk, uint32_t p_offset_in_chunk, uint32_t &r_source_index) const;
    void _refresh_primary_chunk_layout_metrics();
    void _register_primary_asset();
    void _refresh_quantization_dc_compatibility();
    // #513 fail-closed guard: evict every resident chunk before an effective
    // quantization-stride flip so nothing is ever read/accounted at a mismatched stride.
    void _evict_all_resident_chunks_for_stride_change();
    uint32_t _advance_asset_generation(uint32_t asset_id);
    uint32_t _alloc_dense_id(uint32_t asset_id);
    void _release_dense_id(uint32_t dense_id);
    uint32_t _get_dense_generation(uint32_t dense_id) const;
    AtlasAssetState *_get_asset_state(uint32_t asset_id);
    const AtlasAssetState *_get_asset_state(uint32_t asset_id) const;
    LocalVector<StreamingChunk> &_get_asset_chunks(AtlasAssetState &asset);
    const LocalVector<StreamingChunk> &_get_asset_chunks(const AtlasAssetState &asset) const;
    uint64_t _make_chunk_key(uint32_t asset_id, uint32_t chunk_id) const;
    void _update_chunk_visibility(const Transform3D &camera_transform, const Projection &projection);
    void _log_streaming_telemetry();
    void _reload_config_if_dirty();
    float _resolve_frame_delta_seconds(float p_frame_delta_seconds);
    uint32_t _compute_runtime_chunk_capacity_limit() const;
    uint64_t _get_auxiliary_vram_overhead_bytes() const;
    uint64_t _get_total_vram_usage_bytes() const;
    uint64_t _get_evictable_vram_usage_bytes() const;
    uint32_t _get_reserved_chunk_count() const;
    uint64_t _get_pending_upload_bytes_for_diagnostics() const;
    void _load_zero_visible_recovery_config_from_project_settings();
    void _update_camera_tracking(const Vector3 &camera_pos, float p_frame_delta_seconds);
    void _handle_zero_visible_chunk_recovery();
    void _run_streaming_frame_pipeline(const Transform3D &camera_transform, const Projection &projection,
            const Vector3 &camera_pos, float resolved_frame_delta_seconds, uint32_t regulated_max,
            uint32_t effective_max, uint32_t runtime_capacity_max, OS *os, uint64_t scheduler_start_usec);
    void _reset_per_frame_counters();
    void _evict_for_vram_budget(uint32_t &evictions_left, bool &eviction_blocked);
    void _load_visible_chunks(uint32_t effective_max, uint32_t &evictions_left, bool &eviction_blocked);
    void _build_visible_chunk_list();
    void _handle_predictive_prefetch(const Vector3 &camera_pos, uint32_t effective_max);
    void _update_vram_regulator();
    void _log_streaming_frame_stats(uint32_t effective_max);
    Error _load_chunk(uint32_t chunk_idx);
    Error _load_chunk(uint32_t asset_id, uint32_t chunk_idx);
    RenderingDevice *_resolve_submission_device(GaussianSplatManager *manager,
            GaussianSplatManager::ScopedSubmissionLock &submission_lock) const;
    // Per-splat atlas byte stride: 80 B when per-chunk quantization is active,
    // 144 B otherwise. Single source for all persistent-buffer slot arithmetic so the
    // default (non-quantized) path stays byte-identical.
    uint64_t _atlas_gaussian_stride_bytes() const;
    // Packs a chunk into raw atlas bytes (PackedGaussian or PackedGaussianQuantized,
    // per _atlas_gaussian_stride_bytes()). Output size is chunk.count * stride.
    bool _pack_chunk_data(uint32_t asset_id, uint32_t chunk_idx, const AtlasAssetState &asset, StreamingChunk &chunk,
            Vector<uint8_t> &chunk_bytes, SHCompressionMetrics &metrics);
    void _complete_chunk_load_common(uint32_t asset_id, uint32_t chunk_idx, StreamingChunk &chunk);
    void _log_chunk_load_metrics(uint32_t chunk_idx, const SHCompressionMetrics &metrics);
    bool _upload_chunk_to_gpu(RenderingDevice *submission_rd, uint32_t buffer_offset,
            const Vector<uint8_t> &chunk_bytes, uint32_t asset_id, uint32_t chunk_idx,
            uint32_t buffer_slot, uint32_t chunk_count) const;
    bool _begin_chunk_upload(uint32_t asset_id, uint32_t chunk_idx, StreamingChunk &chunk, uint32_t buffer_slot);
    bool _stage_chunk_upload_retirement(uint32_t asset_id, uint32_t chunk_idx,
            StreamingChunk &chunk, uint32_t buffer_slot, uint64_t bytes,
            const SHCompressionMetrics &metrics, RenderingDevice *submission_rd,
            uint32_t override_retire_after_frames = UINT32_MAX,
            StreamingUploadCompletionMode override_completion_mode =
                    GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_NONE);
    void _process_upload_retirements();
    bool _has_pending_upload_retirement(uint32_t asset_id, uint32_t chunk_idx, uint32_t buffer_slot) const;
    void _mark_chunk_upload_failed(uint32_t asset_id, uint32_t chunk_idx, StreamingChunk &chunk, const char *context);
    void _rollback_pending_chunk(uint32_t asset_id, uint32_t chunk_idx, StreamingChunk &chunk, bool release_slot);
    void _assert_chunk_state_invariant(uint32_t asset_id, uint32_t chunk_idx,
            const StreamingChunk &chunk, const char *context,
            bool allow_deferred_allocator_release = false);
    void _finalize_chunk_load(uint32_t asset_id, uint32_t chunk_idx, StreamingChunk &chunk, uint32_t buffer_slot, uint32_t asset_chunk_count);
    void _unload_chunk(uint32_t chunk_idx);
    void _unload_chunk(uint32_t asset_id, uint32_t chunk_idx);
    EvictionResult _evict_least_recently_used(bool p_allow_visible_eviction);
    bool _is_chunk_in_frustum(const AABB &p_bounds, const Vector<Plane> &p_frustum_planes) const;
    void _reload_debug_logging_config();
    void _load_streaming_tuning_config_from_project_settings();
    void _start_pack_threads();
    void _stop_pack_threads();
    bool _queue_chunk_load(uint32_t chunk_idx);
    bool _queue_chunk_load(uint32_t asset_id, uint32_t chunk_idx);
    bool _enqueue_chunk_load_request(uint32_t asset_id, uint32_t chunk_idx,
            bool can_async_pack, bool prioritize_sync_fallback = false);
    bool _enqueue_sync_fallback_chunk_load(uint32_t asset_id, uint32_t chunk_idx, bool prioritize = false);
    uint32_t _drain_sync_fallback_chunk_loads(uint32_t effective_max, uint32_t &evictions_left, bool &eviction_blocked);
    bool _should_force_sync_fallback_for_async_stall(uint32_t pack_queue_depth, uint32_t upload_queue_depth);
    bool _can_use_async_pack_path(uint32_t pack_queue_depth, uint32_t upload_queue_depth);
    bool _is_requested_chunk_in_current_generation(const AtlasAssetState &asset, uint32_t chunk_id) const;
    void _update_requested_chunk_state(
            AtlasAssetState &asset, uint32_t chunk_id, uint8_t request_state, uint8_t request_result,
            Error request_error = OK);
    void _update_requested_chunk_state_for_generation(
            AtlasAssetState &asset, uint32_t chunk_id, uint64_t request_generation,
            uint8_t request_state, uint8_t request_result, Error request_error = OK);
    Dictionary _build_residency_request_status(uint32_t asset_id, uint32_t chunk_id) const;
    static StringName _residency_request_state_name(uint8_t request_state);
    static StringName _residency_request_error_name(Error request_error);
    static bool _is_terminal_residency_request_error(Error request_error);
    uint32_t _get_sync_fallback_queue_depth() const;
    void _compact_sync_fallback_queue();
    void _process_upload_queue();
    void _clear_pending_uploads();
    void _release_persistent_buffer(RenderingDevice *p_rd, const char *p_context);
    // Grow the persistent buffer to p_new_capacity slots, preserving currently
    // resident chunks. Returns false on any failure without corrupting state.
    bool _grow_persistent_buffer(uint32_t p_new_capacity);
    bool _try_grow_persistent_buffer_for_atlas_pressure(uint32_t p_loaded_chunks,
            uint32_t p_effective_max,
            bool p_enforce_vram_regulator_gate,
            bool p_vram_regulator_allows_load);
    void _apply_config_overrides();
    const LODConfig &_get_lod_config() const;
    void _update_culling_config_from_project_settings();
    uint32_t _prefetch_chunks_at_predicted_position(const Vector3 &predicted_pos,
            uint32_t available_slots, uint32_t load_budget, uint32_t max_scan_budget);
    void _load_prefetch_config_from_project_settings();
    void _load_lod_blend_config_from_project_settings();
    void _update_chunk_lod_blend_factors(const Vector3 &camera_pos);
    float _calculate_lod_blend_factor(float distance, float lod_distance) const;

    // Per-chunk quantization helpers
    void _load_quantization_config_from_project_settings();
    void _compute_chunk_quantization(uint32_t chunk_idx);
    void _compute_chunk_quantization(uint32_t asset_id, uint32_t chunk_idx);
    bool _release_quantization_buffer(RenderingDevice *p_rd, const char *p_context, bool p_allow_deferred_release);
    bool _upload_quantization_buffer(RenderingDevice *p_rd);
    ChunkQuantizationGPU _create_gpu_quantization_data(const ChunkQuantizationInfo &info, uint32_t start_idx, uint32_t count) const;

    void _apply_requested_residency(bool can_async_pack);
    void _evict_unrequested_chunks(uint32_t asset_id, AtlasAssetState &asset,
            LocalVector<StreamingChunk> &asset_chunks);
    bool _load_requested_chunks(uint32_t asset_id, AtlasAssetState &asset,
            LocalVector<StreamingChunk> &asset_chunks, bool trace_enabled, bool can_async_pack);
    EvictionResult _evict_for_admission_gate(
            const ResidencyBudgetController::AdmissionGate &p_admission_gate,
            bool &r_visible_fallback_attempted);
    EvictionResult _evict_non_primary_lru();

    // Distance-based LOD (Octree-GS) helpers
    void _update_chunk_lod_parameters(const Vector3 &camera_pos);
    void _load_lod_config_from_project_settings();
    void _collect_lod_debug_stats(const FrameData &frame,
            uint32_t (&lod_level_counts)[8],
            uint32_t (&sh_band_counts)[4],
            uint32_t &total_original_splats,
            uint32_t &total_effective_splats,
            float &min_distance,
            float &max_distance,
            float &total_distance,
            uint32_t &visible_count,
            uint32_t &total_lod_level,
            int &max_skip_factor,
            float &min_opacity,
            uint32_t &chunks_in_transition) const;
    Dictionary _build_streaming_diagnostics_snapshot(uint32_t pack_queue_depth, uint32_t upload_queue_depth,
            uint32_t sync_fallback_queue_depth);
};

#endif // GAUSSIAN_STREAMING_H
