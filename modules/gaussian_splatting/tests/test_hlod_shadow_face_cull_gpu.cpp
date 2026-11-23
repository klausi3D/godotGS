extern "C" int test_hlod_shadow_face_cull_gpu_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../renderer/hlod_shadow_face_cull.h"
#include "../renderer/gs_shadow_caster.h"
#include "../renderer/shader_compilation_helper.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gs_region_override_buffer.h"
#include "../shaders/gs_shadow_caster.glsl.gen.h"

#include "core/math/random_pcg.h"
#include "servers/rendering/rendering_device.h"

#include <cstring>

// Per light view caster lists (renderer/hlod_shadow_face_cull.h) on the production caster shader. Every view of an omni
// cube, a spot and a directional cascade is drawn twice into a D32 target: from the whole draw list and from the view's
// list, which is uploaded byte for byte from the same entries. The depth must be byte-identical. A centre-only list (the
// unsafe negative control) must differ in at least one view, so the comparison can see a wrong cull.
namespace TestHlodShadowFaceCullGpu {

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] per-view caster lists draw byte-identical shadow depth; an unsafe cull does not") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		FAIL("GpuSorting harness RenderingDevice required");
		return;
	}
	struct Resources {
		RenderingDevice *rd;
		LocalVector<RID> owned;
		explicit Resources(RenderingDevice *p_rd) :
				rd(p_rd) {}
		~Resources() {
			for (uint32_t i = owned.size(); i-- > 0;) { rd->free(owned[i]); }
		}
		RID retain(RID p_rid) {
			if (p_rid.is_valid()) { owned.push_back(p_rid); }
			return p_rid;
		}
		RID buffer(uint32_t p_bytes, const void *p_data) {
			Vector<uint8_t> bytes;
			if (bytes.resize(p_bytes) != OK) { return RID(); }
			memcpy(bytes.ptrw(), p_data, p_bytes);
			return retain(rd->storage_buffer_create(p_bytes, bytes));
		}
	} resources(rd);

	constexpr uint32_t size = 128;
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_D32_SFLOAT;
	format.width = format.height = size;
	format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	if (!rd->texture_is_format_supported_for_usage(format.format, format.usage_bits)) {
		FAIL("D32 shadow target with readback is required");
		return;
	}
	const RID target = resources.retain(rd->texture_create(format, RD::TextureView()));
	Vector<RID> attachments;
	attachments.push_back(target);
	const RID framebuffer = target.is_valid() ? resources.retain(rd->framebuffer_create(attachments)) : RID();
	if (!framebuffer.is_valid()) {
		FAIL("view-list draw target unavailable");
		return;
	}
	GsShadowCasterShaderRD source;
	Vector<String> variants;
	variants.push_back(String());
	source.initialize(variants);
	const RID version = source.version_create();
	if (!version.is_valid()) {
		FAIL("production shadow shader source unavailable");
		return;
	}
	const Vector<String> stages = source.version_build_variant_stage_sources(version, 0);
	source.version_free(version);
	if (stages.size() <= RD::SHADER_STAGE_FRAGMENT) {
		FAIL("production shadow stages unavailable");
		return;
	}
	String shader_error;
	const RID shader = resources.retain(ShaderCompilationHelper::compile_graphics_shader_on_device(rd,
			stages[RD::SHADER_STAGE_VERTEX], stages[RD::SHADER_STAGE_FRAGMENT], "hlod_view_list_pixels", Vector<String>(), &shader_error));
	if (!shader.is_valid()) {
		FAIL("production shadow compilation failed: ", shader_error.utf8().get_data());
		return;
	}
	RD::PipelineRasterizationState raster;
	raster.cull_mode = RD::POLYGON_CULL_DISABLED;
	RD::PipelineDepthStencilState depth;
	depth.enable_depth_test = depth.enable_depth_write = true;
	depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
	const RID pipeline = resources.retain(rd->render_pipeline_create(shader, rd->framebuffer_get_format(framebuffer), RD::INVALID_ID,
			RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), depth, RD::PipelineColorBlendState::create_disabled(0)));
	if (!pipeline.is_valid()) {
		FAIL("production shadow pipeline unavailable");
		return;
	}

	// A world of 96 identity-placed nodes around the light, 32 splats each, every splat's three-sigma support inside its
	// node AABB (the loader-validated contract the lists rely on). Payload positions are node-relative.
	constexpr uint32_t node_count = 96;
	constexpr uint32_t node_splats = 32;
	const Vector3 light(0.5f, 1.0f, -2.0f);
	const Vector3 camera(3.0f, 1.5f, 4.0f); // The draw list's frame camera.
	RandomPCG rng(0x0f1e2d3cu);
	LocalVector<PackedGaussian> atlas;
	LocalVector<ChunkMetaGPU> chunks;
	gs_shadow_refresh::PublishedContent content;
	content.gpu_draws.resize(node_count);
	content.geometry.resize(node_count);
	for (uint32_t n = 0; n < node_count; ++n) {
		const Vector3 center = light + Vector3(rng.random(-30.0f, 30.0f), rng.random(-12.0f, 12.0f), rng.random(-30.0f, 30.0f));
		const Vector3 half(rng.random(1.0f, 4.0f), rng.random(1.0f, 4.0f), rng.random(1.0f, 4.0f));
		ChunkMetaGPU meta = {};
		meta.atlas_base = atlas.size();
		meta.splat_count = node_splats;
		chunks.push_back(meta);
		for (uint32_t s = 0; s < node_splats; ++s) {
			PackedGaussian g = {};
			const float max_sigma = std::fmin(half.x, std::fmin(half.y, half.z)) / 3.0f;
			const float sigma = (s % 4 == 0) ? max_sigma : rng.random(0.03f, max_sigma);
			g.scale[0] = sigma;
			g.scale[1] = rng.random(0.02f, sigma);
			g.scale[2] = rng.random(0.02f, sigma);
			const float reach = 3.0f * sigma;
			for (int a = 0; a < 3; ++a) {
				const float lo = -half[a] + reach, hi = half[a] - reach;
				g.position[a] = (s % 2 == 0) ? (rng.randf() < 0.5f ? lo : hi) : rng.random(lo, std::fmax(lo, hi));
			}
			const Quaternion q(Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f).normalized(), rng.random(0.0f, 6.2831853f));
			g.rotation[0] = q.x;
			g.rotation[1] = q.y;
			g.rotation[2] = q.z;
			g.rotation[3] = q.w;
			g.opacity = rng.random(0.55f, 1.0f);
			atlas.push_back(g);
		}
		DrawnNodeGPU drawn = {};
		drawn.instance_id = 0;
		drawn.chunk_id = n;
		drawn.fade = 1.0f;
		const Vector3 offset = center - camera;
		drawn.camera_offset[0] = offset.x;
		drawn.camera_offset[1] = offset.y;
		drawn.camera_offset[2] = offset.z;
		content.gpu_draws[n] = drawn;
		content.geometry[n].bounds = AABB(center - half, half * 2.0f);
		content.geometry[n].support_kind = gs_shadow_face_cull::SUPPORT_WORLD_NODE_AABB;
	}
	InstanceDataGPU row = {};
	row.rotation[3] = row.inv_rotation[3] = row.translation_scale[3] = 1.0f;
	row.params[0] = 1.0f;
	row.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
	LocalVector<InstanceDataGPU> rows;
	rows.push_back(row);
	LocalVector<real_t> margins;
	margins.push_back(0.0f);
	LocalVector<uint32_t> casts;
	casts.push_back(1u);
	gs_shadow_caster::FrameRecord record;
	record.valid = true;
	record.draw_count = node_count;
	record.max_chunk_splats = node_splats;
	record.camera = camera;
	gs_shadow_face_cull::Snapshot snapshot;
	snapshot.build(record, content, rows, margins, 1);
	if (!snapshot.valid) {
		FAIL("snapshot refused the fixture list");
		return;
	}

	const uint32_t page = 0u;
	GsSplatShadowCaster::DeformationUniform deformation; // Wind and effectors off.
	const RegionOverrideGPU neutral_region = GSRegionOverrideBuffer::neutral();
	const RID shared[8] = {
		resources.buffer(atlas.size() * sizeof(PackedGaussian), atlas.ptr()),
		resources.buffer(sizeof(InstanceDataGPU), &row),
		resources.buffer(chunks.size() * sizeof(ChunkMetaGPU), chunks.ptr()),
		resources.buffer(node_count * sizeof(DrawnNodeGPU), content.gpu_draws.ptr()),
		resources.buffer(sizeof(uint32_t), casts.ptr()),
		resources.buffer(sizeof(page), &page),
		resources.buffer(sizeof(deformation), &deformation),
		resources.buffer(sizeof(RegionOverrideGPU), &neutral_region)
	};
	for (const RID &rid : shared) {
		if (!rid.is_valid()) {
			FAIL("view-list draw input unavailable");
			return;
		}
	}
	auto draw = [&](RID p_draws, uint32_t p_count, const Projection &p_clip, uint32_t p_mode, Vector<uint8_t> &r_pixels) -> bool {
		Vector<RD::Uniform> uniforms;
		for (uint32_t binding = 0; binding < 8; ++binding) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = binding;
			u.append_id(binding == 3 ? p_draws : shared[binding]);
			uniforms.push_back(u);
		}
		const RID set = resources.retain(rd->uniform_set_create(uniforms, shader, 0));
		if (!set.is_valid()) { return false; }
		struct Push {
			float matrix[16];
			float viewport[4];
			uint32_t counts[4];
		} push{};
		static_assert(sizeof(Push) == 96, "view-list test push layout");
		for (int c = 0; c < 4; ++c) {
			for (int r = 0; r < 4; ++r) { push.matrix[c * 4 + r] = float(p_clip.columns[c][r]); }
		}
		push.viewport[0] = push.viewport[1] = float(size);
		push.viewport[2] = gs_shadow_caster::kAlphaThreshold;
		push.viewport[3] = gs_shadow_caster::kLowPassTexels2;
		push.counts[0] = p_count;
		push.counts[1] = node_splats;
		push.counts[2] = p_mode;
		const float opacity = 1.0f;
		memcpy(&push.counts[3], &opacity, sizeof(opacity));
		const RD::DrawListID list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_DEPTH, VectorView<Color>(), 0.0f);
		if (list == RD::INVALID_ID) { return false; }
		rd->draw_list_bind_render_pipeline(list, pipeline);
		rd->draw_list_bind_uniform_set(list, set, 0);
		rd->draw_list_set_push_constant(list, &push, sizeof(push));
		rd->draw_list_draw(list, false, p_count * node_splats, 6);
		rd->draw_list_end();
		r_pixels = rd->texture_get_data(target, 0);
		return r_pixels.size() == int(size * size * sizeof(float));
	};
	auto list_buffer = [&](const LocalVector<uint32_t> &p_list) {
		LocalVector<DrawnNodeGPU> entries;
		for (uint32_t index : p_list) { entries.push_back(content.gpu_draws[index]); }
		return resources.buffer(entries.size() * sizeof(DrawnNodeGPU), entries.ptr());
	};

	// Views: six omni cube faces (flip_y false), a spot and a directional cascade (flip_y true), as the renderer builds them.
	struct ViewCase {
		Projection clip;
		uint32_t mode;
	};
	LocalVector<ViewCase> views;
	Transform3D base;
	base.basis = Basis::from_euler(Vector3(-0.3f, 0.4f, 0.1f));
	base.origin = light;
	const Vector3 normals[] = { Vector3(+1, 0, 0), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, +1, 0), Vector3(0, 0, +1), Vector3(0, 0, -1) };
	const Vector3 ups[] = { Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, -1), Vector3(0, 0, +1), Vector3(0, -1, 0), Vector3(0, -1, 0) };
	Projection cube;
	cube.set_perspective(90.0f, 1.0f, 0.025f, 32.0f);
	for (int face = 0; face < 6; ++face) {
		views.push_back({ gs_shadow_caster::frame_relative_shadow_clip(cube, base * Transform3D().looking_at(normals[face], ups[face]), false, camera), 2u });
	}
	Projection spot;
	spot.set_perspective(70.0f, 1.0f, 0.025f, 40.0f);
	views.push_back({ gs_shadow_caster::frame_relative_shadow_clip(spot, base, true, camera), 2u });
	Projection ortho;
	ortho.set_orthogonal(-15.0f, 15.0f, -15.0f, 15.0f, 0.0f, 50.0f);
	views.push_back({ gs_shadow_caster::frame_relative_shadow_clip(ortho, base, true, camera), 1u });

	uint32_t listed_total = 0, covered_views = 0, unsafe_differs = 0;
	for (uint32_t v = 0; v < views.size(); ++v) {
		const ViewCase &view_case = views[v];
		const gs_shadow_face_cull::View view(view_case.clip, camera, Vector2i(size, size), view_case.mode == 2u);
		LocalVector<uint32_t> list, unsafe_list;
		gs_shadow_face_cull::ListStats stats, unsafe_stats;
		gs_shadow_face_cull::build_view_list(snapshot, casts, view, list, stats);
		const gs_shadow_face_cull::View unsafe_view(view_case.clip, camera, Vector2i(size, size), view_case.mode == 2u, 0.0);
		gs_shadow_face_cull::build_view_list(snapshot, casts, unsafe_view, unsafe_list, unsafe_stats, true);
		listed_total += list.size();
		Vector<uint8_t> whole, listed, unsafe;
		if (!draw(shared[3], node_count, view_case.clip, view_case.mode, whole) ||
				!draw(list_buffer(list), list.size(), view_case.clip, view_case.mode, listed) ||
				!draw(list_buffer(unsafe_list), unsafe_list.size(), view_case.clip, view_case.mode, unsafe)) {
			FAIL("view ", v, ": draw or complete depth readback failed");
			return;
		}
		CHECK_MESSAGE(memcmp(whole.ptr(), listed.ptr(), size_t(whole.size())) == 0, "view ", v, " listed ", list.size(), " of ", node_count);
		unsafe_differs += memcmp(whole.ptr(), unsafe.ptr(), size_t(whole.size())) != 0 ? 1u : 0u;
		uint32_t covered = 0;
		for (uint32_t i = 0; i < size * size; ++i) {
			float z = 0.0f;
			memcpy(&z, whole.ptr() + i * sizeof(float), sizeof(z));
			covered += z > 0.0f ? 1u : 0u;
		}
		covered_views += covered > 0 ? 1u : 0u;
	}
	// Non-vacuous: most views receive depth, the lists drop records, and the comparison can see an unsafe cull.
	CHECK_MESSAGE(covered_views * 4 >= views.size() * 3, "views with depth: ", covered_views);
	CHECK_MESSAGE(listed_total < views.size() * node_count / 2, "listed ", listed_total, " of ", views.size() * node_count);
	CHECK_MESSAGE(unsafe_differs > 0, "no view distinguishes the unsafe centre-only cull");
}

} // namespace TestHlodShadowFaceCullGpu
