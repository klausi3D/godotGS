#ifndef TILE_RENDER_RESOURCES_H
#define TILE_RENDER_RESOURCES_H

#include "tile_render_types.h"
#include "gpu_sorter.h"
#include "sort_fallback_policy.h"

#include <memory>

class TileBinningShaderRD;
class TilePrefixScanShaderRD;
class TileRasterizerShaderRD;
class TileRasterizerComputeShaderRD;
class TileResolveShaderRD;

class GPUPerformanceMonitor;
class TileRenderer;
class RenderDeviceManager;

namespace GaussianSplatting {

// Growth/shrink policy for an only-grow GPU scratch buffer. The buffer grows
// immediately to demand (plus optional slack); it shrinks back only after demand
// stays at or below `shrink_trigger_percent` of the current capacity for
// `shrink_hysteresis_frames` consecutive frames. The wide hysteresis is what
// prevents a single demand spike from causing grow/shrink cycling (which stalls
// the driver and transiently double-allocates). See tile_compute_buffer_resize_plan.
struct TileBufferShrinkPolicy {
	uint32_t growth_slack_percent = 0u; // extra headroom added on growth, as a % of demand
	uint32_t min_growth_slack_bytes = 0u; // absolute floor for the growth headroom
	uint32_t shrink_trigger_percent = 50u; // shrink only if required <= capacity * pct/100
	uint32_t shrink_hysteresis_frames = 120u; // frames demand must stay low before shrinking
};

struct TileBufferResizePlan {
	bool should_resize = false;
	uint32_t target_bytes = 0;
	uint32_t next_shrink_candidate_frames = 0;
};

// Pure, deterministic resize-policy decision shared by every only-grow GPU scratch
// buffer (SH color cache, global projection buffer, ...). Keeping the policy in one
// unit-tested function lets the buffer-management call sites stay thin and identical.
TileBufferResizePlan tile_compute_buffer_resize_plan(uint32_t p_required_bytes, uint32_t p_current_bytes,
		uint32_t p_shrink_candidate_frames, const TileBufferShrinkPolicy &p_policy);

// SH color-cache policy (shrink always on; small buffer, proven stable in production).
constexpr uint32_t TILE_SH_CACHE_GROWTH_SLACK_PERCENT = 25u;
constexpr uint32_t TILE_SH_CACHE_MIN_GROWTH_SLACK_BYTES = 4096u;
constexpr uint32_t TILE_SH_CACHE_SHRINK_TRIGGER_PERCENT = 50u;
constexpr uint32_t TILE_SH_CACHE_SHRINK_HYSTERESIS_FRAMES = 120u;

// Shared opt-in policy for the large demand-driven scratch buffers (the global
// projection buffer + the global sort key/value buffers; the separate instance/
// depth sort buffers are a future extension). Both grow with per-frame demand and
// oscillate by ~15x across a single camera sweep, so the
// hysteresis is wide (4s at 60 fps) and resets on any demand recovery — a shrink is
// bounded to views that hold low demand for the full window (a slow sweep staying below
// the trigger for >240 frames can still shrink then re-grow — bounded, not impossible). A 50%
// trigger means "demand dropped at least one power-of-two octave" (the projection
// byte size is power-of-two rounded). Growth slack is 0 for both: the projection
// rounding already provides headroom, and the sorter picks its own capacity.
constexpr uint32_t TILE_SCRATCH_SHRINK_TRIGGER_PERCENT = 50u;
constexpr uint32_t TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES = 240u;

// Backwards-compatible alias + thin wrapper used by the SH cache call site and tests.
typedef TileBufferResizePlan TileSHCacheResizePlan;
TileSHCacheResizePlan tile_compute_sh_cache_resize_plan(uint32_t p_required_bytes, uint32_t p_current_bytes,
		uint32_t p_shrink_candidate_frames);

struct TileRenderTargets {
	explicit TileRenderTargets(TileRenderer &p_owner) : owner(p_owner) {}

	void destroy_output_textures();
	void destroy_resolve_textures();
	void create_output_textures(const Vector2i &p_size, RD::DataFormat p_format);
	void ensure_resolve_resources(const Vector2i &p_size, RD::DataFormat p_format);

	RID get_output_texture(bool p_prefer_raw, bool p_force_resolved) const;
	RID get_depth_texture(bool p_prefer_raw, bool p_force_resolved) const;
	RenderingDevice *get_output_texture_owner(bool p_prefer_raw, bool p_force_resolved) const;
	RenderingDevice *get_depth_texture_owner(bool p_prefer_raw, bool p_force_resolved) const;

	TileRenderer &owner;
	RID output_texture;
	RID depth_texture;
	RID normal_texture;
	RID output_texture_external;
	RID depth_texture_external;
	RID resolved_texture;
	RID resolved_depth_texture;
	RID resolved_texture_external;
	RID resolved_depth_texture_external;
	RID tile_framebuffer;
	RD::FramebufferFormatID tile_framebuffer_format = RD::INVALID_ID;
	RenderingDevice *tile_framebuffer_owner = nullptr;
	BufferOwnership output_texture_owner;
	RenderingDevice *output_texture_local_owner = nullptr;
	bool output_texture_external_owned = false;
	BufferOwnership depth_texture_owner;
	RenderingDevice *depth_texture_local_owner = nullptr;
	bool depth_texture_external_owned = false;
	BufferOwnership normal_texture_owner;
	RenderingDevice *normal_texture_local_owner = nullptr;
	BufferOwnership resolved_texture_owner;
	RenderingDevice *resolved_texture_local_owner = nullptr;
	BufferOwnership resolved_depth_texture_owner;
	RenderingDevice *resolved_depth_texture_local_owner = nullptr;
	bool depth_texture_copy_compatible = false;
};

struct TileResourceController {
	explicit TileResourceController(TileRenderer &p_owner) : owner(p_owner) {}

	void set_contract_main_device(RenderingDevice *p_main_device);
	RenderingDevice *get_contract_main_device() const;

	void set_device_manager(RenderDeviceManager *p_device_manager);
	RenderDeviceManager *get_device_manager() const { return tracked_device_manager; }

	bool resolve_texture_owner(const char *p_label, const RID &p_texture, RenderingDevice *&r_owner, bool p_log_errors) const;
	RenderingDevice *get_output_texture_owner(const TileRenderTargets &p_targets, bool p_prefer_raw, bool p_force_resolved) const;
	RenderingDevice *get_depth_texture_owner(const TileRenderTargets &p_targets, bool p_prefer_raw, bool p_force_resolved) const;

	void track_output_resources(const RID &p_color_output, RenderingDevice *p_color_device,
			const RID &p_depth_output, RenderingDevice *p_depth_device);
	void clear_output_resource_tracking();

	TileRenderer &owner;
	RenderingDevice *contract_main_device = nullptr;
	RenderDeviceManager *tracked_device_manager = nullptr;
	RID tracked_color_output;
	RID tracked_depth_output;
};

struct TileDeviceContext {
	RenderingDevice *resource_rd = nullptr;
	RenderingDevice *submission_rd = nullptr;
	bool warned_missing_submission_device = false;
	GPUPerformanceMonitor *performance_monitor = nullptr;
};

struct TileShaderResources {
	explicit TileShaderResources(TileRenderer &p_owner);
	~TileShaderResources();

	void release(RenderingDevice *p_device, RenderingDevice *p_pipeline_owner);
	void reset_state();

	TileRenderer &owner;
	RID tile_binning_shader;
	RID tile_binning_pipeline;
	RID tile_binning_count_shader;
	RID tile_binning_count_pipeline;
	RID tile_prefix_shader;
	RID tile_prefix_shader_pass2;
	RID tile_prefix_shader_pass3;
	RID tile_prefix_pipeline_pass1;
	RID tile_prefix_pipeline_pass2;
	RID tile_prefix_pipeline_pass3;
	RID tile_raster_shader;
	RID tile_raster_pipeline;
	// Framebuffer format the cached tile_raster_pipeline was created against. The pipeline
	// must be recreated if the live framebuffer's format ID differs (e.g. attachment
	// reformat). Initialized to INVALID_ID when no pipeline exists.
	RD::FramebufferFormatID cached_raster_framebuffer_format = RD::INVALID_ID;
	RID tile_raster_compute_shader;
	RID tile_raster_compute_pipeline;
	RID tile_resolve_shader;
	RID tile_resolve_pipeline;
	RenderingDevice *shader_device = nullptr;
	uint64_t shader_device_instance = 0;
	std::unique_ptr<TileBinningShaderRD> tile_binning_shader_source;
	std::unique_ptr<TilePrefixScanShaderRD> tile_prefix_shader_source;
	std::unique_ptr<TileRasterizerShaderRD> tile_raster_shader_source;
	std::unique_ptr<TileRasterizerComputeShaderRD> tile_raster_compute_shader_source;
	std::unique_ptr<TileResolveShaderRD> tile_resolve_shader_source;
	bool tile_binning_shader_initialized = false;
	bool tile_prefix_shader_initialized = false;
	bool tile_raster_shader_initialized = false;
	bool tile_raster_compute_shader_initialized = false;
	bool tile_resolve_shader_initialized = false;
	bool subgroups_available = false;
	bool resolve_pipeline_initialized = false;
	bool quantized_storage_enabled = false;
	uint64_t shader_defines_hash = 0;
	RD::DataFormat resolve_pipeline_format = RD::DATA_FORMAT_MAX;
};

struct TileGlobalSortResources {
	explicit TileGlobalSortResources(TileRenderer &p_owner) : owner(p_owner) {}

	void release(RenderingDevice *p_default_device);
	void reset_state(bool p_clear_sorter);
	void ensure_resources(uint32_t p_visible_count);

	TileRenderer &owner;
	Ref<IGPUSorter> sorter;
	bool sorter_available = true;
	// Is the unavailability a PERMANENT capability failure (this device/config cannot
	// run the indirect radix sort) or a TRANSIENT recreation/allocation failure that a
	// later attempt can recover from? Only meaningful while sorter_available == false.
	// Classified by GaussianSplatting::sorter_creation_failure_is_permanent(); see the
	// SorterCreationFailure block in sort_fallback_policy.h for why each site lands
	// where it does. Without this split, one failed buffer allocation during a capacity
	// growth black-screened a capable GPU for the rest of the session.
	bool sorter_unavailable_permanent = false;
	// WHICH cause latched, not just whether it was permanent. Only meaningful while
	// sorter_available == false. It exists because "permanent" is permanent for a FIXED
	// sorting configuration, not for the process: the capability cause is decided by
	// probe_supports_indirect(), which reads g_gpu_sorting_config, so a user who corrects
	// a bad radix_bits/workgroup_size must be able to get sorted output back. ensure_resources()
	// re-asks the probe (allocation-free) for the causes
	// GaussianSplatting::sorter_permanent_failure_is_reprobable() admits, and hard-latches the
	// rest. Defaults to the transient cause so an uninitialised read can never unlatch anything.
	GaussianSplatting::SorterCreationFailure sorter_unavailable_cause =
			GaussianSplatting::SorterCreationFailure::CREATION_FAILED;
	// The live-configuration inputs the FAILED build read (#586 round-9). Only meaningful
	// while sorter_available == false with a config-latched cause
	// (GaussianSplatting::sorter_permanent_failure_is_config_latched). A deterministic
	// failure is a statement about a configuration, not about the process: a shader that
	// will not compile for radix_bits=8 x workgroup_size=512 says nothing about 4 x 256, so
	// ensure_resources() drops the latch the first time the signature differs from this one.
	// Without it, the corrected configuration is not merely ignored — the
	// `!sorter_available && !sorter_retry_due` branch below CONSUMES `key_config_changed`,
	// so the change signal is gone from the next call on and every later frame stays
	// rejected until renderer teardown.
	GaussianSplatting::SorterBuildSignature sorter_unavailable_build_signature;
	// Exponential backoff for retrying a TRANSIENT failure, in ensure_resources() calls.
	// delay is monotone non-decreasing (see next_sorter_retry_delay_calls); countdown is
	// the remaining calls before the next recreation attempt. Both zero while healthy.
	uint32_t sorter_retry_delay_calls = 0;
	uint32_t sorter_retry_countdown_calls = 0;
	bool sorter_missing_logged = false;
	uint64_t sorter_device_id = 0;
	uint32_t capacity = 0;
	uint32_t shrink_candidate_frames = 0; // consecutive low-demand frames, for bounded shrink hysteresis
	uint32_t last_observed_demand = 0; // last frame's MEASURED overlap demand (post-count), drives shrink hysteresis
	SortKeyConfig key_config;
	RID keys_buffer;
	RID values_buffer;
	RID tile_counts_buffers[2];
	uint32_t tile_counts_index = 0;
	bool tile_counts_ready[2] = { false, false };
	uint32_t tile_counts_bytes = 0;
	RID tile_ranges_buffer;
	RID prefix_total_buffer;
	RID indirect_dispatch_buffer;
	RID wg_sums_buffer;
	RID wg_offsets_buffer;
	BufferOwnership buffer_owner;
	uint32_t tile_buffer_tiles = 0;

	RID get_tile_counts_buffer() const {
		return tile_counts_buffers[tile_counts_index];
	}

	// Returns the OTHER slot of the ping-pong pair, which holds the previous
	// frame's per-tile count (post-EMIT cursor == per-tile overlap count).
	// Readers must tolerate an invalid/zeroed buffer on the very first frame
	// and when the array has been reallocated this frame.
	RID get_previous_tile_counts_buffer() const {
		return tile_counts_buffers[tile_counts_index ^ 1u];
	}

	void advance_tile_counts_buffer();
	void mark_tile_counts_dirty();
	bool ensure_tile_counts_ready(RenderingDevice *p_device);
	void prepare_next_tile_counts_buffer(RenderingDevice *p_device);
};

struct TileUniformBuffers {
	explicit TileUniformBuffers(TileRenderer &p_owner) : owner(p_owner) {}

	void release(RenderingDevice *p_default_device);
	void reset_state();
	bool ensure_param_buffer(RenderingDevice *p_device);
	bool ensure_prefix_param_buffer(RenderingDevice *p_device, uint32_t p_size);
	RID get_default_state_uniform(RenderingDevice *p_device);

	TileRenderer &owner;
	RID param_uniform_buffer;
	BufferOwnership param_uniform_buffer_owner;
	RID prefix_param_uniform_buffer;
	BufferOwnership prefix_param_owner;
	RID default_state_uniform_buffer;
	RenderingDevice *default_state_uniform_owner = nullptr;
};

struct TileProjectionBuffers {
	explicit TileProjectionBuffers(TileRenderer &p_owner) : owner(p_owner) {}

	void release(RenderingDevice *p_default_device);
	void reset_state();
	void ensure_projection_buffer(uint32_t p_visible_count);

	TileRenderer &owner;
	RID projection_buffer;
	BufferOwnership projection_buffer_owner;
	uint32_t projection_buffer_size = 0;
	uint32_t shrink_candidate_frames = 0; // consecutive low-demand frames, for bounded shrink hysteresis
	uint64_t sorter_device_id = 0;
	bool sorter_available = true;
	bool sorter_missing_logged = false;
};

struct TileSHCacheBuffers {
	explicit TileSHCacheBuffers(TileRenderer &p_owner) : owner(p_owner) {}

	void release(RenderingDevice *p_default_device);
	void reset_state();
	bool ensure_color_cache(uint32_t p_total_gaussians);

	TileRenderer &owner;
	RID sh_color_cache;
	BufferOwnership sh_color_cache_owner;
	uint32_t sh_color_cache_size = 0;
	uint32_t shrink_candidate_frames = 0;
};

struct TileSubpixelHistoryBuffers {
	explicit TileSubpixelHistoryBuffers(TileRenderer &p_owner) : owner(p_owner) {}

	void release(RenderingDevice *p_default_device);
	void reset_state();
	bool ensure_history_buffer(uint32_t p_total_gaussians);

	TileRenderer &owner;
	RID subpixel_history_buffer;
	BufferOwnership subpixel_history_owner;
	uint32_t subpixel_history_size = 0;
};

struct TileSubpixelVisibilityBuffers {
	explicit TileSubpixelVisibilityBuffers(TileRenderer &p_owner) : owner(p_owner) {}

	void release(RenderingDevice *p_default_device);
	void reset_state();
	bool ensure_visibility_buffer(uint32_t p_visible_count);

	TileRenderer &owner;
	RID subpixel_visibility_buffer;
	BufferOwnership subpixel_visibility_owner;
	uint32_t subpixel_visibility_size = 0;
};

} // namespace GaussianSplatting

#endif // TILE_RENDER_RESOURCES_H
