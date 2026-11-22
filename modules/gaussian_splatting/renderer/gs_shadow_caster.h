#ifndef GS_SHADOW_CASTER_H
#define GS_SHADOW_CASTER_H

// GPU half of the HLOD splat shadow caster (#1095, #1163): one graphics pipeline (shaders/gs_shadow_caster.glsl) that
// draws the main view's HLOD draw list as depth-only Gaussian quads into a directional/spot atlas rect or omni cube face.
// Planning and the depth convention live in hlod_shadow_caster_plan.h (device-free, CPU-tested).
//
// Ownership: the shader is compiled on the device passed to draw() and freed through that recorded owner by clear()
// (renderer teardown) or when draw() sees a different device. Uniform sets come from UniformSetCacheRD, which drops a set
// as soon as any buffer it binds is freed, so no set outlives the HLOD bundle it names. Pipelines live in the
// PipelineCacheRD, cleared with the shader.

#include "core/math/projection.h"
#include "core/math/rect2i.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "tile_render_types.h"

#include <cstdint>
#include <memory>

class GsShadowCasterShaderRD;

class GsSplatShadowCaster {
public:
	// Exact std430 mirror of the caster's binding 6. Kept out of push constants
	// so the 96-byte draw packet remains below Vulkan's 128-byte minimum limit.
	struct DeformationUniform {
		float wind_dir_strength[4] = {};
		float wind_time_config[4] = {};
		float effector_meta[4] = {};
		float effector_spheres[4][4] = {};
		float effector_configs[4][4] = {};
		float effector_opacity_configs[4][4] = {};
		float frame_anchor[4] = {};
	};
	static_assert(sizeof(DeformationUniform) == 256, "Caster deformation buffer must match GLSL std430 layout");
	static_assert(sizeof(DeformationUniform) % 16 == 0, "Caster deformation buffer requires 16-byte rounding");

	struct Inputs {
		RID atlas_gaussian_buffer;
		RID instance_buffer;
        RID region_override_buffer;
		RID chunk_meta_buffer;
		RID page_table_buffer;
		RID draw_list_buffer; // DrawnNodeGPU[] of the relative bundle (visible_chunk_buffer).
		uint32_t draw_count = 0;
		uint32_t slot_stride = 0; // max_chunk_splats
		uint32_t instance_slots = 0; // draw_count * slot_stride
		bool pancake = true;
		bool perspective = false; // Spot or omni cube: quotient-rule footprint and depth Jacobians, no pancake.
		// The renderer-wide opacity multiplier (render_config.opacity_multiplier: GaussianSplatWorld3D.opacity via the
		// world's "opacity_multiplier" override), the same value tile_binning.glsl multiplies into every splat of every row.
		float opacity_multiplier = 1.0f;
		const LocalVector<uint32_t> *row_casts = nullptr; // Per bundle row, uploaded into the caster-owned buffer.
		DeformationUniform deformation;
	};

	GsSplatShadowCaster();
	~GsSplatShadowCaster();

	// Records the caster draw into p_shadow_framebuffer, restricted to p_atlas_rect, with depth test and write against the
	// mesh casters Godot already drew there. False (with r_error) if nothing was recorded.
	bool draw(RenderingDevice *p_device, RID p_shadow_framebuffer, const Rect2i &p_atlas_rect,
			const Projection &p_light_from_frame, const Inputs &p_inputs, String &r_error);
	void clear();
	uint64_t get_owned_buffer_bytes() const {
		return uint64_t(row_cast_capacity) * sizeof(uint32_t) + (deformation_buffer.is_valid() ? sizeof(DeformationUniform) : 0);
	}

private:
	bool _ensure_shader(RenderingDevice *p_device, String &r_error);
	bool _upload_row_casts(RenderingDevice *p_device, const LocalVector<uint32_t> &p_row_casts, String &r_error);
	void _free_row_cast_buffer();
	bool _upload_deformation(RenderingDevice *p_device, const DeformationUniform &p_deformation, String &r_error);
	void _free_deformation_buffer();

	std::unique_ptr<GsShadowCasterShaderRD> shader_source;
	bool shader_source_initialized = false;
#ifdef TESTS_ENABLED
    uint32_t test_game_shader_variant = 0;
#endif
	RID shader;
	GaussianSplatting::BufferOwnership shader_owner;
	PipelineCacheRD pipeline_cache;
	// Row cast flags (uint per row), re-uploaded only when they change. Freed by clear() through its owner.
	RID row_cast_buffer;
	uint32_t row_cast_capacity = 0;
	LocalVector<uint32_t> row_cast_uploaded;
	GaussianSplatting::BufferOwnership row_cast_owner;
	// Current frame's wind state, uploaded for every caster draw. Owned and freed
	// by this caster on teardown, device replacement, or a failed upload/draw.
	RID deformation_buffer;
	GaussianSplatting::BufferOwnership deformation_owner;
};

#endif // GS_SHADOW_CASTER_H
