#include "test_macros.h"

#include "../interfaces/tile_rasterizer.h"
#include "../renderer/tile_render_resources.h"
#include "../renderer/tile_prefix_scan_utils.h"
#include "../renderer/tile_renderer.h"
#include "servers/rendering_server.h"

// #641: both cases below need a real local RenderingDevice to create pipelines.
// They previously hard-failed `CHECK(rs != nullptr)` under plain `--test`, and
// were untagged, so no CI lane could ever execute them. `[RequiresGPU]` routes
// them to the `TileRenderer` batch of tests/ci/run_gpu_harness.py (filter
// `*TileRenderer*][RequiresGPU]*`, previously catalogued-but-empty), and
// REQUIRE_LOCAL_GPU_DEVICE() degrades to an explicit skip on headless lanes.
TEST_CASE("[TileRenderer][RequiresGPU] Shader compilation on local device") {
    REQUIRE_LOCAL_GPU_DEVICE();

    Ref<TileRenderer> renderer;
    renderer.instantiate();
    Error err = renderer->initialize(local_device, Vector2i(1920, 1080), TileRenderer::DEFAULT_TILE_SIZE);
    CHECK_MESSAGE(err == OK, "TileRenderer initialization should succeed on a local RenderingDevice");
    CHECK(renderer->is_initialized());
    CHECK(renderer->get_tile_binning_pipeline().is_valid());
    CHECK(renderer->get_tile_raster_pipeline().is_valid());

    renderer->cleanup();
}

TEST_CASE("[TileRenderer][RequiresGPU] Output format coercion keeps deterministic defaults") {
    REQUIRE_LOCAL_GPU_DEVICE();

    Ref<TileRenderer> renderer;
    renderer.instantiate();
    Error err = renderer->initialize(local_device, Vector2i(512, 320), TileRenderer::DEFAULT_TILE_SIZE,
            RD::DATA_FORMAT_R8G8B8A8_SRGB);
    CHECK_MESSAGE(err == OK, "TileRenderer initialization should succeed with explicit SRGB output format");
    if (err != OK) {
        return;
    }

    // #643 CONTRACT (resolved): an explicitly-requested sRGB output format is PRESERVED,
    // not coerced. sRGB cannot back a compute STORAGE image, so create_output_textures()
    // omits TEXTURE_USAGE_STORAGE_BIT for it and the renderer draws via the fragment path
    // (_decide_compute_raster already routes sRGB there). initialize() therefore succeeds
    // with a real output texture and get_output_format() reports the requested sRGB.
    // Regression guard: if STORAGE_BIT were re-added unconditionally, texture_create()
    // would fail for sRGB, create_output_textures() would set output_format = DATA_FORMAT_MAX,
    // and this assertion would fail (232 == 42, the historical failure). (The fail-closed
    // half of #643 -- _ensure_resources returning an error + releasing on genuine allocation
    // failure -- is exercised by mutation, not reachable with a valid device here.)
    CHECK(renderer->get_output_format() == RD::DATA_FORMAT_R8G8B8A8_SRGB);

    renderer->set_output_format(RD::DATA_FORMAT_MAX);
    CHECK(renderer->get_output_format() == RD::DATA_FORMAT_R8G8B8A8_UNORM);

    err = renderer->resize(Vector2i(256, 160), RD::DATA_FORMAT_MAX);
    CHECK_MESSAGE(err == OK, "TileRenderer resize should accept DATA_FORMAT_MAX and preserve fallback format");
    CHECK(renderer->get_output_format() == RD::DATA_FORMAT_R8G8B8A8_UNORM);

    renderer->cleanup();
}

TEST_CASE("[TileRenderer] Prefix emergency fallback only triggers at device-dispatch limits") {
    const uint32_t total_workgroups = 8193u;
    const GaussianSplatting::TilePrefixDispatchCounts dispatch_counts =
            GaussianSplatting::tile_prefix_compute_dispatch_counts(total_workgroups);

    CHECK(dispatch_counts.pass2_dispatch_x > 1u);
    CHECK(!GaussianSplatting::tile_prefix_any_pass_requires_cpu_fallback(total_workgroups, total_workgroups));
    CHECK(GaussianSplatting::tile_prefix_any_pass_requires_cpu_fallback(total_workgroups, total_workgroups - 1u));
    CHECK(GaussianSplatting::tile_prefix_any_pass_requires_cpu_fallback(total_workgroups, dispatch_counts.pass2_dispatch_x - 1u));
}

TEST_CASE("[TileRenderer] Compute raster shared-memory contract matches formula derivation") {
    const uint64_t required_bytes = TileRasterizer::get_compute_raster_shared_memory_requirement_bytes();
    const uint64_t expected_bytes = uint64_t(TileRenderer::MAX_SPLATS_PER_TILE) * (sizeof(uint32_t) + 9u * sizeof(uint32_t)) +
            5u * sizeof(uint32_t);

    CHECK(required_bytes == expected_bytes);
    CHECK(required_bytes == 40980u);
}

// NOTE: the pure resize-policy unit tests (SH cache + projection buffer) live in
// test_tile_buffer_resize.h so they are aggregated by test_gaussian_splatting.cpp
// and actually registered — standalone tests/*.cpp doctest cases in this module are
// dropped by static-lib dead-stripping at link time and never run.

TEST_CASE("[TileRenderer] Compute raster shared-memory requirement equals expected absolute byte count") {
    const uint64_t required_bytes = TileRasterizer::get_compute_raster_shared_memory_requirement_bytes();
    const uint64_t expected_bytes = uint64_t(TileRenderer::MAX_SPLATS_PER_TILE) * (sizeof(uint32_t) + 9u * sizeof(uint32_t)) +
            5u * sizeof(uint32_t);

    CHECK(required_bytes == expected_bytes);
    CHECK(required_bytes == 40980u);
}

// Force-link anchor (#178): a doctest TEST_CASE registers via a file-scope static
// initializer; MSVC drops this whole object from the module static library when
// nothing references it, silently discarding the cases. test_gaussian_splatting.h
// calls this symbol so the linker keeps the object and the cases actually run.
extern "C" int test_tile_renderer_cpp_force_link() {
    return 0;
}
