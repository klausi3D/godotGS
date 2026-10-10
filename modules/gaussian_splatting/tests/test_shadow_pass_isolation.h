#pragma once

#include "gs_test_pump.h"
#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/render_pipeline_stages.h"
#include "../renderer/tile_renderer.h"

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#include <cstring>

TEST_CASE("[GaussianSplatting][Renderer] scoped shadow pass guard restores renderer state after failure scope") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	REQUIRE(renderer.is_valid());

	CHECK(renderer->test_shadow_pass_guard_restores_after_scope());
}

// Color replay may reuse its snapshot; shadow replay needs pass-specific caster evidence.
TEST_CASE("[GaussianSplatting][ViewTransform] raster takes the snapshot sorted count when no sort stage filled the metrics (#1163)") {
	GaussianSplatRenderer::StageMetrics stage_metrics{};
	REQUIRE_FALSE(stage_metrics.sort.did_sort);
	GaussianSplatRenderer::RenderFrameContext context;
	context.pass_kind = GaussianSplatRenderer::RenderPassKind::MAIN_VIEW;
	context.metrics = &stage_metrics;
	context.snapshot.valid = true;
	context.snapshot.sorted_splats = 42;
	context.snapshot.sorted_index_domain = GaussianSplatRenderer::IndexDomain::SPLAT_REF;

	GaussianSplatRenderer::RasterStageInput raster_input;
	RenderPipelineStages::resolve_raster_sort_input(context, raster_input);
	CHECK_EQ(raster_input.sorted_splat_count, 42u);
	CHECK_EQ(raster_input.sorted_index_domain, GaussianSplatRenderer::IndexDomain::SPLAT_REF);
	CHECK_EQ(raster_input.sort_time_ms, 0.0f);

	context.pass_kind = GaussianSplatRenderer::RenderPassKind::SHADOW_MAP;
	GaussianSplatRenderer::RasterStageInput shadow_input;
	shadow_input.sorted_splat_count = 99;
	RenderPipelineStages::resolve_raster_sort_input(context, shadow_input);
	CHECK_EQ(shadow_input.sorted_splat_count, 0u);
	CHECK_EQ(shadow_input.sorted_index_domain, GaussianSplatRenderer::IndexDomain::UNKNOWN);
	CHECK_EQ(shadow_input.sort_time_ms, 0.0f);

	// A sort stage that ran in this pass stays authoritative over the snapshot.
	stage_metrics.sort.did_sort = true;
	stage_metrics.sort.sorted_count = 7;
	stage_metrics.sort.sort_time_ms = 1.5f;
	stage_metrics.sort.output_domain = GaussianSplatRenderer::IndexDomain::GAUSSIAN_GLOBAL;
	GaussianSplatRenderer::RasterStageInput sorted_input;
	RenderPipelineStages::resolve_raster_sort_input(context, sorted_input);
	CHECK_EQ(sorted_input.sorted_splat_count, 7u);
	CHECK_EQ(sorted_input.sorted_index_domain, GaussianSplatRenderer::IndexDomain::GAUSSIAN_GLOBAL);
	CHECK_EQ(sorted_input.sort_time_ms, 1.5f);

	// No sort output and no snapshot: nothing to raster.
	GaussianSplatRenderer::StageMetrics empty_metrics{};
	GaussianSplatRenderer::RenderFrameContext empty_context;
	empty_context.metrics = &empty_metrics;
	GaussianSplatRenderer::RasterStageInput empty_input;
	empty_input.sorted_splat_count = 99;
	RenderPipelineStages::resolve_raster_sort_input(empty_context, empty_input);
	CHECK_EQ(empty_input.sorted_splat_count, 0u);
	CHECK_EQ(empty_input.sorted_index_domain, GaussianSplatRenderer::IndexDomain::UNKNOWN);
}

TEST_CASE("[GaussianSplatting][Renderer] shadow pass reports invalid atlas as shadow-specific failure") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	REQUIRE(renderer.is_valid());

	const Transform3D light_transform;
	Projection light_projection;
	light_projection.set_perspective(45.0, 1.0, 0.1, 100.0);

	const bool rendered = renderer->render_shadow_depth_map(light_projection, light_transform,
			Rect2i(0, 0, 0, 64), RID(), false);
	CHECK_FALSE(rendered);

	const GaussianSplatRenderer::ShadowRenderResult &result = renderer->get_last_shadow_render_result();
	CHECK_EQ(result.pass_kind, GaussianSplatRenderer::RenderPassKind::SHADOW_MAP);
	CHECK_FALSE(result.success);
	CHECK_EQ(result.failure_reason, GaussianSplatRenderer::ShadowRenderFailureReason::INVALID_ATLAS_RECT);
	CHECK_EQ(result.route_label, String("SHADOW_FAIL_INVALID_ATLAS_RECT"));
}

// ---------------------------------------------------------------------------------------------
// #1089 / #1095 slice 1: a splat shadow pass writes into the shadow atlas only depth it
// rasterized from the LIGHT's view. Before the fail-closed guard, the pass rasterized nothing
// and then blitted whatever depth image the shared rasterizer still held -- the main camera's
// splat depth -- into the atlas rect. Here the light looks away from every splat, so no correct
// caster can write a texel, while the rasterizer holds a main-view depth image of exactly the
// atlas-rect size (so a size check alone would not refuse it). The pass must not attempt to
// copy a depth image into the atlas, and the atlas must keep its clear value. This stays true
// once the real caster lands (#1095 slice 2): nothing is in the light's frustum. Tagged [SceneTree][RequiresGPU] (RendererSceneTree GPU batch); FAILs, never skips.
// ---------------------------------------------------------------------------------------------
namespace TestShadowFailClosed {

// A 3x3 grid of opaque grey splats around p_center, facing the camera at the origin.
inline Ref<GaussianData> make_opaque_cluster(const Vector3 &p_center) {
	LocalVector<Gaussian> gaussians;
	gaussians.resize(9);
	for (uint32_t i = 0; i < gaussians.size(); i++) {
		Gaussian &g = gaussians[i];
		g = Gaussian{};
		g.position = p_center + Vector3(0.5f * (float(i % 3) - 1.0f), 0.5f * (float(i / 3) - 1.0f), 0.0f);
		g.scale = Vector3(0.4f, 0.4f, 0.4f);
		g.opacity = 0.995f;
		g.rotation = Quaternion();
		g.normal = Vector3(0.0f, 0.0f, 1.0f);
		g.area = g.scale.x * g.scale.y;
		g.sh_dc = Color(0.0f, 0.0f, 0.0f, 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
	}
	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);
	return data;
}

inline Size2i texture_size(RenderingDevice *p_rd, RID p_texture) {
	if (!p_texture.is_valid() || !p_rd->texture_is_valid(p_texture)) {
		return Size2i();
	}
	const RD::TextureFormat format = p_rd->texture_get_format(p_texture);
	return Size2i(int(format.width), int(format.height));
}

} // namespace TestShadowFailClosed

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Shadow pass leaves the atlas untouched when no splat is in the light's view (#1089, #1095)") {
	if (RenderingServer::get_singleton() == nullptr) {
		FAIL("RenderingServer unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return;
	}
	// The shadow blit writes through the main device, so the atlas and the renderer live there too.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		FAIL("RenderingDevice unavailable in a [RequiresGPU] case");
		return;
	}
	const RD::DataFormat depth_format = RD::DATA_FORMAT_D32_SFLOAT;
	const uint32_t depth_usage = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	if (!rd->texture_is_format_supported_for_usage(depth_format, depth_usage)) {
		FAIL("D32_SFLOAT depth attachment with copy-from is unsupported on this device");
		return;
	}

	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(rd);
	if (!renderer.is_valid()) {
		FAIL("renderer did not instantiate");
		return;
	}
	renderer->initialize();
	renderer->set_painterly_enabled(false);
	renderer->set_max_splats(9);
	if (renderer->set_gaussian_data(TestShadowFailClosed::make_opaque_cluster(Vector3(0.0f, 0.0f, -3.0f))) != OK) {
		FAIL("set_gaussian_data failed");
		return;
	}

	// Main view: the camera at the origin looks down -Z at the cluster.
	const Size2i size(64, 64);
	Projection camera_projection;
	camera_projection.set_perspective(60.0f, 1.0f, 0.1f, 50.0f);
	const TestGaussianSplatting::GSPumpOutcome outcome = TestGaussianSplatting::gs_pump_until([&]() {
		return renderer->render_for_view(Transform3D(), camera_projection, RID(), size);
	});
	if (!outcome.ready()) {
		FAIL(vformat("render_for_view never reported a rendered frame: %s", outcome.describe()));
		return;
	}
	// Premise: the rasterizer now holds the main view's depth image at the atlas-rect size.
	Ref<TileRenderer> tile_renderer = renderer->get_tile_renderer();
	const RID camera_depth = tile_renderer.is_valid() ? tile_renderer->get_depth_texture() : RID();
	const Size2i camera_depth_size = TestShadowFailClosed::texture_size(rd, camera_depth);
	if (camera_depth_size != size) {
		FAIL(vformat("premise: the main view left no %dx%d depth image in the rasterizer (got %dx%d)",
				size.x, size.y, camera_depth_size.x, camera_depth_size.y));
		return;
	}
	// Premise: that depth image holds the splats (texels that differ from the background
	// corner), so a pass that copied it into the atlas would show up below.
	{
		const Vector<uint8_t> camera_bytes = rd->texture_get_data(camera_depth, 0);
		if (camera_bytes.size() != size.x * size.y * int(sizeof(float))) {
			FAIL(vformat("premise: camera depth readback returned %d bytes", camera_bytes.size()));
			return;
		}
		float background = 0.0f;
		memcpy(&background, camera_bytes.ptr(), sizeof(float));
		int splat_texels = 0;
		for (int i = 0; i < size.x * size.y; i++) {
			float depth = 0.0f;
			memcpy(&depth, camera_bytes.ptr() + i * sizeof(float), sizeof(float));
			splat_texels += depth != background ? 1 : 0;
		}
		if (splat_texels == 0) {
			FAIL("premise: the main view's depth image holds no splat texels");
			return;
		}
	}

	// A depth-only shadow atlas of the same size, cleared to 0.0: Godot's reverse-Z clear for
	// shadow atlases (render_forward_clustered.cpp, the directional atlas draw_list_begin).
	RD::TextureFormat atlas_format;
	atlas_format.format = depth_format;
	atlas_format.width = size.x;
	atlas_format.height = size.y;
	atlas_format.usage_bits = depth_usage;
	const RID atlas = rd->texture_create(atlas_format, RD::TextureView());
	Vector<RID> attachments;
	attachments.push_back(atlas);
	const RID atlas_fb = atlas.is_valid() ? rd->framebuffer_create(attachments) : RID();
	if (!atlas_fb.is_valid()) {
		if (atlas.is_valid()) {
			rd->free(atlas);
		}
		renderer.unref();
		FAIL("shadow atlas texture/framebuffer creation failed");
		return;
	}
	const float sentinel = 0.0f;
	rd->draw_list_begin(atlas_fb, RD::DRAW_CLEAR_DEPTH, Vector<Color>(), sentinel);
	rd->draw_list_end();

	// The light sits at the origin looking down +Z: every splat (z = -3) is behind it, outside
	// its orthographic box. A correct splat caster therefore writes no texel.
	Projection light_projection;
	light_projection.set_orthogonal(-4.0f, 4.0f, -4.0f, 4.0f, 0.1f, 20.0f);
	const Transform3D light_transform(Basis(Vector3(0.0f, 1.0f, 0.0f), Math::PI), Vector3());
	renderer->render_shadow_depth_map(light_projection, light_transform, Rect2i(Point2i(), size), atlas_fb, true);
	const GaussianSplatRenderer::ShadowRenderResult result = renderer->get_last_shadow_render_result();

	const Vector<uint8_t> bytes = rd->texture_get_data(atlas, 0);
	rd->free(atlas_fb);
	rd->free(atlas);
	renderer.unref();
	if (bytes.size() != size.x * size.y * int(sizeof(float))) {
		FAIL(vformat("atlas readback returned %d bytes, expected %d", bytes.size(), size.x * size.y * int(sizeof(float))));
		return;
	}
	int written = 0;
	float first_written = sentinel;
	for (int i = 0; i < size.x * size.y; i++) {
		float depth = 0.0f;
		memcpy(&depth, bytes.ptr() + i * sizeof(float), sizeof(float));
		if (depth != sentinel) {
			if (written == 0) {
				first_written = depth;
			}
			written++;
		}
	}
	// Two observations of the one property. The texels are the property itself. The attempt is
	// what this harness can see: it has no UniformSetCacheRD (that is created by
	// RendererCompositorRD), so the copy into the atlas fails here even when it is attempted --
	// before the guard, this pass attempted it (route SHADOW_FAIL_BLIT) and only the missing
	// cache kept the camera depth out. In the engine the same attempt succeeds.
	CHECK_MESSAGE(!result.blit_attempted,
			vformat("the shadow pass tried to copy a depth image into the atlas with no splat in the light's view (route %s)",
					result.route_label));
	CHECK_MESSAGE(written == 0,
			vformat("the shadow pass wrote %d of %d atlas texels (first %f, clear %f) with no splat in the light's view (route %s)",
					written, size.x * size.y, first_written, sentinel, result.route_label));
}
