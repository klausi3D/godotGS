/**************************************************************************/
/*  test_gaussian_splatting.h                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

// Main test header that includes all Gaussian Splatting tests
// This file is included by the generated modules_tests.gen.h

#include "tests/test_macros.h"

// Include all test suites
#include "test_gaussian_data.h"
#include "test_data_authority_hardening.h"
#include "test_gpu_streaming.h"
#include "test_gpu_sorting.h"
#include "test_gpu_buffer_manager_lifetime.h"
#include "test_asset_dependency_manager.h"
#include "test_batched_async_readback.h"
#include "test_render_device_manager_ownership.h"
#include "test_compute_infrastructure.h"
#include "test_phase1_integration.h"
#include "test_painterly_pipeline.h"
#include "test_gpu_sorting_pipeline_readback.h"
#include "test_overflow_auto_tuner_stale_stats.h"
#include "test_render_validation.h"
#include "test_diagnostics.h"
#include "visual_compare.h"
#include "test_output_compositor_composite_hazard.h"
#include "test_logger_rate_limit.h"
#include "test_vram_budget_regulator.h"
#include "test_resident_atlas_budget.h"
#include "test_renderer_pipeline.h"
#include "test_quantized_packing.h"
#include "test_tile_lighting_abi.h"
#include "test_tile_buffer_resize.h"
#include "test_tile_descriptor_cache.h"
#include "test_sort_fallback_policy.h"
#include "test_sort_benchmark_metrics.h"
#include "test_sorter_metrics_metadata.h"
#include "test_gaussian_splat_world_io.h"
#include "test_view_transform.h"
#include "test_memory_leak_detection.h"
#include "test_renderer_lifetime_proof.h"
#include "test_synthetic_splat_generators.h"
#include "test_synthetic_uniform_generator.h"
#include "test_synthetic_clustered_generator.h"
#include "test_synthetic_surface_generator.h"
#include "test_synthetic_cloud_generator.h"
#include "test_synthetic_mandelbrot_generator.h"
#include "test_synthetic_bml_traffic_generator.h"
#include "test_gaussian_splat_node.h"
#include "test_debug_hud_lifecycle.h"
#include "test_node_bootstrap.h"
#include "test_node_surface_cleanup.h"
#include "test_shadow_pass_isolation.h"
#include "test_shadow_instance_subset.h"
#include "test_scene_director_submission_scaffolding.h"
#include "test_scene_director_asset_id_collision.h"
#include "test_sentinel_tier_defaults.h"
#include "generate_synthetic_ply_fixtures.h"

	extern "C" int test_gpu_streaming_cpp_force_link();
	static const volatile int test_gpu_streaming_cpp_force_link_anchor = test_gpu_streaming_cpp_force_link();
	extern "C" int test_gaussian_streaming_lifecycle_cpp_force_link();
	static const volatile int test_gaussian_streaming_lifecycle_cpp_force_link_anchor = test_gaussian_streaming_lifecycle_cpp_force_link();

namespace TestGaussianSplatting {

// Main test runner that executes all tests when called with --test
void test();

} // namespace TestGaussianSplatting
