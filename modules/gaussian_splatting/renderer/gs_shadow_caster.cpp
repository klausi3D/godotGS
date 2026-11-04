#include "gs_shadow_caster.h"

#include "hlod_shadow_caster_plan.h"
#include "shader_compilation_helper.h"
#include "../core/gs_vector_alloc.h"
#include "../shaders/gs_shadow_caster.glsl.gen.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace {

// Mirror of `Params` in shaders/gs_shadow_caster.glsl (std430 push constant, 96 bytes).
struct ShadowCasterPushConstant {
	float light_from_frame[16]; // Column-major mat4.
	float viewport[4]; // xy = atlas rect size, z = alpha threshold, w = low-pass dilation (texels^2).
	uint32_t counts[4]; // x = draw records, y = slot stride, z = pancake, w = 0.
};
static_assert(sizeof(ShadowCasterPushConstant) == 96, "ShadowCasterPushConstant must match gs_shadow_caster.glsl Params");
static_assert(sizeof(ShadowCasterPushConstant) % 16 == 0, "Push constants are reflected in 16-byte units");

RD::Uniform storage_uniform(uint32_t p_binding, RID p_buffer) {
	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u.binding = p_binding;
	u.append_id(p_buffer);
	return u;
}

} // namespace

GsSplatShadowCaster::GsSplatShadowCaster() = default;

GsSplatShadowCaster::~GsSplatShadowCaster() {
	clear();
}

void GsSplatShadowCaster::_free_row_cast_buffer() {
	if (row_cast_buffer.is_valid() && row_cast_owner.device) {
		row_cast_owner.device->free(row_cast_buffer);
	}
	row_cast_buffer = RID();
	row_cast_owner.clear();
	row_cast_capacity = 0;
	row_cast_uploaded.clear();
}

bool GsSplatShadowCaster::_upload_row_casts(RenderingDevice *p_device, const LocalVector<uint32_t> &p_row_casts, String &r_error) {
	if (p_row_casts.is_empty()) {
		r_error = "no row cast flags";
		return false;
	}
	if (row_cast_buffer.is_valid() && (!row_cast_owner.matches(p_device) || !p_device->buffer_is_valid(row_cast_buffer))) {
		_free_row_cast_buffer();
	}
	const uint32_t bytes = p_row_casts.size() * sizeof(uint32_t);
	if (!row_cast_buffer.is_valid() || row_cast_capacity < p_row_casts.size()) {
		_free_row_cast_buffer();
		Vector<uint8_t> initial;
		if (!gs_resize_or_fail(initial, int64_t(bytes), "GsSplatShadowCaster row cast flags")) {
			r_error = "row cast staging allocation failed";
			return false;
		}
		memcpy(initial.ptrw(), p_row_casts.ptr(), bytes);
		row_cast_buffer = p_device->storage_buffer_create(bytes, initial);
		if (!row_cast_buffer.is_valid()) {
			r_error = "row cast buffer allocation failed";
			return false;
		}
		p_device->set_resource_name(row_cast_buffer, "GS_ShadowCaster_RowCasts");
		row_cast_owner.set(p_device);
		row_cast_capacity = p_row_casts.size();
		row_cast_uploaded = p_row_casts;
		return true;
	}
	bool same = row_cast_uploaded.size() == p_row_casts.size();
	for (uint32_t i = 0; same && i < p_row_casts.size(); ++i) {
		same = row_cast_uploaded[i] == p_row_casts[i];
	}
	if (!same) {
		if (p_device->buffer_update(row_cast_buffer, 0, bytes, p_row_casts.ptr()) != OK) {
			r_error = "row cast buffer upload failed";
			return false;
		}
		row_cast_uploaded = p_row_casts;
	}
	return true;
}

void GsSplatShadowCaster::clear() {
	_free_row_cast_buffer();
	pipeline_cache.clear();
	if (shader.is_valid() && shader_owner.device) {
		shader_owner.device->free(shader);
	}
	shader = RID();
	shader_owner.clear();
}

bool GsSplatShadowCaster::_ensure_shader(RenderingDevice *p_device, String &r_error) {
	if (shader.is_valid() && !shader_owner.matches(p_device)) {
		clear();
	}
	if (shader.is_valid()) {
		return true;
	}
	if (!shader_source_initialized) {
		if (!shader_source) {
			shader_source = std::make_unique<GsShadowCasterShaderRD>();
		}
		Vector<String> versions;
		versions.push_back(String());
		shader_source->initialize(versions);
		shader_source_initialized = true;
	}
	RID shader_version = shader_source->version_create();
	if (!shader_version.is_valid()) {
		r_error = "shadow caster shader version unavailable";
		return false;
	}
	Vector<String> stage_sources = shader_source->version_build_variant_stage_sources(shader_version, 0);
	shader_source->version_free(shader_version);
	if (stage_sources.size() <= RD::SHADER_STAGE_FRAGMENT) {
		r_error = "shadow caster shader stages missing";
		return false;
	}
	String compile_error;
	shader = ShaderCompilationHelper::compile_graphics_shader_on_device(p_device, stage_sources[RD::SHADER_STAGE_VERTEX],
			stage_sources[RD::SHADER_STAGE_FRAGMENT], "gs_shadow_caster", Vector<String>(), &compile_error);
	if (!shader.is_valid()) {
		r_error = "shadow caster shader compile failed: " + compile_error;
		return false;
	}
	shader_owner.set(p_device);

	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = RD::POLYGON_CULL_DISABLED;
	RD::PipelineDepthStencilState depth_state;
	depth_state.enable_depth_test = true;
	depth_state.enable_depth_write = true;
	// Reverse-Z, as Godot's own shadow casters (scene_shader_forward_clustered.cpp).
	depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
	RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled(0);
	pipeline_cache.setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, RD::PipelineMultisampleState(), depth_state,
			blend_state, 0);
	return true;
}

bool GsSplatShadowCaster::draw(RenderingDevice *p_device, RID p_shadow_framebuffer, const Rect2i &p_atlas_rect,
		const Projection &p_light_from_frame, const Inputs &p_inputs, String &r_error) {
	if (!p_device || !p_shadow_framebuffer.is_valid() || !p_device->framebuffer_is_valid(p_shadow_framebuffer)) {
		r_error = "shadow framebuffer invalid";
		return false;
	}
	if (p_atlas_rect.size.x <= 0 || p_atlas_rect.size.y <= 0 || p_inputs.instance_slots == 0 || p_inputs.draw_count == 0 ||
			p_inputs.slot_stride == 0) {
		r_error = "empty caster draw";
		return false;
	}
	const RID buffers[4] = { p_inputs.atlas_gaussian_buffer, p_inputs.instance_buffer, p_inputs.chunk_meta_buffer,
		p_inputs.draw_list_buffer };
	for (const RID &buffer : buffers) {
		if (!buffer.is_valid() || !p_device->buffer_is_valid(buffer)) {
			r_error = "caster input buffer invalid on the shadow device";
			return false;
		}
	}
	if (!p_inputs.row_casts || !_upload_row_casts(p_device, *p_inputs.row_casts, r_error)) {
		if (r_error.is_empty()) {
			r_error = "row cast flags missing";
		}
		return false;
	}
	if (!_ensure_shader(p_device, r_error)) {
		return false;
	}
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (!uniform_set_cache) {
		r_error = "uniform set cache unavailable";
		return false;
	}
	const RID uniform_set = uniform_set_cache->get_cache(shader, 0, storage_uniform(0, buffers[0]),
			storage_uniform(1, buffers[1]), storage_uniform(2, buffers[2]), storage_uniform(3, buffers[3]),
			storage_uniform(4, row_cast_buffer));
	if (!uniform_set.is_valid()) {
		r_error = "caster uniform set unavailable";
		return false;
	}
	const RID pipeline = pipeline_cache.get_render_pipeline(RD::INVALID_ID, p_device->framebuffer_get_format(p_shadow_framebuffer));
	if (!pipeline.is_valid()) {
		r_error = "caster pipeline unavailable for the shadow framebuffer format";
		return false;
	}

	ShadowCasterPushConstant push{};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			push.light_from_frame[c * 4 + r] = float(p_light_from_frame.columns[c][r]);
		}
	}
	push.viewport[0] = float(p_atlas_rect.size.x);
	push.viewport[1] = float(p_atlas_rect.size.y);
	push.viewport[2] = gs_shadow_caster::kAlphaThreshold;
	push.viewport[3] = gs_shadow_caster::kLowPassTexels2;
	push.counts[0] = p_inputs.draw_count;
	push.counts[1] = p_inputs.slot_stride;
	push.counts[2] = p_inputs.pancake ? 1u : 0u;
	push.counts[3] = 0u;

	RD::DrawListID draw_list = p_device->draw_list_begin(p_shadow_framebuffer, RD::DRAW_DEFAULT_ALL, VectorView<Color>(), 0.0f, 0,
			Rect2(p_atlas_rect));
	if (draw_list == RD::INVALID_ID) {
		r_error = "draw_list_begin failed";
		return false;
	}
	p_device->draw_list_bind_render_pipeline(draw_list, pipeline);
	p_device->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
	p_device->draw_list_set_push_constant(draw_list, &push, sizeof(push));
	p_device->draw_list_draw(draw_list, false, p_inputs.instance_slots, 6);
	p_device->draw_list_end();
	return true;
}
