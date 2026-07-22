/**************************************************************************/
/*  test_gpu_sorting.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../renderer/gpu_sorter.h"
#include "../renderer/gpu_sorting_config.h"
#include "../core/gaussian_data.h"
#include "core/math/math_funcs.h"
#include "core/math/random_number_generator.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"
#include "tests/test_macros.h"
#include <algorithm>

// C4b (G4), Channel B miss-fix (PR #508 review): exercise the REAL instance_count_clamp.glsl
// shader to prove overflow_flag is STICKY across a skipped async readback.
#include "../compute/compute_infrastructure.h"
#include "../compute/instance_count_clamp.glsl.gen.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/pipeline_io_contracts.h"
#include <cstring>

namespace TestGaussianSplatting {

// Helper to create storage buffer from typed data
template<typename T>
static RID create_storage_buffer(RenderingDevice *rd, const LocalVector<T> &data) {
	Vector<uint8_t> bytes;
	bytes.resize(data.size() * sizeof(T));
	memcpy(bytes.ptrw(), data.ptr(), bytes.size());
	return rd->storage_buffer_create(bytes.size(), bytes);
}

// #622: carries the [GpuSort] tag so it is SELECTED by the required GpuSorting
// harness batch (filter "*Sort*][RequiresGPU]*"). Without a Sort-bearing tag
// BEFORE [RequiresGPU], the batch glob does not match (the "Sorting" in the
// description sits AFTER "][RequiresGPU]"), and this sort-order oracle -- though
// linked and registered -- runs in NO harness batch. The ascending-order CHECKs
// below are what make a deliberately-wrong sort turn the batch RED.
TEST_CASE("[GaussianSplatting][GpuSort][RequiresGPU] GPU Bitonic Sorting") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
		}
	}

	if (!rd) {
		MESSAGE("Skipping GPU sorting tests - no RenderingDevice available");
		return;
	}

	SUBCASE("Initialize GPU sorter") {
		Ref<BitonicSort> sorter;
		sorter.instantiate();

		Error err = sorter->initialize(rd, 10000);
		CHECK(err == OK);
		CHECK(sorter->get_max_elements() == 10000);
	}

	SUBCASE("Sort small dataset") {
		Ref<BitonicSort> sorter;
		sorter.instantiate();

		const uint32_t count = 256;
		Error err = sorter->initialize(rd, count);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		// Create test data with random depths
		LocalVector<float> depths;
		LocalVector<uint32_t> indices;
		depths.resize(count);
		indices.resize(count);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (uint32_t i = 0; i < count; i++) {
			depths[i] = rng.randf_range(0.0f, 100.0f);
			indices[i] = i;
		}

		// Create GPU buffers
		RID keys_buffer = create_storage_buffer(rd, depths);
		RID values_buffer = create_storage_buffer(rd, indices);

		// Sort on GPU
		err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);

		// Read back results
		Vector<uint8_t> sorted_depths = rd->buffer_get_data(keys_buffer);

		// Verify sorting order
		const float *depth_ptr = (const float *)sorted_depths.ptr();
		for (uint32_t i = 1; i < count; i++) {
			CHECK(depth_ptr[i] >= depth_ptr[i - 1]);
		}

		// Clean up
		rd->free(keys_buffer);
		rd->free(values_buffer);
	}

	SUBCASE("Sort power-of-two sizes") {
		const uint32_t test_sizes[] = {128, 256, 512, 1024, 2048};

		for (uint32_t size : test_sizes) {
			Ref<BitonicSort> sorter;
			sorter.instantiate();

			Error err = sorter->initialize(rd, size);
			CHECK(err == OK);
			if (err != OK) {
				continue;
			}

			// Create reverse-sorted data (worst case)
			LocalVector<float> depths;
			LocalVector<uint32_t> indices;
			depths.resize(size);
			indices.resize(size);

			for (uint32_t i = 0; i < size; i++) {
				depths[i] = float(size - i); // Reverse order
				indices[i] = i;
			}

			// Create GPU buffers
			RID keys_buffer = create_storage_buffer(rd, depths);
			RID values_buffer = create_storage_buffer(rd, indices);

			// Sort
			err = sorter->sort(keys_buffer, values_buffer, size);
			CHECK(err == OK);

			// Verify
			Vector<uint8_t> sorted_depths = rd->buffer_get_data(keys_buffer);
			const float *depth_ptr = (const float *)sorted_depths.ptr();

			for (uint32_t i = 0; i < size; i++) {
				CHECK(Math::is_equal_approx(depth_ptr[i], float(i + 1)));
			}

			// Clean up
			rd->free(keys_buffer);
			rd->free(values_buffer);
		}
	}

	SUBCASE("Handle non-power-of-two sizes") {
		Ref<BitonicSort> sorter;
		sorter.instantiate();

		const uint32_t count = 1000; // Not a power of 2
		Error err = sorter->initialize(rd, count);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		// BitonicSort handles non-power-of-two internally
		CHECK(sorter->supports_non_power_of_two());

		// Create test data
		LocalVector<float> depths;
		LocalVector<uint32_t> indices;
		depths.resize(count);
		indices.resize(count);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (uint32_t i = 0; i < count; i++) {
			depths[i] = rng.randf_range(0.0f, 100.0f);
			indices[i] = i;
		}

		// Create GPU buffers
		RID keys_buffer = create_storage_buffer(rd, depths);
		RID values_buffer = create_storage_buffer(rd, indices);

		// Sort
		err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);

		// Verify sorting
		Vector<uint8_t> sorted_depths = rd->buffer_get_data(keys_buffer);
		const float *depth_ptr = (const float *)sorted_depths.ptr();

		for (uint32_t i = 1; i < count; i++) {
			CHECK(depth_ptr[i] >= depth_ptr[i - 1]);
		}

		// Clean up
		rd->free(keys_buffer);
		rd->free(values_buffer);
	}
}

// #622: [GpuSort] tag opts this radix sort-order oracle into the required
// GpuSorting harness batch (see the note on "GPU Bitonic Sorting" above). The
// exact expected_keys / expected_values CHECKs below discriminate a wrong sort.
TEST_CASE("[GaussianSplatting][GpuSort][RequiresGPU] Radix sort factory honors 32-bit key layout") {
	struct GPUSortingConfigRestore {
		GPUSortingConfig previous_config;
		~GPUSortingConfigRestore() {
			g_gpu_sorting_config = previous_config;
		}
	} restore = { g_gpu_sorting_config };

	g_gpu_sorting_config.reset_to_defaults();
	g_gpu_sorting_config.radix_bits = 4;
	g_gpu_sorting_config.workgroup_size = GPUSortingConstants::DEFAULT_WORKGROUP_SIZE;

	RenderingDevice *rd = RenderingDevice::get_singleton();
	bool owns_local_device = false;
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
			owns_local_device = rd != nullptr;
		}
	}

	if (!rd) {
		MESSAGE("Skipping 32-bit radix sort factory test - no RenderingDevice available");
		return;
	}

	SortKeyConfig key_config;
	key_config.key_bits = 32;
	key_config.tile_bits = 16;
	key_config.depth_bits = 16;
	key_config.enable_tie_breaker = false;

	Ref<IGPUSorter> sorter = GPUSorterFactory::create_sorter(
			GPUSorterFactory::ALGORITHM_RADIX,
			rd,
			16,
			key_config);

	CHECK(sorter.is_valid());
	if (!sorter.is_valid()) {
		if (owns_local_device) {
			memdelete(rd);
		}
		return;
	}

	const uint32_t count = 5;
	LocalVector<uint32_t> keys;
	LocalVector<uint32_t> values;
	LocalVector<uint32_t> expected_keys;
	LocalVector<uint32_t> expected_values;
	keys.push_back(0x00030020u);
	keys.push_back(0x00010010u);
	keys.push_back(0x00020030u);
	keys.push_back(0x00000001u);
	keys.push_back(0x00010005u);
	values.push_back(0u);
	values.push_back(1u);
	values.push_back(2u);
	values.push_back(3u);
	values.push_back(4u);
	expected_keys.push_back(0x00000001u);
	expected_keys.push_back(0x00010005u);
	expected_keys.push_back(0x00010010u);
	expected_keys.push_back(0x00020030u);
	expected_keys.push_back(0x00030020u);
	expected_values.push_back(3u);
	expected_values.push_back(4u);
	expected_values.push_back(1u);
	expected_values.push_back(2u);
	expected_values.push_back(0u);

	RID keys_buffer = create_storage_buffer(rd, keys);
	rd->set_resource_name(keys_buffer, "GS_Test_Radix32_Keys");
	RID values_buffer = create_storage_buffer(rd, values);
	rd->set_resource_name(values_buffer, "GS_Test_Radix32_Values");

	Error err = sorter->sort(keys_buffer, values_buffer, count);
	CHECK(err == OK);

	if (err == OK) {
		Vector<uint8_t> keys_result = rd->buffer_get_data(keys_buffer, 0, count * sizeof(uint32_t));
		Vector<uint8_t> values_result = rd->buffer_get_data(values_buffer, 0, count * sizeof(uint32_t));
		const uint32_t *sorted_keys = (const uint32_t *)keys_result.ptr();
		const uint32_t *sorted_values = (const uint32_t *)values_result.ptr();

		for (uint32_t i = 0; i < count; i++) {
			CHECK(sorted_keys[i] == expected_keys[i]);
			CHECK(sorted_values[i] == expected_values[i]);
		}
	}

	SortingMetrics metrics = sorter->get_metrics();
	CHECK(metrics.last_key_bits == 32u);
	CHECK(metrics.last_radix_bits == 4u);
	CHECK(metrics.last_pass_count == 8u);
	CHECK(metrics.last_element_count == count);
	CHECK(metrics.last_element_count_known);
	CHECK_FALSE(metrics.last_sort_indirect);
	CHECK(metrics.last_sort_async);

	rd->free(keys_buffer);
	rd->free(values_buffer);
	sorter->shutdown();
	sorter.unref();
	if (owns_local_device) {
		memdelete(rd);
	}
}

TEST_CASE("[GaussianSplatting][RequiresGPU] GPU Sorting Performance") {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
		}
	}

	if (!rd) {
		MESSAGE("Skipping GPU sorting performance tests - no RenderingDevice available");
		return;
	}

	SUBCASE("Bitonic sort performance scaling") {
		const uint32_t test_sizes[] = {1024, 4096, 16384, 65536};

		for (uint32_t size : test_sizes) {
			Ref<BitonicSort> sorter;
			sorter.instantiate();

			Error err = sorter->initialize(rd, size);
			CHECK(err == OK);
			if (err != OK) {
				continue;
			}

			// Create random test data
			LocalVector<float> depths;
			LocalVector<uint32_t> indices;
			depths.resize(size);
			indices.resize(size);

			RandomNumberGenerator rng;
			rng.set_seed(42);

			for (uint32_t i = 0; i < size; i++) {
				depths[i] = rng.randf_range(0.0f, 1000.0f);
				indices[i] = i;
			}

			// Create GPU buffers
			RID keys_buffer = create_storage_buffer(rd, depths);
			RID values_buffer = create_storage_buffer(rd, indices);

			// Measure sort time
			uint64_t start = OS::get_singleton()->get_ticks_usec();

			err = sorter->sort(keys_buffer, values_buffer, size);
			CHECK(err == OK);

			uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
			float ms = elapsed / 1000.0f;

			// Performance expectations (conservative for CI)
			if (size <= 4096) {
				CHECK_MESSAGE(ms < 50.0f,
					vformat("Sorting %d elements took %.2fms, expected < 50ms", size, ms));
			} else if (size <= 16384) {
				CHECK_MESSAGE(ms < 100.0f,
					vformat("Sorting %d elements took %.2fms, expected < 100ms", size, ms));
			} else if (size <= 65536) {
				CHECK_MESSAGE(ms < 200.0f,
					vformat("Sorting %d elements took %.2fms, expected < 200ms", size, ms));
			}

			// Clean up
			rd->free(keys_buffer);
			rd->free(values_buffer);
		}
	}

	SUBCASE("Compare with CPU sorting") {
		const uint32_t count = 10000;

		// Generate test data
		LocalVector<float> depths;
		LocalVector<uint32_t> indices;
		depths.resize(count);
		indices.resize(count);

		RandomNumberGenerator rng;
		rng.set_seed(42);

		for (uint32_t i = 0; i < count; i++) {
			depths[i] = rng.randf_range(0.0f, 1000.0f);
			indices[i] = i;
		}

		// CPU sort timing
		LocalVector<float> cpu_depths = depths;
		LocalVector<uint32_t> cpu_indices = indices;

		uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();

		// Sort indices by depth
		std::sort(cpu_indices.ptr(), cpu_indices.ptr() + count,
			[&cpu_depths](uint32_t a, uint32_t b) {
				return cpu_depths[a] < cpu_depths[b];
			});

		uint64_t cpu_elapsed = OS::get_singleton()->get_ticks_usec() - cpu_start;
		float cpu_ms = cpu_elapsed / 1000.0f;

		// GPU sort timing
		Ref<BitonicSort> sorter;
		sorter.instantiate();
		Error err = sorter->initialize(rd, count);
		CHECK(err == OK);
		if (err != OK) {
			return;
		}

		RID keys_buffer = create_storage_buffer(rd, depths);
		RID values_buffer = create_storage_buffer(rd, indices);

		uint64_t gpu_start = OS::get_singleton()->get_ticks_usec();

		err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);

		uint64_t gpu_elapsed = OS::get_singleton()->get_ticks_usec() - gpu_start;
		float gpu_ms = gpu_elapsed / 1000.0f;

		// GPU should be reasonable compared to CPU (not always faster due to transfer overhead)
		CHECK_MESSAGE(gpu_ms < cpu_ms * 10.0f,
			vformat("GPU sort (%.2fms) should be within 10x of CPU sort (%.2fms)", gpu_ms, cpu_ms));

		// Clean up
		rd->free(keys_buffer);
		rd->free(values_buffer);
	}
}

// Regression guard for the 8-bit radix path: with 256 histogram/scatter bins, the per-bin shader
// loops must be STRIDED over WORKGROUP_SIZE. A naive `tid < RADIX_SIZE` guard only covers the first
// WORKGROUP_SIZE bins, silently corrupting the sort at workgroup_size 64/128. This test sorts keys
// whose bytes span HIGH digit values (>= 128) so those high bins are exercised, and checks the GPU
// output against a CPU reference at every allowed workgroup_size.
// #622: [GpuSort] tag opts this 8-bit radix sort-order oracle into the required
// GpuSorting harness batch (see the note on "GPU Bitonic Sorting" above). It
// checks the GPU output against a CPU-sorted reference at every workgroup size,
// so a wrong sort at any bin count turns the required batch RED.
TEST_CASE("[GaussianSplatting][GpuSort][RequiresGPU] Radix sort 8-bit is correct at all workgroup sizes") {
	struct GPUSortingConfigRestore {
		GPUSortingConfig previous_config;
		~GPUSortingConfigRestore() { g_gpu_sorting_config = previous_config; }
	} restore = { g_gpu_sorting_config };

	RenderingDevice *rd = RenderingDevice::get_singleton();
	bool owns_local_device = false;
	if (!rd) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs) {
			rd = rs->create_local_rendering_device();
			owns_local_device = rd != nullptr;
		}
	}
	if (!rd) {
		MESSAGE("Skipping 8-bit radix sort test - no RenderingDevice available");
		return;
	}

	LocalVector<uint32_t> base_keys;
	base_keys.push_back(0xFF000001u);
	base_keys.push_back(0x80C04020u);
	base_keys.push_back(0x000000FFu);
	base_keys.push_back(0x01FF0080u);
	base_keys.push_back(0xC0C0C0C0u);
	base_keys.push_back(0x00018000u);
	base_keys.push_back(0xFFFFFFFFu);
	base_keys.push_back(0x00000000u);
	base_keys.push_back(0x7F80FE01u);
	base_keys.push_back(0x40404040u);
	const uint32_t count = base_keys.size();

	LocalVector<uint32_t> expected_keys = base_keys;
	expected_keys.sort(); // ascending reference order

	const uint32_t workgroup_sizes[] = { 64u, 128u, 256u, 512u };
	uint32_t verified = 0u;
	for (uint32_t wg : workgroup_sizes) {
		g_gpu_sorting_config.reset_to_defaults();
		g_gpu_sorting_config.radix_bits = 8;
		g_gpu_sorting_config.workgroup_size = wg;

		SortKeyConfig key_config;
		key_config.key_bits = 32;
		key_config.tile_bits = 16;
		key_config.depth_bits = 16;
		key_config.enable_tie_breaker = false;

		Ref<IGPUSorter> sorter = GPUSorterFactory::create_sorter(
				GPUSorterFactory::ALGORITHM_RADIX, rd, count, key_config);
		if (!sorter.is_valid()) {
			MESSAGE(vformat("Skipping 8-bit radix at workgroup_size=%d (unsupported on this GPU)", wg));
			continue;
		}

		LocalVector<uint32_t> keys = base_keys;
		LocalVector<uint32_t> values;
		values.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			values[i] = i;
		}

		RID keys_buffer = create_storage_buffer(rd, keys);
		RID values_buffer = create_storage_buffer(rd, values);

		Error err = sorter->sort(keys_buffer, values_buffer, count);
		CHECK(err == OK);
		if (err == OK) {
			Vector<uint8_t> keys_result = rd->buffer_get_data(keys_buffer, 0, count * sizeof(uint32_t));
			Vector<uint8_t> values_result = rd->buffer_get_data(values_buffer, 0, count * sizeof(uint32_t));
			const uint32_t *sorted_keys = (const uint32_t *)keys_result.ptr();
			const uint32_t *sorted_values = (const uint32_t *)values_result.ptr();
			for (uint32_t i = 0; i < count; i++) {
				// (1) ascending order matches the CPU reference; (2) the carried value still points
				// back to the original key (proves the scatter moved key+value together).
				CHECK_MESSAGE(sorted_keys[i] == expected_keys[i],
						vformat("8-bit radix wg=%d pos %d: got 0x%08X expected 0x%08X", wg, i, sorted_keys[i], expected_keys[i]));
				// Compute the boolean separately: doctest cannot decompose a compound `a && b` inside CHECK.
				const bool value_maps_back = sorted_values[i] < count && base_keys[sorted_values[i]] == sorted_keys[i];
				CHECK_MESSAGE(value_maps_back,
						vformat("8-bit radix wg=%d pos %d: value %d does not map back to key 0x%08X", wg, i, sorted_values[i], sorted_keys[i]));
			}
			verified++;
		}

		SortingMetrics metrics = sorter->get_metrics();
		CHECK(metrics.last_radix_bits == 8u);
		CHECK(metrics.last_pass_count == 4u); // 32-bit key / 8-bit radix = 4 passes

		rd->free(keys_buffer);
		rd->free(values_buffer);
		sorter->shutdown();
		sorter.unref();
	}

	// If every workgroup size was rejected by the runtime probes (descriptor / shared-
	// memory / workgroup limits), an invalid sorter is the SUPPORTED production fallback —
	// the sort path keeps the 4-bit default — so skip rather than fail the RequiresGPU
	// suite on a low-capability GPU. When at least one variant was supported the per-
	// iteration CHECKs above already validated correctness.
	if (verified == 0u) {
		MESSAGE("Skipping 8-bit radix verification: no workgroup size is supported on this GPU");
	}

	if (owns_local_device) {
		memdelete(rd);
	}
}

// ---------------------------------------------------------------------------
// C4b (G4), Channel B: instance-count overflow_flag stickiness (PR #508 review).
//
// The async instance-count readback is gated on !pending, so a clamp on a frame whose readback
// is skipped would be lost if instance_count.overflow_flag were overwritten per-frame: a later
// sample reads 0 and the clamp is silent. instance_count_clamp.glsl now ACCUMULATES the flag
// (atomicMax) until the CPU consumes it (consume_overflow_flag). This test drives the REAL
// shader over the N / N+1 / N+2 sequence from the review and asserts the N+1 overflow is still
// visible at N+2 -- pre-fix this readback returned 0 (missed); post-fix it returns 1 (caught).
// ---------------------------------------------------------------------------
static inline RID gs_compile_instance_count_clamp_shader(RenderingDevice *p_rd) {
	InstanceCountClampShaderRD clamp_shader_source;
	Vector<String> versions;
	versions.push_back("");
	clamp_shader_source.initialize(versions);

	RID version = clamp_shader_source.version_create();
	if (!version.is_valid()) {
		return RID();
	}
	RS::ShaderNativeSourceCode native = clamp_shader_source.version_get_native_source_code(version);
	clamp_shader_source.version_free(version);
	if (native.versions.is_empty() || native.versions[0].stages.is_empty()) {
		return RID();
	}
	String compute_source;
	for (int i = 0; i < native.versions[0].stages.size(); i++) {
		if (native.versions[0].stages[i].name == "compute") {
			compute_source = native.versions[0].stages[i].code;
			break;
		}
	}
	if (compute_source.is_empty()) {
		return RID();
	}
	RID shader;
	GaussianSplatting::ComputeInfrastructure::StageResult result =
			GaussianSplatting::ComputeInfrastructure::compile_compute_shader_from_source(
					p_rd, "Test.InstanceCountClamp", compute_source, "TestInstanceCountClamp", shader);
	if (!result.ok()) {
		return RID();
	}
	return shader;
}

TEST_CASE("[GaussianSplatting][GPUSortPipeline][RequiresGPU] Instance-count overflow_flag is sticky across a skipped async readback (C4b/G4 miss-fix)") {
	// Prefer the harness/global device (the --gs-gpu-test runner initializes one and exposes it
	// via RenderingDevice::get_singleton() with no RenderingServer); fall back to a local device.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	bool owns_local_device = false;
	if (rd == nullptr) {
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			rd = rs->create_local_rendering_device();
			owns_local_device = true;
		}
	}
	if (rd == nullptr) {
		MESSAGE("Skipping C4b sticky overflow test - no RenderingDevice available");
		return;
	}

	RID shader = gs_compile_instance_count_clamp_shader(rd);
	if (!shader.is_valid()) {
		if (owns_local_device) {
			memdelete(rd);
		}
		FAIL("Failed to compile instance_count_clamp shader");
		return;
	}
	RID pipeline = rd->compute_pipeline_create(shader);
	if (!pipeline.is_valid()) {
		rd->free(shader);
		if (owns_local_device) {
			memdelete(rd);
		}
		FAIL("Failed to create instance_count_clamp pipeline");
		return;
	}

	// binding 0: CounterBuffer { visible_splat_count, overflowed_splats }
	Vector<uint8_t> counter_init;
	counter_init.resize(2 * sizeof(uint32_t));
	counter_init.fill(0);
	RID counter_buffer = rd->storage_buffer_create(counter_init.size(), counter_init);

	// bindings 1 (indirect) and 3 (instance_count): IndirectDispatchLayout, zero-initialized to
	// mirror the production zero-init (so the sticky accumulate never reads uninitialized memory).
	Vector<uint8_t> layout_init;
	layout_init.resize(sizeof(GaussianSplatting::IndirectDispatchLayout));
	layout_init.fill(0);
	RID indirect_buffer = rd->storage_buffer_create(layout_init.size(), layout_init);
	RID instance_count_buffer = rd->storage_buffer_create(layout_init.size(), layout_init);

	// binding 2: Params UBO (full InstanceDepthParamsGPU, as bound in production).
	Vector<uint8_t> params_init;
	params_init.resize(sizeof(InstanceDepthParamsGPU));
	params_init.fill(0);
	RID params_buffer = rd->uniform_buffer_create(params_init.size(), params_init);

	Vector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(counter_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(indirect_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 2;
		u.append_id(params_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 3;
		u.append_id(instance_count_buffer);
		uniforms.push_back(u);
	}
	RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	if (!uniform_set.is_valid()) {
		rd->free(params_buffer);
		rd->free(instance_count_buffer);
		rd->free(indirect_buffer);
		rd->free(counter_buffer);
		rd->free(pipeline);
		rd->free(shader);
		if (owns_local_device) {
			memdelete(rd);
		}
		FAIL("Failed to create instance_count_clamp uniform set");
		return;
	}

	const uint32_t kMaxVisible = 64u;

	// Runs one clamp "frame": sets the visible count and consume signal, dispatches the real
	// shader, and reads back the sticky instance_count.overflow_flag.
	auto run_clamp_frame = [&](uint32_t p_raw_count, uint32_t p_consume) -> uint32_t {
		uint32_t counter_vals[2] = { p_raw_count, 0u };
		rd->buffer_update(counter_buffer, 0, sizeof(counter_vals), counter_vals);
		rd->buffer_update(params_buffer,
				(uint32_t)offsetof(InstanceDepthParamsGPU, max_visible_splats),
				sizeof(uint32_t), &kMaxVisible);
		rd->buffer_update(params_buffer,
				(uint32_t)offsetof(InstanceDepthParamsGPU, consume_overflow_flag),
				sizeof(uint32_t), &p_consume);

		int64_t compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_dispatch(compute_list, 1, 1, 1);
		rd->compute_list_end();
		// buffer_get_data() below calls _flush_and_stall_for_all_frames(), which submits and
		// waits for the recorded buffer_update + dispatch, so no explicit submit()/sync() is
		// needed (and none is safe on the harness's primary device).
		Vector<uint8_t> data = rd->buffer_get_data(instance_count_buffer,
				(uint32_t)offsetof(GaussianSplatting::IndirectDispatchLayout, overflow_flag),
				sizeof(uint32_t));
		uint32_t flag = 0u;
		if (data.size() >= (int)sizeof(uint32_t)) {
			std::memcpy(&flag, data.ptr(), sizeof(uint32_t));
		}
		return flag;
	};

	// Frame N: an async readback is ENQUEUED this frame (so the next frame's clamp gets
	// consume=1). No overflow (raw 32 <= max 64). The sticky flag reads 0.
	uint32_t flag_N = run_clamp_frame(32u, 1u);
	CHECK(flag_N == 0u);

	// Frame N+1: the ONLY frame that clamps (raw 4096 > max 64). consume=1 because frame N was
	// enqueued. The readback for this frame is SKIPPED (a prior readback is still pending), so
	// nothing consumes the flag afterwards. The clamp raises the flag to 1.
	uint32_t flag_Np1 = run_clamp_frame(4096u, 1u);
	CHECK(flag_Np1 == 1u);

	// Frame N+2: no overflow (raw 32 <= max 64). consume=0 because frame N+1 was NOT enqueued,
	// so the shader ACCUMULATES rather than resets. The N+2 async readback samples the buffer:
	//   * post-fix (sticky atomicMax): the flag is STILL 1 -> the N+1 clamp is CAUGHT.
	//   * pre-fix (per-frame overwrite): the flag would be 0 -> the N+1 clamp is MISSED.
	uint32_t flag_Np2 = run_clamp_frame(32u, 0u);
	CHECK(flag_Np2 == 1u); // discriminating assertion: 1 post-fix, 0 pre-fix.

	// End-to-end: feed the N+2 sample into the production CPU readback path and confirm it bumps
	// the persistent overflow-event counter (0 -> 1). Pre-fix the payload carries 0 and the
	// counter stays 0 (the silent miss the review flagged).
	Ref<GPUSortingPipeline> cpu_pipeline;
	cpu_pipeline.instantiate();
	if (cpu_pipeline.is_valid()) {
		CHECK(cpu_pipeline->get_instance_count_overflow_events() == 0u);
		auto &count_state = cpu_pipeline->_test_instance_count_readback_state();
		count_state.pending = true;
		count_state.generation = 3;
		count_state.pending_frame_counter = 202;
		cpu_pipeline->_test_set_last_instance_visible_splat_count_state(0, false, 0);
		Vector<uint8_t> full_sample = rd->buffer_get_data(instance_count_buffer, 0,
				sizeof(GaussianSplatting::IndirectDispatchLayout));
		cpu_pipeline->_on_instance_count_readback(full_sample, 3);
		CHECK(cpu_pipeline->get_instance_count_overflow_events() == 1u);
	}

	rd->free(uniform_set);
	rd->free(params_buffer);
	rd->free(instance_count_buffer);
	rd->free(indirect_buffer);
	rd->free(counter_buffer);
	rd->free(pipeline);
	rd->free(shader);
	if (owns_local_device) {
		memdelete(rd);
	}
}

} // namespace TestGaussianSplatting
