#ifndef GS_OUTPUT_COMPOSITOR_H
#define GS_OUTPUT_COMPOSITOR_H

#include "output_compositor_interfaces.h"
#include "render_device_manager.h"
#include "core/object/ref_counted.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include <functional>
#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"

class GaussianSplatRenderer;
class RenderDataRD;
class RenderSceneBuffersRD;
class ViewportBlitShaderRD;

// Concrete implementation of IOutputCompositor
// Manages framebuffer caching, viewport blitting, and final output composition
class OutputCompositor : public RefCounted, public IOutputCompositor {
    GDCLASS(OutputCompositor, RefCounted);
    friend class GaussianSplatRenderer;

public:
    OutputCompositor();
    ~OutputCompositor();

    // IOutputCompositor interface - Lifecycle
    Error initialize(RenderingDevice *p_device) override;
    void shutdown() override;
    bool is_initialized() const override { return initialized && rd != nullptr; }

    // IOutputCompositor interface - Core output operations
    OutputCopyResult copy_to_render_target(const OutputCopyParams &p_params) override;
    bool copy_to_framebuffer(const FramebufferCopyParams &p_params) override;

    // IOutputCompositor interface - Framebuffer management
    RID get_cached_framebuffer(RenderingDevice *p_device, const RID &p_texture) override;
    void clear_cached_framebuffers() override;
    void clear_viewport_blit_resources() override;

    // IOutputCompositor interface - Attachment validation
    bool validate_framebuffer_attachments(RenderingDevice *p_device, const Vector<RID> &p_attachments,
            Vector<AttachmentValidationInfo> &r_infos, Size2i &r_extent, RD::TextureSamples &r_samples, String &r_error) override;

    // IOutputCompositor interface - State queries
    bool get_last_copy_success() const override { return output_cache.last_viewport_copy_success; }
    Size2i get_last_copy_source_size() const override { return output_cache.last_viewport_copy_source_size; }
    Size2i get_last_copy_dest_size() const override { return output_cache.last_viewport_copy_dest_size; }

    // IOutputCompositor interface - Final render texture management
    void set_final_render_texture(const RID &p_texture) override { final_render_texture = p_texture; }
    RID get_final_render_texture() const override { return final_render_texture; }
    void set_has_valid_render(bool p_valid) override { output_cache.has_valid_render = p_valid; }
    bool get_has_valid_render() const override { return output_cache.has_valid_render; }
    RID get_cached_render_depth() const { return output_cache.cached_render_depth; }

    // IOutputCompositor interface - Implementation info
    String get_name() const override { return "OutputCompositor"; }

    // Set the device manager for resource tracking (required for full functionality)
    void set_device_manager(Ref<RenderDeviceManager> p_device_manager);

    // Callback type for getting texture format from parent renderer
    using TextureFormatCallback = std::function<RD::TextureFormat(RenderingDevice *, RID)>;
    void set_texture_format_callback(TextureFormatCallback p_callback) { texture_format_callback = p_callback; }

    // Set internal render size (from painterly pass graph)
    void set_internal_render_size(const Size2i &p_size) { internal_render_size = p_size; }

    // Cached framebuffer entry. `last_access_id` is bumped on every hit/insert
    // and consumed by the LRU eviction in get_cached_framebuffer().
    struct CachedFramebuffer {
        RID framebuffer;
        RenderingDevice *device = nullptr;
        uint64_t last_access_id = 0;
    };

    // Framebuffer validation cache entry. `last_access_id` is bumped on every
    // hit/insert and consumed by the LRU eviction in
    // validate_framebuffer_attachments().
    struct FramebufferValidationCacheEntry {
        bool valid = false;
        Size2i extent = Size2i();
        RD::TextureSamples samples = RD::TEXTURE_SAMPLES_1;
        Vector<AttachmentValidationInfo> infos;
        uint64_t last_access_id = 0;
    };

    struct OutputCacheState {
        bool has_valid_render = false;
        Size2i last_viewport_copy_source_size = Size2i();
        Size2i last_viewport_copy_dest_size = Size2i();
        bool last_viewport_copy_success = false;
        bool last_output_copy_attempted = false;
        bool last_output_copy_success = false;
        String last_output_copy_error;
        bool last_depth_test_honored = true;
        bool last_copy_degraded = false;
        String last_copy_degradation_reason;
        bool last_strict_depth_contract_required = false;
        RID last_render_target;
        bool cached_render_valid = false;
        Transform3D cached_render_camera_to_world_transform;
        Projection cached_render_camera_projection;
        Size2i cached_render_viewport_size = Size2i();
        Size2i cached_render_internal_size = Size2i();
        RID cached_render_depth;
        bool cached_render_painterly = false;
        uint64_t cached_render_content_generation = 0;
        uint64_t cached_render_cull_config_signature = 0;
        uint64_t cached_render_color_grading_signature = 0;
        uint64_t cached_render_lighting_signature = 0;
        bool render_buffers_commit_pending = false;
        Size2i pending_render_buffers_size = Size2i();
        bool pending_painterly_commit = false;
        HashMap<uint64_t, CachedFramebuffer> cached_framebuffers;
        HashMap<uint64_t, FramebufferValidationCacheEntry> framebuffer_validation_cache;
    };

    // Output cache access (transitional - allows renderer coordination)
    OutputCacheState &get_cache_state() { return output_cache; }
    const OutputCacheState &get_cache_state() const { return output_cache; }
    void set_cached_render_reuse_enabled(bool p_enabled) {
        cached_render_reuse_enabled = p_enabled;
        if (!cached_render_reuse_enabled) {
            invalidate_cached_render();
        }
    }
    bool is_cached_render_reuse_enabled() const { return cached_render_reuse_enabled; }
    void invalidate_cached_render();
    bool can_reuse_cached_render(const Transform3D &p_view_transform, const Projection &p_projection,
            const Size2i &p_viewport_size, bool p_painterly_active, const RID &p_final_render_texture,
            uint64_t p_content_generation = 0,
            uint64_t p_cull_config_signature = 0,
            uint64_t p_color_grading_signature = 0, uint64_t p_lighting_signature = 0,
            bool p_require_valid_depth = false) const;
    void update_render_cache_signature(const Transform3D &p_view_transform, const Projection &p_projection,
            const Size2i &p_viewport_size, bool p_painterly_active, const RID &p_cached_depth,
            const Size2i &p_internal_size, const RID &p_final_render_texture,
            uint64_t p_content_generation = 0,
            uint64_t p_cull_config_signature = 0,
            uint64_t p_color_grading_signature = 0, uint64_t p_lighting_signature = 0,
            bool p_require_valid_depth = false);

    // Integrate final output into the viewport or render target (moved from renderer)
    void integrate_final_output(GaussianSplatRenderer *p_renderer, RenderDataRD *p_render_data, RenderSceneBuffersRD *render_buffers_rd,
            const RID &p_final_output, RID &r_render_target, const Size2i &p_viewport_size, bool p_defer_commit,
            bool p_painterly_active, const RID &p_cached_depth);

    // Statistics
    uint32_t get_cached_framebuffer_count() const { return output_cache.cached_framebuffers.size(); }
    uint32_t get_blit_variant_count() const { return viewport_blit_variants.size(); }
    uint32_t get_viewport_blit_scratch_count() const { return viewport_blit_scratch.size(); }
    uint32_t get_framebuffer_validation_cache_count() const { return output_cache.framebuffer_validation_cache.size(); }
    static constexpr uint32_t get_max_cached_framebuffer_formats() { return MAX_CACHED_FRAMEBUFFER_FORMATS; }

    // Test-only: directly drive synthetic LRU entries through the same
    // eviction path the production cache uses. Lets the regression test
    // assert the cap holds without round-tripping real GPU framebuffers (the
    // attachment-validation path requires color/depth attachments owned by
    // the test, which makes a 16-format sweep awkward). Both helpers reuse
    // the production add/erase site so any future refactor of eviction logic
    // covers them too.
    void test_force_insert_cached_framebuffer(uint64_t p_key);
    void test_force_insert_framebuffer_validation(uint64_t p_key);

    // Debug seam published by _copy_final_output_compute. Test-only contract: the
    // hazard-repro test reads this to verify the scratch-copy path was exercised
    // (vs. graphics fallback or the no-composite path). Do not consume from
    // production callers — the field set and "valid" semantics may change as the
    // composite path evolves. Kept always-compiled so the struct definition and
    // assignment sites stay readable without TESTS_ENABLED preprocessor sprawl;
    // the marginal cost in shipping builds is one 8-byte struct per OutputCompositor.
    struct LastCompositeStats {
        bool valid = false;
        bool composite_with_destination = false;
        bool scratch_used = false;
        bool scratch_reused = false;
        bool fallback_due_to_missing_copy_from = false;
        uint32_t binding_count = 0;
    };
    LastCompositeStats get_last_composite_stats() const { return last_composite_stats; }

protected:
    static void _bind_methods();

private:
    // Viewport blit format enum (for shader variant selection)
    enum class ViewportBlitFormat : uint32_t {
        RGBA8 = 0,
        RGBA16F = 1,
        RGBA32F = 2,
    };

    enum class TrackedResourceKind {
        Texture,
        Framebuffer,
        ComputePipeline,
        Shader,
        Sampler,
    };

    // Viewport blit shader/pipeline variant
    struct ViewportBlitVariant {
        RID shader;
        RID pipeline;
        RenderingDevice *owner_device = nullptr;
    };

    struct ViewportBlitSampler {
        RID sampler;
        RenderingDevice *owner_device = nullptr;
    };

    // Scratch texture used to break the R/W hazard in the composite path: destination
    // is copied here before the compute dispatch, the shader reads from this via a
    // sampler, and only writes the destination (which is writeonly inside the shader).
    // `format` is the canonical storage format used to allocate the scratch (SRGB
    // inputs get swapped to UNORM equivalent to avoid sampler double-decode); the
    // pool reuses entries across destinations that share the canonical format.
    // `last_used_id` is bumped on every hit/insert and consumed by the LRU eviction.
    struct ViewportBlitScratch {
        RID texture;
        Size2i extent;
        RD::DataFormat format = RD::DATA_FORMAT_MAX;
        RenderingDevice *owner_device = nullptr;
        uint64_t last_used_id = 0;
    };
    // Per-(device, canonical-format) entry cap. Viewport resizes in the editor
    // create one scratch entry per intermediate extent; without an LRU the pool
    // accumulates indefinitely until shutdown. 4 is a balance between bounded
    // memory (~32 MiB at 1080p RGBA8) and absorbing dock-resize trajectories.
    static constexpr uint32_t VIEWPORT_BLIT_SCRATCH_MAX_PER_KIND = 4;
    uint64_t viewport_blit_scratch_next_id = 0;

    // Per-cache entry cap shared by `cached_framebuffers` and
    // `framebuffer_validation_cache`. Both are keyed by
    // (RenderingDevice*, attachment-RID-set) so multi-device sessions
    // (multi-window editor, RenderDoc capture, project reload) accumulate one
    // entry per unique attachment combination, never evicting. The scratch
    // pool's matching cap is VIEWPORT_BLIT_SCRATCH_MAX_PER_KIND=4 per kind;
    // these caches are not partitioned by kind so 8 is the global ceiling. On
    // insert past the cap we evict the entry with the lowest `last_access_id`
    // via the existing destroy path (see get_cached_framebuffer and
    // validate_framebuffer_attachments).
    static constexpr uint32_t MAX_CACHED_FRAMEBUFFER_FORMATS = 8;
    uint64_t cached_framebuffer_next_id = 0;
    uint64_t framebuffer_validation_next_id = 0;

    // State
    bool initialized = false;
    RenderingDevice *rd = nullptr;
    Ref<RenderDeviceManager> device_manager;
    TextureFormatCallback texture_format_callback;
    Size2i internal_render_size;

    // Framebuffer cache
    OutputCacheState output_cache;
    HashMap<uint64_t, bool> srgb_format_cache;

    // Viewport blit resources
    HashMap<uint64_t, ViewportBlitVariant> viewport_blit_variants;
    HashMap<uint64_t, ViewportBlitSampler> viewport_blit_samplers;
    HashMap<uint64_t, ViewportBlitScratch> viewport_blit_scratch;
    ViewportBlitShaderRD *viewport_blit_shader_source = nullptr;
    bool viewport_blit_shader_source_initialized = false;
    LastCompositeStats last_composite_stats;

    // Final render state
    RID final_render_texture;
    bool cached_render_reuse_enabled = true;

    // Helper methods
    RD::TextureFormat _get_texture_format(RenderingDevice *p_device, RID p_texture) const;
    bool _is_texture_srgb(RenderingDevice *p_device, RID p_texture);
    bool _is_depth_texture_valid(const RID &p_depth_texture) const;
    void _track_resource(const RID &p_rid, RenderingDevice *p_device, bool p_owned = true, const char *p_label = nullptr);
    void _forget_resource(const RID &p_rid);
    void _free_tracked_resource(RenderingDevice *p_fallback_device, RID &r_rid, TrackedResourceKind p_kind);

    // Viewport blit helpers
    bool _ensure_viewport_blit_pipeline(RenderingDevice *p_device, RD::DataFormat p_format, RID &r_shader, RID &r_pipeline);
    RID _ensure_viewport_blit_sampler(RenderingDevice *p_device);
    // Returns (or reuses) a scratch texture matching p_format/p_extent. r_reused tells
    // the caller whether this was a pool hit (true) or a fresh allocation (false).
    RID _ensure_viewport_blit_scratch(RenderingDevice *p_device, const RD::TextureFormat &p_format,
            const Size2i &p_extent, bool *r_reused);
    static bool _determine_viewport_blit_format(RD::DataFormat p_format, ViewportBlitFormat &r_format);
    static const char *_viewport_blit_define(ViewportBlitFormat p_format);
    static uint64_t _viewport_blit_key(RenderingDevice *p_device, ViewportBlitFormat p_format);
    static uint64_t _viewport_blit_scratch_key(RenderingDevice *p_device, RD::DataFormat p_format, const Size2i &p_extent);

    // Compute copy helper
    bool _copy_final_output_compute(RenderingDevice *p_device, RID p_source, RID p_destination,
            const Size2i &p_source_extent, const Size2i &p_copy_extent, const Vector3i &p_destination_offset,
            bool p_composite_with_destination, bool p_source_is_premultiplied, bool p_destination_is_srgb,
            const RD::TextureFormat &p_destination_format, RID p_source_depth, RID p_destination_depth,
            bool p_depth_test_enabled, bool p_depth_is_orthogonal, float p_z_near, float p_z_far,
            float p_depth_linearize_mul, float p_depth_linearize_add, float p_depth_epsilon);

    // LRU eviction for the format-keyed caches. Both share the
    // MAX_CACHED_FRAMEBUFFER_FORMATS cap; on insert past the cap the entry
    // with the lowest `last_access_id` is freed via the existing destroy
    // path. Skips eviction when the key already exists (caller is refreshing
    // an entry, not adding a new one).
    void _evict_oldest_cached_framebuffer_if_needed(uint64_t p_incoming_key);
    void _evict_oldest_framebuffer_validation_if_needed(uint64_t p_incoming_key);

    // Attachment validation helpers
    uint64_t _compute_framebuffer_validation_key(RenderingDevice *p_device, const Vector<RID> &p_attachments) const;
    static bool _is_depth_attachment_format(RD::DataFormat p_format);
    static bool _is_srgb_format(RD::DataFormat p_format);
    static String _attachment_usage_label(bool p_is_depth);
    void test_reset_last_viewport_copy_state();
};

#endif // GS_OUTPUT_COMPOSITOR_H
