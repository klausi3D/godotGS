#pragma once

#include "test_macros.h"

// Exercise the shared importance metric + top-k selection through the NEUTRAL core
// header directly (NOT via renderer/resident_atlas_budget.h). This proves the core
// header is independently usable by a non-renderer consumer -- the whole point of
// the #456 / ADR-Decision-1 extraction: the importer (io/) will include exactly this
// header without pulling in renderer/. Behavior must match #420 exactly.
#include "../core/gaussian_importance.h"

#include <limits>

namespace GaussianImportanceTests {

static Gaussian make_g(float p_opacity, float p_scale_x, float p_scale_y, float p_scale_z) {
    Gaussian g = {};
    g.opacity = p_opacity;
    g.scale = Vector3(p_scale_x, p_scale_y, p_scale_z);
    return g;
}

} // namespace GaussianImportanceTests

TEST_CASE("[GaussianSplatting][GaussianImportance] Core header is independently usable (metric matches #420)") {
    using namespace ResidentAtlasBudget;
    using namespace GaussianImportanceTests;
    // opacity (clamped) * (max|scale| + 1e-4). 0.5 * (2.0 + 1e-4) == 1.00005.
    const Gaussian g = make_g(0.5f, 2.0f, 0.5f, 0.25f);
    CHECK(gaussian_importance(g) == doctest::Approx(0.5f * (2.0f + 0.0001f)));
    // opacity is clamped to [0,1] before multiplying (an over-1 opacity does not inflate it).
    const Gaussian over = make_g(4.0f, 1.0f, 1.0f, 1.0f);
    CHECK(gaussian_importance(over) == doctest::Approx(1.0f * (1.0f + 0.0001f)));
    // scale magnitude, not signed axis: a large NEGATIVE scale still ranks high (#420 refinement).
    const Gaussian big_neg = make_g(1.0f, -5.0f, -4.0f, -3.0f);
    const Gaussian small_pos = make_g(1.0f, 0.1f, 0.05f, 0.02f);
    CHECK(gaussian_importance(big_neg) > gaussian_importance(small_pos));
}

TEST_CASE("[GaussianSplatting][GaussianImportance] Selection is deterministic (same input -> same output)") {
    using namespace ResidentAtlasBudget;
    const float importance[6] = { 0.01f, 0.90f, 0.25f, 0.04f, 2.00f, 0.30f };
    LocalVector<uint32_t> a;
    LocalVector<uint32_t> b;
    select_top_k_indices(importance, 6, 3, a);
    select_top_k_indices(importance, 6, 3, b);
    REQUIRE(a.size() == 3);
    REQUIRE(b.size() == 3);
    // Byte-for-byte identical across runs -- the whole selection is deterministic.
    for (uint32_t i = 0; i < a.size(); i++) {
        CHECK(a[i] == b[i]);
    }
    // Kept set is the three largest (indices 4, 1, 5), returned in ascending SOURCE order.
    CHECK(a[0] == 1u);
    CHECK(a[1] == 4u);
    CHECK(a[2] == 5u);
}

TEST_CASE("[GaussianSplatting][GaussianImportance] Ties broken by ascending source index") {
    using namespace ResidentAtlasBudget;
    // All equal importance: the kept set must be the LOWEST source indices, in ascending order.
    const float tied[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    LocalVector<uint32_t> keep;
    select_top_k_indices(tied, 5, 3, keep);
    REQUIRE(keep.size() == 3);
    CHECK(keep[0] == 0u);
    CHECK(keep[1] == 1u);
    CHECK(keep[2] == 2u);

    // A partial tie at the selection boundary is resolved by ascending index too: importances
    // { 5, 1, 1, 1 } keeping 2 -> index 0 (the 5) plus the lowest-index of the tied 1s (index 1).
    const float boundary[4] = { 5.0f, 1.0f, 1.0f, 1.0f };
    LocalVector<uint32_t> keep2;
    select_top_k_indices(boundary, 4, 2, keep2);
    REQUIRE(keep2.size() == 2);
    CHECK(keep2[0] == 0u);
    CHECK(keep2[1] == 1u);

    // keep >= count returns the identity range (ascending, all indices).
    LocalVector<uint32_t> all;
    select_top_k_indices(tied, 5, 9, all);
    REQUIRE(all.size() == 5);
    for (uint32_t i = 0; i < 5; i++) {
        CHECK(all[i] == i);
    }
    // keep == 0 (or count == 0) yields an empty set.
    LocalVector<uint32_t> none;
    select_top_k_indices(tied, 5, 0, none);
    CHECK(none.size() == 0u);
}

TEST_CASE("[GaussianSplatting][GaussianImportance] Non-finite inputs floor to 1e-4 (documented floor)") {
    using namespace ResidentAtlasBudget;
    using namespace GaussianImportanceTests;
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Every non-finite path floors to the documented 1e-4 minimum AND stays finite (so the
    // strict-weak-ordering comparator in select_top_k_indices never sees a non-finite value).
    const Gaussian all_nan = make_g(1.0f, qnan, qnan, qnan);       // size_factor NaN
    const Gaussian inf_scale = make_g(1.0f, inf, 1.0f, 1.0f);      // Inf scale
    const Gaussian nan_opacity = make_g(qnan, 1.0f, 1.0f, 1.0f);   // NaN opacity
    CHECK(gaussian_importance(all_nan) == doctest::Approx(0.0001f));
    CHECK(gaussian_importance(inf_scale) == doctest::Approx(0.0001f));
    CHECK(gaussian_importance(nan_opacity) == doctest::Approx(0.0001f));
    CHECK(Math::is_finite(gaussian_importance(all_nan)));
    CHECK(Math::is_finite(gaussian_importance(inf_scale)));
    CHECK(Math::is_finite(gaussian_importance(nan_opacity)));

    // A finite splat out-ranks the floored degenerate ones and is the survivor under a keep-1 budget.
    const Gaussian normal = make_g(0.5f, 0.5f, 0.5f, 0.5f);
    CHECK(gaussian_importance(normal) > 0.0001f);
    const float mixed[3] = {
        gaussian_importance(all_nan),     // 0 -> floored
        gaussian_importance(normal),      // 1 -> highest finite
        gaussian_importance(nan_opacity), // 2 -> floored
    };
    LocalVector<uint32_t> keep;
    select_top_k_indices(mixed, 3, 1, keep);
    REQUIRE(keep.size() == 1);
    CHECK(keep[0] == 1u); // the finite splat survives, deterministically
}
