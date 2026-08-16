// Phase 9: Output compositor extracted from GaussianSplatRenderer
// Handles framebuffer caching, viewport blitting, and final output composition

#include "output_compositor.h"
#include "gs_scene_depth_linearize.h"
#include "../logger/gs_logger.h"
#include "../core/gaussian_splat_manager.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/gpu_debug_utils.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "../renderer/shader_compilation_helper.h"
#include "core/crypto/crypto_core.h"
#include "../shaders/viewport_blit.glsl.gen.h"

using GaussianSplatting::ScopedGpuMarker;

namespace {
constexpr char GS_COMPOSITE_DEPTH_TEST_SETTING[] = "rendering/gaussian_splatting/composite/depth_test";
constexpr bool GS_COMPOSITE_DEPTH_TEST_DEFAULT = true;
constexpr char GS_SCENE_COMPOSITE_DEPTH_POLICY_SETTING[] = "rendering/gaussian_splatting/composite/scene_depth_policy";

enum GSSceneCompositeDepthPolicy {
    GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT = 0,
    GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED = 1,
};

constexpr GSSceneCompositeDepthPolicy GS_SCENE_COMPOSITE_DEPTH_POLICY_DEFAULT = GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT;

bool gs_get_composite_depth_test_enabled() {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings || !project_settings->has_setting(GS_COMPOSITE_DEPTH_TEST_SETTING)) {
        return GS_COMPOSITE_DEPTH_TEST_DEFAULT;
    }

    return (bool)project_settings->get_setting_with_override(GS_COMPOSITE_DEPTH_TEST_SETTING);
}

GSSceneCompositeDepthPolicy gs_get_scene_composite_depth_policy() {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    if (!project_settings || !project_settings->has_setting(GS_SCENE_COMPOSITE_DEPTH_POLICY_SETTING)) {
        return GS_SCENE_COMPOSITE_DEPTH_POLICY_DEFAULT;
    }

    Variant value = project_settings->get_setting_with_override(GS_SCENE_COMPOSITE_DEPTH_POLICY_SETTING);
    switch (value.get_type()) {
        case Variant::BOOL: {
            return value.operator bool() ? GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED : GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT;
        }
        case Variant::INT: {
            int64_t raw = value.operator int64_t();
            return raw >= GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED ? GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED : GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT;
        }
        case Variant::FLOAT: {
            double raw = value.operator double();
            return raw >= double(GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED) ? GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED : GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT;
        }
        case Variant::STRING:
        case Variant::STRING_NAME: {
            String policy = ((String)value).strip_edges().to_lower();
            if (policy == "relaxed") {
                return GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED;
            }
            if (policy == "strict") {
                return GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT;
            }
            break;
        }
        default:
            break;
    }

    return GS_SCENE_COMPOSITE_DEPTH_POLICY_DEFAULT;
}

void gs_log_scene_depth_contract_skip_once(bool p_missing_source_depth, bool p_missing_destination_depth) {
    static bool warned_once = false;
    if (warned_once) {
        return;
    }
    warned_once = true;

    String missing_inputs = "source_depth";
    if (p_missing_source_depth && p_missing_destination_depth) {
        missing_inputs = "source_depth + scene_depth";
    } else if (p_missing_destination_depth) {
        missing_inputs = "scene_depth";
    }

    GS_LOG_WARN(gs_logger::Category::COMPOSITOR,
            vformat("[OutputCompositor] Scene compositing skipped: missing required %s while strict depth policy is active. "
                    "Set %s=1 (or 'relaxed') to allow no-depth blend fallback.",
                    missing_inputs, GS_SCENE_COMPOSITE_DEPTH_POLICY_SETTING));
}
} // namespace

void OutputCompositor::_bind_methods() {
    // Bind methods for script access if needed
}

OutputCompositor::OutputCompositor() {
}

OutputCompositor::~OutputCompositor() {
    shutdown();
}

Error OutputCompositor::initialize(RenderingDevice *p_device) {
    if (!p_device) {
        return ERR_INVALID_PARAMETER;
    }

    rd = p_device;
    initialized = true;
    return OK;
}

void OutputCompositor::shutdown() {
    clear_cached_framebuffers();
    clear_viewport_blit_resources();

    if (viewport_blit_shader_source) {
        delete viewport_blit_shader_source;
        viewport_blit_shader_source = nullptr;
    }
    viewport_blit_shader_source_initialized = false;

    final_render_texture = RID();
    output_cache = OutputCacheState();
    cached_render_reuse_enabled = true;

    initialized = false;
    rd = nullptr;
}

void OutputCompositor::invalidate_cached_render() {
    output_cache.cached_render_valid = false;
    output_cache.cached_render_depth = RID();
    output_cache.cached_render_content_generation = 0;
    output_cache.cached_render_cull_config_signature = 0;
    output_cache.cached_render_color_grading_signature = 0;
    output_cache.cached_render_lighting_signature = 0;
}

bool OutputCompositor::_is_depth_texture_valid(const RID &p_depth_texture) const {
    if (!p_depth_texture.is_valid()) {
        return false;
    }
    if (rd && !rd->texture_is_valid(p_depth_texture)) {
        return false;
    }
    return true;
}

bool OutputCompositor::can_reuse_cached_render(const Transform3D &p_world_to_camera_to_world_transform, const Projection &p_projection,
        const Size2i &p_viewport_size, bool p_painterly_active, const RID &p_final_render_texture,
        uint64_t p_content_generation,
        uint64_t p_cull_config_signature,
        uint64_t p_color_grading_signature, uint64_t p_lighting_signature,
        bool p_require_valid_depth) const {
    if (!cached_render_reuse_enabled) {
        return false;
    }
    if (!output_cache.cached_render_valid || !output_cache.has_valid_render || !p_final_render_texture.is_valid()) {
        return false;
    }
    if (output_cache.cached_render_painterly != p_painterly_active) {
        return false;
    }
    if (output_cache.cached_render_viewport_size != p_viewport_size) {
        return false;
    }
    if (output_cache.cached_render_camera_projection != p_projection) {
        return false;
    }
    if (!output_cache.cached_render_camera_to_world_transform.is_equal_approx(p_world_to_camera_to_world_transform)) {
        return false;
    }
    if (output_cache.cached_render_content_generation != p_content_generation) {
        return false;
    }
    if (output_cache.cached_render_cull_config_signature != p_cull_config_signature) {
        return false;
    }
    if (output_cache.cached_render_color_grading_signature != p_color_grading_signature) {
        return false;
    }
    if (output_cache.cached_render_lighting_signature != p_lighting_signature) {
        return false;
    }
    if (p_require_valid_depth && !_is_depth_texture_valid(output_cache.cached_render_depth)) {
        return false;
    }
    return true;
}

void OutputCompositor::update_render_cache_signature(const Transform3D &p_world_to_camera_to_world_transform, const Projection &p_projection,
        const Size2i &p_viewport_size, bool p_painterly_active, const RID &p_cached_depth,
        const Size2i &p_internal_size, const RID &p_final_render_texture,
        uint64_t p_content_generation,
        uint64_t p_cull_config_signature,
        uint64_t p_color_grading_signature, uint64_t p_lighting_signature,
        bool p_require_valid_depth) {
    const bool cached_depth_valid = _is_depth_texture_valid(p_cached_depth);
    output_cache.cached_render_camera_to_world_transform = p_world_to_camera_to_world_transform;
    output_cache.cached_render_camera_projection = p_projection;
    output_cache.cached_render_viewport_size = p_viewport_size;
    output_cache.cached_render_internal_size = p_internal_size;
    output_cache.cached_render_painterly = p_painterly_active;
    output_cache.cached_render_depth = cached_depth_valid ? p_cached_depth : RID();
    output_cache.cached_render_valid = p_final_render_texture.is_valid() && (!p_require_valid_depth || cached_depth_valid);
    output_cache.cached_render_content_generation = p_content_generation;
    output_cache.cached_render_cull_config_signature = p_cull_config_signature;
    output_cache.cached_render_color_grading_signature = p_color_grading_signature;
    output_cache.cached_render_lighting_signature = p_lighting_signature;
}

void OutputCompositor::test_reset_last_viewport_copy_state() {
    output_cache.last_viewport_copy_success = false;
    output_cache.last_viewport_copy_source_size = Size2i();
    output_cache.last_viewport_copy_dest_size = Size2i();
    output_cache.last_output_copy_attempted = false;
    output_cache.last_output_copy_success = false;
    output_cache.last_output_copy_error = String();
    output_cache.last_depth_test_honored = true;
    output_cache.last_copy_degraded = false;
    output_cache.last_copy_degradation_reason = String();
    output_cache.last_strict_depth_contract_required = false;
}

void OutputCompositor::set_device_manager(Ref<RenderDeviceManager> p_device_manager) {
    device_manager = p_device_manager;
}

// Helper methods delegating to device manager or callbacks
RD::TextureFormat OutputCompositor::_get_texture_format(RenderingDevice *p_device, RID p_texture) const {
    if (texture_format_callback) {
        return texture_format_callback(p_device, p_texture);
    }

    if (!p_texture.is_valid() || !p_device) {
        return RD::TextureFormat();
    }

    if (p_device->texture_is_valid(p_texture)) {
        return p_device->texture_get_format(p_texture);
    }

    return RD::TextureFormat();
}

bool OutputCompositor::_is_texture_srgb(RenderingDevice *p_device, RID p_texture) {
    if (!p_texture.is_valid() || !p_device) {
        return false;
    }

    const uint64_t key = p_texture.get_id();
    if (bool *cached = srgb_format_cache.getptr(key)) {
        if (p_device->texture_is_valid(p_texture)) {
            return *cached;
        }
        srgb_format_cache.erase(key);
    }

    if (!p_device->texture_is_valid(p_texture)) {
        return false;
    }

    RD::TextureFormat format = _get_texture_format(p_device, p_texture);
    bool srgb = _is_srgb_format(format.format);
    srgb_format_cache.insert(key, srgb);
    return srgb;
}

void OutputCompositor::_track_resource(const RID &p_rid, RenderingDevice *p_device, bool p_owned, const char *p_label) {
    if (device_manager.is_valid()) {
        device_manager->track_resource(p_rid, p_device, p_owned, p_label);
    }
}

void OutputCompositor::_forget_resource(const RID &p_rid) {
    if (device_manager.is_valid()) {
        device_manager->forget_resource(p_rid);
    }
}

void OutputCompositor::_free_tracked_resource(RenderingDevice *p_fallback_device, RID &r_rid, TrackedResourceKind p_kind) {
    if (!r_rid.is_valid()) {
        return;
    }

    if (device_manager.is_valid()) {
        if (device_manager->has_tracked_resource(r_rid)) {
            device_manager->free_owned_resource(p_fallback_device, r_rid);
            return;
        }
    }

    if (p_fallback_device) {
        bool resource_valid = false;
        switch (p_kind) {
            case TrackedResourceKind::Texture:
                resource_valid = p_fallback_device->texture_is_valid(r_rid);
                break;
            case TrackedResourceKind::Framebuffer:
                resource_valid = p_fallback_device->framebuffer_is_valid(r_rid);
                break;
            case TrackedResourceKind::ComputePipeline:
                resource_valid = p_fallback_device->compute_pipeline_is_valid(r_rid);
                break;
            case TrackedResourceKind::Shader:
            case TrackedResourceKind::Sampler:
                // RenderingDevice exposes no shader/sampler validity query. These
                // resources are normally ledger-tracked; untracked fallback keeps
                // the previous best-effort behavior for legacy cached entries.
                resource_valid = true;
                break;
        }
        if (resource_valid) {
            p_fallback_device->free(r_rid);
        }
    }
    r_rid = RID();
}

// Static helper methods
bool OutputCompositor::_is_depth_attachment_format(RD::DataFormat p_format) {
    switch (p_format) {
        case RD::DATA_FORMAT_D16_UNORM:
        case RD::DATA_FORMAT_X8_D24_UNORM_PACK32:
        case RD::DATA_FORMAT_D32_SFLOAT:
        case RD::DATA_FORMAT_S8_UINT:
        case RD::DATA_FORMAT_D16_UNORM_S8_UINT:
        case RD::DATA_FORMAT_D24_UNORM_S8_UINT:
        case RD::DATA_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            break;
    }
    return false;
}

bool OutputCompositor::_is_srgb_format(RD::DataFormat p_format) {
    switch (p_format) {
        case RD::DATA_FORMAT_R8_SRGB:
        case RD::DATA_FORMAT_R8G8_SRGB:
        case RD::DATA_FORMAT_R8G8B8_SRGB:
        case RD::DATA_FORMAT_B8G8R8_SRGB:
        case RD::DATA_FORMAT_R8G8B8A8_SRGB:
        case RD::DATA_FORMAT_B8G8R8A8_SRGB:
        case RD::DATA_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            break;
    }
    return false;
}

// Map an SRGB data format to its UNORM equivalent so that sampling does not
// auto-decode SRGB → linear. Used by the scratch-copy path to avoid double-
// decoding the destination (sampler decode + shader srgb_to_linear). For
// non-SRGB inputs the format is returned unchanged.
static RD::DataFormat _canonical_storage_format(RD::DataFormat p_format) {
    switch (p_format) {
        case RD::DATA_FORMAT_R8_SRGB:
            return RD::DATA_FORMAT_R8_UNORM;
        case RD::DATA_FORMAT_R8G8_SRGB:
            return RD::DATA_FORMAT_R8G8_UNORM;
        case RD::DATA_FORMAT_R8G8B8_SRGB:
            return RD::DATA_FORMAT_R8G8B8_UNORM;
        case RD::DATA_FORMAT_B8G8R8_SRGB:
            return RD::DATA_FORMAT_B8G8R8_UNORM;
        case RD::DATA_FORMAT_R8G8B8A8_SRGB:
            return RD::DATA_FORMAT_R8G8B8A8_UNORM;
        case RD::DATA_FORMAT_B8G8R8A8_SRGB:
            return RD::DATA_FORMAT_B8G8R8A8_UNORM;
        case RD::DATA_FORMAT_A8B8G8R8_SRGB_PACK32:
            return RD::DATA_FORMAT_A8B8G8R8_UNORM_PACK32;
        default:
            return p_format;
    }
}

String OutputCompositor::_attachment_usage_label(bool p_is_depth) {
    return p_is_depth ? String("DEPTH_STENCIL_ATTACHMENT") : String("COLOR_ATTACHMENT");
}

bool OutputCompositor::_determine_viewport_blit_format(RD::DataFormat p_format, ViewportBlitFormat &r_format) {
    switch (p_format) {
        case RD::DATA_FORMAT_R8G8B8A8_UNORM:
        case RD::DATA_FORMAT_B8G8R8A8_UNORM:
        case RD::DATA_FORMAT_A8B8G8R8_UNORM_PACK32:
        case RD::DATA_FORMAT_R8G8B8A8_SRGB:
        case RD::DATA_FORMAT_B8G8R8A8_SRGB:
        case RD::DATA_FORMAT_A8B8G8R8_SRGB_PACK32:
            r_format = ViewportBlitFormat::RGBA8;
            return true;
        case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
            r_format = ViewportBlitFormat::RGBA16F;
            return true;
        case RD::DATA_FORMAT_R32G32B32A32_SFLOAT:
            r_format = ViewportBlitFormat::RGBA32F;
            return true;
        default:
            break;
    }
    return false;
}

const char *OutputCompositor::_viewport_blit_define(ViewportBlitFormat p_format) {
    switch (p_format) {
        case ViewportBlitFormat::RGBA8:
            return "#define VIEWPORT_BLIT_FORMAT 0";
        case ViewportBlitFormat::RGBA16F:
            return "#define VIEWPORT_BLIT_FORMAT 1";
        case ViewportBlitFormat::RGBA32F:
            return "#define VIEWPORT_BLIT_FORMAT 2";
        default:
            break;
    }
    return "#define VIEWPORT_BLIT_FORMAT 0";
}

uint64_t OutputCompositor::_viewport_blit_key(RenderingDevice *p_device, ViewportBlitFormat p_format) {
    uint64_t format_key = static_cast<uint64_t>(p_format) & 0xFFFFull;
    uint64_t device_key = reinterpret_cast<uint64_t>(p_device);
    return (device_key << 16) ^ format_key;
}

uint64_t OutputCompositor::_viewport_blit_scratch_key(RenderingDevice *p_device, RD::DataFormat p_format, const Size2i &p_extent) {
    uint64_t seed = hash64_murmur3_64(reinterpret_cast<uint64_t>(p_device), HASH_MURMUR3_SEED);
    seed = hash64_murmur3_64(static_cast<uint64_t>(p_format), seed);
    seed = hash64_murmur3_64(static_cast<uint64_t>(uint32_t(p_extent.x)) | (static_cast<uint64_t>(uint32_t(p_extent.y)) << 32), seed);
    return seed;
}

uint64_t OutputCompositor::_compute_framebuffer_validation_key(RenderingDevice *p_device, const Vector<RID> &p_attachments) const {
    uint64_t seed = hash64_murmur3_64(reinterpret_cast<uint64_t>(p_device), HASH_MURMUR3_SEED);
    seed = hash64_murmur3_64(static_cast<uint64_t>(p_attachments.size()), seed);
    for (const RID &attachment : p_attachments) {
        seed = hash64_murmur3_64(attachment.get_id(), seed);
    }
    return seed;
}

RID OutputCompositor::_ensure_viewport_blit_sampler(RenderingDevice *p_device) {
    if (!p_device) {
        return RID();
    }

    uint64_t device_key = reinterpret_cast<uint64_t>(p_device);
    if (ViewportBlitSampler *existing = viewport_blit_samplers.getptr(device_key)) {
        if (existing->sampler.is_valid() && existing->owner_device == p_device) {
            return existing->sampler;
        }

        RenderingDevice *owner_device = existing->owner_device ? existing->owner_device : p_device;
        _free_tracked_resource(owner_device, existing->sampler, TrackedResourceKind::Sampler);
        viewport_blit_samplers.erase(device_key);
    }

    RD::SamplerState sampler_state;
    sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
    sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
    sampler_state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
    sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
    sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
    sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
    sampler_state.enable_compare = false;

    RID sampler = p_device->sampler_create(sampler_state);
    if (!sampler.is_valid()) {
        GS_LOG_WARN_DEFAULT("[OutputCompositor] Failed to create sampler for viewport compute blit");
        return RID();
    }
    p_device->set_resource_name(sampler, "GS_OutputCompositor_ViewportBlitSampler");

    _track_resource(sampler, p_device, true, "viewport_blit_sampler");
    ViewportBlitSampler sampler_entry;
    sampler_entry.sampler = sampler;
    sampler_entry.owner_device = p_device;
    viewport_blit_samplers.insert(device_key, sampler_entry);
    return sampler;
}

RID OutputCompositor::_ensure_viewport_blit_scratch(RenderingDevice *p_device, const RD::TextureFormat &p_format,
        const Size2i &p_extent, bool *r_reused) {
    if (r_reused) {
        *r_reused = false;
    }
    if (!p_device || p_extent.x <= 0 || p_extent.y <= 0) {
        return RID();
    }

    // Canonical scratch format: swap SRGB inputs to their UNORM equivalent so
    // that sampling via sampler2D does not auto-decode SRGB → linear (the
    // composite shader applies srgb_to_linear itself; without the swap the
    // destination would be gamma-decoded twice). For UNORM/HDR inputs this is
    // identity. Pool key uses the canonical format so SRGB and UNORM
    // destinations of the same dimensions share scratch.
    const RD::DataFormat canonical_format = _canonical_storage_format(p_format.format);
    const uint64_t key = _viewport_blit_scratch_key(p_device, canonical_format, p_extent);
    if (ViewportBlitScratch *existing = viewport_blit_scratch.getptr(key)) {
        if (existing->texture.is_valid() && existing->owner_device == p_device &&
                existing->extent == p_extent && existing->format == canonical_format &&
                p_device->texture_is_valid(existing->texture)) {
            existing->last_used_id = ++viewport_blit_scratch_next_id;
            if (r_reused) {
                *r_reused = true;
            }
            return existing->texture;
        }
        RenderingDevice *owner_device = existing->owner_device ? existing->owner_device : p_device;
        _free_tracked_resource(owner_device, existing->texture, TrackedResourceKind::Texture);
        viewport_blit_scratch.erase(key);
    }

    // LRU eviction: cap the number of scratch entries per (device, canonical
    // format). Editor viewport resizes accumulate one entry per intermediate
    // extent; without this bound the pool grows until shutdown.
    {
        uint32_t same_kind_count = 0;
        uint64_t oldest_key = 0;
        uint64_t oldest_id = UINT64_MAX;
        for (const KeyValue<uint64_t, ViewportBlitScratch> &kv : viewport_blit_scratch) {
            if (kv.value.owner_device == p_device && kv.value.format == canonical_format) {
                same_kind_count++;
                if (kv.value.last_used_id < oldest_id) {
                    oldest_id = kv.value.last_used_id;
                    oldest_key = kv.key;
                }
            }
        }
        if (same_kind_count >= VIEWPORT_BLIT_SCRATCH_MAX_PER_KIND) {
            ViewportBlitScratch *victim = viewport_blit_scratch.getptr(oldest_key);
            if (victim) {
                RenderingDevice *owner_device = victim->owner_device ? victim->owner_device : p_device;
                _free_tracked_resource(owner_device, victim->texture, TrackedResourceKind::Texture);
                viewport_blit_scratch.erase(oldest_key);
            }
        }
    }

    RD::TextureFormat scratch_format = p_format;
    scratch_format.format = canonical_format;
    scratch_format.width = p_extent.x;
    scratch_format.height = p_extent.y;
    scratch_format.depth = 1;
    scratch_format.array_layers = 1;
    scratch_format.mipmaps = 1;
    scratch_format.texture_type = RD::TEXTURE_TYPE_2D;
    scratch_format.samples = RD::TEXTURE_SAMPLES_1;
    // COLOR_ATTACHMENT: when the composite destination lacks CAN_COPY_FROM (the internal
    // render-buffer color texture in scaled/multiview viewports), the scratch is filled by
    // a graphics blit into a framebuffer wrapping it instead of texture_copy. All supported
    // blit formats are color-attachment capable.
    scratch_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
            RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
    // p_format was cloned from the destination texture; its shareable_formats
    // list reflects the destination's mutability contract (e.g. SRGB↔UNORM
    // aliasing for a swapchain target). The scratch is a freshly-allocated,
    // self-contained texture and never aliased into another format — but
    // texture_create() validates that the chosen `format` is present in
    // shareable_formats when the list is non-empty, so an SRGB-only shareable
    // list combined with our canonical UNORM choice would reject the
    // allocation and silently disable the compute composite path. Clear the
    // list so texture_create() applies its default-shareable handling.
    scratch_format.shareable_formats.clear();

    RID texture = p_device->texture_create(scratch_format, RD::TextureView());
    if (!texture.is_valid()) {
        GS_LOG_WARN_DEFAULT("[OutputCompositor] Failed to create viewport blit scratch texture");
        return RID();
    }
    p_device->set_resource_name(texture, "GS_OutputCompositor_ViewportBlitScratch");

    _track_resource(texture, p_device, true, "viewport_blit_scratch");
    ViewportBlitScratch entry;
    entry.texture = texture;
    entry.extent = p_extent;
    entry.format = canonical_format;
    entry.owner_device = p_device;
    entry.last_used_id = ++viewport_blit_scratch_next_id;
    viewport_blit_scratch.insert(key, entry);
    return texture;
}

bool OutputCompositor::_ensure_viewport_blit_pipeline(RenderingDevice *p_device, RD::DataFormat p_format, RID &r_shader, RID &r_pipeline) {
    if (!p_device) {
        return false;
    }

    ViewportBlitFormat format;
    if (!_determine_viewport_blit_format(p_format, format)) {
        // Unsupported destination format for the depth-aware compute blit. Warn once —
        // returning false silently would drop the requested depth test into the
        // no-depth graphics fallback with no visible cause.
        WARN_PRINT_ONCE(vformat("[OutputCompositor] Composite destination format %d has no viewport-blit compute variant; the depth-aware composite cannot run on this target.", int(p_format)));
        return false;
    }

    uint64_t key = _viewport_blit_key(p_device, format);
    if (ViewportBlitVariant *variant = viewport_blit_variants.getptr(key)) {
        if (variant->owner_device == p_device && variant->shader.is_valid() && variant->pipeline.is_valid()) {
            r_shader = variant->shader;
            r_pipeline = variant->pipeline;
            return true;
        }

        RenderingDevice *owner_device = variant->owner_device ? variant->owner_device : p_device;
        _free_tracked_resource(owner_device, variant->pipeline, TrackedResourceKind::ComputePipeline);
        _free_tracked_resource(owner_device, variant->shader, TrackedResourceKind::Shader);
        viewport_blit_variants.erase(key);
    }

    if (!viewport_blit_shader_source) {
        viewport_blit_shader_source = new ViewportBlitShaderRD();
    }

    if (!viewport_blit_shader_source_initialized) {
        Vector<String> versions;
        versions.push_back("");
        viewport_blit_shader_source->initialize(versions);
        viewport_blit_shader_source_initialized = true;
    }

    RID shader_version = viewport_blit_shader_source->version_create();
    if (!shader_version.is_valid()) {
        GS_LOG_WARN_DEFAULT("[OutputCompositor] Failed to create viewport blit shader version");
        return false;
    }

    Vector<String> stage_sources = viewport_blit_shader_source->version_build_variant_stage_sources(shader_version, 0);
    viewport_blit_shader_source->version_free(shader_version);
    if (stage_sources.size() <= RD::SHADER_STAGE_COMPUTE) {
        GS_LOG_WARN_DEFAULT("[OutputCompositor] Viewport blit shader missing compute stage");
        return false;
    }

    String compute_source = stage_sources[RD::SHADER_STAGE_COMPUTE];
    Vector<String> defines;
    defines.push_back(_viewport_blit_define(format));

    String compile_error;
    String processed_source;
    RID shader = ShaderCompilationHelper::compile_shader_on_device(p_device, compute_source, "viewport_blit", defines, &compile_error, &processed_source);
    if (!shader.is_valid()) {
        GS_LOG_WARN_DEFAULT(vformat("[OutputCompositor] Failed to compile viewport blit shader: %s",
                compile_error.is_empty() ? String("unknown error") : compile_error));
        return false;
    }

    RID pipeline = p_device->compute_pipeline_create(shader);
    if (!pipeline.is_valid()) {
        GS_LOG_WARN_DEFAULT("[OutputCompositor] Failed to create viewport blit compute pipeline");
        p_device->free(shader);
        return false;
    }

    _track_resource(shader, p_device, true, "viewport_blit_shader");
    _track_resource(pipeline, p_device, true, "viewport_blit_pipeline");

    ViewportBlitVariant new_variant;
    new_variant.shader = shader;
    new_variant.pipeline = pipeline;
    new_variant.owner_device = p_device;
    viewport_blit_variants.insert(key, new_variant);

    r_shader = shader;
    r_pipeline = pipeline;
    return true;
}

void OutputCompositor::clear_cached_framebuffers() {
    for (KeyValue<uint64_t, CachedFramebuffer> &E : output_cache.cached_framebuffers) {
        CachedFramebuffer &entry = E.value;
        RenderingDevice *fb_device = entry.device ? entry.device : rd;
        _free_tracked_resource(fb_device, entry.framebuffer, TrackedResourceKind::Framebuffer);
    }

    output_cache.cached_framebuffers.clear();
    output_cache.framebuffer_validation_cache.clear();
    srgb_format_cache.clear();
}

void OutputCompositor::_evict_oldest_cached_framebuffer_if_needed(uint64_t p_incoming_key) {
    // Caller has already erased any stale entry under p_incoming_key, so the
    // cache size here reflects only entries that would remain after a fresh
    // insert. If we're at the cap, evict the least-recently-used entry via
    // the same destroy path clear_cached_framebuffers() uses.
    if (output_cache.cached_framebuffers.size() < MAX_CACHED_FRAMEBUFFER_FORMATS) {
        return;
    }
    uint64_t oldest_key = 0;
    uint64_t oldest_id = UINT64_MAX;
    bool found = false;
    for (const KeyValue<uint64_t, CachedFramebuffer> &kv : output_cache.cached_framebuffers) {
        if (kv.key == p_incoming_key) {
            continue;
        }
        if (!found || kv.value.last_access_id < oldest_id) {
            oldest_id = kv.value.last_access_id;
            oldest_key = kv.key;
            found = true;
        }
    }
    if (!found) {
        return;
    }
    CachedFramebuffer *victim = output_cache.cached_framebuffers.getptr(oldest_key);
    if (victim) {
        RenderingDevice *owner_device = victim->device ? victim->device : rd;
        _free_tracked_resource(owner_device, victim->framebuffer, TrackedResourceKind::Framebuffer);
        output_cache.cached_framebuffers.erase(oldest_key);
    }
}

void OutputCompositor::_evict_oldest_framebuffer_validation_if_needed(uint64_t p_incoming_key) {
    if (output_cache.framebuffer_validation_cache.size() < MAX_CACHED_FRAMEBUFFER_FORMATS) {
        return;
    }
    uint64_t oldest_key = 0;
    uint64_t oldest_id = UINT64_MAX;
    bool found = false;
    for (const KeyValue<uint64_t, FramebufferValidationCacheEntry> &kv : output_cache.framebuffer_validation_cache) {
        if (kv.key == p_incoming_key) {
            continue;
        }
        if (!found || kv.value.last_access_id < oldest_id) {
            oldest_id = kv.value.last_access_id;
            oldest_key = kv.key;
            found = true;
        }
    }
    if (!found) {
        return;
    }
    // Validation entries hold only POD + Vector<AttachmentValidationInfo>; no
    // GPU resources to free, just drop the map slot.
    output_cache.framebuffer_validation_cache.erase(oldest_key);
}

void OutputCompositor::test_force_insert_cached_framebuffer(uint64_t p_key) {
    if (CachedFramebuffer *existing = output_cache.cached_framebuffers.getptr(p_key)) {
        existing->last_access_id = ++cached_framebuffer_next_id;
        return;
    }
    _evict_oldest_cached_framebuffer_if_needed(p_key);
    CachedFramebuffer entry;
    entry.framebuffer = RID();
    entry.device = rd;
    entry.last_access_id = ++cached_framebuffer_next_id;
    output_cache.cached_framebuffers.insert(p_key, entry);
}

void OutputCompositor::test_force_insert_framebuffer_validation(uint64_t p_key) {
    if (FramebufferValidationCacheEntry *existing = output_cache.framebuffer_validation_cache.getptr(p_key)) {
        existing->last_access_id = ++framebuffer_validation_next_id;
        return;
    }
    _evict_oldest_framebuffer_validation_if_needed(p_key);
    FramebufferValidationCacheEntry entry;
    entry.valid = true;
    entry.extent = Size2i(0, 0);
    entry.samples = RD::TEXTURE_SAMPLES_1;
    entry.last_access_id = ++framebuffer_validation_next_id;
    output_cache.framebuffer_validation_cache.insert(p_key, entry);
}

void OutputCompositor::clear_viewport_blit_resources() {
    for (KeyValue<uint64_t, ViewportBlitVariant> &E : viewport_blit_variants) {
        ViewportBlitVariant &variant = E.value;
        RenderingDevice *owner_device = variant.owner_device ? variant.owner_device : rd;
        _free_tracked_resource(owner_device, variant.pipeline, TrackedResourceKind::ComputePipeline);
        _free_tracked_resource(owner_device, variant.shader, TrackedResourceKind::Shader);
    }

    for (KeyValue<uint64_t, ViewportBlitSampler> &E : viewport_blit_samplers) {
        ViewportBlitSampler &sampler_entry = E.value;
        RenderingDevice *owner_device = sampler_entry.owner_device ? sampler_entry.owner_device : rd;
        _free_tracked_resource(owner_device, sampler_entry.sampler, TrackedResourceKind::Sampler);
    }

    for (KeyValue<uint64_t, ViewportBlitScratch> &E : viewport_blit_scratch) {
        ViewportBlitScratch &scratch_entry = E.value;
        RenderingDevice *owner_device = scratch_entry.owner_device ? scratch_entry.owner_device : rd;
        _free_tracked_resource(owner_device, scratch_entry.texture, TrackedResourceKind::Texture);
    }

    viewport_blit_variants.clear();
    viewport_blit_samplers.clear();
    viewport_blit_scratch.clear();
}

RID OutputCompositor::get_cached_framebuffer(RenderingDevice *p_device, const RID &p_texture) {
    if (!p_device || !p_texture.is_valid()) {
        return RID();
    }

    uint64_t key = p_texture.get_id();
    if (CachedFramebuffer *entry = output_cache.cached_framebuffers.getptr(key)) {
        RenderingDevice *entry_device = entry->device ? entry->device : p_device;
        bool device_matches = entry_device == p_device;
        if (entry->framebuffer.is_valid() && device_matches &&
                entry_device && entry_device->framebuffer_is_valid(entry->framebuffer)) {
            entry->last_access_id = ++cached_framebuffer_next_id;
            return entry->framebuffer;
        }
        _free_tracked_resource(entry_device, entry->framebuffer, TrackedResourceKind::Framebuffer);
        output_cache.cached_framebuffers.erase(key);
    }

    // Eviction is deferred to AFTER validate_framebuffer_attachments() and
    // framebuffer_create() succeed (Codex review #3294032623 on PR #388). If we
    // evict here and validation/creation then fails (invalid/non-attachable RID,
    // transient driver failure), we would have dropped an unrelated cached
    // framebuffer permanently without inserting a replacement -- repeated failed
    // requests would drain the cache.

    Vector<RID> attachments;
    attachments.push_back(p_texture);

    Vector<AttachmentValidationInfo> attachment_infos;
    Size2i attachment_extent;
    RD::TextureSamples attachment_samples = RD::TEXTURE_SAMPLES_1;
    String validation_error;
    if (!validate_framebuffer_attachments(p_device, attachments, attachment_infos, attachment_extent, attachment_samples,
                validation_error)) {
        GS_LOG_ERROR_DEFAULT(vformat("[OutputCompositor] Cached framebuffer validation failed for texture RID %s: %s",
                String::num_uint64(p_texture.get_id()), validation_error));
        return RID();
    }

    Vector<RID> framebuffer_attachments;
    framebuffer_attachments.push_back(p_texture);
    RID framebuffer = p_device->framebuffer_create(framebuffer_attachments);
    if (!framebuffer.is_valid()) {
        GS_LOG_ERROR_DEFAULT(vformat("[OutputCompositor] framebuffer_create failed for cached framebuffer (RID %s)",
                String::num_uint64(p_texture.get_id())));
        return RID();
    }
    p_device->set_resource_name(framebuffer, "GS_OutputCompositor_CachedFramebuffer");
    _track_resource(framebuffer, p_device, true, "cached_framebuffer");

    // Insertion is committed: only now is it safe to evict an LRU entry to make
    // room. The stale entry under `key` (if any) was already erased above, so
    // the helper's "skip incoming key" branch is a no-op here and it will pick
    // a genuinely different LRU victim when at cap.
    _evict_oldest_cached_framebuffer_if_needed(key);

    CachedFramebuffer new_entry;
    new_entry.framebuffer = framebuffer;
    new_entry.device = p_device;
    new_entry.last_access_id = ++cached_framebuffer_next_id;
    output_cache.cached_framebuffers.insert(key, new_entry);
    return framebuffer;
}

bool OutputCompositor::validate_framebuffer_attachments(RenderingDevice *p_device, const Vector<RID> &p_attachments,
        Vector<AttachmentValidationInfo> &r_infos, Size2i &r_extent, RD::TextureSamples &r_samples, String &r_error) {
    r_infos.clear();
    r_extent = Size2i();
    r_samples = RD::TEXTURE_SAMPLES_1;

    if (!p_device) {
        r_error = "Target RenderingDevice is null";
        return false;
    }

    if (p_attachments.is_empty()) {
        r_error = "No framebuffer attachments were provided";
        return false;
    }

    uint64_t cache_key = _compute_framebuffer_validation_key(p_device, p_attachments);
    if (FramebufferValidationCacheEntry *cache_entry = output_cache.framebuffer_validation_cache.getptr(cache_key)) {
        if (cache_entry->valid) {
            bool cache_still_valid = true;
            for (const AttachmentValidationInfo &cached_info : cache_entry->infos) {
                const RID &attachment = cached_info.original_attachment;
                if (!attachment.is_valid() || !p_device->texture_is_valid(attachment)) {
                    cache_still_valid = false;
                    break;
                }
            }
            if (cache_still_valid) {
                r_infos = cache_entry->infos;
                r_extent = cache_entry->extent;
                r_samples = cache_entry->samples;
                cache_entry->last_access_id = ++framebuffer_validation_next_id;
                return true;
            }
        }
        // Stale entry (invalid bit, or one of the cached attachments was freed/recreated
        // and the new RID reuses the same key path). Erase it BEFORE the eviction helper
        // runs so the helper sees the cache below cap and does not evict a different,
        // still-live entry. Without this, the eviction helper would skip cache_key,
        // evict an unrelated slot, and the trailing insert() would just overwrite the
        // stale entry -- net effect: cache silently drops one slot below the intended
        // cap, causing avoidable thrash under attachment-churn workloads.
        output_cache.framebuffer_validation_cache.erase(cache_key);
    }
    Vector<String> errors;

    bool extent_initialized = false;
    RD::TextureSamples expected_samples = RD::TEXTURE_SAMPLES_1;

    for (int i = 0; i < p_attachments.size(); i++) {
        const RID &attachment = p_attachments[i];
        AttachmentValidationInfo info;
        info.original_attachment = attachment;

        if (!attachment.is_valid()) {
            errors.push_back(vformat("Attachment %d is invalid (RID %s)", i, String::num_uint64(attachment.get_id())));
            continue;
        }

        if (!p_device->texture_is_valid(attachment)) {
            errors.push_back(vformat("Attachment %d (RID %s) is not valid on the target RenderingDevice", i,
                    String::num_uint64(attachment.get_id())));
            continue;
        }

        RD::TextureFormat texture_format = p_device->texture_get_format(attachment);
        if (texture_format.format == RD::DATA_FORMAT_MAX) {
            errors.push_back(vformat("Attachment %d (RID %s) reports an unknown texture format", i,
                    String::num_uint64(attachment.get_id())));
            continue;
        }

        info.original_format = texture_format;
        info.is_depth = _is_depth_attachment_format(texture_format.format);

        if (texture_format.width == 0 || texture_format.height == 0) {
            errors.push_back(vformat("Attachment %d (RID %s) has zero extent", i, String::num_uint64(attachment.get_id())));
        }
        if (texture_format.array_layers == 0) {
            errors.push_back(vformat("Attachment %d (RID %s) has no array layers", i, String::num_uint64(attachment.get_id())));
        }
        if (texture_format.mipmaps == 0) {
            errors.push_back(vformat("Attachment %d (RID %s) has no mipmap levels", i,
                    String::num_uint64(attachment.get_id())));
        }
        if (texture_format.texture_type != RD::TEXTURE_TYPE_2D && texture_format.texture_type != RD::TEXTURE_TYPE_2D_ARRAY) {
            errors.push_back(vformat("Attachment %d (RID %s) uses unsupported texture type %d", i,
                    String::num_uint64(attachment.get_id()), (int)texture_format.texture_type));
        }
        if (texture_format.texture_type == RD::TEXTURE_TYPE_2D && texture_format.depth > 1) {
            errors.push_back(vformat("Attachment %d (RID %s) declares depth %d for 2D texture", i,
                    String::num_uint64(attachment.get_id()), texture_format.depth));
        }

        Size2i attachment_extent(texture_format.width, texture_format.height);
        if (!extent_initialized) {
            r_extent = attachment_extent;
            extent_initialized = true;
        } else if (r_extent != attachment_extent) {
            errors.push_back(vformat("Attachment %d (RID %s) size %dx%d does not match expected %dx%d", i,
                    String::num_uint64(attachment.get_id()), attachment_extent.x, attachment_extent.y, r_extent.x, r_extent.y));
        }

        if (i == 0) {
            expected_samples = texture_format.samples;
            r_samples = texture_format.samples;
        } else if (texture_format.samples != expected_samples) {
            errors.push_back(vformat("Attachment %d (RID %s) sample count %d mismatches expected %d", i,
                    String::num_uint64(attachment.get_id()), (int)texture_format.samples, (int)expected_samples));
        }

        BitField<RD::TextureUsageBits> required_usage = info.is_depth ? BitField<RD::TextureUsageBits>(RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                                                                      : BitField<RD::TextureUsageBits>(RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
        uint32_t required_usage_bits = info.is_depth ? (uint32_t)RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : (uint32_t)RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;

        bool has_required_usage = (texture_format.usage_bits & required_usage_bits) != 0;
        bool format_supported = p_device->texture_is_format_supported_for_usage(texture_format.format, required_usage);

        if (!has_required_usage || !format_supported) {
            errors.push_back(vformat("Attachment %d (RID %s) lacks required usage %s or is unsupported by the device",
                    i, String::num_uint64(attachment.get_id()), _attachment_usage_label(info.is_depth)));
        }

        r_infos.push_back(info);
    }

    if (!errors.is_empty()) {
        String error_text;
        for (int i = 0; i < errors.size(); i++) {
            if (i > 0) {
                error_text += "\n";
            }
            error_text += errors[i];
        }
        r_error = error_text;
        return false;
    }

    _evict_oldest_framebuffer_validation_if_needed(cache_key);

    FramebufferValidationCacheEntry cache_entry;
    cache_entry.valid = true;
    cache_entry.extent = r_extent;
    cache_entry.samples = r_samples;
    cache_entry.infos = r_infos;
    cache_entry.last_access_id = ++framebuffer_validation_next_id;
    output_cache.framebuffer_validation_cache.insert(cache_key, cache_entry);

    return true;
}

bool OutputCompositor::_copy_final_output_compute(RenderingDevice *p_device, RID p_source, RID p_destination,
        const Size2i &p_source_extent, const Size2i &p_copy_extent, const Vector3i &p_destination_offset,
        bool p_composite_with_destination, bool p_source_is_premultiplied, bool p_destination_is_srgb,
        bool p_source_decode_srgb,
        const RD::TextureFormat &p_destination_format, RID p_source_depth, RID p_destination_depth,
        bool p_depth_test_enabled, bool p_depth_is_orthogonal, float p_z_near, float p_z_far,
        float p_depth_linearize_mul, float p_depth_linearize_add, float p_depth_epsilon) {
    // last_composite_stats is reset at the entry of copy_to_render_target (the
    // public dispatcher) so non-compute branches (direct-copy, graphics
    // fallback) also see a clean slate. Don't reset here a second time — the
    // dispatcher's contract is "stats reflect THIS copy_to_render_target call",
    // regardless of which internal path is taken.

    if (!p_device || !p_source.is_valid() || !p_destination.is_valid()) {
        return false;
    }

    if (p_copy_extent.x <= 0 || p_copy_extent.y <= 0) {
        return false;
    }

    last_composite_stats.composite_with_destination = p_composite_with_destination;

    // Scratch-copy path: when compositing, we read the prior destination through a
    // sampled scratch texture rather than imageLoad on the live destination image.
    // This eliminates the cross-invocation R/W hazard responsible for incident #256.
    // Bind the source as a dummy when not compositing so the shader's binding-4
    // sampler slot always has a valid resource.
    RID scratch_for_binding = p_source;
    if (p_composite_with_destination) {
        const RD::TextureFormat dest_actual = p_device->texture_get_format(p_destination);
        const bool dest_can_copy_from = (dest_actual.usage_bits & RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT) != 0;
        // The internal render-buffer color texture (the composite destination whenever
        // 3D scaling is active) never carries CAN_COPY_FROM, so texture_copy cannot fill
        // the scratch there. It IS sampleable, so fill the scratch with a graphics blit
        // instead — otherwise every scaled viewport silently loses the depth test to the
        // no-depth graphics fallback. Exclusions: sRGB destinations (decode-on-sample
        // would double-decode against the shader's srgb_to_linear), non-main devices
        // (CopyEffects is main-RD-only) — in practice both only occur for present
        // targets, which have CAN_COPY_FROM — and MULTIVIEW ARRAY targets: the internal
        // color buffer is a TEXTURE_TYPE_2D_ARRAY with view_count layers there, which
        // this single-view blit (p_multiview=false) and the compute shader's
        // sampler2D/image2D destination bindings cannot legally address. Arrays keep the
        // pre-existing no-depth fallback until a multiview blit variant exists.
        const bool blit_fill_possible = (dest_actual.usage_bits & RD::TEXTURE_USAGE_SAMPLING_BIT) != 0 &&
                !_is_srgb_format(dest_actual.format) && p_device == RD::get_singleton() &&
                dest_actual.texture_type == RD::TEXTURE_TYPE_2D && dest_actual.array_layers <= 1;
        if (!dest_can_copy_from && !blit_fill_possible) {
            last_composite_stats.fallback_due_to_missing_copy_from = true;
            last_composite_stats.valid = true;
            WARN_PRINT_ONCE("[OutputCompositor] Composite destination supports neither CAN_COPY_FROM nor a sampled scratch fill; falling back to caller-provided graphics path.");
            return false;
        }
        const int32_t dest_w = p_destination_format.width > 0 ? (int32_t)p_destination_format.width
                                                              : MAX(p_destination_offset.x + p_copy_extent.x, 1);
        const int32_t dest_h = p_destination_format.height > 0 ? (int32_t)p_destination_format.height
                                                               : MAX(p_destination_offset.y + p_copy_extent.y, 1);
        const Size2i scratch_extent(dest_w, dest_h);
        bool scratch_reused = false;
        RID scratch_rid = _ensure_viewport_blit_scratch(p_device, p_destination_format, scratch_extent, &scratch_reused);
        if (!scratch_rid.is_valid()) {
            return false;
        }
        if (dest_can_copy_from) {
            const Vector3 copy_zero(0, 0, 0);
            const Vector3 copy_size(dest_w, dest_h, 1);
            Error copy_err = p_device->texture_copy(p_destination, scratch_rid, copy_zero, copy_zero, copy_size, 0, 0, 0, 0);
            if (copy_err != OK) {
                GS_LOG_WARN_DEFAULT(vformat("[OutputCompositor] texture_copy destination->scratch failed (err=%d); skipping composite frame", int(copy_err)));
                return false;
            }
        } else {
            // Sampled fill: destination -> scratch framebuffer via a full-rect blit. The
            // destination is only SAMPLED here and written by the separate compute
            // dispatch afterwards — never same-dispatch R/W, so the #256 hazard fix holds.
            // Both textures are non-sRGB UNORM/float, so the pass is a 1:1 pixel move.
            RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
            RID scratch_fb = copy_effects ? get_cached_framebuffer(p_device, scratch_rid) : RID();
            if (!scratch_fb.is_valid()) {
                GS_LOG_WARN_DEFAULT("[OutputCompositor] Sampled scratch fill unavailable (no CopyEffects or scratch framebuffer); skipping composite frame");
                return false;
            }
            const Rect2i full_rect(Vector2i(0, 0), Size2i(dest_w, dest_h));
            const Rect2 full_src(Vector2(0.0f, 0.0f), Vector2(1.0f, 1.0f));
            copy_effects->copy_to_fb_rect(p_destination, scratch_fb, full_rect, false, false, false, false, RID(), false, false, false, false, full_src, false, false);
        }
        scratch_for_binding = scratch_rid;
        last_composite_stats.scratch_used = true;
        last_composite_stats.scratch_reused = scratch_reused;
    }

    RID shader;
    RID pipeline;
    if (!_ensure_viewport_blit_pipeline(p_device, p_destination_format.format, shader, pipeline)) {
        return false;
    }

    RID sampler = _ensure_viewport_blit_sampler(p_device);
    if (!sampler.is_valid()) {
        return false;
    }

    RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
    RID fallback_depth = texture_storage ? texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH) : RID();

    RID source_depth = p_source_depth.is_valid() ? p_source_depth : fallback_depth;
    RID destination_depth = p_destination_depth.is_valid() ? p_destination_depth : fallback_depth;

    bool depth_test_enabled = p_depth_test_enabled && p_source_depth.is_valid() && p_destination_depth.is_valid() &&
            source_depth.is_valid() && destination_depth.is_valid();
    if (!depth_test_enabled) {
        source_depth = fallback_depth;
        destination_depth = fallback_depth;
    }

    RD::Uniform sampler_uniform;
    sampler_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
    sampler_uniform.binding = 0;
    sampler_uniform.append_id(sampler);
    sampler_uniform.append_id(p_source);

    RD::Uniform image_uniform;
    image_uniform.uniform_type = RD::UNIFORM_TYPE_IMAGE;
    image_uniform.binding = 1;
    image_uniform.append_id(p_destination);

    RD::Uniform source_depth_uniform;
    source_depth_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
    source_depth_uniform.binding = 2;
    source_depth_uniform.append_id(sampler);
    source_depth_uniform.append_id(source_depth);

    RD::Uniform destination_depth_uniform;
    destination_depth_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
    destination_depth_uniform.binding = 3;
    destination_depth_uniform.append_id(sampler);
    destination_depth_uniform.append_id(destination_depth);

    RD::Uniform scratch_uniform;
    scratch_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
    scratch_uniform.binding = 4;
    scratch_uniform.append_id(sampler);
    scratch_uniform.append_id(scratch_for_binding);

    UniformSetCacheRD *uniform_cache = UniformSetCacheRD::get_singleton();
    RID uniform_set;
    bool manual_uniform_set = false;
    if (uniform_cache) {
        uniform_set = uniform_cache->get_cache(shader, 0, sampler_uniform, image_uniform, source_depth_uniform, destination_depth_uniform, scratch_uniform);
    } else {
        Vector<RD::Uniform> uniforms;
        uniforms.push_back(sampler_uniform);
        uniforms.push_back(image_uniform);
        uniforms.push_back(source_depth_uniform);
        uniforms.push_back(destination_depth_uniform);
        uniforms.push_back(scratch_uniform);
        uniform_set = p_device->uniform_set_create(uniforms, shader, 0);
        manual_uniform_set = true;
        if (!uniform_set.is_valid()) {
            return false;
        }
        p_device->set_resource_name(uniform_set, "GS_OutputCompositor_CopyFinalOutputUniformSet");
    }
    last_composite_stats.binding_count = 5;
    last_composite_stats.valid = true;

    // GPU Debug: Output composition pass (Purple tones for compositing)
    ScopedGpuMarker composite_marker(p_device, "GS_Composite", Color(0.7f, 0.3f, 0.9f, 1.0f));

    RD::ComputeListID compute_list = p_device->compute_list_begin();
    if (compute_list == RD::INVALID_ID) {
        if (manual_uniform_set && uniform_set.is_valid() && p_device->uniform_set_is_valid(uniform_set)) {
            p_device->free(uniform_set);
        }
        return false;
    }

    p_device->compute_list_bind_compute_pipeline(compute_list, pipeline);
    p_device->compute_list_bind_uniform_set(compute_list, uniform_set, 0);

    struct ViewportBlitPushConstant {
        int32_t copy_size[2];
        int32_t source_size[2];
        int32_t destination_size[2];
        int32_t destination_offset[2];
        int32_t composite_with_destination;
        int32_t destination_is_srgb;
        int32_t source_is_premultiplied;
        int32_t depth_test_enabled;
        int32_t depth_is_orthogonal;
        float z_near;
        float z_far;
        float depth_epsilon;
        float depth_linearize_mul;
        float depth_linearize_add;
        // Occupies the former pad0 slot: identical 4-byte size/alignment, so
        // every other field offset and the total push-constant size are
        // unchanged. Mirrors `int source_decode_srgb` in viewport_blit.glsl.
        int32_t source_decode_srgb;
        float pad1;
    } params = {};

    params.copy_size[0] = p_copy_extent.x;
    params.copy_size[1] = p_copy_extent.y;
    params.source_size[0] = MAX(p_source_extent.x, 1);
    params.source_size[1] = MAX(p_source_extent.y, 1);

    int32_t dest_width = p_destination_format.width > 0 ? (int32_t)p_destination_format.width
                                                        : MAX(p_destination_offset.x + p_copy_extent.x, 1);
    int32_t dest_height = p_destination_format.height > 0 ? (int32_t)p_destination_format.height
                                                          : MAX(p_destination_offset.y + p_copy_extent.y, 1);
    params.destination_size[0] = dest_width;
    params.destination_size[1] = dest_height;
    params.destination_offset[0] = p_destination_offset.x;
    params.destination_offset[1] = p_destination_offset.y;
    params.composite_with_destination = p_composite_with_destination ? 1 : 0;
    params.destination_is_srgb = p_destination_is_srgb ? 1 : 0;
    params.source_is_premultiplied = p_source_is_premultiplied ? 1 : 0;
    params.depth_test_enabled = depth_test_enabled ? 1 : 0;
    params.depth_is_orthogonal = p_depth_is_orthogonal ? 1 : 0;
    params.z_near = p_z_near;
    params.z_far = p_z_far;
    params.depth_epsilon = p_depth_epsilon;
    params.depth_linearize_mul = p_depth_linearize_mul;
    params.depth_linearize_add = p_depth_linearize_add;
    params.source_decode_srgb = p_source_decode_srgb ? 1 : 0;

    p_device->compute_list_set_push_constant(compute_list, &params, sizeof(ViewportBlitPushConstant));

    uint32_t groups_x = (uint32_t)((p_copy_extent.x + 7) / 8);
    uint32_t groups_y = (uint32_t)((p_copy_extent.y + 7) / 8);
    groups_x = MAX(groups_x, 1u);
    groups_y = MAX(groups_y, 1u);

    p_device->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
    p_device->compute_list_end();

    if (manual_uniform_set && uniform_set.is_valid() && p_device->uniform_set_is_valid(uniform_set)) {
        p_device->free(uniform_set);
    }

    return true;
}

bool OutputCompositor::copy_to_framebuffer(const FramebufferCopyParams &p_params) {
    output_cache.last_viewport_copy_success = false;
    output_cache.last_viewport_copy_source_size = Size2i();
    output_cache.last_viewport_copy_dest_size = Size2i();

    if (!p_params.source_texture.is_valid() || !p_params.framebuffer.is_valid()) {
        return false;
    }

    RenderingDevice *main_rd = RenderingDevice::get_singleton();
    if (device_manager.is_valid()) {
        if (RenderingDevice *main_device = device_manager->get_main_device()) {
            main_rd = main_device;
        }
    }
    if (!main_rd) {
        return false;
    }

    if (!main_rd->texture_is_valid(p_params.source_texture)) {
        GS_LOG_ERROR_DEFAULT("[OutputCompositor] Source texture is not valid on main RenderingDevice; framebuffer copy unsupported");
        return false;
    }

    RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
    if (!copy_effects) {
        GS_LOG_WARN_DEFAULT("[OutputCompositor] CopyEffects unavailable for framebuffer copy");
        return false;
    }

    Size2i viewport_size = p_params.viewport_size;
    if (viewport_size.x <= 0 || viewport_size.y <= 0) {
        viewport_size = Size2i(1920, 1080); // Fallback
    }

    bool srgb_destination = false;
    if (p_params.destination_texture.is_valid()) {
        srgb_destination = _is_texture_srgb(main_rd, p_params.destination_texture);
    }

    Rect2i dest_rect(Vector2i(0, 0), Size2i(viewport_size.x, viewport_size.y));
    Rect2 src_rect(Vector2(0.0f, 0.0f), Vector2(1.0f, 1.0f));

    bool enable_blend = p_params.composite_with_destination;
    bool use_premultiplied_alpha = enable_blend && p_params.source_is_premultiplied;

    copy_effects->copy_to_fb_rect(p_params.source_texture, p_params.framebuffer, dest_rect, false, false, false, srgb_destination, RID(), false, false, false, false, src_rect, enable_blend, use_premultiplied_alpha);

    output_cache.last_viewport_copy_success = true;
    output_cache.last_viewport_copy_source_size = viewport_size;
    output_cache.last_viewport_copy_dest_size = viewport_size;
    return true;
}

OutputCopyResult OutputCompositor::copy_to_render_target(const OutputCopyParams &p_params) {
    // Reset composite stats at the dispatcher entry so every call starts with a
    // clean state regardless of which internal path runs (compute, direct-copy,
    // or graphics fallback). Without this, get_last_composite_stats() can
    // return stale values from a prior compute call when the current invocation
    // takes the direct-copy or graphics-fallback path — masking regressions in
    // tests that verify scratch-vs-fallback behavior.
    last_composite_stats = LastCompositeStats();

    OutputCopyResult result;
    result.success = false;
    output_cache.last_viewport_copy_success = false;
    output_cache.last_viewport_copy_source_size = Size2i();
    output_cache.last_viewport_copy_dest_size = Size2i();

    if (!p_params.source_texture.is_valid() || !p_params.destination_texture.is_valid()) {
        result.error = "Invalid source or destination texture";
        return result;
    }

    if (!rd) {
        result.error = "RenderingDevice not initialized";
        return result;
    }

    RenderingDevice *main_rd = RenderingDevice::get_singleton();
    if (device_manager.is_valid()) {
        if (RenderingDevice *main_device = device_manager->get_main_device()) {
            main_rd = main_device;
        }
    }
    if (!main_rd) {
        result.error = "Main RenderingDevice unavailable";
        return result;
    }

    RenderingDevice *copy_device = main_rd; // Copy executed on main RenderingDevice
    if (!copy_device->texture_is_valid(p_params.source_texture) ||
            !copy_device->texture_is_valid(p_params.destination_texture)) {
        result.error = "Output textures are not valid on main RenderingDevice";
        return result;
    }

    Size2i destination_extent = p_params.viewport_size;
    if (destination_extent.x <= 0 || destination_extent.y <= 0) {
        destination_extent = Size2i(1, 1);
    }

    RD::TextureFormat destination_format = _get_texture_format(copy_device, p_params.destination_texture);
    if (destination_format.width > 0 && destination_format.height > 0) {
        destination_extent.x = destination_format.width;
        destination_extent.y = destination_format.height;
    }

    RD::TextureFormat source_format = _get_texture_format(copy_device, p_params.source_texture);
    Size2i source_extent(source_format.width, source_format.height);
    if (source_extent.x <= 0 || source_extent.y <= 0) {
        if (internal_render_size.x > 0 && internal_render_size.y > 0) {
            source_extent = internal_render_size;
        }
    }
    if (source_extent.x <= 0 || source_extent.y <= 0) {
        source_extent = destination_extent;
    }

    output_cache.last_viewport_copy_source_size = source_extent;
    result.source_size = source_extent;

    Size2i copy_extent = source_extent;
    if (destination_extent.x > 0 && destination_extent.y > 0) {
        copy_extent.x = MIN(copy_extent.x, destination_extent.x);
        copy_extent.y = MIN(copy_extent.y, destination_extent.y);
    }

    output_cache.last_viewport_copy_dest_size = copy_extent;
    result.dest_size = copy_extent;

    if (copy_extent.x <= 0 || copy_extent.y <= 0) {
        result.error = "Copy extent is zero";
        return result;
    }

    // Check format compatibility
    bool format_mismatch = false;
    if (destination_format.format != RD::DATA_FORMAT_MAX && source_format.format != RD::DATA_FORMAT_MAX) {
        if (destination_format.format != source_format.format) {
            format_mismatch = true;
        }
    }

    bool sample_mismatch = destination_format.samples != source_format.samples;

    // Check texture usage flags
    uint64_t source_usage = (uint64_t)source_format.usage_bits;
    uint64_t destination_usage = (uint64_t)destination_format.usage_bits;
    bool source_can_copy = (source_usage & RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT) != 0;
    bool destination_can_copy = (destination_usage & RD::TEXTURE_USAGE_CAN_COPY_TO_BIT) != 0;
    bool source_can_sample = (source_usage & RD::TEXTURE_USAGE_SAMPLING_BIT) != 0;

    // A depth-test request must never resolve to a plain pixel transfer: the compute
    // path below handles composite_with_destination=false fine, so route depth-requested
    // copies there instead of silently overwriting mesh pixels the splats are behind.
    // source_decode_srgb likewise excludes the plain pixel transfer: a direct copy
    // cannot perform the sRGB->linear source decode the pre-upscale composite
    // contract requires (defensive — the pre-upscale caller always composites).
    bool can_direct_copy = !p_params.composite_with_destination && !format_mismatch && !sample_mismatch && source_can_copy && destination_can_copy && !p_params.depth_test_enabled && !p_params.source_decode_srgb;

    // Try direct copy first
    if (can_direct_copy) {
        Vector3i src_offset(0, 0, 0);
        Vector3i dst_offset(0, 0, 0);
        if (destination_extent.x > copy_extent.x) {
            dst_offset.x = (destination_extent.x - copy_extent.x) / 2;
        }
        if (destination_extent.y > copy_extent.y) {
            dst_offset.y = (destination_extent.y - copy_extent.y) / 2;
        }

        Vector3i region(copy_extent.x, copy_extent.y, 1);
        Error copy_err = copy_device->texture_copy(p_params.source_texture, p_params.destination_texture, src_offset, dst_offset, region, 0, 0, 0, 0);
        if (copy_err == OK) {
            result.success = true;
            // Direct copy is a pure pixel transfer with no depth comparison.
            // can_direct_copy excludes depth_test_enabled, so this block is
            // defensive only — kept so a future routing change can never turn
            // an unoccluded overwrite into a false-positive depth-correct copy.
            if (p_params.depth_test_enabled) {
                result.depth_test_honored = false;
                if (result.error.is_empty()) {
                    result.error = "depth_test_enabled requested but direct-copy path performs no depth comparison; this combination requires composite_with_destination=true to route through the compute path";
                }
            }
            output_cache.last_viewport_copy_success = (copy_extent == source_extent);
            return result;
        }
    }

    // Compute composite when a depth test is requested — and also when a source
    // sRGB->linear decode is requested (pre-upscale phase), which only the
    // compute blit can perform; this keeps the depth_test=false pre-upscale
    // configs on the same internal-buffer compute path instead of the graphics
    // framebuffer blit that cannot decode.
    if (p_params.depth_test_enabled || p_params.source_decode_srgb) {
        Vector3i dst_offset(0, 0, 0);
        if (destination_extent.x > copy_extent.x) {
            dst_offset.x = (destination_extent.x - copy_extent.x) / 2;
        }
        if (destination_extent.y > copy_extent.y) {
            dst_offset.y = (destination_extent.y - copy_extent.y) / 2;
        }

        bool ok = _copy_final_output_compute(copy_device, p_params.source_texture, p_params.destination_texture,
                source_extent, copy_extent, dst_offset, p_params.composite_with_destination,
                p_params.source_is_premultiplied, _is_srgb_format(destination_format.format),
                p_params.source_decode_srgb,
                destination_format, p_params.source_depth, p_params.destination_depth, p_params.depth_test_enabled,
                p_params.depth_is_orthogonal, p_params.z_near, p_params.z_far,
                p_params.depth_linearize_mul, p_params.depth_linearize_add, p_params.depth_epsilon);
        if (ok) {
            result.success = true;
            // _copy_final_output_compute internally disables depth comparison
            // when either source_depth or destination_depth RID is invalid (it
            // substitutes a fallback depth texture and zeroes the depth-test
            // push-constant). Detect that case here so callers that requested
            // depth_test_enabled but didn't supply valid depth textures see
            // depth_test_honored=false rather than a false-positive true.
            if (p_params.depth_test_enabled &&
                    (!p_params.source_depth.is_valid() || !p_params.destination_depth.is_valid())) {
                result.depth_test_honored = false;
                WARN_PRINT_ONCE("[OutputCompositor] Depth test requested but a depth texture RID was invalid; compute composite ran WITHOUT depth comparison (splats will not be occluded by meshes this session until depths become valid).");
                if (result.error.is_empty()) {
                    result.error = "depth_test_enabled requested but source_depth or destination_depth was invalid; compute path internally disabled depth comparison";
                }
            }
            output_cache.last_viewport_copy_success = (copy_extent == source_extent);
            return result;
        }
    }

    // Use CopyEffects blit fallback
    RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
    if (!copy_effects) {
        result.error = "CopyEffects subsystem unavailable";
        return result;
    }

    if (p_params.source_decode_srgb) {
        // No silent encoding fallback: the graphics blit blends the raw sRGB-encoded
        // source into the linear destination without the requested decode, so splats
        // will render too bright on this path. Composite anyway (presence beats
        // absence) but say so: warn once and surface the reason through
        // result.error / output_cache.last_output_copy_error.
        WARN_PRINT_ONCE("[OutputCompositor] Source sRGB->linear decode requested but the compute composite could not run; the graphics fallback composites WITHOUT the decode (splats will appear too bright until the compute path recovers).");
        if (result.error.is_empty()) {
            result.error = "source_decode_srgb requested but the graphics fallback path cannot decode; composited without decode";
        }
    }

    // Create framebuffer from destination texture
    RID framebuffer = get_cached_framebuffer(main_rd, p_params.destination_texture);
    if (!framebuffer.is_valid()) {
        result.error = "Failed to create framebuffer for destination";
        return result;
    }

    // Determine sRGB handling
    bool srgb_destination = _is_srgb_format(destination_format.format);
    if (destination_format.format != RD::DATA_FORMAT_MAX &&
            p_params.destination_texture.is_valid() &&
            copy_device->texture_is_valid(p_params.destination_texture)) {
        srgb_format_cache.insert(p_params.destination_texture.get_id(), srgb_destination);
    }

    Vector3i dst_offset(0, 0, 0);
    if (destination_extent.x > copy_extent.x) {
        dst_offset.x = (destination_extent.x - copy_extent.x) / 2;
    }
    if (destination_extent.y > copy_extent.y) {
        dst_offset.y = (destination_extent.y - copy_extent.y) / 2;
    }

    Rect2i dest_rect(Vector2i(dst_offset.x, dst_offset.y), Size2i(copy_extent.x, copy_extent.y));
    Vector2 src_scale(1.0f, 1.0f);
    if (source_extent.x > 0) {
        src_scale.x = float(copy_extent.x) / float(source_extent.x);
    }
    if (source_extent.y > 0) {
        src_scale.y = float(copy_extent.y) / float(source_extent.y);
    }
    Rect2 src_rect(Vector2(0.0f, 0.0f), src_scale);

    bool enable_blend = p_params.composite_with_destination;
    bool use_premultiplied_alpha = enable_blend && p_params.source_is_premultiplied;

    copy_effects->copy_to_fb_rect(p_params.source_texture, framebuffer, dest_rect, false, false, false, srgb_destination, RID(), false, false, false, false, src_rect, enable_blend, use_premultiplied_alpha);

    result.success = true;
    // The graphics fallback path does not carry depth through copy_to_fb_rect.
    // If the caller requested depth-tested compositing and we reached this path
    // (compute path returned false), report the ACTUAL reason from the compute
    // attempt's stats instead of guessing, and warn once — a silently skipped
    // depth test reads as "splats never occluded by meshes" to the user.
    if (p_params.depth_test_enabled) {
        result.depth_test_honored = false;
        const char *fallback_reason = last_composite_stats.valid && last_composite_stats.fallback_due_to_missing_copy_from
                ? "destination is missing TEXTURE_USAGE_CAN_COPY_FROM_BIT for the scratch copy"
                : "the depth-aware compute composite could not run on this destination";
        WARN_PRINT_ONCE(vformat("[OutputCompositor] Depth test requested but the graphics fallback composited WITHOUT depth (%s); splats will not be occluded by meshes on this path.", fallback_reason));
        if (result.error.is_empty()) {
            result.error = vformat("depth_test_enabled requested but graphics fallback path does not carry depth; %s", fallback_reason);
        }
    }
    output_cache.last_viewport_copy_success = (copy_extent == source_extent);
    return result;
}

void OutputCompositor::integrate_final_output(GaussianSplatRenderer *p_renderer, RenderDataRD *p_render_data, RenderSceneBuffersRD *render_buffers_rd,
        const RID &p_final_output, RID &r_render_target, const Size2i &p_viewport_size, bool p_defer_commit,
        bool p_painterly_active, const RID &p_cached_depth) {
    if (!p_renderer) {
        return;
    }
    (void)p_defer_commit;

    final_render_texture = p_final_output;
    output_cache.has_valid_render = p_final_output.is_valid();
    output_cache.last_output_copy_attempted = false;
    output_cache.last_output_copy_success = false;
    output_cache.last_output_copy_error = String();
    output_cache.last_depth_test_honored = true;
    output_cache.last_copy_degraded = false;
    output_cache.last_copy_degradation_reason = String();
    output_cache.last_strict_depth_contract_required = false;

    if (render_buffers_rd && p_final_output.is_valid()) {
        Size2i viewport_size = p_viewport_size;
        if (viewport_size.x <= 0 || viewport_size.y <= 0) {
            viewport_size = render_buffers_rd->get_internal_size();
        }
        if (viewport_size.x <= 0 || viewport_size.y <= 0) {
            viewport_size = Size2i(1, 1);
        }

        RID pipeline_render_target = r_render_target;
        RID present_render_target;
        RID render_target_framebuffer;

        // Resolve the final presented viewport target separately from the pipeline's
        // internal scene color texture. The depth-tested compute path can only write
        // directly to the presented target when the viewport is single-view and the
        // render path is not relying on a later scale/upscale step.
        RID godot_render_target = render_buffers_rd->get_render_target();
        if (godot_render_target.is_valid()) {
            RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
            if (texture_storage) {
                render_target_framebuffer = texture_storage->render_target_get_rd_framebuffer(godot_render_target);
                present_render_target = texture_storage->render_target_get_rd_texture(godot_render_target);
            }
        }

        // GPU-001 Option B: when the engine invoked this composite at the
        // pre-upscale seam (before FSR2/MetalFX/TAA/tonemap consume the internal
        // color texture), the ONLY correct destination is the internal scene
        // buffer — every later consumer reads it, so one write serves the whole
        // single-view config matrix. Redirecting to the presented target here
        // would be overwritten by tonemap. The legacy (post-scene) phase keeps
        // the historical present-redirect for the paths that still use it
        // (forward mobile, multiview, reflection probes).
        const bool pre_upscale_phase = p_render_data != nullptr && p_render_data->gaussian_composite_pre_upscale;

        bool can_write_directly_to_present = false;
        if (!pre_upscale_phase && present_render_target.is_valid()) {
            const bool single_view = render_buffers_rd->get_view_count() == 1;
            const bool internal_matches_target = render_buffers_rd->get_internal_size() == render_buffers_rd->get_target_size();
            const bool scaling_disabled = render_buffers_rd->get_scaling_3d_mode() == RS::VIEWPORT_SCALING_3D_MODE_OFF;
            can_write_directly_to_present = single_view && internal_matches_target && scaling_disabled;
        }

        RID composite_target = pipeline_render_target;
        if (!pre_upscale_phase && (!composite_target.is_valid() || can_write_directly_to_present)) {
            composite_target = present_render_target;
        }
        output_cache.last_render_target = composite_target;

        bool composited = false;
        auto &subsystem_state = p_renderer->get_subsystem_state();
        if (p_painterly_active && subsystem_state.painterly_renderer.is_valid()) {
            RID depth_for_composite = p_renderer->get_painterly_depth_texture();
            if (!depth_for_composite.is_valid()) {
                depth_for_composite = p_cached_depth;
            }
            if (depth_for_composite.is_valid()) {
                composited = subsystem_state.painterly_renderer->composite_painterly_output(
                        p_renderer, p_render_data, p_final_output, depth_for_composite, viewport_size);
                if (composited) {
                    output_cache.last_viewport_copy_success = true;
                }
            }
        }

        const bool scene_depth_test_requested = gs_get_composite_depth_test_enabled();
        const GSSceneCompositeDepthPolicy scene_depth_policy = gs_get_scene_composite_depth_policy();
        output_cache.last_strict_depth_contract_required =
                scene_depth_test_requested && scene_depth_policy == GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT;

        RID scene_depth = render_buffers_rd ? render_buffers_rd->get_depth_texture() : RID();
        const bool has_source_depth = p_cached_depth.is_valid();
        const bool has_scene_depth = scene_depth.is_valid();
        const bool depth_test_enabled = scene_depth_test_requested && has_source_depth && has_scene_depth;
        const bool missing_required_scene_depth = scene_depth_test_requested && (!has_source_depth || !has_scene_depth);
        const bool allow_scene_blend_fallback = scene_depth_policy == GS_SCENE_COMPOSITE_DEPTH_POLICY_RELAXED;


        if (!composited && (render_target_framebuffer.is_valid() || composite_target.is_valid())) {
            if (missing_required_scene_depth && !allow_scene_blend_fallback) {
                gs_log_scene_depth_contract_skip_once(!has_source_depth, !has_scene_depth);
                output_cache.last_output_copy_attempted = true;
                output_cache.last_output_copy_success = false;
                output_cache.last_output_copy_error = "strict depth composite skipped: source or scene depth missing";
                output_cache.last_viewport_copy_success = false;
                output_cache.last_depth_test_honored = false;
                output_cache.last_copy_degraded = true;
                output_cache.last_copy_degradation_reason = "strict depth composite skipped: source or scene depth missing";
            } else if (!pre_upscale_phase && render_target_framebuffer.is_valid() && !depth_test_enabled) {
                // Legacy-phase only: this graphics blend writes the PRESENTED
                // framebuffer, which in the pre-upscale phase would be overwritten
                // by tonemap — pre-upscale routes depth_test=false through the
                // compute path into the internal buffer instead (source_decode_srgb
                // forces the compute branch in copy_to_render_target).
                FramebufferCopyParams params;
                params.source_texture = p_final_output;
                params.framebuffer = render_target_framebuffer;
                params.destination_texture = present_render_target;
                params.viewport_size = viewport_size;
                params.composite_with_destination = true;
                params.source_is_premultiplied = true;
                output_cache.last_output_copy_attempted = true;
                const bool framebuffer_copy_success = copy_to_framebuffer(params);
                output_cache.last_output_copy_success = framebuffer_copy_success;
                if (!framebuffer_copy_success) {
                    output_cache.last_output_copy_error = "framebuffer copy failed";
                    output_cache.last_copy_degraded = true;
                    output_cache.last_copy_degradation_reason = output_cache.last_output_copy_error;
                }
                if (missing_required_scene_depth && allow_scene_blend_fallback) {
                    WARN_PRINT_ONCE("[OutputCompositor] Relaxed depth policy: source or scene depth missing; compositing WITHOUT depth (splats will not be occluded by meshes until depths become available).");
                    output_cache.last_depth_test_honored = false;
                    output_cache.last_copy_degraded = true;
                    output_cache.last_copy_degradation_reason = "relaxed depth composite fallback: source or scene depth missing";
                }
            } else if (!pre_upscale_phase && depth_test_enabled && !composite_target.is_valid() && render_target_framebuffer.is_valid()) {
                // The presented target has no RD texture (DIRECT_TO_SCREEN-style): the
                // depth-aware compute composite has no image to write, and the else-branch
                // below would fail with an invalid destination while leaving the honored
                // flag at its default true. Handle it explicitly: STRICT skips like the
                // missing-depth contract; RELAXED blends without depth, flagged.
                WARN_PRINT_ONCE("[OutputCompositor] Presented target has no RD texture; the depth-aware compute composite cannot run on this target.");
                output_cache.last_output_copy_attempted = true;
                output_cache.last_depth_test_honored = false;
                output_cache.last_copy_degraded = true;
                if (!allow_scene_blend_fallback) {
                    output_cache.last_output_copy_success = false;
                    output_cache.last_output_copy_error = "strict depth composite skipped: destination has no RD texture";
                    output_cache.last_viewport_copy_success = false;
                    output_cache.last_copy_degradation_reason = output_cache.last_output_copy_error;
                } else {
                    FramebufferCopyParams params;
                    params.source_texture = p_final_output;
                    params.framebuffer = render_target_framebuffer;
                    params.destination_texture = present_render_target;
                    params.viewport_size = viewport_size;
                    params.composite_with_destination = true;
                    params.source_is_premultiplied = true;
                    const bool framebuffer_copy_success = copy_to_framebuffer(params);
                    output_cache.last_output_copy_success = framebuffer_copy_success;
                    output_cache.last_copy_degradation_reason = "relaxed depth composite fallback: destination has no RD texture";
                    if (!framebuffer_copy_success) {
                        output_cache.last_output_copy_error = "framebuffer copy failed";
                    }
                }
            } else {
                OutputCopyParams params;
                params.source_texture = p_final_output;
                params.source_depth = p_cached_depth;
                params.destination_texture = composite_target;
                params.destination_depth = scene_depth;
                params.viewport_size = viewport_size;
                params.composite_with_destination = true;
                params.source_is_premultiplied = true;
                // Pre-upscale destination is the LINEAR pre-tonemap scene buffer;
                // the source encoding is premultiplied sRGB-encoded LDR (contract
                // in output_compositor_interfaces.h), so request the decode.
                params.source_decode_srgb = pre_upscale_phase;
                params.depth_test_enabled = depth_test_enabled;
                params.depth_linearize_mul = params.z_near;
                params.depth_linearize_add = params.z_far;
                if (p_render_data && p_render_data->scene_data) {
                    params.z_near = p_render_data->scene_data->z_near;
                    params.z_far = p_render_data->scene_data->z_far;
                    params.depth_is_orthogonal = p_render_data->scene_data->cam_orthogonal;
                    // Shared derivation (gs_scene_depth_linearize.h) — the per-splat
                    // raster clip must convert scene depth with identical math.
                    const GSSceneDepthLinearize lin = gs_derive_scene_depth_linearize(
                            p_render_data->scene_data->cam_projection,
                            params.depth_is_orthogonal, params.z_near, params.z_far);
                    params.depth_linearize_mul = lin.mul;
                    params.depth_linearize_add = lin.add;
                }
                OutputCopyResult copy_result = copy_to_render_target(params);
                const bool relaxed_missing_depth_fallback = missing_required_scene_depth && allow_scene_blend_fallback;
                output_cache.last_output_copy_attempted = true;
                output_cache.last_output_copy_success = copy_result.success;
                output_cache.last_output_copy_error = copy_result.error;
                // An outright copy failure leaves copy_result.depth_test_honored at its
                // default true — never report a failed depth-requested composite as honored.
                const bool depth_honored = copy_result.depth_test_honored &&
                        !(depth_test_enabled && !copy_result.success);
                output_cache.last_depth_test_honored = relaxed_missing_depth_fallback ? false : depth_honored;
                if (!copy_result.success) {
                    output_cache.last_copy_degraded = true;
                    output_cache.last_copy_degradation_reason = copy_result.error.is_empty()
                            ? String("render target copy failed")
                            : copy_result.error;
                }
                if (relaxed_missing_depth_fallback) {
                    WARN_PRINT_ONCE("[OutputCompositor] Relaxed depth policy: source or scene depth missing; composited WITHOUT depth (splats will not be occluded by meshes until depths become available).");
                    output_cache.last_copy_degraded = true;
                    if (copy_result.success) {
                        output_cache.last_copy_degradation_reason = "relaxed depth composite fallback: source or scene depth missing";
                    }
                } else if (!copy_result.depth_test_honored) {
                    output_cache.last_copy_degraded = true;
                    output_cache.last_copy_degradation_reason = copy_result.error.is_empty()
                            ? String("depth composite fallback did not honor requested depth test")
                            : copy_result.error;
                    if (scene_depth_policy == GS_SCENE_COMPOSITE_DEPTH_POLICY_STRICT) {
                        output_cache.last_viewport_copy_success = false;
                    }
                }
            }
        } else if (!composited) {
            output_cache.last_output_copy_attempted = true;
            output_cache.last_output_copy_success = false;
            output_cache.last_output_copy_error = "no valid render target for final output copy";
            output_cache.last_copy_degraded = true;
            output_cache.last_copy_degradation_reason = output_cache.last_output_copy_error;
        }
        r_render_target = composite_target.is_valid() ? composite_target : pipeline_render_target;
    }

    output_cache.render_buffers_commit_pending = false;
    output_cache.pending_painterly_commit = false;
}
