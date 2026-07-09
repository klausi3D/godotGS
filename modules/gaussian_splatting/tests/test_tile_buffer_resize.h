/**************************************************************************/
/*  test_tile_buffer_resize.h                                             */
/*  Unit tests for the shared only-grow GPU scratch-buffer resize policy. */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_TILE_BUFFER_RESIZE_H
#define GAUSSIAN_SPLATTING_TEST_TILE_BUFFER_RESIZE_H

#include "test_macros.h"

#include "../renderer/tile_render_resources.h"

// NOTE: these exercise the pure, deterministic resize-policy decision in
// tile_compute_buffer_resize_plan (and its SH-cache wrapper). They live in this
// aggregated header rather than test_tile_renderer.cpp because only the headers
// included by test_gaussian_splatting.cpp survive static-lib dead-stripping at
// link time; standalone tests/*.cpp doctest cases are not registered.

namespace TestTileBufferResize {

inline GaussianSplatting::TileBufferShrinkPolicy projection_policy() {
	GaussianSplatting::TileBufferShrinkPolicy policy;
	policy.growth_slack_percent = 0u;
	policy.min_growth_slack_bytes = 0u;
	policy.shrink_trigger_percent = GaussianSplatting::TILE_SCRATCH_SHRINK_TRIGGER_PERCENT;
	policy.shrink_hysteresis_frames = GaussianSplatting::TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES;
	return policy;
}

} // namespace TestTileBufferResize

// --- SH color cache policy (default-on shrink; small buffer) -----------------

TEST_CASE("[GaussianSplatting][TileBufferResize] SH cache resize plan adds growth slack") {
	const uint32_t required_bytes = 16u * 1024u;
	const uint32_t current_bytes = 8u * 1024u;

	const GaussianSplatting::TileSHCacheResizePlan plan =
			GaussianSplatting::tile_compute_sh_cache_resize_plan(required_bytes, current_bytes, 0u);

	CHECK(plan.should_resize);
	CHECK(plan.target_bytes > required_bytes);
	CHECK(plan.target_bytes >= required_bytes + GaussianSplatting::TILE_SH_CACHE_MIN_GROWTH_SLACK_BYTES);
	CHECK(plan.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] SH cache shrink hysteresis defers resize until threshold") {
	const uint32_t required_bytes = 1024u;
	const uint32_t current_bytes = 16u * 1024u;

	const GaussianSplatting::TileSHCacheResizePlan pending =
			GaussianSplatting::tile_compute_sh_cache_resize_plan(required_bytes, current_bytes, 0u);
	CHECK(!pending.should_resize);
	CHECK(pending.next_shrink_candidate_frames == 1u);

	const GaussianSplatting::TileSHCacheResizePlan trigger =
			GaussianSplatting::tile_compute_sh_cache_resize_plan(required_bytes, current_bytes,
					GaussianSplatting::TILE_SH_CACHE_SHRINK_HYSTERESIS_FRAMES - 1u);
	CHECK(trigger.should_resize);
	CHECK(trigger.target_bytes > required_bytes);
	CHECK(trigger.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] SH cache shrink hysteresis resets when usage recovers") {
	const uint32_t required_bytes = 9u * 1024u;
	const uint32_t current_bytes = 16u * 1024u;

	const GaussianSplatting::TileSHCacheResizePlan plan =
			GaussianSplatting::tile_compute_sh_cache_resize_plan(required_bytes, current_bytes, 57u);

	CHECK(!plan.should_resize);
	CHECK(plan.next_shrink_candidate_frames == 0u);
}

// --- Global projection-buffer policy (opt-in shrink; no growth slack) --------

TEST_CASE("[GaussianSplatting][TileBufferResize] Buffer resize plan grows immediately with no projection slack") {
	const GaussianSplatting::TileBufferResizePlan plan =
			GaussianSplatting::tile_compute_buffer_resize_plan(16u * 1024u, 8u * 1024u, 0u,
					TestTileBufferResize::projection_policy());

	CHECK(plan.should_resize);
	// Projection policy carries no growth slack: the power-of-two element rounding
	// upstream already provides headroom, so target == demand exactly.
	CHECK(plan.target_bytes == 16u * 1024u);
	CHECK(plan.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] Buffer resize plan holds capacity inside the keep band") {
	// Demand at 60% of capacity is above the 50% shrink trigger: no resize, streak stays 0.
	const GaussianSplatting::TileBufferResizePlan plan =
			GaussianSplatting::tile_compute_buffer_resize_plan(6u * 1024u, 10u * 1024u, 0u,
					TestTileBufferResize::projection_policy());

	CHECK(!plan.should_resize);
	CHECK(plan.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] Buffer resize plan defers projection shrink until hysteresis elapses") {
	const uint32_t required_bytes = 1u * 1024u; // well under half of capacity
	const uint32_t current_bytes = 16u * 1024u;

	const GaussianSplatting::TileBufferResizePlan pending =
			GaussianSplatting::tile_compute_buffer_resize_plan(required_bytes, current_bytes, 0u,
					TestTileBufferResize::projection_policy());
	CHECK(!pending.should_resize);
	CHECK(pending.next_shrink_candidate_frames == 1u);

	const GaussianSplatting::TileBufferResizePlan trigger =
			GaussianSplatting::tile_compute_buffer_resize_plan(required_bytes, current_bytes,
					GaussianSplatting::TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES - 1u,
					TestTileBufferResize::projection_policy());
	CHECK(trigger.should_resize);
	CHECK(trigger.target_bytes == required_bytes); // no slack on shrink either
	CHECK(trigger.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] Buffer resize plan shrink trigger includes the exact half boundary") {
	const uint32_t current_bytes = 16u * 1024u;

	// Exactly 50% of capacity is the boundary: the `<=` trigger must treat it as
	// "low" and begin the shrink streak.
	const GaussianSplatting::TileBufferResizePlan at_boundary =
			GaussianSplatting::tile_compute_buffer_resize_plan(8u * 1024u, current_bytes, 0u,
					TestTileBufferResize::projection_policy());
	CHECK(!at_boundary.should_resize);
	CHECK(at_boundary.next_shrink_candidate_frames == 1u);

	// One byte above half stays in the keep band (no streak).
	const GaussianSplatting::TileBufferResizePlan above_boundary =
			GaussianSplatting::tile_compute_buffer_resize_plan(8u * 1024u + 1u, current_bytes, 0u,
					TestTileBufferResize::projection_policy());
	CHECK(!above_boundary.should_resize);
	CHECK(above_boundary.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] Buffer resize plan frees the buffer on sustained zero demand") {
	const uint32_t current_bytes = 16u * 1024u;

	// Zero demand accumulates the streak like any other low frame...
	const GaussianSplatting::TileSHCacheResizePlan pending =
			GaussianSplatting::tile_compute_sh_cache_resize_plan(0u, current_bytes, 0u);
	CHECK(!pending.should_resize);
	CHECK(pending.next_shrink_candidate_frames == 1u);

	// ...and once the hysteresis elapses it releases the buffer entirely (target 0).
	const GaussianSplatting::TileSHCacheResizePlan trigger =
			GaussianSplatting::tile_compute_sh_cache_resize_plan(0u, current_bytes,
					GaussianSplatting::TILE_SH_CACHE_SHRINK_HYSTERESIS_FRAMES - 1u);
	CHECK(trigger.should_resize);
	CHECK(trigger.target_bytes == 0u);
	CHECK(trigger.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] Buffer resize plan resets projection shrink streak on demand recovery") {
	// A frame whose demand climbs back into the keep band must reset the streak so a
	// later dip restarts the full hysteresis window (no premature realloc).
	const GaussianSplatting::TileBufferResizePlan plan =
			GaussianSplatting::tile_compute_buffer_resize_plan(9u * 1024u, 16u * 1024u, 120u,
					TestTileBufferResize::projection_policy());

	CHECK(!plan.should_resize);
	CHECK(plan.next_shrink_candidate_frames == 0u);
}

TEST_CASE("[GaussianSplatting][TileBufferResize] Shrink-then-grow never under-sizes below demand (GS-PERF-S2)") {
	// Models the adaptive-sizing default-ON sequence: capacity shrinks after sustained
	// low demand, then a demand SPIKE must be met without ever leaving capacity below
	// demand (which is what would clamp records / drop bottom-of-screen tiles). The
	// shrink decision is the hysteretic resize plan; the spike-grow is the caller's
	// immediate `capacity < demand` reallocation, modeled here on the plan's contract.
	const GaussianSplatting::TileBufferShrinkPolicy policy = TestTileBufferResize::projection_policy();

	// 1) Sustained low demand (1/8 of capacity) accumulates the shrink streak and, once
	//    hysteresis elapses, targets EXACTLY the demand — no slack, but also never below.
	const uint32_t capacity_bytes = 32u * 1024u;
	const uint32_t low_demand_bytes = 4u * 1024u;
	const GaussianSplatting::TileBufferResizePlan shrink =
			GaussianSplatting::tile_compute_buffer_resize_plan(low_demand_bytes, capacity_bytes,
					GaussianSplatting::TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES - 1u, policy);
	CHECK(shrink.should_resize);
	CHECK(shrink.target_bytes == low_demand_bytes); // shrinks down to demand exactly
	CHECK(shrink.target_bytes >= low_demand_bytes); // never below demand -> no clamp

	// 2) After the shrink, a spike whose demand EXCEEDS the shrunk capacity must be met by
	//    a GROW that lands at or above demand — never below, which is what would clamp
	//    records / drop bottom-of-screen tiles. The grow is immediate (no hysteresis) and
	//    the shrink streak resets so a later genuine dip restarts the full window.
	const uint32_t shrunk_capacity_bytes = shrink.target_bytes;
	const uint32_t spike_demand_bytes = shrunk_capacity_bytes * 3u; // sudden dense viewpoint
	const GaussianSplatting::TileBufferResizePlan spike =
			GaussianSplatting::tile_compute_buffer_resize_plan(spike_demand_bytes, shrunk_capacity_bytes,
					GaussianSplatting::TILE_SCRATCH_SHRINK_HYSTERESIS_FRAMES - 1u, policy);
	CHECK(spike.should_resize); // grows immediately to meet the spike
	CHECK(spike.target_bytes >= spike_demand_bytes); // never under-sizes below demand -> no clamp
	CHECK(spike.next_shrink_candidate_frames == 0u); // shrink streak reset by the high frame
}

#endif // GAUSSIAN_SPLATTING_TEST_TILE_BUFFER_RESIZE_H
