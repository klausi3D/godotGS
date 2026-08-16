#ifndef GS_OUTPUT_COMPOSITOR_INTERFACES_H
#define GS_OUTPUT_COMPOSITOR_INTERFACES_H

#include "core/math/vector2i.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device.h"

// View-space depth tolerance (meters) for GS<->scene depth comparisons. Single
// source of truth shared by the whole-pixel composite test (viewport_blit) and
// the per-splat raster clip (slice D) so the raster clip stays a provable
// superset of what the composite would keep.
static constexpr float GS_COMPOSITE_DEPTH_EPSILON_VIEW = 0.01f;

// Parameters for copying final output to render target
struct OutputCopyParams {
    RID source_texture;
    RID source_depth;
    RID destination_texture;
    RID destination_depth;
    Size2i viewport_size;
    bool composite_with_destination = false;
    bool source_is_premultiplied = false;
    // GPU-001 Option B source-encoding contract: the GS raster output (RGBA8,
    // see _resolve_compute_friendly_raster_format) is treated as premultiplied,
    // display-referred, sRGB-encoded LDR. Ground truth for that definition is
    // the shipped legacy composite: it wrote these bytes UNCONVERTED into the
    // presented target, whose content is sRGB-encoded — so "as presented" and
    // "sRGB-encoded" are the same claim. The raster/resolve shaders perform no
    // color-space conversion, so this holds for every asset source (PLY SH-DC
    // and SPZ direct color alike): whatever looked right under the legacy
    // present-target composite keeps exactly that meaning under the decode.
    // When the composite destination is the LINEAR pre-tonemap scene buffer
    // (pre-upscale phase), the compute blit must decode the source to linear
    // before blending; set this flag to request that decode. Leave false for
    // legacy post-tonemap destinations, whose content is sRGB-encoded like the
    // source.
    bool source_decode_srgb = false;
    bool depth_test_enabled = false;
    bool depth_is_orthogonal = false;
    float z_near = 0.0f;
    float z_far = 1.0f;
    float depth_linearize_mul = 0.0f;
    float depth_linearize_add = 1.0f;
    float depth_epsilon = GS_COMPOSITE_DEPTH_EPSILON_VIEW;
};

// Result from output copy operation
struct OutputCopyResult {
    bool success = false;
    Size2i source_size;
    Size2i dest_size;
    String error;
    // True iff the caller's depth-test request (params.depth_test_enabled) was
    // actually honored by the executed path. The graphics-blit fallback used
    // when destination lacks TEXTURE_USAGE_CAN_COPY_FROM_BIT (which would
    // otherwise require the scratch-copy compute path) does NOT carry depth
    // information through copy_to_fb_rect, so callers that requested
    // depth-tested compositing on such targets will see success=true here but
    // depth_test_honored=false. Set to true when depth was not requested.
    bool depth_test_honored = true;
    // True iff a requested source sRGB->linear decode (params.source_decode_srgb)
    // was actually performed by the executed path. The CopyEffects graphics
    // fallback used when the compute composite cannot run performs NO decode,
    // so a pre-upscale caller will see success=true (splats still composited —
    // presence beats absence) but source_decode_honored=false; the caller must
    // surface that as a degraded copy, never as a clean success. True when no
    // decode was requested.
    bool source_decode_honored = true;
};

// Parameters for framebuffer-based copy
struct FramebufferCopyParams {
    RID source_texture;
    RID framebuffer;
    RID destination_texture;
    Size2i viewport_size;
    bool composite_with_destination = false;
    bool source_is_premultiplied = false;
};

// Information about a framebuffer attachment's validation status
struct AttachmentValidationInfo {
    RID original_attachment;
    RD::TextureFormat original_format;
    bool is_depth = false;
};

// Pure abstract interface for output composition operations
// Handles framebuffer management, texture copying, and viewport integration
class IOutputCompositor {
public:
    virtual ~IOutputCompositor() = default;

    // Lifecycle
    virtual Error initialize(RenderingDevice *p_device) = 0;
    virtual void shutdown() = 0;
    virtual bool is_initialized() const = 0;

    // Core output operations
    virtual OutputCopyResult copy_to_render_target(const OutputCopyParams &p_params) = 0;
    virtual bool copy_to_framebuffer(const FramebufferCopyParams &p_params) = 0;

    // Framebuffer management
    virtual RID get_cached_framebuffer(RenderingDevice *p_device, const RID &p_texture) = 0;
    virtual void clear_cached_framebuffers() = 0;
    virtual void clear_viewport_blit_resources() = 0;

    // Attachment validation
    virtual bool validate_framebuffer_attachments(RenderingDevice *p_device, const Vector<RID> &p_attachments,
            Vector<AttachmentValidationInfo> &r_infos, Size2i &r_extent, RD::TextureSamples &r_samples, String &r_error) = 0;

    // State queries
    virtual bool get_last_copy_success() const = 0;
    virtual Size2i get_last_copy_source_size() const = 0;
    virtual Size2i get_last_copy_dest_size() const = 0;

    // Final render texture management
    virtual void set_final_render_texture(const RID &p_texture) = 0;
    virtual RID get_final_render_texture() const = 0;
    virtual void set_has_valid_render(bool p_valid) = 0;
    virtual bool get_has_valid_render() const = 0;

    // Implementation info
    virtual String get_name() const = 0;
};

#endif // GS_OUTPUT_COMPOSITOR_INTERFACES_H
