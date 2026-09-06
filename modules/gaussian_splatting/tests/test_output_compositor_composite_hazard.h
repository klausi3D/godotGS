#pragma once

#include "test_macros.h"
#include "visual_compare.h"

#include "../interfaces/output_compositor.h"
#include "../interfaces/output_compositor_interfaces.h"
#include "../interfaces/render_device_manager.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/object/ref_counted.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#ifdef TESTS_ENABLED

namespace TestGaussianSplatting {
namespace CompositeHazardRepro {

// Build a 256x256 RGBA8 destination pre-filled with a horizontal gradient deliberately
// offset 3 pixels off the 8-pixel workgroup boundary. The historic black-blocks symptom
// manifested at 8-pixel grid lines because invocations in workgroup N read stale
// destination written by workgroup N-1 in unspecified order; an off-grid gradient makes
// any block-boundary artifact visible against a smooth reference baseline.
static Vector<uint8_t> _build_destination_gradient_256() {
	Vector<uint8_t> data;
	data.resize(256 * 256 * 4);
	uint8_t *w = data.ptrw();
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			const int i = (y * 256 + x) * 4;
			const int gx = (x + 3) % 256;
			w[i + 0] = uint8_t(64 + (gx * 128) / 255);
			w[i + 1] = 128;
			w[i + 2] = 0;
			w[i + 3] = 255;
		}
	}
	return data;
}

// Build a 256x256 RGBA8 source: 25% area-equivalent centered circle (radius 72),
// premultiplied alpha (source.a = 0.5 inside, 0 outside).
static Vector<uint8_t> _build_source_premultiplied_circle_256() {
	Vector<uint8_t> data;
	data.resize(256 * 256 * 4);
	uint8_t *w = data.ptrw();
	const float cx = 128.0f;
	const float cy = 128.0f;
	const float r2 = 72.0f * 72.0f;
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			const int i = (y * 256 + x) * 4;
			const float dx = float(x) - cx;
			const float dy = float(y) - cy;
			const bool inside = (dx * dx + dy * dy) < r2;
			w[i + 0] = inside ? 128 : 0;
			w[i + 1] = inside ? 128 : 0;
			w[i + 2] = inside ? 128 : 0;
			w[i + 3] = inside ? 128 : 0;
		}
	}
	return data;
}

} // namespace CompositeHazardRepro

// Scope guard tied to this TU's hazard test: tracks textures and the
// compositor, and frees them in destruction order regardless of how the
// test exits (success, REQUIRE failure → doctest exception, or normal
// return). Without this guard, any REQUIRE failure before the manual
// rd->free(...) block at the end of the test would leak RIDs into the
// next test case. The RenderingDevice itself is owned by
// REQUIRE_GPU_DEVICE()'s ScopedFallbackRD (declared first in the test
// scope, destroyed last LIFO), so this guard intentionally does NOT free
// the device — that would be a double-delete.
class HazardTestScope {
public:
	explicit HazardTestScope(RenderingDevice *p_rd) :
			rd(p_rd) {}

	~HazardTestScope() {
		if (rd) {
			for (int i = textures.size() - 1; i >= 0; i--) {
				if (textures[i].is_valid() && rd->texture_is_valid(textures[i])) {
					rd->free(textures[i]);
				}
			}
			if (compositor.is_valid()) {
				compositor->shutdown();
			}
		}
	}

	void track(RID p_texture) { textures.push_back(p_texture); }

	RenderingDevice *rd = nullptr;
	Vector<RID> textures;
	Ref<OutputCompositor> compositor;
};

TEST_CASE("[GaussianSplatting][RequiresGPU][HazardRepro] Compositor scratch-copy preserves destination outside source") {
	REQUIRE_GPU_DEVICE();
	// rd was declared by REQUIRE_GPU_DEVICE and is kept alive by its inline
	// ScopedFallbackRD guard (#334). HazardTestScope only owns the textures
	// and compositor — the RD itself is freed by the macro's guard on scope
	// exit, after this scope's destructor runs (LIFO destruction order).
	HazardTestScope _scope(rd);

	RD::TextureFormat source_format;
	source_format.width = 256;
	source_format.height = 256;
	source_format.depth = 1;
	source_format.array_layers = 1;
	source_format.mipmaps = 1;
	source_format.texture_type = RD::TEXTURE_TYPE_2D;
	source_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	RD::TextureFormat destination_format;
	destination_format.width = 256;
	destination_format.height = 256;
	destination_format.depth = 1;
	destination_format.array_layers = 1;
	destination_format.mipmaps = 1;
	destination_format.texture_type = RD::TEXTURE_TYPE_2D;
	destination_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	destination_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	RID source_tex = rd->texture_create(source_format, RD::TextureView());
	RID destination_tex = rd->texture_create(destination_format, RD::TextureView());
	_scope.track(source_tex);
	_scope.track(destination_tex);
	REQUIRE(source_tex.is_valid());
	REQUIRE(destination_tex.is_valid());
	rd->set_resource_name(source_tex, "GS_HazardRepro_Source");
	rd->set_resource_name(destination_tex, "GS_HazardRepro_Destination");

	// OutputCompositor::copy_to_render_target routes into _copy_final_output_compute
	// (the scratch-copy path this PR fixes) only when params.depth_test_enabled is
	// true. Composite-without-depth falls through to copy_to_fb_rect — the graphics
	// path that doesn't carry the hazard but also doesn't exercise the fix. Provide
	// dummy depth textures here and set both to the far plane (depth=1.0) so the
	// shader's `gs_depth < 0.999999` guard at viewport_blit.glsl:127 is false, the
	// depth comparison short-circuits, and the test exercises the composite path
	// deterministically across runs/drivers without depending on
	// texture_create's zero-init behavior (which is not portable).
	RD::TextureFormat depth_format;
	depth_format.width = 256;
	depth_format.height = 256;
	depth_format.depth = 1;
	depth_format.array_layers = 1;
	depth_format.mipmaps = 1;
	depth_format.texture_type = RD::TEXTURE_TYPE_2D;
	depth_format.format = RD::DATA_FORMAT_D32_SFLOAT;
	depth_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	RID source_depth = rd->texture_create(depth_format, RD::TextureView());
	RID destination_depth = rd->texture_create(depth_format, RD::TextureView());
	_scope.track(source_depth);
	_scope.track(destination_depth);
	REQUIRE(source_depth.is_valid());
	REQUIRE(destination_depth.is_valid());
	rd->set_resource_name(source_depth, "GS_HazardRepro_SourceDepth");
	rd->set_resource_name(destination_depth, "GS_HazardRepro_DestinationDepth");

	// Encode 256*256 floats of value 1.0 once and upload to both depths.
	Vector<uint8_t> depth_far_bytes;
	depth_far_bytes.resize(256 * 256 * sizeof(float));
	{
		const float far_depth = 1.0f;
		uint8_t *dp = depth_far_bytes.ptrw();
		for (int i = 0; i < 256 * 256; i++) {
			memcpy(dp + i * sizeof(float), &far_depth, sizeof(float));
		}
	}
	REQUIRE(rd->texture_update(source_depth, 0, depth_far_bytes) == OK);
	REQUIRE(rd->texture_update(destination_depth, 0, depth_far_bytes) == OK);

	Vector<uint8_t> destination_data = CompositeHazardRepro::_build_destination_gradient_256();
	Vector<uint8_t> source_data = CompositeHazardRepro::_build_source_premultiplied_circle_256();
	REQUIRE(rd->texture_update(destination_tex, 0, destination_data) == OK);
	REQUIRE(rd->texture_update(source_tex, 0, source_data) == OK);

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	_scope.compositor = compositor;
	REQUIRE(compositor->initialize(rd) == OK);

	OutputCopyParams params;
	params.source_texture = source_tex;
	params.source_depth = source_depth;
	params.destination_texture = destination_tex;
	params.destination_depth = destination_depth;
	params.viewport_size = Size2i(256, 256);
	params.composite_with_destination = true;
	params.source_is_premultiplied = true;
	params.depth_test_enabled = true;
	params.z_near = 0.05f;
	params.z_far = 4000.0f;
	params.depth_linearize_mul = 1.0f;
	params.depth_linearize_add = 1.0f;
	params.depth_epsilon = 0.01f;

	OutputCopyResult result = compositor->copy_to_render_target(params);
	if (!result.success) {
		print_line(vformat("[HazardRepro] copy_to_render_target failed: %s", result.error));
	}
	CHECK(result.success);

	const OutputCompositor::LastCompositeStats stats = compositor->get_last_composite_stats();
	CHECK(stats.valid);
	CHECK(stats.composite_with_destination);
	CHECK(stats.scratch_used);
	CHECK_FALSE(stats.fallback_due_to_missing_copy_from);
	CHECK_EQ(stats.binding_count, 5u);

	String visual_reason;
	const bool visual_ok = TestGaussianSplatting::VisualCompare::capture_and_compare(
			rd, destination_tex, "composite_hazard_256x256.png", 1.0, 45.0, &visual_reason);
	if (!visual_ok) {
		print_line(vformat("[HazardRepro] visual gate: %s", visual_reason));
	}
	CHECK(visual_ok);

	// Second invocation must reuse the scratch entry (same format and extent).
	Vector<uint8_t> destination_reset = CompositeHazardRepro::_build_destination_gradient_256();
	REQUIRE(rd->texture_update(destination_tex, 0, destination_reset) == OK);
	OutputCopyResult result_second = compositor->copy_to_render_target(params);
	CHECK(result_second.success);
	const OutputCompositor::LastCompositeStats stats_second = compositor->get_last_composite_stats();
	CHECK(stats_second.scratch_used);
	CHECK(stats_second.scratch_reused);

	// All cleanup (textures, compositor shutdown, locally-allocated fallback
	// RD if any) is handled by _scope's destructor on function exit — including
	// any path where an earlier REQUIRE failure threw a doctest exception.
}

TEST_CASE("[GaussianSplatting][RequiresGPU] OutputCompositor scratch resources return owned tracking to baseline across repeated shutdown") {
	REQUIRE_GPU_DEVICE();

	Ref<RenderDeviceManager> device_manager;
	device_manager.instantiate();
	REQUIRE(device_manager->initialize(rd) == OK);

	const uint32_t baseline_tracked = device_manager->get_tracked_resource_count();
	const uint32_t baseline_owned = device_manager->get_tracked_owned_resource_count();

	for (int iteration = 0; iteration < 2; iteration++) {
		HazardTestScope scope(rd);

		RD::TextureFormat color_source_format;
		color_source_format.width = 16;
		color_source_format.height = 16;
		color_source_format.depth = 1;
		color_source_format.array_layers = 1;
		color_source_format.mipmaps = 1;
		color_source_format.texture_type = RD::TEXTURE_TYPE_2D;
		color_source_format.samples = RD::TEXTURE_SAMPLES_1;
		color_source_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		color_source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
				RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

		RD::TextureFormat color_destination_format = color_source_format;
		color_destination_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
				RD::TEXTURE_USAGE_SAMPLING_BIT |
				RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

		RID source_tex = rd->texture_create(color_source_format, RD::TextureView());
		RID destination_tex = rd->texture_create(color_destination_format, RD::TextureView());
		scope.track(source_tex);
		scope.track(destination_tex);
		REQUIRE(source_tex.is_valid());
		REQUIRE(destination_tex.is_valid());

		Vector<uint8_t> source_data;
		source_data.resize(16 * 16 * 4);
		Vector<uint8_t> destination_data;
		destination_data.resize(16 * 16 * 4);
		for (int i = 0; i < source_data.size(); i += 4) {
			source_data.write[i + 0] = 64;
			source_data.write[i + 1] = 64;
			source_data.write[i + 2] = 64;
			source_data.write[i + 3] = 128;
			destination_data.write[i + 0] = 12;
			destination_data.write[i + 1] = 24;
			destination_data.write[i + 2] = 48;
			destination_data.write[i + 3] = 255;
		}
		REQUIRE(rd->texture_update(source_tex, 0, source_data) == OK);
		REQUIRE(rd->texture_update(destination_tex, 0, destination_data) == OK);

		RD::TextureFormat depth_format;
		depth_format.width = 16;
		depth_format.height = 16;
		depth_format.depth = 1;
		depth_format.array_layers = 1;
		depth_format.mipmaps = 1;
		depth_format.texture_type = RD::TEXTURE_TYPE_2D;
		depth_format.samples = RD::TEXTURE_SAMPLES_1;
		depth_format.format = RD::DATA_FORMAT_D32_SFLOAT;
		depth_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
				RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
				RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

		RID source_depth = rd->texture_create(depth_format, RD::TextureView());
		RID destination_depth = rd->texture_create(depth_format, RD::TextureView());
		scope.track(source_depth);
		scope.track(destination_depth);
		REQUIRE(source_depth.is_valid());
		REQUIRE(destination_depth.is_valid());

		Vector<uint8_t> depth_far_bytes;
		depth_far_bytes.resize(16 * 16 * sizeof(float));
		const float far_depth = 1.0f;
		for (int i = 0; i < 16 * 16; i++) {
			memcpy(depth_far_bytes.ptrw() + i * sizeof(float), &far_depth, sizeof(float));
		}
		REQUIRE(rd->texture_update(source_depth, 0, depth_far_bytes) == OK);
		REQUIRE(rd->texture_update(destination_depth, 0, depth_far_bytes) == OK);

		Ref<OutputCompositor> compositor;
		compositor.instantiate();
		compositor->set_device_manager(device_manager);
		scope.compositor = compositor;
		REQUIRE(compositor->initialize(rd) == OK);

		OutputCopyParams params;
		params.source_texture = source_tex;
		params.source_depth = source_depth;
		params.destination_texture = destination_tex;
		params.destination_depth = destination_depth;
		params.viewport_size = Size2i(16, 16);
		params.composite_with_destination = true;
		params.source_is_premultiplied = true;
		params.depth_test_enabled = true;
		params.z_near = 0.05f;
		params.z_far = 4000.0f;
		params.depth_linearize_mul = 1.0f;
		params.depth_linearize_add = 1.0f;
		params.depth_epsilon = 0.01f;

		OutputCopyResult result = compositor->copy_to_render_target(params);
		if (!result.success) {
			print_line(vformat("[OutputCompositorLifetime] copy_to_render_target failed: %s", result.error));
		}
		REQUIRE(result.success);

		CHECK(compositor->get_viewport_blit_scratch_count() == 1u);
		CHECK(compositor->get_blit_variant_count() == 1u);
		CHECK(device_manager->get_tracked_owned_resource_count() > baseline_owned);

		compositor->shutdown();
		CHECK(compositor->get_viewport_blit_scratch_count() == 0u);
		CHECK(compositor->get_blit_variant_count() == 0u);
		CHECK(device_manager->get_tracked_resource_count() == baseline_tracked);
		CHECK(device_manager->get_tracked_owned_resource_count() == baseline_owned);
		scope.compositor.unref();
	}

	device_manager->shutdown();
	CHECK(device_manager->get_last_shutdown_owned_resource_count() == 0u);
}

TEST_CASE("[GaussianSplatting][RequiresGPU] OutputCompositor untracked cleanup skips stale resources") {
	REQUIRE_GPU_DEVICE();

	HazardTestScope scope(rd);

	RD::TextureFormat color_format;
	color_format.width = 16;
	color_format.height = 16;
	color_format.depth = 1;
	color_format.array_layers = 1;
	color_format.mipmaps = 1;
	color_format.texture_type = RD::TEXTURE_TYPE_2D;
	color_format.samples = RD::TEXTURE_SAMPLES_1;
	color_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	color_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;

	RID source_tex = rd->texture_create(color_format, RD::TextureView());
	RID destination_tex = rd->texture_create(color_format, RD::TextureView());
	RID framebuffer_tex = rd->texture_create(color_format, RD::TextureView());
	scope.track(source_tex);
	scope.track(destination_tex);
	scope.track(framebuffer_tex);
	REQUIRE(source_tex.is_valid());
	REQUIRE(destination_tex.is_valid());
	REQUIRE(framebuffer_tex.is_valid());

	Vector<uint8_t> source_data;
	source_data.resize(16 * 16 * 4);
	Vector<uint8_t> destination_data;
	destination_data.resize(16 * 16 * 4);
	for (int i = 0; i < source_data.size(); i += 4) {
		source_data.write[i + 0] = 80;
		source_data.write[i + 1] = 120;
		source_data.write[i + 2] = 160;
		source_data.write[i + 3] = 192;
		destination_data.write[i + 0] = 16;
		destination_data.write[i + 1] = 32;
		destination_data.write[i + 2] = 48;
		destination_data.write[i + 3] = 255;
	}
	REQUIRE(rd->texture_update(source_tex, 0, source_data) == OK);
	REQUIRE(rd->texture_update(destination_tex, 0, destination_data) == OK);

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	scope.compositor = compositor;
	REQUIRE(compositor->initialize(rd) == OK);

	OutputCopyParams params;
	params.source_texture = source_tex;
	params.destination_texture = destination_tex;
	params.viewport_size = Size2i(16, 16);
	params.composite_with_destination = true;
	params.source_is_premultiplied = true;
	params.depth_test_enabled = false;

	OutputCopyResult result = compositor->copy_to_render_target(params);
	if (!result.success) {
		print_line(vformat("[OutputCompositorUntrackedLifetime] copy_to_render_target failed: %s", result.error));
	}
	REQUIRE(result.success);
	CHECK(compositor->get_viewport_blit_scratch_count() == 1u);
	CHECK(compositor->get_blit_variant_count() == 1u);

	RID framebuffer = compositor->get_cached_framebuffer(rd, framebuffer_tex);
	REQUIRE(framebuffer.is_valid());
	CHECK(compositor->get_cached_framebuffer_count() == 1u);
	rd->free(framebuffer);
	CHECK_FALSE(rd->framebuffer_is_valid(framebuffer));

	compositor->clear_cached_framebuffers();
	CHECK(compositor->get_cached_framebuffer_count() == 0u);

	compositor->shutdown();
	CHECK(compositor->get_viewport_blit_scratch_count() == 0u);
	CHECK(compositor->get_blit_variant_count() == 0u);
	scope.compositor.unref();
}

TEST_CASE("[GaussianSplatting][OutputCompositor][RequiresGPU] framebuffer LRU caps prevent unbounded growth across device switches") {
	REQUIRE_GPU_DEVICE();

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	REQUIRE(compositor->initialize(rd) == OK);

	const uint32_t cap = OutputCompositor::get_max_cached_framebuffer_formats();
	CHECK(cap == 8u);

	// Drive 16 distinct synthetic format keys through the eviction path twice
	// (once per cache). Without the cap added by this PR, the caches would
	// hold all 16 entries; with the cap, each must stay <= 8.
	for (uint64_t i = 1; i <= 16; ++i) {
		compositor->test_force_insert_cached_framebuffer(i);
		compositor->test_force_insert_framebuffer_validation(i + 0x1000ull);
	}

	CHECK(compositor->get_cached_framebuffer_count() <= cap);
	CHECK(compositor->get_framebuffer_validation_cache_count() <= cap);
	CHECK(compositor->get_cached_framebuffer_count() == cap);
	CHECK(compositor->get_framebuffer_validation_cache_count() == cap);

	// LRU correctness: the 8 most recently inserted keys (9..16) survive,
	// the 8 oldest (1..8) were evicted.
	OutputCompositor::OutputCacheState &state = compositor->get_cache_state();
	for (uint64_t i = 1; i <= 8; ++i) {
		CHECK_FALSE(state.cached_framebuffers.has(i));
		CHECK_FALSE(state.framebuffer_validation_cache.has(i + 0x1000ull));
	}
	for (uint64_t i = 9; i <= 16; ++i) {
		CHECK(state.cached_framebuffers.has(i));
		CHECK(state.framebuffer_validation_cache.has(i + 0x1000ull));
	}

	// Driving an additional batch must not grow either cache.
	for (uint64_t i = 17; i <= 24; ++i) {
		compositor->test_force_insert_cached_framebuffer(i);
		compositor->test_force_insert_framebuffer_validation(i + 0x1000ull);
	}
	CHECK(compositor->get_cached_framebuffer_count() == cap);
	CHECK(compositor->get_framebuffer_validation_cache_count() == cap);

	// Re-inserting an already-present key (LRU refresh) must not evict.
	const uint32_t before_refresh = compositor->get_cached_framebuffer_count();
	compositor->test_force_insert_cached_framebuffer(20);
	CHECK(compositor->get_cached_framebuffer_count() == before_refresh);

	compositor->shutdown();
}

// Regression for PR #388 bot review: when validate_framebuffer_attachments encounters
// a stale entry for an existing cache_key (entry present but cache_still_valid == false),
// the prior implementation called _evict_oldest_framebuffer_validation_if_needed(cache_key)
// FIRST -- which skips cache_key by design and evicts a DIFFERENT entry -- then the
// trailing insert(cache_key, ...) overwrote the stale entry. Net effect at cap: cache
// silently drops from 8 to 7. Fix erases the stale entry BEFORE the eviction helper, so
// the helper sees size < cap and returns immediately, the trailing insert restores cap,
// and the refreshed key becomes MRU.
TEST_CASE("[GaussianSplatting][OutputCompositor][RequiresGPU] framebuffer validation cache refresh-stale-key preserves cap") {
	REQUIRE_GPU_DEVICE();

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	REQUIRE(compositor->initialize(rd) == OK);

	const uint32_t cap = OutputCompositor::get_max_cached_framebuffer_formats();
	REQUIRE(cap == 8u);

	// Create `cap` real RGBA8 color textures so validate_framebuffer_attachments
	// has live attachments to hash into cache keys. Tracking them in a scope guard
	// keeps cleanup deterministic across REQUIRE failures.
	HazardTestScope _scope(rd);

	RD::TextureFormat color_format;
	color_format.width = 32;
	color_format.height = 32;
	color_format.depth = 1;
	color_format.array_layers = 1;
	color_format.mipmaps = 1;
	color_format.texture_type = RD::TEXTURE_TYPE_2D;
	color_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	color_format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT;

	Vector<RID> textures;
	for (uint32_t i = 0; i < cap; i++) {
		RID tex = rd->texture_create(color_format, RD::TextureView());
		REQUIRE(tex.is_valid());
		_scope.track(tex);
		textures.push_back(tex);

		Vector<RID> attachments;
		attachments.push_back(tex);

		Vector<AttachmentValidationInfo> infos;
		Size2i extent;
		RD::TextureSamples samples = RD::TEXTURE_SAMPLES_1;
		String err;
		REQUIRE(compositor->validate_framebuffer_attachments(rd, attachments, infos, extent, samples, err));
	}

	REQUIRE(compositor->get_framebuffer_validation_cache_count() == cap);

	// Capture the (single) key in the cache that corresponds to textures[0], by
	// identifying which cache entry holds it as the original_attachment. We can't
	// recompute the private hash, but we can find the entry by attachment match.
	OutputCompositor::OutputCacheState &state = compositor->get_cache_state();
	uint64_t stale_key = 0;
	bool stale_key_found = false;
	for (KeyValue<uint64_t, OutputCompositor::FramebufferValidationCacheEntry> &kv : state.framebuffer_validation_cache) {
		const Vector<AttachmentValidationInfo> &infos = kv.value.infos;
		if (infos.size() == 1 && infos[0].original_attachment == textures[0]) {
			stale_key = kv.key;
			stale_key_found = true;
			break;
		}
	}
	REQUIRE(stale_key_found);

	// Mark the entry as stale by flipping `valid` to false. This forces the
	// production refresh-stale code path on the next validate call for the same
	// attachments (cache_still_valid never gets a chance to be true because the
	// outer `valid` check short-circuits the hit-path).
	{
		OutputCompositor::FramebufferValidationCacheEntry *entry = state.framebuffer_validation_cache.getptr(stale_key);
		REQUIRE(entry != nullptr);
		entry->valid = false;
	}

	// Re-validate using the SAME attachment, which hashes to the SAME cache_key.
	// Pre-fix: stale entry stays put, eviction helper skips it and evicts a different
	// slot, insert overwrites stale -> cache size = cap - 1. Post-fix: stale entry is
	// erased first, eviction helper no-ops because size < cap, insert restores cap.
	{
		Vector<RID> attachments;
		attachments.push_back(textures[0]);
		Vector<AttachmentValidationInfo> infos;
		Size2i extent;
		RD::TextureSamples samples = RD::TEXTURE_SAMPLES_1;
		String err;
		REQUIRE(compositor->validate_framebuffer_attachments(rd, attachments, infos, extent, samples, err));
	}

	// Primary assertion: cache stays at cap (the bug dropped it to cap - 1).
	CHECK(compositor->get_framebuffer_validation_cache_count() == cap);
	CHECK(state.framebuffer_validation_cache.has(stale_key));

	// Secondary assertion: the refreshed key is MRU. Drive `cap - 1` additional
	// distinct synthetic inserts via the test helper; the stale_key entry must
	// survive because it was most recently touched, while the other 7 original
	// entries are the LRU victims. NOTE: we deliberately use `cap - 1`, not
	// `cap`. At cap, each insert triggers exactly one eviction; we want to
	// evict the 7 other originals only. A `cap`-th insert would then evict the
	// new oldest entry (stale_key, since the 7 original co-tenants are gone),
	// silently invalidating the MRU claim. The off-by-one in the original
	// version of this test produced the very failure mode it was meant to
	// detect, and the disk-exhaustion CI crash for PR #388's prior run had
	// masked it.
	for (uint64_t i = 0xF000ull; i < 0xF000ull + (cap - 1); ++i) {
		compositor->test_force_insert_framebuffer_validation(i);
	}
	CHECK(compositor->get_framebuffer_validation_cache_count() == cap);
	CHECK(state.framebuffer_validation_cache.has(stale_key));
	// All 7 other original keys should have been evicted.
	uint32_t original_survivors = 0;
	for (uint32_t i = 0; i < cap; i++) {
		for (KeyValue<uint64_t, OutputCompositor::FramebufferValidationCacheEntry> &kv : state.framebuffer_validation_cache) {
			const Vector<AttachmentValidationInfo> &infos = kv.value.infos;
			if (infos.size() == 1 && infos[0].original_attachment == textures[i]) {
				original_survivors++;
				break;
			}
		}
	}
	CHECK(original_survivors == 1u);

	compositor->shutdown();
}

// Regression for PR #388 Codex review #3294032623: get_cached_framebuffer()
// previously called _evict_oldest_cached_framebuffer_if_needed(key) BEFORE
// validate_framebuffer_attachments() and framebuffer_create(). If either
// downstream call failed (invalid/non-attachable RID, transient driver
// failure), the function returned without inserting a replacement, so an
// unrelated cached framebuffer had already been evicted permanently. Repeated
// failed requests would drain the cache below cap. Fix defers eviction until
// after both calls succeed and an insertion is about to commit.
TEST_CASE("[GaussianSplatting][OutputCompositor][RequiresGPU] cached framebuffer failed request preserves LRU cache") {
	REQUIRE_GPU_DEVICE();

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	REQUIRE(compositor->initialize(rd) == OK);

	const uint32_t cap = OutputCompositor::get_max_cached_framebuffer_formats();
	REQUIRE(cap == 8u);

	HazardTestScope _scope(rd);
	_scope.compositor = compositor;

	// Color-attachable format so get_cached_framebuffer() goes all the way
	// through validate_framebuffer_attachments() and framebuffer_create().
	RD::TextureFormat color_format;
	color_format.width = 32;
	color_format.height = 32;
	color_format.depth = 1;
	color_format.array_layers = 1;
	color_format.mipmaps = 1;
	color_format.texture_type = RD::TEXTURE_TYPE_2D;
	color_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	color_format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT;

	// Fill the cached_framebuffers cache to cap with real, valid framebuffers.
	Vector<RID> textures;
	Vector<uint64_t> cached_keys;
	for (uint32_t i = 0; i < cap; i++) {
		RID tex = rd->texture_create(color_format, RD::TextureView());
		REQUIRE(tex.is_valid());
		_scope.track(tex);
		textures.push_back(tex);

		RID fb = compositor->get_cached_framebuffer(rd, tex);
		REQUIRE(fb.is_valid());
		cached_keys.push_back(tex.get_id());
	}
	REQUIRE(compositor->get_cached_framebuffer_count() == cap);

	// Sanity: every key we just inserted is present in the cache map.
	OutputCompositor::OutputCacheState &state = compositor->get_cache_state();
	for (uint64_t k : cached_keys) {
		REQUIRE(state.cached_framebuffers.has(k));
	}

	// Snapshot LRU ordering by last_access_id so we can detect any eviction.
	HashMap<uint64_t, uint64_t> pre_failure_access_ids;
	for (KeyValue<uint64_t, OutputCompositor::CachedFramebuffer> &kv : state.cached_framebuffers) {
		pre_failure_access_ids.insert(kv.key, kv.value.last_access_id);
	}

	// Construct a request that will deliberately fail at
	// validate_framebuffer_attachments(): create a fresh texture, capture its
	// RID, then immediately free it. The RID stays a valid object (so the
	// early-out `!p_texture.is_valid()` does not trip), but texture_is_valid()
	// returns false, which causes validate_framebuffer_attachments() to
	// return false and get_cached_framebuffer() to bail.
	RID doomed_tex = rd->texture_create(color_format, RD::TextureView());
	REQUIRE(doomed_tex.is_valid());
	const uint64_t doomed_key = doomed_tex.get_id();
	// Guard against the (extremely unlikely) RID-id collision with any of the
	// already-cached keys; if this fires the test setup is wrong and we'd be
	// testing the cache-hit branch instead of the eviction branch.
	for (uint64_t k : cached_keys) {
		REQUIRE(k != doomed_key);
	}
	rd->free(doomed_tex);
	REQUIRE_FALSE(rd->texture_is_valid(doomed_tex));

	// The bug being fixed: pre-fix, this call evicts an unrelated LRU entry
	// BEFORE validation runs, then returns RID() because validation fails.
	// Post-fix, eviction is deferred until after both validation and
	// framebuffer_create succeed, so a failed request must NOT mutate the
	// cache at all.
	RID result = compositor->get_cached_framebuffer(rd, doomed_tex);
	CHECK_FALSE(result.is_valid());

	// Primary assertion: cache size unchanged (no eviction occurred).
	CHECK(compositor->get_cached_framebuffer_count() == cap);

	// Secondary assertion: every originally-cached key is still present and
	// its last_access_id is unchanged (no LRU rotation, no silent replacement).
	for (uint64_t k : cached_keys) {
		CHECK(state.cached_framebuffers.has(k));
		OutputCompositor::CachedFramebuffer *entry = state.cached_framebuffers.getptr(k);
		REQUIRE(entry != nullptr);
		const uint64_t *prev_id = pre_failure_access_ids.getptr(k);
		REQUIRE(prev_id != nullptr);
		CHECK(entry->last_access_id == *prev_id);
	}

	// Tertiary assertion: the doomed key was never inserted as a side effect.
	CHECK_FALSE(state.cached_framebuffers.has(doomed_key));

	compositor->shutdown();
	_scope.compositor.unref();
}


namespace CompositeAlphaRepro {

// 64x64 RGBA8 PREMULTIPLIED source carrying a horizontal alpha RAMP: column x
// has coverage alpha = x*4 (0, 4, 8 ... 252) over a straight mid-grey, stored
// premultiplied (rgb = 0.5 * a). A Gaussian silhouette is exactly this: a band
// of partially-covered texels between "nothing" and "solid". That band is what
// #928 destroyed, so the fixture must contain it rather than a hard-edged mask.
static Vector<uint8_t> _build_source_premultiplied_alpha_ramp_64() {
	Vector<uint8_t> data;
	data.resize(64 * 64 * 4);
	uint8_t *w = data.ptrw();
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			const int i = (y * 64 + x) * 4;
			const uint8_t a = uint8_t(x * 4);
			const uint8_t premultiplied_grey = uint8_t((int(a) * 128) / 255);
			w[i + 0] = premultiplied_grey;
			w[i + 1] = premultiplied_grey;
			w[i + 2] = premultiplied_grey;
			w[i + 3] = a;
		}
	}
	return data;
}

// 64x64 RGBA16F destination filled with a uniform alpha -- the pre-upscale
// composite's real destination is the LINEAR pre-tonemap internal scene buffer,
// which a transparent_bg viewport clears to alpha 0.
static Vector<uint8_t> _build_destination_rgba16f_64(float p_alpha) {
	Vector<uint8_t> data;
	data.resize(64 * 64 * 4 * 2);
	uint8_t *w = data.ptrw();
	const uint16_t zero_h = Math::make_half_float(0.0f);
	const uint16_t alpha_h = Math::make_half_float(p_alpha);
	for (int px = 0; px < 64 * 64; px++) {
		uint16_t *texel = reinterpret_cast<uint16_t *>(w + px * 8);
		texel[0] = zero_h;
		texel[1] = zero_h;
		texel[2] = zero_h;
		texel[3] = alpha_h;
	}
	return data;
}

static float _rgba16f_alpha_at(const Vector<uint8_t> &p_data, int p_x, int p_y) {
	const uint16_t *texel = reinterpret_cast<const uint16_t *>(p_data.ptr() + (p_y * 64 + p_x) * 8);
	return Math::half_to_float(texel[3]);
}

} // namespace CompositeAlphaRepro

// #928: the pre-upscale compute composite forced `result_color.a = 1.0` for
// every texel it touched. That assumption was written against the LEGACY
// post-tonemap destination, whose alpha channel is undefined. Under the Option B
// contract the destination is the internal scene buffer, whose alpha IS
// meaningful, so the force flattened splat coverage: on a transparent_bg
// viewport every splat-touched texel became fully opaque and its premultiplied
// fringe read dark (the consumer un-premultiplies by 1.0 instead of by the real
// coverage).
//
// WHY THIS CASE DISCRIMINATES. A pixel oracle that only ever reports "alpha is
// what I expected" is worthless if the harness cannot represent any other
// alpha. So the SAME source, destination and code path are run twice, and the
// two runs must disagree:
//
//   * destination_has_alpha = true  -> straight source-over; the alpha ramp
//                                      SURVIVES (63 distinct partial values).
//   * destination_has_alpha = false -> legacy contract; every covered texel is
//                                      exactly 1.0 (bit-identical to pre-fix).
//
// The legacy leg is load-bearing twice over: it proves the readback can observe
// alpha other than the value leg 1 expects, and it pins that this fix did not
// change the post-tonemap path that mobile/multiview/reflection probes use.
// Tagged [SceneTree] so the harness bootstraps a RenderingServer for this case.
// Two reasons, both load-bearing:
//   1. NO ENVIRONMENT SKIP. REQUIRE_GPU_DEVICE() would add a site to the
//      shrink-only environment-skip inventory (#595), which by design has no
//      add path. A [SceneTree][RequiresGPU] case instead ASSERTS that the
//      harness supplied the environment, the same shape as the #980 case in
//      test_scene_director_submission_scaffolding.h:1263-1274.
//   2. It is the shape the #980 precedent uses for a GPU case that needs engine
//      services rather than a bare local device.
//
// MEASURED LIMIT of this harness, stated rather than papered over: even with a
// RenderingServer, `--gs-gpu-test` does not supply a usable
// RendererRD::TextureStorage DEFAULT_RD_TEXTURE_DEPTH, which
// _copy_final_output_compute substitutes for bindings 2/3 when depth_test is
// off (output_compositor.cpp:1116-1127). With depth_test=false the uniform set
// therefore fails to build ("Texture (binding: 2, index 0) is not a valid
// texture") and the composite silently falls through to the graphics path --
// i.e. the case would measure the wrong path. So this case supplies real depth
// textures at the far plane and requests the depth test, exactly as the
// HazardRepro case above does.
//
// That costs no coverage of the defect: the branch under test
// (viewport_blit.glsl, the `composite_with_destination != 0` block) is reached
// identically for both depth_test values and its alpha math does not read the
// depth result. The depth_test=FALSE routing that #924 newly sent through this
// shader is covered instead by the PR's real-scan runtime A/B, which was
// captured at BOTH depth_test values, and by check_gs_pre_upscale_hook.py,
// which pins the routing itself.
TEST_CASE("[GaussianSplatting][OutputCompositor][SceneTree][RequiresGPU] Pre-upscale composite preserves destination alpha instead of forcing opaque") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		FAIL("RenderingServer unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return;
	}
	RenderingDevice *rd = rs->get_rendering_device();
	if (rd == nullptr) {
		FAIL("RenderingDevice unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return;
	}
	HazardTestScope _scope(rd);

	RD::TextureFormat source_format;
	source_format.width = 64;
	source_format.height = 64;
	source_format.depth = 1;
	source_format.array_layers = 1;
	source_format.mipmaps = 1;
	source_format.texture_type = RD::TEXTURE_TYPE_2D;
	source_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	// RGBA16F: the real pre-upscale destination format, and the one that
	// selects the shader's VIEWPORT_BLIT_FORMAT==1 permutation.
	RD::TextureFormat destination_format;
	destination_format.width = 64;
	destination_format.height = 64;
	destination_format.depth = 1;
	destination_format.array_layers = 1;
	destination_format.mipmaps = 1;
	destination_format.texture_type = RD::TEXTURE_TYPE_2D;
	destination_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	destination_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	RID source_tex = rd->texture_create(source_format, RD::TextureView());
	RID destination_tex = rd->texture_create(destination_format, RD::TextureView());
	_scope.track(source_tex);
	_scope.track(destination_tex);
	REQUIRE(source_tex.is_valid());
	REQUIRE(destination_tex.is_valid());
	rd->set_resource_name(source_tex, "GS_AlphaRepro_Source");
	rd->set_resource_name(destination_tex, "GS_AlphaRepro_Destination");

	const Vector<uint8_t> source_data = CompositeAlphaRepro::_build_source_premultiplied_alpha_ramp_64();
	REQUIRE(rd->texture_update(source_tex, 0, source_data) == OK);

	RD::TextureFormat depth_format;
	depth_format.width = 64;
	depth_format.height = 64;
	depth_format.depth = 1;
	depth_format.array_layers = 1;
	depth_format.mipmaps = 1;
	depth_format.texture_type = RD::TEXTURE_TYPE_2D;
	depth_format.format = RD::DATA_FORMAT_D32_SFLOAT;
	depth_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	RID source_depth = rd->texture_create(depth_format, RD::TextureView());
	RID destination_depth = rd->texture_create(depth_format, RD::TextureView());
	_scope.track(source_depth);
	_scope.track(destination_depth);
	REQUIRE(source_depth.is_valid());
	REQUIRE(destination_depth.is_valid());

	Vector<uint8_t> depth_far_bytes;
	depth_far_bytes.resize(64 * 64 * sizeof(float));
	{
		const float far_depth = 1.0f;
		uint8_t *dp = depth_far_bytes.ptrw();
		for (int i = 0; i < 64 * 64; i++) {
			memcpy(dp + i * sizeof(float), &far_depth, sizeof(float));
		}
	}
	REQUIRE(rd->texture_update(source_depth, 0, depth_far_bytes) == OK);
	REQUIRE(rd->texture_update(destination_depth, 0, depth_far_bytes) == OK);

	Ref<OutputCompositor> compositor;
	compositor.instantiate();
	_scope.compositor = compositor;
	REQUIRE(compositor->initialize(rd) == OK);

	OutputCopyParams params;
	params.source_texture = source_tex;
	params.destination_texture = destination_tex;
	params.viewport_size = Size2i(64, 64);
	params.composite_with_destination = true;
	params.source_is_premultiplied = true;
	// source_decode_srgb marks this as the pre-upscale phase (output_compositor.cpp
	// :1755 sets it from pre_upscale_phase), which is also what routes the
	// configuration onto the compute path at :1460.
	params.source_decode_srgb = true;
	// Both depths sit at the far plane, so the shader's `gs_depth < 0.999999`
	// guard is false and the comparison discards nothing -- the depth test is
	// requested only to keep the composite on the compute path under this
	// harness (see the MEASURED LIMIT note on the case above).
	params.depth_test_enabled = true;
	params.source_depth = source_depth;
	params.destination_depth = destination_depth;
	params.z_near = 0.05f;
	params.z_far = 4000.0f;
	params.depth_linearize_mul = 1.0f;
	params.depth_linearize_add = 1.0f;
	params.depth_epsilon = 0.01f;

	// ---- Leg 1: destination carries meaningful alpha, cleared transparent ----
	REQUIRE(rd->texture_update(destination_tex, 0,
				   CompositeAlphaRepro::_build_destination_rgba16f_64(0.0f)) == OK);
	params.destination_has_alpha = true;
	OutputCopyResult result_alpha = compositor->copy_to_render_target(params);
	if (!result_alpha.success) {
		print_line(vformat("[AlphaRepro] copy_to_render_target failed: %s", result_alpha.error));
	}
	REQUIRE(result_alpha.success);
	CHECK(result_alpha.source_decode_honored);

	Vector<uint8_t> out_alpha = rd->texture_get_data(destination_tex, 0);
	REQUIRE(out_alpha.size() == 64 * 64 * 4 * 2);

	// Over a fully transparent destination, straight source-over reduces to
	// out.a == src.a, so the source's coverage ramp must survive intact.
	int distinct_partial = 0;
	int forced_opaque = 0;
	for (int x = 1; x < 64; x++) {
		const float expected = float(x * 4) / 255.0f;
		const float actual = CompositeAlphaRepro::_rgba16f_alpha_at(out_alpha, x, 32);
		CHECK(Math::abs(actual - expected) < 0.01f);
		if (actual > 0.001f && actual < 0.999f) {
			distinct_partial++;
		}
		if (actual > 0.999f) {
			forced_opaque++;
		}
	}
	// 63 sampled columns, coverage 4/255 .. 252/255 -- none of them reaches 1.0.
	CHECK(distinct_partial == 63);
	CHECK(forced_opaque == 0);

	// Column 0 has source alpha 0: the shader early-returns without storing, so
	// the destination keeps the value it was cleared to.
	CHECK(CompositeAlphaRepro::_rgba16f_alpha_at(out_alpha, 0, 32) < 0.001f);

	// ---- Leg 2: destination alpha is meaningful AND non-zero ----
	REQUIRE(rd->texture_update(destination_tex, 0,
				   CompositeAlphaRepro::_build_destination_rgba16f_64(0.5f)) == OK);
	OutputCopyResult result_over = compositor->copy_to_render_target(params);
	REQUIRE(result_over.success);
	Vector<uint8_t> out_over = rd->texture_get_data(destination_tex, 0);
	REQUIRE(out_over.size() == 64 * 64 * 4 * 2);
	for (int x = 1; x < 64; x++) {
		const float src_a = float(x * 4) / 255.0f;
		const float expected = src_a + 0.5f * (1.0f - src_a);
		const float actual = CompositeAlphaRepro::_rgba16f_alpha_at(out_over, x, 32);
		CHECK(Math::abs(actual - expected) < 0.01f);
	}

	// ---- Leg 3 (discrimination + legacy pin): opaque-destination contract ----
	// Same source, same destination clear, same code path, one flag flipped.
	// If this leg reported the same alphas as leg 1, leg 1 would be measuring
	// nothing; if it reported anything other than 1.0, this fix would have
	// changed the post-tonemap path that multiview/mobile/probes still use.
	REQUIRE(rd->texture_update(destination_tex, 0,
				   CompositeAlphaRepro::_build_destination_rgba16f_64(0.0f)) == OK);
	params.destination_has_alpha = false;
	OutputCopyResult result_legacy = compositor->copy_to_render_target(params);
	REQUIRE(result_legacy.success);
	Vector<uint8_t> out_legacy = rd->texture_get_data(destination_tex, 0);
	REQUIRE(out_legacy.size() == 64 * 64 * 4 * 2);

	int legacy_opaque = 0;
	int legacy_partial = 0;
	for (int x = 1; x < 64; x++) {
		const float actual = CompositeAlphaRepro::_rgba16f_alpha_at(out_legacy, x, 32);
		if (actual > 0.999f) {
			legacy_opaque++;
		} else if (actual > 0.001f) {
			legacy_partial++;
		}
	}
	CHECK(legacy_opaque == 63);
	CHECK(legacy_partial == 0);

	// The two legs must actually disagree -- this is the assertion that makes
	// leg 1's "no forced-opaque texels" result mean something.
	CHECK(distinct_partial != legacy_partial);
}

} // namespace TestGaussianSplatting

#endif // TESTS_ENABLED
