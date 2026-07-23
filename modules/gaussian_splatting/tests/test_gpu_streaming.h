/**************************************************************************/
/*  test_gpu_streaming.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../core/gaussian_streaming.h"
#include "../renderer/gpu_memory_stream.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "core/math/math_defs.h"
#include "core/math/random_number_generator.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"
#include "tests/test_macros.h"
#include <atomic>

namespace TestGaussianSplatting {

static Ref<::GaussianData> _create_registered_streaming_test_gaussian_data(uint32_t p_count) {
	Ref<::GaussianData> data;
	data.instantiate();

	LocalVector<Gaussian> gaussians;
	gaussians.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		Gaussian &g = gaussians[i];
		g.position = Vector3(i * 0.1f, i * 0.2f, i * 0.3f);
		g.scale = Vector3(0.5f, 0.5f, 0.5f);
		g.rotation = Quaternion();
		g.opacity = 0.9f;
		g.sh_dc = Color(1.0f, 0.5f, 0.2f, 0.9f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.area = 0.25f;
	}

	data->set_gaussians(gaussians);
	return data;
}

// #513: builds an asset whose gaussians carry MIXED DC encodings, so
// _data_has_uniform_dc_encoding() reports false and registering it flips the
// effective per-chunk quantization state (and therefore the atlas byte stride).
static Ref<::GaussianData> _create_mixed_dc_streaming_test_gaussian_data(uint32_t p_count) {
	Ref<::GaussianData> data;
	data.instantiate();

	LocalVector<Gaussian> gaussians;
	gaussians.resize(MAX(2u, p_count));
	for (uint32_t i = 0; i < gaussians.size(); i++) {
		Gaussian &g = gaussians[i];
		g.position = Vector3(i * 0.1f, i * 0.2f, i * 0.3f);
		g.scale = Vector3(0.5f, 0.5f, 0.5f);
		g.rotation = Quaternion();
		g.opacity = 0.9f;
		g.sh_dc = Color(1.0f, 0.5f, 0.2f, 0.9f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.area = 0.25f;
		// Alternate the DC encoding bit so the asset is NOT DC-uniform.
		const GaussianDCEncoding encoding = (i % 2u == 0u)
				? GAUSSIAN_DC_ENCODING_LEGACY_BIAS
				: GAUSSIAN_DC_ENCODING_LINEAR_RGB;
		g.render_meta = gaussian_set_dc_encoding(g.render_meta, encoding);
	}

	data->set_gaussians(gaussians);
	return data;
}

struct ConcurrentStreamingAssertionsContext {
	Ref<GaussianMemoryStream> stream;
	uint32_t uploads_per_thread = 0;
	uint32_t gaussians_per_upload = 0;
	std::atomic<int> successful_uploads{0};
	std::atomic<int> failed_uploads{0};
	Semaphore begin;
	Semaphore done;
};

static void _concurrent_streaming_assertions_worker(void *p_userdata) {
	ConcurrentStreamingAssertionsContext *ctx = static_cast<ConcurrentStreamingAssertionsContext *>(p_userdata);
	if (!ctx || !ctx->stream.is_valid()) {
		return;
	}

	ctx->begin.wait();
	for (uint32_t i = 0; i < ctx->uploads_per_thread; i++) {
		LocalVector<Gaussian> gaussians;
		gaussians.resize(ctx->gaussians_per_upload);
		for (uint32_t j = 0; j < ctx->gaussians_per_upload; j++) {
			Gaussian &g = gaussians[j];
			g.position = Vector3(float(j) * 0.01f, float(i), 0.0f);
			g.scale = Vector3(0.5f, 0.5f, 0.5f);
			g.rotation = Quaternion();
			g.opacity = 1.0f;
			g.sh_dc = Color(1.0f, 0.6f, 0.3f, 1.0f);
			g.normal = Vector3(0.0f, 1.0f, 0.0f);
			g.area = 0.25f;
		}

		const Error upload_err = ctx->stream->stream_gaussians_async(gaussians);
		if (upload_err == OK) {
			ctx->successful_uploads.fetch_add(1, std::memory_order_relaxed);
		} else {
			ctx->failed_uploads.fetch_add(1, std::memory_order_relaxed);
		}
	}
	ctx->done.post();
}

TEST_CASE("[GaussianSplatting][RequiresGPU] GPU Memory Streaming") {
	// Get or create rendering device
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
		}
	}

	// Skip GPU tests if no rendering device available
	if (!rd) {
		MESSAGE("Skipping GPU streaming tests - no RenderingDevice available");
		return;
	}

	SUBCASE("Initialize memory stream") {
		Ref<GaussianMemoryStream> stream;
		stream.instantiate();

		Error err = stream->initialize(rd, 10000, 256);
		CHECK(err == OK);
		CHECK(stream->get_max_gaussians() == 10000);
	}

	SUBCASE("Upload gaussians to GPU") {
		Ref<GaussianMemoryStream> stream;
		stream.instantiate();

		Error err = stream->initialize(rd, 1000, 256);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		// Create test data
		LocalVector<Gaussian> splats;
		splats.resize(100);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (int i = 0; i < 100; i++) {
			Gaussian &g = splats[i];
			g.position = Vector3(
				rng.randf_range(-5.0f, 5.0f),
				rng.randf_range(-5.0f, 5.0f),
				rng.randf_range(-5.0f, 5.0f)
			);
			g.scale = Vector3(0.5f, 0.5f, 0.5f);
			g.rotation = Quaternion();
			g.opacity = 1.0f;
			g.sh_dc = Color(1, 0, 0, 1);
			g.normal = Vector3(0, 1, 0);
			g.area = 0.785f;
		}

		// Test upload
		stream->begin_frame(0);

		err = stream->stream_gaussians_immediate(splats);
		CHECK(err == OK);

		stream->end_frame();
		CHECK(stream->is_upload_complete());
	}

	SUBCASE("Triple buffering") {
		Ref<GaussianMemoryStream> stream;
		stream.instantiate();

		Error err = stream->initialize(rd, 500, 256);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		// Create test data for multiple frames
		LocalVector<Gaussian> frame_data[3];
		for (int frame = 0; frame < 3; frame++) {
			frame_data[frame].resize(100);
			for (int i = 0; i < 100; i++) {
				Gaussian &g = frame_data[frame][i];
				g.position = Vector3(frame, i, 0);
				g.scale = Vector3(1, 1, 1);
				g.rotation = Quaternion();
				g.opacity = 1.0f;
				g.sh_dc = Color(1, 1, 1, 1);
				g.normal = Vector3(0, 1, 0);
				g.area = static_cast<float>(Math::PI);
			}
		}

		// Test triple buffering
		for (int frame = 0; frame < 6; frame++) {
			stream->begin_frame(frame);

			int data_idx = frame % 3;
			err = stream->stream_gaussians_immediate(frame_data[data_idx]);
			CHECK(err == OK);

			stream->end_frame();
			CHECK(stream->is_upload_complete());

			// Verify we get valid buffer for each frame
			RID current = stream->get_current_gpu_buffer();
			CHECK(current.is_valid());
		}
	}

	SUBCASE("Invalid initialization") {
		Ref<GaussianMemoryStream> stream;
		stream.instantiate();

		// Test with null device
		Error err = stream->initialize(nullptr, 1000, 256);
		CHECK(err == ERR_INVALID_PARAMETER);
		CHECK(stream->get_max_gaussians() == 1000000); // Default value, not changed

		// Test with zero capacity
		err = stream->initialize(rd, 0, 256);
		CHECK(err == ERR_INVALID_PARAMETER);
	}

	SUBCASE("Concurrent async uploads keep deterministic accounting") {
#ifndef THREADS_ENABLED
		MESSAGE("Skipping concurrent upload assertions - THREADS_ENABLED is not enabled");
		return;
#endif

		Ref<GaussianMemoryStream> stream;
		stream.instantiate();

		const uint32_t gaussians_per_upload = 256;
		const uint32_t uploads_per_thread = 8;
		Error err = stream->initialize(rd, gaussians_per_upload * uploads_per_thread * 2, 64);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		ConcurrentStreamingAssertionsContext ctx;
		ctx.stream = stream;
		ctx.uploads_per_thread = uploads_per_thread;
		ctx.gaussians_per_upload = gaussians_per_upload;

		Thread worker_a;
		Thread worker_b;
		worker_a.start(_concurrent_streaming_assertions_worker, &ctx);
		worker_b.start(_concurrent_streaming_assertions_worker, &ctx);
		const bool worker_a_started = worker_a.is_started();
		const bool worker_b_started = worker_b.is_started();
		CHECK(worker_a_started);
		CHECK(worker_b_started);
		if (!worker_a_started || !worker_b_started) {
			if (worker_a_started) {
				ctx.begin.post();
				worker_a.wait_to_finish();
			}
			if (worker_b_started) {
				ctx.begin.post();
				worker_b.wait_to_finish();
			}
			stream->shutdown();
			return;
		}

		ctx.begin.post(2);
		ctx.done.wait();
		ctx.done.wait();
		worker_a.wait_to_finish();
		worker_b.wait_to_finish();

		stream->wait_for_all_uploads();
		CHECK(stream->is_upload_complete());

		const int successful_uploads = ctx.successful_uploads.load(std::memory_order_relaxed);
		const int failed_uploads = ctx.failed_uploads.load(std::memory_order_relaxed);
		const int total_uploads = int(uploads_per_thread * 2);
		CHECK(successful_uploads + failed_uploads == total_uploads);
		CHECK(successful_uploads > 0);

		const StreamingStats stats = stream->get_stats();
		CHECK(int(stats.buffer_switches) == successful_uploads);
		const uint64_t expected_bytes_uploaded = uint64_t(successful_uploads) *
				uint64_t(gaussians_per_upload) * uint64_t(sizeof(PackedGaussian));
		CHECK(stats.total_bytes_uploaded == expected_bytes_uploaded);
	}
}

TEST_CASE("[GaussianSplatting][RequiresGPU] GPU Memory Streaming Performance") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
		}
	}

	if (!rd) {
		MESSAGE("Skipping GPU performance tests - no RenderingDevice available");
		return;
	}

	SUBCASE("Upload performance scaling") {
		const uint32_t test_sizes[] = {100, 1000, 10000, 50000};

		for (uint32_t size : test_sizes) {
			Ref<GaussianMemoryStream> stream;
			stream.instantiate();

			Error err = stream->initialize(rd, size, 256);
			CHECK(err == OK);
			if (err != OK) {
				return;
			}

			// Create test data
			LocalVector<Gaussian> splats;
			splats.resize(size);

			for (uint32_t i = 0; i < size; i++) {
				Gaussian &g = splats[i];
				g.position = Vector3(i, 0, 0);
				g.scale = Vector3(1, 1, 1);
				g.rotation = Quaternion();
				g.opacity = 1.0f;
				g.sh_dc = Color(1, 1, 1, 1);
				g.normal = Vector3(0, 1, 0);
				g.area = static_cast<float>(Math::PI);
			}

			// Measure upload time
			uint64_t start = OS::get_singleton()->get_ticks_usec();

			stream->begin_frame(0);

			err = stream->stream_gaussians_immediate(splats);
			CHECK(err == OK);

			stream->end_frame();

			uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
			float ms = elapsed / 1000.0f;

			// Performance targets
			if (size <= 10000) {
				CHECK_MESSAGE(ms < 10.0f,
					vformat("Upload of %d splats took %.2fms, expected < 10ms", size, ms));
			} else if (size <= 50000) {
				CHECK_MESSAGE(ms < 50.0f,
					vformat("Upload of %d splats took %.2fms, expected < 50ms", size, ms));
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][RequiresGPU] Stage-B instance depth culling toggles") {
	GaussianSplatManager *manager_owner = nullptr;
	GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
	if (!manager) {
		manager_owner = memnew(GaussianSplatManager);
		manager = manager_owner;
	}

	CHECK(manager != nullptr);
	if (manager == nullptr) {
		return;
	}

	RenderingDevice *primary_device = manager->get_primary_rendering_device();
	if (primary_device == nullptr) {
		MESSAGE("Skipping Stage-B culling test - primary RenderingDevice unavailable");
		if (manager_owner) {
			memdelete(manager_owner);
		}
		return;
	}

	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(primary_device);
	CHECK(renderer.is_valid());
	if (!renderer.is_valid()) {
		if (manager_owner) {
			memdelete(manager_owner);
		}
		return;
	}

	const uint32_t total_gaussians = 4096;
	LocalVector<Gaussian> gaussians;
	gaussians.resize(total_gaussians);
	const uint32_t near_band_count = total_gaussians / 2;
	for (uint32_t i = 0; i < total_gaussians; i++) {
		Gaussian &g = gaussians[i];
		g = Gaussian{};
		const bool in_near_band = i < near_band_count;
		const float band_x = in_near_band ? -0.8f : 28.0f;
		const float local_x = float(i % 64) * 0.02f;
		const float local_y = (float((i / 64) % 64) - 32.0f) * 0.03f;
		g.position = Vector3(band_x + local_x, local_y, -6.0f);
		g.scale = Vector3(0.03f, 0.03f, 0.03f);
		g.rotation = Quaternion();
		g.opacity = 1.0f;
		g.sh_dc = Color(1.0f, 1.0f, 1.0f, 1.0f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.area = 0.01f;
	}

	Ref<::GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);

	Error set_data_err = renderer->set_gaussian_data(data);
	CHECK(set_data_err == OK);
	if (set_data_err != OK) {
		renderer.unref();
		if (manager_owner) {
			memdelete(manager_owner);
		}
		return;
	}

	renderer->set_lod_enabled(true);
	renderer->set_lod_bias(1.0f);
	renderer->set_lod_min_screen_size(0.0f);
	renderer->set_lod_max_distance(0.0f);
	renderer->set_tiny_splat_screen_radius(0.0f);
	renderer->set_frustum_culling(false);

	Transform3D cam_transform;
	Projection projection;
	projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);

	auto render_sample = [&](int p_frames) {
		uint32_t visible = 0;
		for (int i = 0; i < p_frames; i++) {
			const bool rendered = renderer->render_for_view(cam_transform, projection, RID(), Size2i(512, 512));
			CHECK(rendered);
			if (!rendered) {
				break;
			}
			visible = renderer->get_visible_splat_count();
			OS::get_singleton()->delay_usec(500);
		}
		return visible;
	};

	uint32_t baseline_visible = 0;
	for (int i = 0; i < 180; i++) {
		const uint32_t visible = render_sample(1);
		if (renderer->has_instance_pipeline_buffers() && renderer->has_rendered_content() && visible > 0) {
			baseline_visible = visible;
			break;
		}
	}

	if (baseline_visible == 0) {
		MESSAGE("Skipping Stage-B culling test - instance pipeline did not become ready");
		renderer.unref();
		if (manager_owner) {
			memdelete(manager_owner);
		}
		return;
	}

	renderer->set_frustum_culling(true);
	const uint32_t frustum_visible = render_sample(6);

	renderer->set_frustum_culling(false);
	renderer->set_tiny_splat_screen_radius(64.0f);
	const uint32_t screen_visible = render_sample(6);

	renderer->set_tiny_splat_screen_radius(0.0f);
	renderer->set_lod_max_distance(8.0f);
	const uint32_t distance_visible = render_sample(6);

	CHECK(frustum_visible < baseline_visible);
	CHECK(screen_visible < baseline_visible);
	CHECK(distance_visible < baseline_visible);

	renderer.unref();
	if (manager_owner) {
		memdelete(manager_owner);
	}
}

TEST_CASE("[GaussianSplatting][Streaming] Dense-id remap rejects stale mappings without aliasing to primary") {
	Ref<GaussianStreamingSystem> system;
	system.instantiate();
	system->initialize_empty(nullptr);

	const uint32_t asset_a = 101;
	const uint32_t asset_b = 202;
	const uint32_t asset_c = 303;
	system->register_asset(asset_a, _create_registered_streaming_test_gaussian_data(1024));
	system->register_asset(asset_b, _create_registered_streaming_test_gaussian_data(1024));

	LocalVector<InstanceDataGPU> mapped_a;
	mapped_a.resize(1);
	mapped_a[0] = {};
	mapped_a[0].ids[0] = asset_a;
	CHECK(system->remap_instance_asset_ids(mapped_a, false));

	const uint32_t dense_a = mapped_a[0].ids[0];
	const uint32_t dense_a_generation = mapped_a[0].lod[1];
	CHECK(dense_a != 0u);
	CHECK(dense_a_generation != 0u);

	system->unregister_asset(asset_a);
	system->register_asset(asset_c, _create_registered_streaming_test_gaussian_data(1024));

	LocalVector<InstanceDataGPU> mapped_c;
	mapped_c.resize(1);
	mapped_c[0] = {};
	mapped_c[0].ids[0] = asset_c;
	CHECK(system->remap_instance_asset_ids(mapped_c, false));

	const uint32_t dense_c = mapped_c[0].ids[0];
	const uint32_t dense_c_generation = mapped_c[0].lod[1];
	CHECK(dense_c == dense_a);
	CHECK(dense_c_generation != dense_a_generation);

	LocalVector<InstanceDataGPU> stale_dense_mapping;
	stale_dense_mapping.resize(1);
	stale_dense_mapping[0] = {};
	stale_dense_mapping[0].ids[0] = dense_a;
	stale_dense_mapping[0].lod[1] = dense_a_generation;

	CHECK_FALSE(system->remap_instance_asset_ids(stale_dense_mapping, false));
	CHECK(stale_dense_mapping[0].ids[0] == dense_a);
	CHECK(stale_dense_mapping[0].lod[1] == dense_a_generation);
}

TEST_CASE("[GaussianSplatting][Streaming] Dense-id remap rejects unknown asset ids without aliasing to primary") {
	Ref<GaussianStreamingSystem> system;
	system.instantiate();
	system->initialize_empty(nullptr);

	const uint32_t asset_a = 101;
	const uint32_t missing_asset = 404;
	system->register_asset(asset_a, _create_registered_streaming_test_gaussian_data(1024));

	LocalVector<InstanceDataGPU> missing_mapping;
	missing_mapping.resize(1);
	missing_mapping[0] = {};
	missing_mapping[0].ids[0] = missing_asset;
	missing_mapping[0].lod[1] = 77u;

	CHECK_FALSE(system->remap_instance_asset_ids(missing_mapping, false));
	CHECK(missing_mapping[0].ids[0] == missing_asset);
	CHECK(missing_mapping[0].lod[1] == 77u);
}

TEST_CASE("[GaussianSplatting] Atlas byte stride tracks the EFFECTIVE quantization state (Q80A/Q80B)") {
	// Regression for the mixed-DC fallback: _refresh_quantization_dc_compatibility()
	// disables per-chunk quantization for mixed DC-encoding assets and promises the
	// "non-quantized upload path". The atlas stride (and the pack layout it drives) must
	// therefore key off the EFFECTIVE state is_per_chunk_quantization_enabled()
	// (enabled && dc_compatible), not the raw enabled flag -- otherwise it allocates 80 B
	// slots and packs the quantized layout while the renderer reads 144 B (corruption).
	Ref<GaussianStreamingSystem> sys;
	sys.instantiate();
	REQUIRE(sys.is_valid());

	// enabled + DC-compatible -> quantized 80 B stride.
	sys->_test_set_quantization_state(true, true);
	CHECK(sys->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussianQuantized)));

	// enabled but DC-INCOMPATIBLE -> must fall back to the 144 B raw stride (the bug: this
	// used to stay 80 B while the renderer interpreted 144 B).
	sys->_test_set_quantization_state(true, false);
	CHECK(sys->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));

	// disabled -> 144 B raw stride regardless of DC state.
	sys->_test_set_quantization_state(false, true);
	CHECK(sys->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));
	sys->_test_set_quantization_state(false, false);
	CHECK(sys->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));
}

TEST_CASE("[GaussianSplatting][Streaming] Registering a mixed-DC asset evicts chunks resident at the old stride (#513)") {
	// #513 stride-flip hazard: with per-chunk quantization enabled the atlas packs an 80 B
	// stride. Registering a mixed-DC asset flips the EFFECTIVE quantization state off inside
	// _refresh_quantization_dc_compatibility(), so _atlas_gaussian_stride_bytes() jumps to
	// 144 B. Any chunk already resident was packed/accounted at 80 B: leaving it resident makes
	// the renderer reinterpret its 80 B payload at 144 B (GPU corruption) and skews
	// budget.vram_usage when it is decremented at the new 144 B stride. The fail-closed guard
	// must evict every resident chunk under the OLD stride before committing the flip.
	//
	// Discrimination vs. origin/master: on base the flip leaves the resident chunk loaded and
	// the payload accounting inflated, so get_loaded_chunks() == 1 and get_vram_usage() stays
	// at the loaded value -> both CHECKs below are RED. With the guard the chunk is evicted at
	// the old stride, so loaded == 0 and vram returns exactly to its pre-load baseline.
	Ref<GaussianStreamingSystem> system;
	system.instantiate();
	if (!system.is_valid()) {
		FAIL("streaming system failed to instantiate");
		return;
	}

	// Enable per-chunk quantization (effective 80 B stride) before anything is resident.
	system->_test_set_quantization_state(true, true);
	CHECK(system->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussianQuantized)));

	// A DC-uniform asset keeps the effective quantized stride; register it and make one chunk
	// resident at 80 B.
	const uint32_t asset_uniform = 5130;
	system->register_asset(asset_uniform, _create_registered_streaming_test_gaussian_data(1024));
	system->_test_reset_atlas_allocator(4);

	const uint64_t vram_baseline = system->get_vram_usage();
	system->_test_mark_chunk_loaded_for_eviction(asset_uniform, 0, true, 1, 1, 1.0f);
	CHECK(system->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussianQuantized)));
	CHECK(system->get_loaded_chunks() == 1);
	const uint64_t vram_loaded = system->get_vram_usage();
	CHECK(vram_loaded > vram_baseline); // the resident 80 B chunk added payload bytes

	// Registering a MIXED-DC asset flips the effective quantization state off -> stride 144 B.
	const uint32_t asset_mixed = 5131;
	system->register_asset(asset_mixed, _create_mixed_dc_streaming_test_gaussian_data(64));
	CHECK(system->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));

	// Fail closed: no chunk may remain resident across the stride flip, and the payload
	// accounting must return to its pre-load baseline (no skew).
	CHECK(system->get_loaded_chunks() == 0);
	CHECK(system->get_vram_usage() == vram_baseline);
}

TEST_CASE("[GaussianSplatting][Streaming] A stride flip drops a pending upload staged at the old stride (#757 / #513)") {
	// #757 closes the window #513 left open. #513 only evicts chunks that are already
	// *resident* (is_loaded) before flipping the effective atlas stride. A chunk that has been
	// packed and uploaded but is still waiting in the frame-delay retirement queue is
	// upload_pending (is_loaded == false), so #513's eviction skips it. If the effective stride
	// flips (a mixed-DC asset registration toggles per_chunk_quantization_dc_compatible) before
	// that ticket retires, _process_upload_retirements() would mark the OLD-stride (80 B) payload
	// resident and account/render it at the NEW stride (144 B) -> silent GPU corruption + skewed
	// budget.vram_usage. The fail-closed stride guard must drop such a ticket instead.
	//
	// This drives the exact ordering: stage a pending retirement at 80 B, flip the stride to
	// 144 B via a mixed-DC registration (the pending chunk survives #513's eviction), then let
	// the ticket retire through the real begin_frame() -> _process_upload_retirements() path.
	//
	// Mutation/discrimination: remove or bypass the stride guard in
	// _process_upload_retirements() and the pending chunk is completed at 144 B -> loaded == 1,
	// vram inflated by count*144 B, and stride_flip_dropped == 0 -> all four post-retire CHECKs
	// go RED. With the guard: loaded == 0, vram unchanged, slot drained, dropped == 1.
	GaussianStreamingSystem system;

	// Effective 80 B stride (per-chunk quantization enabled and DC-compatible).
	system._test_set_quantization_state(true, true);
	CHECK(system._test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussianQuantized)));

	// A DC-uniform asset keeps the quantized stride; register it and reset the atlas allocator.
	const uint32_t asset_uniform = 7570;
	system.register_asset(asset_uniform, _create_registered_streaming_test_gaussian_data(1024));
	system._test_reset_atlas_allocator(4);

	// Stage a pending upload for chunk 0 at the OLD (80 B) stride: allocate a slot, begin the
	// upload, then stage a MAIN_RD frame-delay retirement ticket. The chunk stays upload_pending
	// (is_loaded == false), which is precisely the state #513's resident-only eviction ignores.
	GaussianStreamingSystem::AtlasAssetState *asset = system._test_get_asset_state(asset_uniform);
	if (!asset) {
		FAIL("uniform asset state missing");
		return;
	}
	LocalVector<GaussianStreamingSystem::StreamingChunk> &asset_chunks = system._test_get_asset_chunks(*asset);
	if (asset_chunks.is_empty()) {
		FAIL("uniform asset has no chunks");
		return;
	}
	GaussianStreamingSystem::StreamingChunk &chunk = asset_chunks[0];

	const uint64_t old_stride = system._test_atlas_gaussian_stride_bytes();
	CHECK(old_stride == uint64_t(sizeof(PackedGaussianQuantized)));
	const uint64_t upload_bytes = uint64_t(chunk.count) * old_stride;
	CHECK(upload_bytes > 0);

	uint32_t buffer_slot = UINT32_MAX;
	if (!system._test_atlas_allocator().allocate_slot(system._test_make_chunk_key(asset_uniform, 0), buffer_slot)) {
		FAIL("failed to allocate atlas slot for the pending chunk");
		return;
	}
	if (!system._test_begin_chunk_upload(asset_uniform, 0, chunk, buffer_slot)) {
		FAIL("failed to begin the chunk upload");
		return;
	}
	if (!system._test_stage_chunk_upload_retirement(asset_uniform, 0, chunk, buffer_slot, upload_bytes,
				/*retire_after_frames*/ 2,
				GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER)) {
		FAIL("failed to stage the upload retirement");
		return;
	}
	CHECK(system.get_pending_upload_retirement_slots() == 1);
	CHECK(system.get_loaded_chunks() == 0); // pending, not resident
	CHECK(system._test_get_stride_flip_dropped_upload_retirements() == 0);

	// Flip the effective stride to 144 B by registering a MIXED-DC asset. #513's guard runs but
	// only evicts resident chunks, so the pending chunk above survives at the old 80 B stride.
	// NOTE: register_asset() may rehash atlas_assets, so `asset`/`asset_chunks`/`chunk` may
	// dangle after this line -- they are re-fetched below before being read again.
	const uint32_t asset_mixed = 7571;
	system.register_asset(asset_mixed, _create_mixed_dc_streaming_test_gaussian_data(64));
	CHECK(system._test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));
	CHECK(system.get_pending_upload_retirement_slots() == 1); // the flip did not drain the ticket

	// Snapshot vram in the post-flip pending state. Nothing is resident yet, so this is the true
	// pre-retirement baseline; a wrong completion would add count*144 B on top of it.
	const uint64_t vram_before_retire = system.get_vram_usage();

	// Advance two frames so the ticket (retire_after_frames == 2) retires through the real
	// production entry point begin_frame() -> _process_upload_retirements().
	system.begin_frame();
	system.begin_frame();

	// Fail closed: the old-stride payload must NOT be made resident at the new stride. The ticket
	// is dropped, the pending slot drained, and vram is unchanged (no skew). The scheduler will
	// re-request and re-pack the chunk at 144 B on a later frame.
	CHECK(system.get_loaded_chunks() == 0); // guard removed -> 1 (corruption)
	CHECK(system.get_vram_usage() == vram_before_retire); // guard removed -> inflated by count*144 B
	CHECK(system.get_pending_upload_retirement_slots() == 0); // ticket drained
	CHECK(system._test_get_stride_flip_dropped_upload_retirements() == 1); // guard fired exactly once
	CHECK(system._test_get_failed_upload_retirements() == 0); // not an invariant violation, a clean drop

	// The pending chunk was rolled back to idle. Re-fetch state: the mixed registration may have
	// rehashed atlas_assets, invalidating the earlier `asset`/`chunk` handles.
	GaussianStreamingSystem::AtlasAssetState *asset_after = system._test_get_asset_state(asset_uniform);
	if (!asset_after) {
		FAIL("uniform asset state missing after the flip");
		return;
	}
	LocalVector<GaussianStreamingSystem::StreamingChunk> &chunks_after = system._test_get_asset_chunks(*asset_after);
	if (chunks_after.is_empty()) {
		FAIL("uniform asset lost its chunks after the flip");
		return;
	}
	CHECK_FALSE(chunks_after[0].is_loaded);
	CHECK_FALSE(chunks_after[0].upload_pending);
	CHECK(chunks_after[0].buffer_slot == UINT32_MAX);
}

} // namespace TestGaussianSplatting
