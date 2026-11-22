#pragma once
#include "test_macros.h"
#include "deformation_cpu_mirror.h"
#include "../core/gaussian_semantic.h"
#include "../renderer/gs_wind_bounds.h"
#include "../core/gaussian_splat_scene_director.h"

static_assert(GS_INSTANCE_WIND_MODE_SEMANTIC_INHERIT == GaussianSplatSceneDirector::INSTANCE_WIND_SEMANTIC_INHERIT, "B3 CPU and shader row mode");

TEST_CASE("[GaussianSplatting][HLOD][B3] W4 rigid semantic splats stay still and legacy forcing moves them") {
    using namespace GaussianSplatting::DeformationCPUMirror;
    const Vector3 rest(1.25, 0.5, -4.0);
    const uint32_t rigid = gaussian_pack_semantic(7, 0);
    const float fixed = wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, 0.75f, 1.0f, 1.0f, rigid, WIND_MODE_SEMANTIC_INHERIT);
    const float forced = wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, 0.75f, 1.0f, 1.0f, rigid, WIND_MODE_SEMANTIC_INHERIT, true);
    CHECK(fixed == 0.0f);
    CHECK(Math::abs(forced) > 0.01f);
}

TEST_CASE("[GaussianSplatting][HLOD][B3] baked 255 and 128 weights replace opacity stiffness at full and half amplitude") {
    using namespace GaussianSplatting::DeformationCPUMirror;
    const Vector3 rest(1.25, 0.5, -4.0);
    const float phase = float(rest.x + rest.z) * 0.1f + 1.1f + wind_phase_noise(rest);
    const float oracle = Math::sin(phase) * 2.0f * 0.7f;
    CHECK(Math::abs(oracle) > 0.01f);
    for (const float opacity : { 0.1f, 0.75f, 1.0f }) {
        const float full = wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, opacity, 0.7f, 1.0f, gaussian_pack_semantic(1, 255));
        const float half = wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, opacity, 0.7f, 1.0f, gaussian_pack_semantic(1, 128));
        CHECK(full == doctest::Approx(oracle));
        CHECK(half == doctest::Approx(oracle * 128.0f / 255.0f));
    }
}

TEST_CASE("[GaussianSplatting][HLOD][B3] O4 AUTO legacy is still while ON and instance rows retain opacity stiffness") {
    using namespace GaussianSplatting::DeformationCPUMirror;
    CHECK(splat_wind_weight(0, 0.75f, WIND_MODE_SEMANTIC_INHERIT) == 0.0f);
    CHECK(splat_wind_weight(0, 0.75f, WIND_MODE_INHERIT) == doctest::Approx(0.4f));
    CHECK(splat_wind_weight(0x00ff0000u, 0.75f, WIND_MODE_SEMANTIC_INHERIT) == 0.0f); // Invalid word never enables wind.
    CHECK(splat_wind_weight(gaussian_pack_semantic(1, 0), 0.1f, WIND_MODE_SEMANTIC_INHERIT) == 0.0f); // No authored wind input.
}

TEST_CASE("[GaussianSplatting][HLOD][B3] node inflation shrinks only with payload semantic-valid bit 5") {
    for (const double bound : { 0.0, 0.1, 3.0 }) {
        for (const uint8_t weight : { uint8_t(0), uint8_t(128), uint8_t(255) }) {
            CHECK(gs_wind::node_displacement_bound(bound, weight, true) == doctest::Approx(bound * double(weight) / 255.0));
            CHECK(gs_wind::node_displacement_bound(bound, weight, false) == bound);
        }
    }
}
