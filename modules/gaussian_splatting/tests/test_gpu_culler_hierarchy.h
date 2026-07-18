/**************************************************************************/
/*  test_gpu_culler_hierarchy.h                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "../core/gaussian_data.h"
#include "../interfaces/gpu_culler.h"
#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/templates/local_vector.h"
#include "tests/test_macros.h"

namespace TestGaussianSplattingCullerHierarchy {

// Regression for #604: GPUCuller caches a coarse LOD hierarchy per GaussianData.
// Before the fix it only rebuilt on null/first-create/empty and ignored
// GaussianData::content_revision, so an in-place position/scale edit left stale
// bounds that wrongly culled valid splats. The hierarchy must rebuild when the
// source content revision (or resource identity) changes, and must stay a no-op
// when nothing changed.
//
// This test lives in an auto-registered header (modules_tests.gen.h globs
// tests/*.h) rather than in test_lod_system.cpp: that .cpp is compiled but its
// object is never referenced, so the linker drops it and its doctest cases never
// run. Headers included by the generated doctest TU always register.
TEST_CASE("[GaussianSplatting][LOD] GPUCuller rebuilds LOD hierarchy after in-place GaussianData edit") {
	Ref<::GaussianData> data;
	data.instantiate();
	REQUIRE(data.is_valid());

	const uint32_t count = 512;
	LocalVector<::Gaussian> splats;
	splats.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		::Gaussian &g = splats[i];
		g.position = Vector3(
				Math::fmod(float(i) * 0.37f, 4.0f) - 2.0f,
				Math::fmod(float(i) * 0.53f, 4.0f) - 2.0f,
				Math::fmod(float(i) * 0.71f, 4.0f) - 2.0f);
		g.scale = Vector3(0.25f, 0.25f, 0.25f);
		g.rotation = Quaternion();
		g.opacity = 1.0f;
		g.sh_dc = Color(1, 1, 1, 1);
		g.normal = Vector3(0, 1, 0);
		g.area = 0.2f;
	}
	data->set_gaussians(splats);

	Ref<GPUCuller> culler;
	culler.instantiate();
	REQUIRE(culler.is_valid());

	culler->ensure_hierarchical_structure(data);
	const GPUCuller::CullingState &state = culler->get_state();
	REQUIRE(state.hierarchical_structure != nullptr);

	const uint64_t build_count_before = state.hierarchical_structure_build_count;
	const AABB bounds_before = state.hierarchical_structure->get_bounds();
	CHECK(build_count_before == 1);
	// The original cluster is centered near the origin.
	CHECK(bounds_before.get_center().length() < 10.0f);

	// Calling again with no change must NOT rebuild (preserve no-op-when-clean).
	culler->ensure_hierarchical_structure(data);
	CHECK(state.hierarchical_structure_build_count == build_count_before);

	// Move every splat far away in place; set_positions bumps content_revision.
	PackedVector3Array moved;
	moved.resize(count);
	const Vector3 offset(1000.0f, 0.0f, 0.0f);
	for (uint32_t i = 0; i < count; i++) {
		moved.write[i] = splats[i].position + offset;
	}
	data->set_positions(moved);

	culler->ensure_hierarchical_structure(data);
	REQUIRE(state.hierarchical_structure != nullptr);

	// The hierarchy must have rebuilt (counter advanced) and its bounds must now
	// track the moved cluster instead of serving the stale origin-centered bounds.
	CHECK(state.hierarchical_structure_build_count == build_count_before + 1);
	const AABB bounds_after = state.hierarchical_structure->get_bounds();
	CHECK(bounds_after.get_center().x > 900.0f);
	CHECK_FALSE(bounds_after.has_point(Vector3(0.0f, 0.0f, 0.0f)));
}

} // namespace TestGaussianSplattingCullerHierarchy
