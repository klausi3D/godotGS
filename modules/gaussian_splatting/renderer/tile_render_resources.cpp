#include "tile_render_resources.h"
#include "tile_renderer.h"

#include "core/error/error_macros.h"
#include "gaussian_gpu_layout.h"
#include "pipeline_io_contracts.h"
#include "gpu_sorting_config.h"
#include "gpu_sorting_constants.h"
#include "sort_fallback_policy.h"
#include "../interfaces/render_device_manager.h"
#include "../interfaces/sync_policy.h"
#include "../logger/gs_logger.h"
#include "../shaders/tile_binning.glsl.gen.h"
#include "../shaders/tile_prefix_scan.glsl.gen.h"
#include "../shaders/tile_rasterizer.glsl.gen.h"
#include "../shaders/tile_rasterizer_compute.glsl.gen.h"
#include "../shaders/tile_resolve.glsl.gen.h"

#include <cstring>

namespace {

struct InteractiveStateUniforms {
	float highlight_strength = 0.0f;
	float outline_width = 0.0f;
	float state = 0.0f;
	float reserved = 0.0f;
	float highlight_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float outline_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

inline void safe_free_buffer_rid(RenderingDevice *p_device, RID &p_rid) {
	if (!p_rid.is_valid()) {
		p_rid = RID();
		return;
	}
	if (p_device && p_device->buffer_is_valid(p_rid)) {
		p_device->free(p_rid);
	}
	p_rid = RID();
}

// Words (uint32) per sort key for the global composite sort: 64-bit keys use two
// words, everything else one. Single source of truth for both the keys-buffer
// allocation and the bounded-shrink byte estimate, so they cannot drift apart.
inline uint32_t global_sort_key_stride_words(const SortKeyConfig &p_key_config) {
	return (p_key_config.key_bits > 32) ? 2u : 1u;
}

} // namespace

namespace GaussianSplatting {

TileBufferResizePlan tile_compute_buffer_resize_plan(uint32_t p_required_bytes, uint32_t p_current_bytes,
		uint32_t p_shrink_candidate_frames, const TileBufferShrinkPolicy &p_policy) {
	TileBufferResizePlan plan;

	auto compute_target_with_slack = [&p_policy](uint32_t p_base_bytes) -> uint32_t {
		if (p_base_bytes == 0u) {
			return 0u;
		}

		const uint64_t percent_slack = (uint64_t(p_base_bytes) * uint64_t(p_policy.growth_slack_percent) + 99u) / 100u;
		const uint64_t slack_bytes = MAX<uint64_t>(percent_slack, uint64_t(p_policy.min_growth_slack_bytes));
		const uint64_t target_bytes64 = uint64_t(p_base_bytes) + slack_bytes;
		return uint32_t(MIN<uint64_t>(target_bytes64, uint64_t(UINT32_MAX)));
	};

	// Grow immediately when demand exceeds the current capacity.
	if (p_required_bytes > p_current_bytes) {
		plan.should_resize = true;
		plan.target_bytes = compute_target_with_slack(p_required_bytes);
		return plan;
	}

	// Nothing allocated yet and no demand: nothing to do.
	if (p_current_bytes == 0u) {
		return plan;
	}

	// Hysteresis of 0 would mean "shrink instantly", which is exactly the cycling
	// we are guarding against; clamp to at least one frame so the counter logic holds.
	const uint32_t hysteresis_frames = MAX<uint32_t>(p_policy.shrink_hysteresis_frames, 1u);

	// Demand fell to zero: free the buffer once the low streak is long enough.
	if (p_required_bytes == 0u) {
		const uint32_t next_frames = MIN<uint32_t>(p_shrink_candidate_frames + 1u, hysteresis_frames);
		if (next_frames >= hysteresis_frames) {
			plan.should_resize = true;
			plan.target_bytes = 0u;
			return plan;
		}
		plan.next_shrink_candidate_frames = next_frames;
		return plan;
	}

	// Demand dropped well below capacity: shrink once the low streak is long enough.
	const uint64_t shrink_trigger_bytes = (uint64_t(p_current_bytes) * uint64_t(p_policy.shrink_trigger_percent)) / 100u;
	if (uint64_t(p_required_bytes) <= shrink_trigger_bytes) {
		const uint32_t next_frames = MIN<uint32_t>(p_shrink_candidate_frames + 1u, hysteresis_frames);
		if (next_frames >= hysteresis_frames) {
			plan.should_resize = true;
			plan.target_bytes = compute_target_with_slack(p_required_bytes);
			return plan;
		}
		plan.next_shrink_candidate_frames = next_frames;
		return plan;
	}

	// Demand is within the keep band: hold capacity, reset the low streak.
	return plan;
}

TileSHCacheResizePlan tile_compute_sh_cache_resize_plan(uint32_t p_required_bytes, uint32_t p_current_bytes,
		uint32_t p_shrink_candidate_frames) {
	const TileBufferShrinkPolicy policy{
		TILE_SH_CACHE_GROWTH_SLACK_PERCENT,
		TILE_SH_CACHE_MIN_GROWTH_SLACK_BYTES,
		TILE_SH_CACHE_SHRINK_TRIGGER_PERCENT,
		TILE_SH_CACHE_SHRINK_HYSTERESIS_FRAMES,
	};
	return tile_compute_buffer_resize_plan(p_required_bytes, p_current_bytes, p_shrink_candidate_frames, policy);
}

void TileResourceController::set_contract_main_device(RenderingDevice *p_main_device) {
	contract_main_device = p_main_device;
}

RenderingDevice *TileResourceController::get_contract_main_device() const {
	if (contract_main_device) {
		return contract_main_device;
	}
	return RenderingDevice::get_singleton();
}

void TileResourceController::set_device_manager(RenderDeviceManager *p_device_manager) {
	if (tracked_device_manager == p_device_manager) {
		return;
	}
	clear_output_resource_tracking();
	tracked_device_manager = p_device_manager;
}

bool TileResourceController::resolve_texture_owner(const char *p_label, const RID &p_texture, RenderingDevice *&r_owner,
		bool p_log_errors) const {
	if (!p_texture.is_valid()) {
		r_owner = nullptr;
		return true;
	}

	RenderingDevice *main_device = get_contract_main_device();
	if (!r_owner && tracked_device_manager) {
		r_owner = tracked_device_manager->get_resource_owner(p_texture, nullptr);
	}
	if (!r_owner && main_device && main_device->texture_is_valid(p_texture)) {
		r_owner = main_device;
		if (p_log_errors) {
			GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] %s owner missing; using main RenderingDevice for RID=%s",
					String(p_label), String::num_uint64(p_texture.get_id())));
		}
		return true;
	}
	if (!r_owner) {
		if (p_log_errors) {
			GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] %s ownership contract violation: missing owner for RID=%s",
					String(p_label), String::num_uint64(p_texture.get_id())));
		}
		return false;
	}
	if (!r_owner->texture_is_valid(p_texture)) {
		if (main_device && main_device != r_owner && main_device->texture_is_valid(p_texture)) {
			if (tracked_device_manager) {
				tracked_device_manager->push_cross_device_operation(
						String(p_label) + String("_owner_remap"), r_owner, main_device);
			}
			r_owner = main_device;
			return true;
		}
		if (p_log_errors) {
			GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] %s ownership contract violation: owner does not validate RID=%s",
					String(p_label), String::num_uint64(p_texture.get_id())));
		}
		return false;
	}
	if (main_device && r_owner != main_device) {
		const bool visible_on_main = main_device->texture_is_valid(p_texture);
		if (visible_on_main) {
			if (tracked_device_manager) {
				tracked_device_manager->push_cross_device_operation(
						String(p_label) + String("_owner_remap"), r_owner, main_device);
			}
			r_owner = main_device;
			return true;
		}
		if (tracked_device_manager) {
			tracked_device_manager->push_cross_device_operation(
					String(p_label) + String("_contract_violation"), r_owner, main_device);
		}
		if (p_log_errors) {
			GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] %s ownership contract violation: RID=%s is not visible on the main RenderingDevice",
					String(p_label), String::num_uint64(p_texture.get_id())));
		}
		return false;
	}
	return true;
}

RenderingDevice *TileResourceController::get_output_texture_owner(const TileRenderTargets &p_targets, bool p_prefer_raw, bool p_force_resolved) const {
	RenderingDevice *resolved_owner = p_targets.resolved_texture_owner.device ? p_targets.resolved_texture_owner.device : p_targets.output_texture_owner.device;
	if (p_force_resolved && (p_targets.resolved_texture.is_valid() || p_targets.resolved_texture_external.is_valid())) {
		return resolved_owner;
	}
	if (!p_prefer_raw && (p_targets.resolved_texture.is_valid() || p_targets.resolved_texture_external.is_valid())) {
		return resolved_owner;
	}
	return p_targets.output_texture_owner.device;
}

RenderingDevice *TileResourceController::get_depth_texture_owner(const TileRenderTargets &p_targets, bool p_prefer_raw, bool p_force_resolved) const {
	RenderingDevice *resolved_owner = p_targets.resolved_depth_texture_owner.device ? p_targets.resolved_depth_texture_owner.device : p_targets.depth_texture_owner.device;
	if (p_force_resolved && (p_targets.resolved_depth_texture.is_valid() || p_targets.resolved_depth_texture_external.is_valid())) {
		return resolved_owner;
	}
	if (!p_prefer_raw && (p_targets.resolved_depth_texture.is_valid() || p_targets.resolved_depth_texture_external.is_valid())) {
		return resolved_owner;
	}
	return p_targets.depth_texture_owner.device;
}

void TileResourceController::track_output_resources(const RID &p_color_output, RenderingDevice *p_color_device,
		const RID &p_depth_output, RenderingDevice *p_depth_device) {
	if (!tracked_device_manager) {
		return;
	}

	RenderingDevice *main_device = get_contract_main_device();

	if (p_color_output.is_valid()) {
		if (tracked_color_output.is_valid() && tracked_color_output != p_color_output) {
			tracked_device_manager->forget_resource(tracked_color_output);
		}
		RenderingDevice *owner_device = p_color_device ? p_color_device : main_device;
		if (resolve_texture_owner("tile_color_output", p_color_output, owner_device, false)) {
			tracked_device_manager->track_resource(p_color_output, owner_device, false, "tile_renderer_color_output");
			tracked_color_output = p_color_output;
		} else if (tracked_color_output.is_valid()) {
			tracked_device_manager->forget_resource(tracked_color_output);
			tracked_color_output = RID();
		}
	} else if (tracked_color_output.is_valid()) {
		tracked_device_manager->forget_resource(tracked_color_output);
		tracked_color_output = RID();
	}

	if (p_depth_output.is_valid()) {
		if (tracked_depth_output.is_valid() && tracked_depth_output != p_depth_output) {
			tracked_device_manager->forget_resource(tracked_depth_output);
		}
		RenderingDevice *owner_device = p_depth_device ? p_depth_device : main_device;
		if (resolve_texture_owner("tile_depth_output", p_depth_output, owner_device, false)) {
			tracked_device_manager->track_resource(p_depth_output, owner_device, false, "tile_renderer_depth_output");
			tracked_depth_output = p_depth_output;
		} else if (tracked_depth_output.is_valid()) {
			tracked_device_manager->forget_resource(tracked_depth_output);
			tracked_depth_output = RID();
		}
	} else if (tracked_depth_output.is_valid()) {
		tracked_device_manager->forget_resource(tracked_depth_output);
		tracked_depth_output = RID();
	}
}

void TileResourceController::clear_output_resource_tracking() {
	if (tracked_device_manager) {
		if (tracked_color_output.is_valid()) {
			tracked_device_manager->forget_resource(tracked_color_output);
		}
		if (tracked_depth_output.is_valid()) {
			tracked_device_manager->forget_resource(tracked_depth_output);
		}
	}
	tracked_color_output = RID();
	tracked_depth_output = RID();
}

TileShaderResources::TileShaderResources(TileRenderer &p_owner) :
		owner(p_owner),
		tile_binning_shader_source(std::make_unique<TileBinningShaderRD>()),
		tile_prefix_shader_source(std::make_unique<TilePrefixScanShaderRD>()),
		tile_raster_shader_source(std::make_unique<TileRasterizerShaderRD>()),
		tile_raster_compute_shader_source(std::make_unique<TileRasterizerComputeShaderRD>()),
		tile_resolve_shader_source(std::make_unique<TileResolveShaderRD>()) {
}

TileShaderResources::~TileShaderResources() = default;

RID TileRenderTargets::get_output_texture(bool p_prefer_raw, bool p_force_resolved) const {
	RID resolved = resolved_texture_external.is_valid() ? resolved_texture_external : resolved_texture;
	if (p_force_resolved && resolved.is_valid()) {
		return resolved;
	}
	if (!p_prefer_raw && resolved.is_valid()) {
		return resolved;
	}
	return output_texture_external.is_valid() ? output_texture_external : output_texture;
}

RID TileRenderTargets::get_depth_texture(bool p_prefer_raw, bool p_force_resolved) const {
	RID resolved = resolved_depth_texture_external.is_valid() ? resolved_depth_texture_external : resolved_depth_texture;
	if (p_force_resolved && resolved.is_valid()) {
		return resolved;
	}
	if (!p_prefer_raw && resolved.is_valid()) {
		return resolved;
	}
	return depth_texture_external.is_valid() ? depth_texture_external : depth_texture;
}

RenderingDevice *TileRenderTargets::get_output_texture_owner(bool p_prefer_raw, bool p_force_resolved) const {
	return owner.resource_controller.get_output_texture_owner(*this, p_prefer_raw, p_force_resolved);
}

RenderingDevice *TileRenderTargets::get_depth_texture_owner(bool p_prefer_raw, bool p_force_resolved) const {
	return owner.resource_controller.get_depth_texture_owner(*this, p_prefer_raw, p_force_resolved);
}

void TileRenderTargets::destroy_output_textures() {
	RenderingDevice *resource_device = owner._get_resource_device();

	auto safe_texture_free = [](RenderingDevice *p_device, RID &p_rid) {
		if (!p_device || !p_rid.is_valid()) {
			p_rid = RID();
			return;
		}
		if (p_device->texture_is_valid(p_rid)) {
			p_device->free(p_rid);
		}
		p_rid = RID();
	};

	if (tile_framebuffer.is_valid()) {
		RenderingDevice *framebuffer_owner = tile_framebuffer_owner ? tile_framebuffer_owner : resource_device;
		if (framebuffer_owner && framebuffer_owner->framebuffer_is_valid(tile_framebuffer)) {
			framebuffer_owner->free(tile_framebuffer);
		}
		tile_framebuffer = RID();
	}

	safe_texture_free(output_texture_local_owner ? output_texture_local_owner : resource_device, output_texture);
	safe_texture_free(depth_texture_local_owner ? depth_texture_local_owner : resource_device, depth_texture);
	safe_texture_free(normal_texture_local_owner ? normal_texture_local_owner : resource_device, normal_texture);

	RenderingDevice *pipeline_owner = output_texture_local_owner ? output_texture_local_owner : resource_device;
	if (owner.shader_resources.tile_raster_pipeline.is_valid()) {
		if (pipeline_owner && pipeline_owner->render_pipeline_is_valid(owner.shader_resources.tile_raster_pipeline)) {
			pipeline_owner->free(owner.shader_resources.tile_raster_pipeline);
		}
		owner.shader_resources.tile_raster_pipeline = RID();
	}
	owner.shader_resources.cached_raster_framebuffer_format = RD::INVALID_ID;
	if (owner.raster_stage.cached_raster_image_uniform_set.is_valid() && owner.raster_stage.cached_raster_image_device &&
			owner.raster_stage.cached_raster_image_device->uniform_set_is_valid(owner.raster_stage.cached_raster_image_uniform_set)) {
		owner.raster_stage.cached_raster_image_device->free(owner.raster_stage.cached_raster_image_uniform_set);
	}
	owner.raster_stage.cached_raster_image_uniform_set = RID();
	owner.raster_stage.cached_raster_image_device = nullptr;
	owner.raster_stage.cached_raster_image_output = RID();
	owner.raster_stage.cached_raster_image_depth = RID();
	owner.raster_stage.cached_raster_image_normal = RID();

	output_texture = RID();
	depth_texture = RID();
	normal_texture = RID();

	if (output_texture_external.is_valid()) {
		if (output_texture_external_owned) {
			RenderingDevice *external_owner = output_texture_owner.device;
			if (external_owner) {
				safe_texture_free(external_owner, output_texture_external);
			} else {
				GS_LOG_WARN_DEFAULT("[TileRenderer] Missing owner for output_texture_external during teardown");
				output_texture_external = RID();
			}
		} else {
			output_texture_external = RID();
		}
	}

	if (depth_texture_external.is_valid()) {
		if (depth_texture_external_owned) {
			RenderingDevice *external_owner = depth_texture_owner.device;
			if (external_owner) {
				safe_texture_free(external_owner, depth_texture_external);
			} else {
				GS_LOG_WARN_DEFAULT("[TileRenderer] Missing owner for depth_texture_external during teardown");
				depth_texture_external = RID();
			}
		} else {
			depth_texture_external = RID();
		}
	}

	output_texture_external_owned = false;
	depth_texture_external_owned = false;
	output_texture_owner.clear();
	output_texture_local_owner = nullptr;
	depth_texture_owner.clear();
	depth_texture_local_owner = nullptr;
	normal_texture_owner.clear();
	normal_texture_local_owner = nullptr;
	depth_texture_copy_compatible = false;
	tile_framebuffer = RID();
	tile_framebuffer_format = RD::INVALID_ID;
	tile_framebuffer_owner = nullptr;
	owner.shader_resources.tile_raster_pipeline = RID();
	owner.shader_resources.cached_raster_framebuffer_format = RD::INVALID_ID;

	destroy_resolve_textures();
}

void TileRenderTargets::destroy_resolve_textures() {
	owner.resolve_stage.destroy_resolve_textures();
}

void TileRenderTargets::create_output_textures(const Vector2i &p_size, RD::DataFormat p_format) {
	RenderingDevice *device = owner._get_resource_device();
	ERR_FAIL_NULL(device);

	RenderingDevice *main_device = owner._get_contract_main_device();
	bool can_share_with_main = main_device != nullptr && main_device != device;

	tile_framebuffer = RID();
	tile_framebuffer_format = RD::INVALID_ID;
	tile_framebuffer_owner = nullptr;

	auto share_texture_with_main = [&](const RD::TextureFormat &fmt, RID &r_local, RID &r_external,
			RenderingDevice *&r_owner) -> bool {
		if (!can_share_with_main) {
			return false;
		}

		RID main_texture = main_device->texture_create(fmt, RD::TextureView());
		if (!main_texture.is_valid()) {
			return false;
		}

		uint64_t driver_handle = main_device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, main_texture, 0);
		if (driver_handle == 0) {
			main_device->free(main_texture);
			return false;
		}

		RID local_reference = device->texture_create_from_extension(fmt.texture_type, fmt.format, fmt.samples, fmt.usage_bits,
				driver_handle, fmt.width, fmt.height, fmt.depth, fmt.array_layers, fmt.mipmaps);
		if (!local_reference.is_valid()) {
			main_device->free(main_texture);
			return false;
		}

		// Resource names will be set by the caller after assignment to final variables.
		r_local = local_reference;
		r_external = main_texture;
		r_owner = main_device;
		return true;
	};

	// Create color output texture.
	RD::TextureFormat format;
	format.width = p_size.x;
	format.height = p_size.y;
	format.depth = 1;
	format.array_layers = 1;
	format.mipmaps = 1;
	format.texture_type = RD::TEXTURE_TYPE_2D;
	format.samples = RD::TEXTURE_SAMPLES_1;
	format.format = p_format != RD::DATA_FORMAT_MAX ? p_format : RD::DATA_FORMAT_R8G8B8A8_UNORM;
	// #643: sRGB (and BGRA) color formats cannot carry TEXTURE_USAGE_STORAGE_BIT. The
	// renderer routes such formats to the fragment raster path (TileRenderer::_decide_compute_raster),
	// which writes the output as a COLOR_ATTACHMENT, so STORAGE is never needed for them.
	// Requesting it unconditionally made texture_create() fail for sRGB, leaving the renderer
	// with no output texture while initialize() still returned OK. Only request STORAGE when the
	// format actually supports it; the sampling/attachment/copy bits are load-bearing on the
	// fragment path and stay for every format.
	const bool storage_compatible = TileRenderer::output_format_supports_storage(format.format);
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			(storage_compatible ? RD::TEXTURE_USAGE_STORAGE_BIT : 0);

	output_texture_owner.clear();
	output_texture_local_owner = nullptr;
	output_texture_external = RID();
	output_texture_external_owned = false;
	RenderingDevice *shared_color_owner = nullptr;
	bool shared_color = share_texture_with_main(format, output_texture, output_texture_external, shared_color_owner);
	if (!shared_color && can_share_with_main) {
		GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to share output texture with main RenderingDevice; output cannot be presented");
		output_texture = RID();
		output_texture_external = RID();
		output_texture_external_owned = false;
		output_texture_owner.clear();
		output_texture_local_owner = nullptr;
		owner.config_state.output_format = RD::DATA_FORMAT_MAX;
		return;
	}
	if (!shared_color) {
		output_texture = device->texture_create(format, RD::TextureView());
		if (!output_texture.is_valid()) {
			GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate output texture");
			owner.config_state.output_format = RD::DATA_FORMAT_MAX;
			output_texture_external = RID();
			output_texture_external_owned = false;
			output_texture_owner.clear();
			return;
		}
		output_texture_external = output_texture;
		output_texture_owner.set(device);
	} else if (shared_color_owner) {
		output_texture_owner.set(shared_color_owner);
	}
	if (shared_color && output_texture_external.is_valid() && output_texture_external != output_texture) {
		output_texture_external_owned = true;
	}
	if (output_texture.is_valid()) {
		output_texture_local_owner = device;
		device->set_resource_name(output_texture, "GS_TileRenderer_OutputTexture");
	}
	if (output_texture_external.is_valid() && output_texture_owner.device && output_texture_external != output_texture) {
		output_texture_owner.device->set_resource_name(output_texture_external, "GS_TileRenderer_OutputTextureExternal");
	}
	owner.config_state.output_format = output_texture.is_valid() ? format.format : RD::DATA_FORMAT_MAX;

	RD::TextureFormat depth_format;
	depth_format.width = p_size.x;
	depth_format.height = p_size.y;
	depth_format.depth = 1;
	depth_format.array_layers = 1;
	depth_format.mipmaps = 1;
	depth_format.texture_type = RD::TEXTURE_TYPE_2D;
	depth_format.samples = RD::TEXTURE_SAMPLES_1;
	depth_format.format = RD::DATA_FORMAT_R32_SFLOAT;
	depth_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	depth_texture_copy_compatible = false;
	depth_texture_owner.clear();
	depth_texture_local_owner = nullptr;
	depth_texture_external = RID();
	depth_texture_external_owned = false;

	RenderingDevice *shared_depth_owner = nullptr;
	bool shared_depth = share_texture_with_main(depth_format, depth_texture, depth_texture_external, shared_depth_owner);
	if (!shared_depth && can_share_with_main) {
		GS_LOG_WARN_DEFAULT("[TileRenderer] Failed to share depth texture with main RenderingDevice; painterly depth unavailable");
	}
	if (!shared_depth) {
		depth_texture = device->texture_create(depth_format, RD::TextureView());
		if (!depth_texture.is_valid()) {
			GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate depth texture");
			depth_texture_external = RID();
			depth_texture_external_owned = false;
			depth_texture_owner.clear();
		} else {
			depth_texture_external = depth_texture;
			depth_texture_owner.set(device);
		}
	} else if (shared_depth_owner) {
		depth_texture_owner.set(shared_depth_owner);
	}
	if (shared_depth && depth_texture_external.is_valid() && depth_texture_external != depth_texture) {
		depth_texture_external_owned = true;
	}

	if (depth_texture.is_valid()) {
		depth_texture_local_owner = device;
		device->set_resource_name(depth_texture, "GS_TileRenderer_DepthTexture");
	}
	if (depth_texture_external.is_valid() && depth_texture_owner.device && depth_texture_external != depth_texture) {
		depth_texture_owner.device->set_resource_name(depth_texture_external, "GS_TileRenderer_DepthTextureExternal");
	}

	// Detect if the main depth texture can be copied for post-processing (same device and format).
	depth_texture_copy_compatible = depth_texture.is_valid() && depth_texture_external.is_valid() &&
			(depth_texture_external == depth_texture || depth_texture_owner.device == device);

	// Create normal output texture for resolve lighting (view-space normal * alpha).
	RD::TextureFormat normal_format;
	normal_format.width = p_size.x;
	normal_format.height = p_size.y;
	normal_format.depth = 1;
	normal_format.array_layers = 1;
	normal_format.mipmaps = 1;
	normal_format.texture_type = RD::TEXTURE_TYPE_2D;
	normal_format.samples = RD::TEXTURE_SAMPLES_1;
	normal_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	normal_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	normal_texture_owner.clear();
	normal_texture_local_owner = nullptr;
	normal_texture = device->texture_create(normal_format, RD::TextureView());
	if (!normal_texture.is_valid()) {
		GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate normal texture");
	} else {
		normal_texture_owner.set(device);
		normal_texture_local_owner = device;
		device->set_resource_name(normal_texture, "GS_TileRenderer_NormalTexture");
	}

	// Create framebuffer for fragment rasterizer.
	if (output_texture.is_valid() && depth_texture.is_valid() && normal_texture.is_valid()) {
		Vector<RID> attachments;
		attachments.push_back(output_texture);
		attachments.push_back(depth_texture);
		attachments.push_back(normal_texture);
		tile_framebuffer = device->framebuffer_create(attachments);
		if (tile_framebuffer.is_valid()) {
			tile_framebuffer_format = device->framebuffer_get_format(tile_framebuffer);
			tile_framebuffer_owner = device;
			device->set_resource_name(tile_framebuffer, "GS_TileRenderer_Framebuffer");
		} else {
			GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to create tile framebuffer");
		}
	}

	destroy_resolve_textures();
	// Ensure resolve textures are created after output textures - required for tile resolve lighting pass
	ensure_resolve_resources(p_size, p_format);
}

void TileRenderTargets::ensure_resolve_resources(const Vector2i &p_size, RD::DataFormat p_format) {
	owner.resolve_stage.ensure_resolve_resources(p_size, p_format);
}

void TileProjectionBuffers::release(RenderingDevice *p_default_device) {
	RenderingDevice *owner_device = projection_buffer_owner.device ? projection_buffer_owner.device : p_default_device;
	safe_free_buffer_rid(owner_device, projection_buffer);

	reset_state();
}

void TileProjectionBuffers::reset_state() {
	projection_buffer = RID();
	projection_buffer_owner.clear();
	projection_buffer_size = 0;
	shrink_candidate_frames = 0;
}

void TileProjectionBuffers::ensure_projection_buffer(uint32_t p_visible_count) {
	RenderingDevice *device = owner._get_resource_device();
	if (!device) {
		return;
	}
	uint64_t device_id = device->get_device_instance_id();
	if (device_id != sorter_device_id) {
		sorter_device_id = device_id;
		sorter_available = true;
		sorter_missing_logged = false;
	}

	uint32_t target_capacity = 0;
	const uint64_t required_bytes64 = owner._compute_projection_buffer_bytes(p_visible_count, target_capacity);
	if (required_bytes64 > UINT32_MAX) {
		GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Global projection buffer exceeds RD limits (capacity=%u, bytes=%s)",
				target_capacity, String::num_uint64(required_bytes64)));
		if (projection_buffer.is_valid()) {
			release(device);
			owner._invalidate_descriptor_cache();
		}
		return;
	}
	const uint32_t required_bytes = uint32_t(required_bytes64);

	// Resize policy. The visible-splat count oscillates by ~15x as the camera
	// sweeps (e.g. 250K..3.74M), so this >1 GB buffer must never grow/shrink-cycle:
	// an earlier 8x instantaneous shrink hysteresis was not wide enough and freed +
	// recreated the buffer hundreds of times per session, stalling the driver and
	// transiently double-allocating. We therefore default to ONLY-GROW (hold the
	// session high-water mark; release wholesale on teardown / scene change).
	//
	// When `bounded_buffer_shrink_enabled` is set (opt-in, primarily for low-end
	// GPUs that cannot afford to pin peak VRAM for the whole session), we instead
	// use the shared wide-hysteresis resize plan: it reclaims the buffer only after
	// demand stays at/below half capacity for TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES
	// consecutive frames — far wider than the streak that caused the old cycling.
	bool needs_resize;
	uint32_t target_bytes;
	if (g_gpu_sorting_config.bounded_buffer_shrink_enabled) {
		const TileBufferShrinkPolicy policy{
			0u, // growth slack: the power-of-two element rounding already provides headroom
			0u,
			TILE_SCRATCH_SHRINK_TRIGGER_PERCENT,
			TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES,
		};
		const TileBufferResizePlan plan = tile_compute_buffer_resize_plan(
				required_bytes, projection_buffer_size, shrink_candidate_frames, policy);
		shrink_candidate_frames = plan.next_shrink_candidate_frames;
		needs_resize = plan.should_resize;
		target_bytes = plan.should_resize ? plan.target_bytes : required_bytes;
	} else {
		shrink_candidate_frames = 0;
		needs_resize = !projection_buffer.is_valid() || projection_buffer_size < required_bytes;
		target_bytes = required_bytes;
	}

	if (!needs_resize) {
#ifdef DEV_ENABLED
		if (projection_buffer.is_valid() && projection_buffer_size < required_bytes) {
			ERR_PRINT(vformat("[TileRenderer] Projection buffer smaller than required (size=%u bytes required=%u bytes)",
					projection_buffer_size, required_bytes));
		}
#endif
		return;
	}

	if (projection_buffer.is_valid()) {
		safe_free_buffer_rid(device, projection_buffer);
		projection_buffer_owner.clear();
	}

	projection_buffer = device->storage_buffer_create(target_bytes);
	projection_buffer_size = projection_buffer.is_valid() ? target_bytes : 0;
	if (!projection_buffer.is_valid()) {
		GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate global projection buffer");
		projection_buffer_owner.clear();
		// The old buffer was already freed above; any descriptor set still caching it
		// now dangles. Invalidate here too (success path invalidates below) so a failed
		// realloc cannot leave the cache pointing at a freed RID.
		owner._invalidate_descriptor_cache();
		return;
	}
	device->set_resource_name(projection_buffer, "GS_TileRenderer_ProjectionBuffer");
	projection_buffer_owner.set(device);

#ifdef DEV_ENABLED
	if (projection_buffer.is_valid() && projection_buffer_size < required_bytes) {
		ERR_PRINT(vformat("[TileRenderer] Projection buffer allocation truncated (size=%u bytes required=%u bytes)",
				projection_buffer_size, required_bytes));
	}
	uint32_t projection_stride = owner._get_projection_stride();
	if (projection_buffer.is_valid() && projection_stride > 0 && (projection_buffer_size % projection_stride) != 0) {
		ERR_PRINT(vformat("[TileRenderer] Projection buffer size misaligned (size=%u bytes stride=%u)",
				projection_buffer_size, projection_stride));
	}
#endif

	owner._invalidate_descriptor_cache();
}

void TileSHCacheBuffers::release(RenderingDevice *p_default_device) {
	RenderingDevice *owner_device = sh_color_cache_owner.device ? sh_color_cache_owner.device : p_default_device;
	safe_free_buffer_rid(owner_device, sh_color_cache);

	reset_state();
}

void TileSHCacheBuffers::reset_state() {
	sh_color_cache = RID();
	sh_color_cache_owner.clear();
	sh_color_cache_size = 0;
	shrink_candidate_frames = 0;
}

bool TileSHCacheBuffers::ensure_color_cache(uint32_t p_total_gaussians) {
	RenderingDevice *device = owner._get_resource_device();
	if (!device) {
		return false;
	}

	const uint64_t required_bytes64 = uint64_t(p_total_gaussians) * sizeof(uint32_t);
	if (required_bytes64 > UINT32_MAX) {
		GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] SH color cache exceeds RD limits (gaussians=%u bytes=%s)",
				p_total_gaussians, String::num_uint64(required_bytes64)));
		if (sh_color_cache.is_valid()) {
			release(device);
			owner._invalidate_descriptor_cache();
		}
		return false;
	}
	const uint32_t required_bytes = uint32_t(required_bytes64);

	const TileSHCacheResizePlan resize_plan =
			tile_compute_sh_cache_resize_plan(required_bytes, sh_color_cache_size, shrink_candidate_frames);
	shrink_candidate_frames = resize_plan.next_shrink_candidate_frames;

	if (!resize_plan.should_resize) {
		return false;
	}

	if (sh_color_cache.is_valid()) {
		safe_free_buffer_rid(device, sh_color_cache);
		sh_color_cache_owner.clear();
	}

	if (resize_plan.target_bytes == 0u) {
		sh_color_cache_size = 0;
		shrink_candidate_frames = 0;
		owner._invalidate_descriptor_cache();
		return true;
	}

	sh_color_cache = device->storage_buffer_create(resize_plan.target_bytes);
	sh_color_cache_size = sh_color_cache.is_valid() ? resize_plan.target_bytes : 0;
	if (!sh_color_cache.is_valid() && resize_plan.target_bytes != required_bytes) {
		// Retry exact-fit allocation if slack allocation fails under pressure.
		sh_color_cache = device->storage_buffer_create(required_bytes);
		sh_color_cache_size = sh_color_cache.is_valid() ? required_bytes : 0;
	}
	if (!sh_color_cache.is_valid()) {
		GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate SH color cache buffer");
		sh_color_cache_owner.clear();
		shrink_candidate_frames = 0;
		return false;
	}

	device->set_resource_name(sh_color_cache, "GS_TileRenderer_SHColorCache");
	sh_color_cache_owner.set(device);
	shrink_candidate_frames = 0;
	owner._invalidate_descriptor_cache();
	return true;
}

void TileSubpixelHistoryBuffers::release(RenderingDevice *p_default_device) {
	RenderingDevice *owner_device = subpixel_history_owner.device ? subpixel_history_owner.device : p_default_device;
	safe_free_buffer_rid(owner_device, subpixel_history_buffer);

	reset_state();
}

void TileSubpixelHistoryBuffers::reset_state() {
	subpixel_history_buffer = RID();
	subpixel_history_owner.clear();
	subpixel_history_size = 0;
}

bool TileSubpixelHistoryBuffers::ensure_history_buffer(uint32_t p_total_gaussians) {
	RenderingDevice *device = owner._get_resource_device();
	if (!device) {
		return false;
	}

	const uint64_t required_bytes64 = uint64_t(p_total_gaussians) * sizeof(uint32_t);
	if (required_bytes64 > UINT32_MAX) {
		GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Subpixel history buffer exceeds RD limits (gaussians=%u bytes=%s)",
				p_total_gaussians, String::num_uint64(required_bytes64)));
		if (subpixel_history_buffer.is_valid()) {
			release(device);
			owner._invalidate_descriptor_cache();
		}
		return false;
	}
	const uint32_t required_bytes = uint32_t(required_bytes64);

	const bool should_shrink = subpixel_history_buffer.is_valid() && subpixel_history_size > 0 &&
			subpixel_history_size > required_bytes * 8u;
	const bool needs_resize = !subpixel_history_buffer.is_valid() || subpixel_history_size < required_bytes || should_shrink;

	if (!needs_resize) {
		return false;
	}

	if (subpixel_history_buffer.is_valid()) {
		safe_free_buffer_rid(device, subpixel_history_buffer);
		subpixel_history_owner.clear();
	}

	Vector<uint8_t> zero_bytes;
	zero_bytes.resize(required_bytes);
	subpixel_history_buffer = device->storage_buffer_create(required_bytes, zero_bytes);
	subpixel_history_size = subpixel_history_buffer.is_valid() ? required_bytes : 0;
	if (!subpixel_history_buffer.is_valid()) {
		GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate subpixel history buffer");
		subpixel_history_owner.clear();
		return false;
	}

	device->set_resource_name(subpixel_history_buffer, "GS_TileRenderer_SubpixelHistory");
	subpixel_history_owner.set(device);
	owner._invalidate_descriptor_cache();
	return true;
}

void TileSubpixelVisibilityBuffers::release(RenderingDevice *p_default_device) {
	RenderingDevice *owner_device = subpixel_visibility_owner.device ? subpixel_visibility_owner.device : p_default_device;
	safe_free_buffer_rid(owner_device, subpixel_visibility_buffer);

	reset_state();
}

void TileSubpixelVisibilityBuffers::reset_state() {
	subpixel_visibility_buffer = RID();
	subpixel_visibility_owner.clear();
	subpixel_visibility_size = 0;
}

bool TileSubpixelVisibilityBuffers::ensure_visibility_buffer(uint32_t p_visible_count) {
	RenderingDevice *device = owner._get_resource_device();
	if (!device) {
		return false;
	}

	const uint32_t required_elements = MAX<uint32_t>(p_visible_count, 1u);
	const uint64_t required_bytes64 = uint64_t(required_elements) * sizeof(uint32_t);
	if (required_bytes64 > UINT32_MAX) {
		GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Subpixel visibility buffer exceeds RD limits (elements=%u bytes=%s)",
				required_elements, String::num_uint64(required_bytes64)));
		if (subpixel_visibility_buffer.is_valid()) {
			release(device);
			owner._invalidate_descriptor_cache();
		}
		return false;
	}
	const uint32_t required_bytes = uint32_t(required_bytes64);

	const bool should_shrink = subpixel_visibility_buffer.is_valid() && subpixel_visibility_size > 0 &&
			subpixel_visibility_size > required_bytes * 8u;
	const bool needs_resize = !subpixel_visibility_buffer.is_valid() ||
			subpixel_visibility_size < required_bytes || should_shrink;

	if (!needs_resize) {
		return false;
	}

	if (subpixel_visibility_buffer.is_valid()) {
		safe_free_buffer_rid(device, subpixel_visibility_buffer);
		subpixel_visibility_owner.clear();
	}

	Vector<uint8_t> zero_bytes;
	zero_bytes.resize(required_bytes);
	subpixel_visibility_buffer = device->storage_buffer_create(required_bytes, zero_bytes);
	subpixel_visibility_size = subpixel_visibility_buffer.is_valid() ? required_bytes : 0;
	if (!subpixel_visibility_buffer.is_valid()) {
		GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate subpixel visibility buffer");
		subpixel_visibility_owner.clear();
		return false;
	}

	device->set_resource_name(subpixel_visibility_buffer, "GS_TileRenderer_SubpixelVisibility");
	subpixel_visibility_owner.set(device);
	owner._invalidate_descriptor_cache();
	return true;
}

void TileShaderResources::release(RenderingDevice *p_device, RenderingDevice *p_pipeline_owner) {
	RenderingDevice *pipeline_owner = p_pipeline_owner ? p_pipeline_owner : p_device;

	if (pipeline_owner) {
		if (tile_binning_pipeline.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_binning_pipeline)) {
			pipeline_owner->free(tile_binning_pipeline);
		}
		if (tile_binning_count_pipeline.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_binning_count_pipeline)) {
			pipeline_owner->free(tile_binning_count_pipeline);
		}
		if (tile_prefix_pipeline_pass1.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_prefix_pipeline_pass1)) {
			pipeline_owner->free(tile_prefix_pipeline_pass1);
		}
		if (tile_prefix_pipeline_pass2.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_prefix_pipeline_pass2)) {
			pipeline_owner->free(tile_prefix_pipeline_pass2);
		}
		if (tile_prefix_pipeline_pass3.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_prefix_pipeline_pass3)) {
			pipeline_owner->free(tile_prefix_pipeline_pass3);
		}
		if (tile_raster_pipeline.is_valid() && pipeline_owner->render_pipeline_is_valid(tile_raster_pipeline)) {
			pipeline_owner->free(tile_raster_pipeline);
		}
		if (tile_raster_compute_pipeline.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_raster_compute_pipeline)) {
			pipeline_owner->free(tile_raster_compute_pipeline);
		}
		if (tile_resolve_pipeline.is_valid() && pipeline_owner->compute_pipeline_is_valid(tile_resolve_pipeline)) {
			pipeline_owner->free(tile_resolve_pipeline);
		}
	}
	if (p_device) {
		if (tile_binning_shader.is_valid()) {
			p_device->free(tile_binning_shader);
		}
		if (tile_binning_count_shader.is_valid()) {
			p_device->free(tile_binning_count_shader);
		}
		if (tile_prefix_shader.is_valid()) {
			p_device->free(tile_prefix_shader);
		}
		if (tile_prefix_shader_pass2.is_valid()) {
			p_device->free(tile_prefix_shader_pass2);
		}
		if (tile_prefix_shader_pass3.is_valid()) {
			p_device->free(tile_prefix_shader_pass3);
		}
		if (tile_raster_shader.is_valid()) {
			p_device->free(tile_raster_shader);
		}
		if (tile_raster_compute_shader.is_valid()) {
			p_device->free(tile_raster_compute_shader);
		}
		if (tile_resolve_shader.is_valid()) {
			p_device->free(tile_resolve_shader);
		}
	}

	reset_state();
}

void TileShaderResources::reset_state() {
	tile_binning_pipeline = RID();
	tile_binning_count_pipeline = RID();
	tile_prefix_pipeline_pass1 = RID();
	tile_prefix_pipeline_pass2 = RID();
	tile_prefix_pipeline_pass3 = RID();
	tile_raster_pipeline = RID();
	cached_raster_framebuffer_format = RD::INVALID_ID;
	tile_raster_compute_pipeline = RID();
	tile_resolve_pipeline = RID();
	tile_binning_shader = RID();
	tile_binning_count_shader = RID();
	tile_prefix_shader = RID();
	tile_prefix_shader_pass2 = RID();
	tile_prefix_shader_pass3 = RID();
	tile_raster_shader = RID();
	tile_raster_compute_shader = RID();
	tile_resolve_shader = RID();
	tile_binning_shader_initialized = false;
	tile_prefix_shader_initialized = false;
	tile_raster_shader_initialized = false;
	tile_raster_compute_shader_initialized = false;
	tile_resolve_shader_initialized = false;
	subgroups_available = false;
	resolve_pipeline_initialized = false;
	quantized_storage_enabled = false;
	shader_defines_hash = 0;
	resolve_pipeline_format = RD::DATA_FORMAT_MAX;
	shader_device = nullptr;
	shader_device_instance = 0;
	tile_binning_shader_source = std::make_unique<TileBinningShaderRD>();
	tile_prefix_shader_source = std::make_unique<TilePrefixScanShaderRD>();
	tile_raster_shader_source = std::make_unique<TileRasterizerShaderRD>();
	tile_raster_compute_shader_source = std::make_unique<TileRasterizerComputeShaderRD>();
	tile_resolve_shader_source = std::make_unique<TileResolveShaderRD>();
}

void TileGlobalSortResources::release(RenderingDevice *p_default_device) {
	if (sorter.is_valid()) {
		sorter->shutdown();
		sorter.unref();
	}

	// ISSUE-010: Validate stored device pointer is still the same generation
	// before using it for cleanup. If the device was recycled, skip freeing
	// (the old device already cleaned up its resources on destruction).
	RenderingDevice *owner_device = nullptr;
	if (buffer_owner.device && buffer_owner.matches(buffer_owner.device)) {
		owner_device = buffer_owner.device;
	} else {
		owner_device = p_default_device;
	}
	if (owner_device) {
		if (keys_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, keys_buffer);
		}
		if (values_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, values_buffer);
		}
		for (RID &buffer : tile_counts_buffers) {
			safe_free_buffer_rid(owner_device, buffer);
		}
		if (tile_ranges_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, tile_ranges_buffer);
		}
		if (prefix_total_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, prefix_total_buffer);
		}
		if (indirect_dispatch_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, indirect_dispatch_buffer);
		}
		if (wg_sums_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, wg_sums_buffer);
		}
		if (wg_offsets_buffer.is_valid()) {
			safe_free_buffer_rid(owner_device, wg_offsets_buffer);
		}
	}

	reset_state(false);
}

void TileGlobalSortResources::reset_state(bool p_clear_sorter) {
	if (p_clear_sorter) {
		sorter.unref();
	}
	sorter_available = true;
	sorter_missing_logged = false;
	sorter_init_failure_count = 0;
	last_sorter_init_failure_frame = 0;
	sorter_recoveries = 0;
	sorter_grow_failure_count = 0;
	last_sorter_grow_failure_frame = 0;
	sorter_grow_recoveries = 0;
	sorter_grow_failure_logged = false;
	sorter_device_id = 0;
	capacity = 0;
	shrink_candidate_frames = 0;
	last_observed_demand = 0;
	key_config = SortKeyConfig();
	keys_buffer = RID();
	values_buffer = RID();
	tile_counts_buffers[0] = RID();
	tile_counts_buffers[1] = RID();
	tile_counts_index = 0;
	tile_counts_ready[0] = false;
	tile_counts_ready[1] = false;
	tile_counts_bytes = 0;
	tile_ranges_buffer = RID();
	prefix_total_buffer = RID();
	indirect_dispatch_buffer = RID();
	wg_sums_buffer = RID();
	wg_offsets_buffer = RID();
	buffer_owner.clear();
	tile_buffer_tiles = 0;
}

void TileGlobalSortResources::advance_tile_counts_buffer() {
	tile_counts_index ^= 1u;
}

void TileGlobalSortResources::mark_tile_counts_dirty() {
	tile_counts_ready[tile_counts_index] = false;
}

bool TileGlobalSortResources::ensure_tile_counts_ready(RenderingDevice *p_device) {
	if (!p_device || tile_counts_bytes == 0) {
		return false;
	}
	RID buffer = get_tile_counts_buffer();
	if (!buffer.is_valid()) {
		return false;
	}
	if (!tile_counts_ready[tile_counts_index]) {
		p_device->buffer_clear(buffer, 0, tile_counts_bytes);
		// The clear command is ordered before subsequent COUNT/EMIT dispatches on
		// this device. Avoid forcing submit+sync here because this path is hit
		// every frame and would introduce a hard CPU/GPU stall loop on local RDs.
		tile_counts_ready[tile_counts_index] = true;
	}
	return true;
}

void TileGlobalSortResources::prepare_next_tile_counts_buffer(RenderingDevice *p_device) {
	if (!p_device || tile_counts_bytes == 0) {
		return;
	}
	uint32_t next_index = tile_counts_index ^ 1u;
	// Do not mark pre-cleared buffers as ready across frames/devices.
	// Always force an explicit clear on the active submission path before COUNT/EMIT.
	tile_counts_ready[next_index] = false;
}

void TileGlobalSortResources::ensure_resources(uint32_t p_visible_count) {
	RenderingDevice *device = owner._get_resource_device();
	if (!device) {
		return;
	}

	// Keep allocation demand-driven based on current overlap estimates while still
	// respecting configured caps.
	uint32_t requested_elements = MAX<uint32_t>(p_visible_count, 1u);
	if (g_gpu_sorting_config.max_overlap_records > 0) {
		requested_elements = MIN<uint32_t>(requested_elements, g_gpu_sorting_config.max_overlap_records);
	}
	uint32_t attempt_elements = requested_elements;
	// NOTE: max_sort_elements is used by the instance/depth sorting pipeline.
	// Global tile-overlap records are governed by max_overlap_records.
	if (g_gpu_sorting_config.max_sort_elements > 0 && requested_elements > g_gpu_sorting_config.max_sort_elements) {
		static int overlap_vs_sort_budget_log_counter = 0;
		if (++overlap_vs_sort_budget_log_counter % 600 == 1) {
			GS_LOG_GPU_SORT_INFO(vformat(
					"[TileRenderer] Overlap budget (%d) exceeds max_sort_elements (%d); overlap path is capped by max_overlap_records only.",
					int(requested_elements), int(g_gpu_sorting_config.max_sort_elements)));
		}
	}
	bool attempted_fallback = false;

	auto free_global_sort_buffers = [&](RenderingDevice *p_owner, bool p_clear_owner) {
		if (p_owner) {
			if (keys_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, keys_buffer);
			}
			if (values_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, values_buffer);
			}
			for (RID &buffer : tile_counts_buffers) {
				safe_free_buffer_rid(p_owner, buffer);
			}
			if (tile_ranges_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, tile_ranges_buffer);
			}
			if (prefix_total_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, prefix_total_buffer);
			}
			if (indirect_dispatch_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, indirect_dispatch_buffer);
			}
			if (wg_sums_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, wg_sums_buffer);
			}
			if (wg_offsets_buffer.is_valid()) {
				safe_free_buffer_rid(p_owner, wg_offsets_buffer);
			}
		}
		keys_buffer = RID();
		values_buffer = RID();
		tile_counts_buffers[0] = RID();
		tile_counts_buffers[1] = RID();
		tile_counts_index = 0;
		tile_counts_ready[0] = false;
		tile_counts_ready[1] = false;
		tile_counts_bytes = 0;
		tile_ranges_buffer = RID();
		prefix_total_buffer = RID();
		indirect_dispatch_buffer = RID();
		wg_sums_buffer = RID();
		wg_offsets_buffer = RID();
		tile_buffer_tiles = 0;
		if (p_clear_owner) {
			buffer_owner.clear();
		}
	};

	for (;;) {
		bool sorter_recreated = false;

		SortKeyConfig desired_key_config = owner._get_effective_sort_key_config();
		bool key_config_changed = desired_key_config.key_bits != key_config.key_bits ||
				desired_key_config.tile_bits != key_config.tile_bits ||
				desired_key_config.depth_bits != key_config.depth_bits ||
				desired_key_config.enable_tie_breaker != key_config.enable_tie_breaker;

		// Bounded shrink (opt-in): when overlap demand stays well below the reserved
		// sort capacity for a wide hysteresis window, recreate the sorter at the lower
		// demand so the (up to >1 GB) key/value buffers are reclaimed. The hysteresis is
		// wide and resets on any demand recovery, so a shrink is bounded to views that hold
		// low overlap for the full window: a slow sweep that stays below the trigger for
		// >240 frames between high-demand views can still shrink and later re-grow — bounded,
		// not impossible (default-off makes the occasional realloc acceptable). Growth and
		// key-config changes keep their existing dedicated triggers below; this only adds a
		// shrink trigger.
		//
		// CRITICAL: the frame-start argument (p_visible_count) is only a coarse PRE-count
		// estimate; the true per-frame overlap demand is measured after the count/prefix
		// pass and stored in last_observed_demand. The estimate is systematically LOW for
		// zoomed-in views (close splats cover 100+ tiles each), so driving the streak off
		// the estimate alone would shrink while real demand is still high and then
		// immediately re-grow (with an expensive re-count). We therefore gate shrink on
		// MAX(estimate, last measured demand): the streak advances only when BOTH agree
		// demand is low, and any measured-high frame in the 240-frame window resets it.
		// Residual edge (accepted): a demand spike on the exact trigger frame shrinks off
		// the prior frame's low measurement, then the post-count grow path re-grows the
		// same frame — a single, self-correcting re-count, not sustained thrash. A pending-
		// shrink state machine would remove even that, at a complexity cost not worth it
		// for a default-off knob.
		uint32_t effective_demand = MAX<uint32_t>(attempt_elements, last_observed_demand);
		// Cap the demand to max_overlap_records: the prefix path clamps usable records to it,
		// so any capacity above the cap is dead VRAM. Clamping the demand here (together with
		// the forced cap-reduction shrink below) lets the shrink GATE fire AND the buffers
		// actually reclaim when the cap is lowered below the current allocation.
		const uint32_t overlap_hard_cap = g_gpu_sorting_config.get_overlap_records_hard_cap();
		if (overlap_hard_cap > 0u) {
			effective_demand = MIN(effective_demand, overlap_hard_cap);
		}
		bool wants_shrink = false;
		// The shrink is gated on a live sorter because it reclaims by RECREATING the sorter
		// at the lower capacity. On a GPU that lacks RadixSort indirect support the sorter is
		// disabled and every translucent global-composite frame is rejected (#586); the
		// key/value buffers then stay at their high-water capacity and are not reclaimed by
		// this path. That is an accepted limitation of the opt-in shrink on an already-degraded
		// (rejecting) fallback — reclaiming there would need a separate sorter-less
		// buffer-resize path, which is not worth the added complexity for that narrow,
		// low-capability-GPU case.
		if (g_gpu_sorting_config.bounded_buffer_shrink_enabled && sorter_available && sorter.is_valid() &&
				!key_config_changed && capacity > effective_demand) {
			// keys (1-2 words) + values (1 word), matching the real allocation below.
			const uint32_t bytes_per_element = (global_sort_key_stride_words(key_config) + 1u) * uint32_t(sizeof(uint32_t));
			const uint32_t current_bytes = uint32_t(MIN<uint64_t>(uint64_t(capacity) * bytes_per_element, uint64_t(UINT32_MAX)));
			const uint32_t required_bytes = uint32_t(MIN<uint64_t>(uint64_t(effective_demand) * bytes_per_element, uint64_t(UINT32_MAX)));
			const TileBufferShrinkPolicy policy{ 0u, 0u,
				TILE_SCRATCH_SHRINK_TRIGGER_PERCENT, TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES };
			const TileBufferResizePlan plan = tile_compute_buffer_resize_plan(
					required_bytes, current_bytes, shrink_candidate_frames, policy);
			shrink_candidate_frames = plan.next_shrink_candidate_frames;
			wants_shrink = plan.should_resize && plan.target_bytes < current_bytes;
			// A capacity above the configured cap is always unused (the prefix clamps records
			// to the cap), so a cap LOWERED below the allocation must reclaim even when measured
			// demand still sits above the policy's ~50% trigger. effective_demand is already
			// clamped to the cap, so forcing the shrink here targets the cap (or lower). This
			// is thrash-free: once capacity == cap the condition no longer holds.
			if (overlap_hard_cap > 0u && capacity > overlap_hard_cap) {
				wants_shrink = true;
			}
		} else {
			shrink_candidate_frames = 0;
		}

		// #586 PR 2: "no sorter" is a RETRY state, not a latch. Every failure below goes
		// through disable_sorter(), which counts it and stamps the frame; the gate
		// computed here decides whether THIS call may re-attempt creation. The policy
		// is the shared GPU-003 backoff (sort_fallback_policy.h): 60 frames after the
		// first failure, doubling per consecutive failure, capped at 1800 frames, and
		// never a permanent stop. Before the retry the delta is computed in uint32
		// modular arithmetic so a frame-serial wrap cannot bypass the window.
		const uint32_t frames_since_last_failure =
				uint32_t(owner.frame_state.current_frame_serial) - uint32_t(last_sorter_init_failure_frame);
		const bool sorter_retry_due = !sorter_available &&
				GaussianSplatting::should_attempt_sorter_init(sorter_init_failure_count, frames_since_last_failure);

		// #586 PR 3: a GROW or key-layout change with a LIVE sorter is a replacement, not a
		// teardown. The replacement is built first and the working sorter retired only on
		// success; on failure the working sorter, its capacity and its buffers stay, the
		// frame renders sorted at the old budget, and the grow is re-attempted on the same
		// GPU-003 backoff (its own counter, because frames keep publishing meanwhile, so the
		// published-frame episode close of PR 2 must not reset it). Same wrap-safe delta.
		const bool grow_wanted = sorter_available && sorter.is_valid() &&
				(capacity < attempt_elements || key_config_changed);
		const uint32_t frames_since_last_grow_failure =
				uint32_t(owner.frame_state.current_frame_serial) - uint32_t(last_sorter_grow_failure_frame);
		const bool grow_due = grow_wanted &&
				GaussianSplatting::should_attempt_sorter_init(sorter_grow_failure_count, frames_since_last_grow_failure);

		auto disable_sorter = [&](const char *p_reason) {
			if (sorter.is_valid()) {
				sorter->shutdown();
				sorter.unref();
			}
			const uint32_t new_capacity = MAX<uint32_t>(attempt_elements, 1u);
			// Reallocate the key/value/tile buffers only when their size or layout
			// changed. A failed RETRY at an unchanged capacity keeps the existing
			// buffers, so the frame keeps rejecting at the choke point instead of
			// churning hundreds of MB of VRAM once per probe interval.
			if (sorter_available || capacity != new_capacity || key_config_changed) {
				sorter_recreated = true;
			}
			sorter_available = false;
			key_config = desired_key_config;
			capacity = new_capacity;
			if (sorter_init_failure_count < UINT32_MAX) {
				sorter_init_failure_count++;
			}
			last_sorter_init_failure_frame = owner.frame_state.current_frame_serial;
			const uint64_t backoff_frames = GaussianSplatting::sorter_init_backoff_frames(sorter_init_failure_count);
			if (!sorter_missing_logged) {
				GS_LOG_WARN_DEFAULT(p_reason);
				sorter_missing_logged = true;
			}
			if (GaussianSplatting::sorter_init_degraded(sorter_init_failure_count)) {
				// Persistent degradation keeps re-surfacing in the log, once per probe
				// interval (the attempts are rate-limited by the backoff, so this is
				// bounded); mirrors refresh_gpu_sorter's NOT-one-shot-forever contract.
				GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Global composite sorter creation failed %d consecutive times - translucent frames stay REJECTED (#586); probing for recovery every %d frames",
						int(sorter_init_failure_count), int(backoff_frames)));
			} else {
				GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] Global composite sorter creation failed (attempt %d) - retrying in %d frames",
						int(sorter_init_failure_count), int(backoff_frames)));
			}
		};

		// A failed grow that cannot be re-attempted yet: the working sorter keeps
		// rendering at its capacity; the frame is sorted, records above the budget are
		// clamped by the prefix pass (overflow-drop telemetry), nothing is torn down.
		auto keep_sorter_after_failed_grow = [&](const char *p_reason) {
			if (sorter_grow_failure_count < UINT32_MAX) {
				sorter_grow_failure_count++;
			}
			last_sorter_grow_failure_frame = owner.frame_state.current_frame_serial;
			const uint64_t backoff_frames = GaussianSplatting::sorter_init_backoff_frames(sorter_grow_failure_count);
			if (!sorter_grow_failure_logged) {
				GS_LOG_WARN_DEFAULT(p_reason);
				sorter_grow_failure_logged = true;
			}
			if (GaussianSplatting::sorter_init_degraded(sorter_grow_failure_count)) {
				GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Global composite sort grow to %d failed %d consecutive times - keeping the working sorter at capacity %d; overlap records above it are dropped (sorted) until a grow succeeds; probing every %d frames (#586)",
						int(attempt_elements), int(sorter_grow_failure_count), int(capacity), int(backoff_frames)));
			} else {
				GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] Global composite sort grow to %d failed (attempt %d) - keeping the working sorter at capacity %d; overlap records above it are dropped (sorted); retrying in %d frames (#586)",
						int(attempt_elements), int(sorter_grow_failure_count), int(capacity), int(backoff_frames)));
			}
		};

		if (!sorter_available && !sorter_retry_due) {
			// In backoff: track the demand so the buffers stay sized for it, but do not
			// attempt creation this call.
			if (capacity < attempt_elements || key_config_changed) {
				key_config = desired_key_config;
				capacity = MAX<uint32_t>(attempt_elements, 1u);
				sorter_recreated = true;
			}
		} else if (grow_wanted && !grow_due) {
			// Grow in backoff: nothing to do -- the working sorter renders at its
			// capacity; capacity, key layout and buffers stay matched to it.
		} else if (sorter_retry_due || !sorter.is_valid() || grow_due || wants_shrink) {
			// Every replacement of a LIVE sorter -- the optional bounded shrink, a grow,
			// a key-layout change -- builds the replacement BEFORE retiring the working
			// one, so a failed build keeps a correct (if smaller or differently laid out)
			// sorter rendering instead of leaving no sorter at all (#586 PR 3; before it
			// only the shrink did this and a failed grow tore the working sorter down,
			// which since #976 meant every translucent frame rejected until the retry).
			// Only a MISSING sorter (first creation, or the no-sorter retry) has nothing
			// to keep; its failure goes through disable_sorter().
			const bool replacing_live_sorter = sorter.is_valid();
			const bool grow_or_relayout = replacing_live_sorter && !wants_shrink;

			SortKeyConfig config_to_use = desired_key_config;

			// On a shrink, recreate at the MEASURED demand (effective_demand), not the
			// low pre-count estimate — otherwise the freshly shrunk capacity would be
			// below real demand and the very next count pass would re-grow (thrash).
			// Growth/key-config recreation keeps the estimate, which it must reserve
			// ahead of the count. wants_shrink is mutually exclusive with the grow
			// triggers (it requires capacity > effective_demand >= attempt_elements).
			// effective_demand is already clamped to the overlap cap above, so the
			// shrink target never recreates above max_overlap_records.
			const uint32_t resize_elements = wants_shrink ? effective_demand : attempt_elements;

			// A failure with NO live sorter leaves none, so every translucent frame is
			// rejected at the choke point until the retry (#586 PR 2); a failed GROW or
			// key-layout change keeps the working sorter and backs the grow off (PR 3);
			// an optional-shrink failure keeps the working sorter and skips the reclaim.
			auto handle_recreate_failure = [&](const char *p_reason) {
				if (!replacing_live_sorter) {
					disable_sorter(p_reason);
				} else if (grow_or_relayout) {
					keep_sorter_after_failed_grow(p_reason);
				} else {
					WARN_PRINT_ONCE("[TileRenderer] Bounded overlap shrink could not build a smaller sorter; keeping the current sort capacity");
				}
			};

			// Global composite sort requires indirect support for GPU-driven element count.
			//
			// Allocation-free PREFLIGHTS come first (#586 PR 2). create_sorter() compiles
			// the four radix kernels through glslang and builds their pipelines BEFORE it
			// allocates a single buffer (RadixSort::initialize -> create_variant), so any
			// refusal it can only report afterwards costs a full compile. The two failure
			// classes a limits query can predict -- capability (workgroup size, storage
			// buffers per set, shared memory) and the RenderingDevice 32-bit buffer-size
			// limit at this capacity -- are decided here from the same single-source
			// helpers RadixSort itself uses, so a retry of a persistent config-induced
			// failure costs a few device-limit lookups, not a recompile. What remains for
			// create_sorter() to discover is a genuine allocation failure (retrying it is
			// the point) or a shader/pipeline build failure (which nothing cheaper can
			// predict; see the deferral note in sort_fallback_policy.h).
			if (!GPUSorterFactory::probe_supports_indirect(GPUSorterFactory::ALGORITHM_RADIX, device)) {
				handle_recreate_failure("[TileRenderer] Global composite sort requires RadixSort indirect support; translucent frames will be REJECTED (nothing presented) until a sorter exists (#586)");
			} else if (!GPUSortingConstants::sort_path_allocation_fits_device_size(uint64_t(resize_elements),
							   g_gpu_sorting_config.radix_bits, g_gpu_sorting_config.workgroup_size, config_to_use.key_bits)) {
				const String size_reason = vformat(
						"[TileRenderer] Global composite sort capacity %d needs a sort buffer above the RenderingDevice 32-bit size limit at radix_bits=%d workgroup_size=%d key_bits=%d; no sorter can be built for it and translucent frames will be REJECTED (nothing presented) until the demand or the configuration changes (#586)",
						int(resize_elements), int(g_gpu_sorting_config.radix_bits), int(g_gpu_sorting_config.workgroup_size), int(config_to_use.key_bits));
				handle_recreate_failure(size_reason.utf8().get_data());
			} else {
				Ref<IGPUSorter> created_sorter = GPUSorterFactory::create_sorter(
						GPUSorterFactory::ALGORITHM_RADIX, device, resize_elements, config_to_use);
				if (!created_sorter.is_valid()) {
					handle_recreate_failure("[TileRenderer] Failed to create global composite GPU sorter; translucent frames will be REJECTED (nothing presented) until a sorter exists (#586)");
				} else if (!created_sorter->supports_indirect()) {
					created_sorter->shutdown();
					handle_recreate_failure("[TileRenderer] Created sorter does not support indirect sorting; translucent frames will be REJECTED (nothing presented) until a sorter exists (#586)");
				} else {
					// Replacement is good. The old sorter (shrink, grow or relayout) is still
					// live; retire it only now that the replacement is known to initialize.
					if (sorter.is_valid()) {
						sorter->shutdown();
						sorter.unref();
					}
					sorter = created_sorter;
					key_config = desired_key_config;
					const uint32_t previous_capacity = capacity;
					capacity = sorter->get_max_elements();
					sorter_available = true;
					if (grow_or_relayout && sorter_grow_failure_count > 0) {
						// The pending grow succeeded: this episode's frames were sorted at the
						// old budget throughout, so the grow's own success is its recovery
						// (unlike the no-sorter episode, which closes on a published frame).
						GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] Global composite sort grow succeeded after %d failed attempt(s): capacity %d -> %d (#586)",
								int(sorter_grow_failure_count), int(previous_capacity), int(capacity)));
						sorter_grow_failure_count = 0;
						sorter_grow_recoveries++;
						sorter_grow_failure_logged = false;
					}
					// NOTE: a non-zero sorter_init_failure_count is NOT cleared here. The sorter
					// coming back is necessary but not sufficient: the key/value/tile buffers
					// below can still fail under the same pressure, and then the frame is
					// rejected at the pre-binning check. The episode closes at the end of this
					// function, once every buffer the frame needs has been verified (#977 review).
					GS_LOG_INFO_DEFAULT(vformat("[TileRenderer] Global sort capacity initialized: %d (config max_overlap_records=%d)",
						int(capacity), int(g_gpu_sorting_config.max_overlap_records)));
					if (capacity < resize_elements) {
						WARN_PRINT_ONCE(vformat("[TileRenderer] Global sort capacity capped (requested=%d, actual=%d)",
							int(resize_elements), int(capacity)));
					}
					// Free and reallocate the key/value/tile buffers only when their size or
					// layout actually changed. A retry that succeeds at the capacity and key
					// layout the buffers were already sized for (the shape a transient failure
					// recovers in) keeps them: reallocating hundreds of MB under the same
					// pressure that caused the failure would turn the recovery into another
					// rejected frame (#977 review round 2). Device changes are handled by the
					// buffer_owner checks below, independently of this flag.
					if (capacity != previous_capacity || key_config_changed) {
						sorter_recreated = true;
					}
				}
			}
		}

		if (capacity == 0) {
			return;
		}
		if (sorter_available && !sorter.is_valid()) {
			return;
		}

		bool buffers_recreated = false;

		if (sorter_recreated) {
			// BUF-10 FIX: Free all global sort buffers using the correct owner device.
			// This includes tile_counts and tile_ranges which were previously missing.
			RenderingDevice *owner_device = buffer_owner.device ? buffer_owner.device : device;
			free_global_sort_buffers(owner_device, false);
			buffers_recreated = true;
		}

		if (buffer_owner.device && !buffer_owner.matches(device)) {
			RenderingDevice *owner_device = buffer_owner.device;
			free_global_sort_buffers(owner_device, true);
			buffers_recreated = true;
		}

		const uint32_t key_stride_words = global_sort_key_stride_words(key_config);
		const uint64_t keys_bytes64 = uint64_t(capacity) * sizeof(uint32_t) * key_stride_words;
		const uint64_t values_bytes64 = uint64_t(capacity) * sizeof(uint32_t);
		if (keys_bytes64 > UINT32_MAX || values_bytes64 > UINT32_MAX) {
			GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Global composite sort buffers exceed RD limits (keys=%s bytes, values=%s bytes)",
					String::num_uint64(keys_bytes64),
					String::num_uint64(values_bytes64)));
			if (buffers_recreated) {
				// Buffers were freed above but we bail before recreating; drop cached
				// descriptor sets that still reference the freed RIDs.
				owner._invalidate_descriptor_cache();
			}
			return;
		}
		const uint32_t keys_bytes = uint32_t(keys_bytes64);
		const uint32_t values_bytes = uint32_t(values_bytes64);

		if (!keys_buffer.is_valid() || !buffer_owner.matches(device)) {
			keys_buffer = device->storage_buffer_create(keys_bytes);
			if (keys_buffer.is_valid()) {
				device->set_resource_name(keys_buffer, "GS_TileRenderer_OverlapKeysBuffer");
			}
			buffers_recreated = true;
		}

		if (!values_buffer.is_valid() || !buffer_owner.matches(device)) {
			values_buffer = device->storage_buffer_create(values_bytes);
			if (values_buffer.is_valid()) {
				device->set_resource_name(values_buffer, "GS_TileRenderer_OverlapValuesBuffer");
			}
			buffers_recreated = true;
		}

		// Guard: Don't create tile buffers if grid_state.total_tiles is 0 (prevents size-0 buffer creation errors).
		// Must set owner and invalidate caches to prevent leaking the key/value buffers created above.
		if (owner.grid_state.total_tiles == 0) {
			buffer_owner.set(device);
			if (buffers_recreated) {
				owner._invalidate_descriptor_cache();
			}
			return;
		}

		if (tile_buffer_tiles != owner.grid_state.total_tiles || !buffer_owner.matches(device) ||
				!tile_counts_buffers[0].is_valid() || !tile_counts_buffers[1].is_valid() ||
				!tile_ranges_buffer.is_valid() || !prefix_total_buffer.is_valid() ||
				!indirect_dispatch_buffer.is_valid() || !wg_sums_buffer.is_valid() || !wg_offsets_buffer.is_valid()) {
			// BUF-10 FIX: Use the original owner device to free buffers to prevent device mismatch leaks.
			// The buffers may have been created on a different device than the current one.
			RenderingDevice *free_device = buffer_owner.device ? buffer_owner.device : device;
			for (RID &buffer : tile_counts_buffers) {
				safe_free_buffer_rid(free_device, buffer);
			}
			if (tile_ranges_buffer.is_valid()) {
				safe_free_buffer_rid(free_device, tile_ranges_buffer);
			}
			if (prefix_total_buffer.is_valid()) {
				safe_free_buffer_rid(free_device, prefix_total_buffer);
			}
			if (indirect_dispatch_buffer.is_valid()) {
				safe_free_buffer_rid(free_device, indirect_dispatch_buffer);
			}
			if (wg_sums_buffer.is_valid()) {
				safe_free_buffer_rid(free_device, wg_sums_buffer);
			}
			if (wg_offsets_buffer.is_valid()) {
				safe_free_buffer_rid(free_device, wg_offsets_buffer);
			}

			const uint64_t counts_bytes64 = uint64_t(owner.grid_state.total_tiles) * sizeof(uint32_t);
			const uint64_t ranges_bytes64 = uint64_t(owner.grid_state.total_tiles) * sizeof(uint32_t) * 2u;
			if (counts_bytes64 > UINT32_MAX || ranges_bytes64 > UINT32_MAX) {
				GS_LOG_ERROR_DEFAULT(vformat("[TileRenderer] Global tile buffers exceed RD limits (counts=%s bytes, ranges=%s bytes)",
						String::num_uint64(counts_bytes64),
						String::num_uint64(ranges_bytes64)));
				// The tile buffers were just freed above; invalidate so no cached descriptor
				// set keeps a dangling reference to them when we bail here.
				owner._invalidate_descriptor_cache();
				return;
			}

			const uint32_t counts_bytes = uint32_t(counts_bytes64);
			const uint32_t ranges_bytes = uint32_t(ranges_bytes64);
			tile_counts_buffers[0] = device->storage_buffer_create(counts_bytes);
			tile_counts_buffers[1] = device->storage_buffer_create(counts_bytes);
			tile_ranges_buffer = device->storage_buffer_create(ranges_bytes);
			prefix_total_buffer = device->storage_buffer_create(sizeof(uint32_t));
			// Indirect dispatch buffer layout matches IndirectDispatchLayout.
			indirect_dispatch_buffer = device->storage_buffer_create(sizeof(GaussianSplatting::IndirectDispatchLayout),
					Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
			uint32_t wg_count = (owner.grid_state.total_tiles + TileRenderer::BINNING_GROUP_SIZE - 1) / TileRenderer::BINNING_GROUP_SIZE;
			uint32_t wg_bytes = MAX<uint32_t>(wg_count, 1u) * sizeof(uint32_t);
			wg_sums_buffer = device->storage_buffer_create(wg_bytes);
			wg_offsets_buffer = device->storage_buffer_create(wg_bytes);

			// Name the newly created buffers for RenderDoc visibility.
			if (tile_counts_buffers[0].is_valid()) {
				device->set_resource_name(tile_counts_buffers[0], "GS_TileRenderer_TileCountBufferA");
			}
			if (tile_counts_buffers[1].is_valid()) {
				device->set_resource_name(tile_counts_buffers[1], "GS_TileRenderer_TileCountBufferB");
			}
			if (tile_ranges_buffer.is_valid()) {
				device->set_resource_name(tile_ranges_buffer, "GS_TileRenderer_TileRangeBuffer");
			}
			if (prefix_total_buffer.is_valid()) {
				device->set_resource_name(prefix_total_buffer, "GS_TileRenderer_PrefixTotalBuffer");
			}
			if (indirect_dispatch_buffer.is_valid()) {
				device->set_resource_name(indirect_dispatch_buffer, "GS_TileRenderer_IndirectDispatchBuffer");
			}
			if (wg_sums_buffer.is_valid()) {
				device->set_resource_name(wg_sums_buffer, "GS_TileRenderer_WorkgroupSumsBuffer");
			}
			if (wg_offsets_buffer.is_valid()) {
				device->set_resource_name(wg_offsets_buffer, "GS_TileRenderer_WorkgroupOffsetsBuffer");
			}

			// Zero-initialize both ping-pong tile-count buffers so that the
			// "previous" buffer read by hotspot-aware pruning returns 0 (no
			// pressure) on the first frame and after reallocation, instead of
			// uninitialized data that could trigger spurious pruning.
			if (tile_counts_buffers[0].is_valid()) {
				device->buffer_clear(tile_counts_buffers[0], 0, counts_bytes);
			}
			if (tile_counts_buffers[1].is_valid()) {
				device->buffer_clear(tile_counts_buffers[1], 0, counts_bytes);
			}

			tile_buffer_tiles = owner.grid_state.total_tiles;
			tile_counts_bytes = counts_bytes;
			tile_counts_index = 0;
			tile_counts_ready[0] = false;
			tile_counts_ready[1] = false;
			buffers_recreated = true;
		}

		// Every buffer a frame binds must be live before this call may report success:
		// the prefix stage's uniform set and sort_indirect_async() both require the
		// indirect-dispatch buffer, which this check used to omit (#977 review round 2).
		if (!keys_buffer.is_valid() || !values_buffer.is_valid() ||
				!tile_counts_buffers[0].is_valid() || !tile_counts_buffers[1].is_valid() ||
				!tile_ranges_buffer.is_valid() || !prefix_total_buffer.is_valid() ||
				!indirect_dispatch_buffer.is_valid() ||
				!wg_sums_buffer.is_valid() || !wg_offsets_buffer.is_valid()) {
			GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate global composite sort buffers");
			if (!attempted_fallback && attempt_elements > 1u) {
				uint32_t reduced = MAX<uint32_t>(attempt_elements / 2u, 1u);
				if (reduced < attempt_elements) {
					GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] Retrying global composite sort allocation at reduced capacity: %u -> %u",
							attempt_elements, reduced));
					if (sorter.is_valid()) {
						sorter->shutdown();
						sorter.unref();
					}
					capacity = 0;
					RenderingDevice *owner_device = buffer_owner.device ? buffer_owner.device : device;
					free_global_sort_buffers(owner_device, true);
					attempted_fallback = true;
					attempt_elements = reduced;
					continue;
				}
			}
			if (buffers_recreated) {
				// Buffers were freed during recreate but the new allocation failed; drop any
				// cached descriptor sets that still reference the now-freed sort buffers so a
				// later frame cannot bind a dangling RID (the success path invalidates below).
				owner._invalidate_descriptor_cache();
			}
			return;
		}

		buffer_owner.set(device);

		if (buffers_recreated) {
			owner._invalidate_descriptor_cache();
		}
		// NOTE: a pending failure episode is NOT closed here either. Resources being live
		// is still not a presented frame: the count/emit uniform sets, the tile-range build
		// and the raster stage can all fail after this point. The episode closes in
		// note_sorted_frame_published(), called by the frame executor only once a sorted
		// frame has actually been published (#977 review round 3).
		return;
	}
}

void TileGlobalSortResources::note_sorted_frame_published() {
	if (sorter_init_failure_count == 0) {
		return;
	}
	GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] Global composite sorter recovered after %d failed attempt(s): a sorted frame was published again (#586)",
			int(sorter_init_failure_count)));
	sorter_init_failure_count = 0;
	sorter_recoveries++;
	// The one-shot guard for disable_sorter()'s root-cause line is per EPISODE, not per
	// process: a later, independent failure must log its own cause, otherwise the log
	// reports the first episode and goes quiet while the counters keep moving (#977
	// review round 4). Cleared here, at the only episode boundary.
	sorter_missing_logged = false;
}

void TileUniformBuffers::release(RenderingDevice *p_default_device) {
	if (param_uniform_buffer.is_valid()) {
		RenderingDevice *owner_device = param_uniform_buffer_owner.device ? param_uniform_buffer_owner.device : p_default_device;
		safe_free_buffer_rid(owner_device, param_uniform_buffer);
	}
	if (prefix_param_uniform_buffer.is_valid()) {
		RenderingDevice *owner_device = prefix_param_owner.device ? prefix_param_owner.device : p_default_device;
		safe_free_buffer_rid(owner_device, prefix_param_uniform_buffer);
	}
	if (default_state_uniform_buffer.is_valid()) {
		RenderingDevice *owner_device = default_state_uniform_owner ? default_state_uniform_owner : p_default_device;
		safe_free_buffer_rid(owner_device, default_state_uniform_buffer);
	}

	reset_state();
}

void TileUniformBuffers::reset_state() {
	param_uniform_buffer = RID();
	param_uniform_buffer_owner.clear();
	prefix_param_uniform_buffer = RID();
	prefix_param_owner.clear();
	default_state_uniform_buffer = RID();
	default_state_uniform_owner = nullptr;
	owner.last_param_hash_valid = false;
	owner.last_param_uniform_buffer = RID();
}

bool TileUniformBuffers::ensure_param_buffer(RenderingDevice *p_device) {
	ERR_FAIL_NULL_V(p_device, false);

	if (param_uniform_buffer.is_valid() && param_uniform_buffer_owner.matches(p_device)) {
		return true;
	}

	if (param_uniform_buffer.is_valid() && param_uniform_buffer_owner.device && !param_uniform_buffer_owner.matches(p_device)) {
		safe_free_buffer_rid(param_uniform_buffer_owner.device, param_uniform_buffer);
	}

	Vector<uint8_t> zero_data;
	zero_data.resize(sizeof(TileRenderParamsGPU));
	zero_data.fill(0);
	param_uniform_buffer = p_device->uniform_buffer_create(zero_data.size(), zero_data);
	if (!param_uniform_buffer.is_valid()) {
		return false;
	}
	p_device->set_resource_name(param_uniform_buffer, "GS_TileRenderer_ParamsBuffer");

	param_uniform_buffer_owner.set(p_device);
	owner._invalidate_descriptor_cache();
	return true;
}

RID TileUniformBuffers::get_default_state_uniform(RenderingDevice *p_device) {
	ERR_FAIL_NULL_V(p_device, RID());

	if (default_state_uniform_buffer.is_valid() && default_state_uniform_owner == p_device) {
		return default_state_uniform_buffer;
	}

	if (default_state_uniform_buffer.is_valid() && default_state_uniform_owner && default_state_uniform_owner != p_device) {
		safe_free_buffer_rid(default_state_uniform_owner, default_state_uniform_buffer);
	}

	InteractiveStateUniforms default_state;
	Vector<uint8_t> state_data;
	// #798: unchecked by the compile-time-constant-count rule (gs_vector_alloc.h).
	state_data.resize(sizeof(InteractiveStateUniforms));
	std::memcpy(state_data.ptrw(), &default_state, sizeof(InteractiveStateUniforms));
	default_state_uniform_buffer = p_device->uniform_buffer_create(state_data.size(), state_data);
	if (default_state_uniform_buffer.is_valid()) {
		p_device->set_resource_name(default_state_uniform_buffer, "GS_TileRenderer_InteractiveStateBuffer");
	}
	default_state_uniform_owner = p_device;
	return default_state_uniform_buffer;
}

bool TileUniformBuffers::ensure_prefix_param_buffer(RenderingDevice *p_device, uint32_t p_size) {
	if (prefix_param_uniform_buffer.is_valid() && prefix_param_owner.matches(p_device)) {
		return true;
	}

	if (prefix_param_uniform_buffer.is_valid() && prefix_param_owner.device && !prefix_param_owner.matches(p_device)) {
		safe_free_buffer_rid(prefix_param_owner.device, prefix_param_uniform_buffer);
	}
	prefix_param_uniform_buffer = p_device->uniform_buffer_create(p_size);
	if (prefix_param_uniform_buffer.is_valid()) {
		p_device->set_resource_name(prefix_param_uniform_buffer, "GS_TileRenderer_PrefixScanParamsBuffer");
	}
	prefix_param_owner.set(p_device);
	return prefix_param_uniform_buffer.is_valid();
}

} // namespace GaussianSplatting
