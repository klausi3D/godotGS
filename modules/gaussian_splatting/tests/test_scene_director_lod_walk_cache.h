#pragma once

// Guards the LODConfig value-equality used by the per-instance LOD-walk
// memoization in GaussianSplatSceneDirector::update_instance_lods_for_renderer().
//
// That walk is skipped when nothing that affects LOD changed since the last walk;
// the LODConfig is one of the compared inputs, so LODConfig::operator== must react
// to EVERY data field. A field left out of the comparator would let a settings
// change be silently ignored by the early-out. This flips each field from its
// default and asserts inequality.
//
// The end-to-end "a moved instance's LOD is not frozen" behavior is validated on
// real hardware (the walk resolves through the shared renderer, which needs a
// RenderingDevice); it is not encoded as a headless doctest because it would skip
// in every device-less lane while adding a deferred [RequiresGPU] entry to the
// renderer-closure blocker set. The correctness itself is structural: the skip is
// gated on instance_generation, which bumps on every instance
// add/remove/transform/param change.

#include "test_macros.h"

#include "../lod/lod_config.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

TEST_CASE("[GaussianSplatting][LOD] LODConfig equality reacts to every field") {
	const LODConfig base;
	CHECK(base == LODConfig());
	CHECK_FALSE(base != LODConfig());

	{ LODConfig c = base; c.enabled = !c.enabled; CHECK(c != base); }
	{ LODConfig c = base; c.num_levels += 1; CHECK(c != base); }
	{ LODConfig c = base; c.max_distance += 1.0f; CHECK(c != base); }
	{ LODConfig c = base; c.base_threshold += 1.0f; CHECK(c != base); }
	{ LODConfig c = base; c.splat_skip_enabled = !c.splat_skip_enabled; CHECK(c != base); }
	{ LODConfig c = base; c.sh_reduction_enabled = !c.sh_reduction_enabled; CHECK(c != base); }
	{ LODConfig c = base; c.opacity_fade_enabled = !c.opacity_fade_enabled; CHECK(c != base); }
	{ LODConfig c = base; c.diagnostic_logging = !c.diagnostic_logging; CHECK(c != base); }
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
