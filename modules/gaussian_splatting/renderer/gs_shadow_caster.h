#ifndef GS_SHADOW_CASTER_H
#define GS_SHADOW_CASTER_H

// GPU half of the HLOD splat shadow caster (#1095, #1163): one graphics pipeline (shaders/gs_shadow_caster.glsl) that
// draws the main view's HLOD draw list as depth-only Gaussian quads into the shadow-atlas rect Godot hands a directional
// shadow pass. Planning and the depth convention live in hlod_shadow_caster_plan.h (device-free, CPU-tested).
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
	struct Inputs {
		RID atlas_gaussian_buffer;
		RID instance_buffer;
		RID chunk_meta_buffer;
		RID draw_list_buffer; // DrawnNodeGPU[] of the relative bundle (visible_chunk_buffer).
		uint32_t draw_count = 0;
		uint32_t slot_stride = 0; // max_chunk_splats
		uint32_t instance_slots = 0; // draw_count * slot_stride
		bool pancake = true;
		const LocalVector<uint32_t> *row_casts = nullptr; // Per bundle row, uploaded into the caster-owned buffer.
	};

	GsSplatShadowCaster();
	~GsSplatShadowCaster();

	// Records the caster draw into p_shadow_framebuffer, restricted to p_atlas_rect, with depth test and write against the
	// mesh casters Godot already drew there. False (with r_error) if nothing was recorded.
	bool draw(RenderingDevice *p_device, RID p_shadow_framebuffer, const Rect2i &p_atlas_rect,
			const Projection &p_light_from_frame, const Inputs &p_inputs, String &r_error);
	void clear();

private:
	bool _ensure_shader(RenderingDevice *p_device, String &r_error);
	bool _upload_row_casts(RenderingDevice *p_device, const LocalVector<uint32_t> &p_row_casts, String &r_error);
	void _free_row_cast_buffer();

	std::unique_ptr<GsShadowCasterShaderRD> shader_source;
	bool shader_source_initialized = false;
	RID shader;
	GaussianSplatting::BufferOwnership shader_owner;
	PipelineCacheRD pipeline_cache;
	// Row cast flags (uint per row), re-uploaded only when they change. Freed by clear() through its owner.
	RID row_cast_buffer;
	uint32_t row_cast_capacity = 0;
	LocalVector<uint32_t> row_cast_uploaded;
	GaussianSplatting::BufferOwnership row_cast_owner;
};

#endif // GS_SHADOW_CASTER_H
