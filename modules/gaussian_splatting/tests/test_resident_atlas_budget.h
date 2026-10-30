#pragma once

#include "test_macros.h"

#include "../renderer/resident_atlas_budget.h"

#include <limits>

namespace ResidentAtlasBudgetTests {

static constexpr uint64_t k2GB = uint64_t(2) * 1024 * 1024 * 1024;
static constexpr uint64_t k1GB = uint64_t(1) * 1024 * 1024 * 1024;
static constexpr uint64_t k8GB = uint64_t(8) * 1024 * 1024 * 1024;

static Gaussian make_g(float p_opacity, float p_scale, float p_marker) {
    Gaussian g = {};
    g.opacity = p_opacity;
    g.scale = Vector3(p_scale, p_scale * 0.5f, p_scale * 0.25f); // max axis == p_scale
    g.position = Vector3(p_marker, 0.0f, 0.0f); // marker to identify after compaction
    return g;
}

} // namespace ResidentAtlasBudgetTests

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Unknown / below-floor device budget keeps the 2 GB staging cap") {
    using namespace ResidentAtlasBudget;
    // device budget unknown (0) -> capacity unknown -> do-no-harm, cap stays staging ceiling.
    CHECK(compute_effective_atlas_cap_bytes(0, 0) == ResidentAtlasBudgetTests::k2GB);
    // 200 MiB is below the 256 MiB sane floor -> treated as unknown.
    CHECK(compute_effective_atlas_cap_bytes(uint64_t(200) * 1024 * 1024, 0) == ResidentAtlasBudgetTests::k2GB);
    // exactly at the floor is trusted.
    CHECK(compute_effective_atlas_cap_bytes(kMinTrustedDeviceBudgetBytes, 0) ==
            uint64_t(double(kMinTrustedDeviceBudgetBytes) * kResidentAtlasHeadroomFraction));
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Capable GPU is byte-identical to today (staging dominates)") {
    using namespace ResidentAtlasBudget;
    // 8 GB device: 0.60*8GB = 4.8 GB, MIN with 2 GB staging => 2 GB (no clamp on capable GPUs).
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k8GB, 0) == ResidentAtlasBudgetTests::k2GB);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Low-end device budget binds beneath the staging cap") {
    using namespace ResidentAtlasBudget;
    // 1 GB device: 0.60*1GB binds (< 2 GB staging).
    const uint64_t expected = uint64_t(double(ResidentAtlasBudgetTests::k1GB) * kResidentAtlasHeadroomFraction);
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 0) == expected);
    CHECK(expected < ResidentAtlasBudgetTests::k2GB);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Override forces a clamp and never loosens (MIN semantics)") {
    using namespace ResidentAtlasBudget;
    // override forces a clamp even on a huge GPU (the validation lever).
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k8GB, 512) == uint64_t(512) * 1024 * 1024);
    // an override larger than the budget term cannot loosen the cap.
    const uint64_t budget_cap = uint64_t(double(ResidentAtlasBudgetTests::k1GB) * kResidentAtlasHeadroomFraction);
    CHECK(compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 4096) == budget_cap);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Subset plan: full atlas fits -> not reduced") {
    using namespace ResidentAtlasBudget;
    const SubsetPlan plan = compute_subset_plan(100000, ResidentAtlasBudgetTests::k2GB, 144);
    CHECK_FALSE(plan.reduced);
    CHECK(plan.keep_ratio == doctest::Approx(1.0));
    CHECK(plan.target_keep == 100000);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Subset plan: over budget -> reduced, target never exceeds cap") {
    using namespace ResidentAtlasBudget;
    const uint64_t cap = compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 0);
    const SubsetPlan plan = compute_subset_plan(20000000, cap, 144);
    CHECK(plan.reduced);
    CHECK(plan.target_keep < plan.source_count);
    CHECK(plan.keep_ratio < 1.0);
    // The soft splat target must never imply more bytes than the cap.
    CHECK(plan.target_keep * uint64_t(144) <= cap);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] A source above the staging limit thins to fit, not reject") {
    using namespace ResidentAtlasBudget;
    // Unknown/UMA device budget => cap stays at the 2 GB staging ceiling.
    const uint64_t cap = compute_effective_atlas_cap_bytes(0, 0);
    CHECK(cap == ResidentAtlasBudgetTests::k2GB);
    // A source ~3x the staging capacity (well over 2 GB) must still produce a thinned plan whose
    // target fits the per-buffer staging upload -- so a direct resident node degrades instead of
    // black-framing.
    const uint64_t over_staging_splats = (cap / 144u) * 3u;
    const SubsetPlan plan = compute_subset_plan(over_staging_splats, cap, 144);
    CHECK(plan.reduced);
    CHECK(plan.target_keep < plan.source_count);
    CHECK(plan.target_keep * uint64_t(144) <= cap); // thinned atlas fits the 2 GB upload
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Per-chunk keep count floors at 1 and respects the ratio") {
    using namespace ResidentAtlasBudget;
    CHECK(compute_chunk_keep_count(0, 0.5) == 0);          // empty stays empty
    CHECK(compute_chunk_keep_count(10, 1.0) == 10);        // ratio >= 1 keeps all
    CHECK(compute_chunk_keep_count(10, 1.5) == 10);        // clamped to count
    CHECK(compute_chunk_keep_count(10, 0.0001) == 1);      // floor: never delete a region
    CHECK(compute_chunk_keep_count(10, 0.0) == 1);
    CHECK(compute_chunk_keep_count(10, 0.4) == 4);         // round(4.0)
    CHECK(compute_chunk_keep_count(10, 0.45) == 5);        // round(4.5)
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Top-K selection: highest importance, ascending, deterministic ties") {
    using namespace ResidentAtlasBudget;
    const float importance[5] = { 0.01f, 0.90f, 0.25f, 0.04f, 2.00f };
    LocalVector<uint32_t> keep;
    select_top_k_indices(importance, 5, 2, keep);
    REQUIRE(keep.size() == 2);
    CHECK(keep[0] == 1u); // returned ascending
    CHECK(keep[1] == 4u); // indices of the two largest (0.90, 2.00)

    // determinism: identical input -> identical output.
    LocalVector<uint32_t> keep2;
    select_top_k_indices(importance, 5, 2, keep2);
    REQUIRE(keep2.size() == 2);
    CHECK(keep2[0] == keep[0]);
    CHECK(keep2[1] == keep[1]);

    // ties broken by ascending index.
    const float tied[3] = { 1.0f, 1.0f, 1.0f };
    LocalVector<uint32_t> keep_tied;
    select_top_k_indices(tied, 3, 2, keep_tied);
    REQUIRE(keep_tied.size() == 2);
    CHECK(keep_tied[0] == 0u);
    CHECK(keep_tied[1] == 1u);

    // keep >= count returns the identity range.
    LocalVector<uint32_t> keep_all;
    select_top_k_indices(importance, 5, 9, keep_all);
    REQUIRE(keep_all.size() == 5);
    CHECK(keep_all[0] == 0u);
    CHECK(keep_all[4] == 4u);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Chunk compaction keeps top importance and SH stays parallel") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;

    LocalVector<Gaussian> gaussians;
    gaussians.push_back(make_g(0.1f, 0.1f, 0.0f)); // idx0 importance ~0.010
    gaussians.push_back(make_g(0.9f, 1.0f, 1.0f)); // idx1 importance ~0.900
    gaussians.push_back(make_g(0.5f, 0.5f, 2.0f)); // idx2 importance ~0.250
    gaussians.push_back(make_g(0.2f, 0.2f, 3.0f)); // idx3 importance ~0.040
    gaussians.push_back(make_g(1.0f, 2.0f, 4.0f)); // idx4 importance ~2.000

    const uint32_t stride = 15;
    LocalVector<Vector3> sh;
    sh.resize(5 * stride);
    for (uint32_t i = 0; i < 5; i++) {
        for (uint32_t c = 0; c < stride; c++) {
            sh[i * stride + c] = Vector3(float(i), float(c), 0.0f); // x encodes source index
        }
    }

    // keep_ratio 0.4 over count 5 -> round(2.0) == 2 kept (the two highest: idx1, idx4).
    uint32_t budget = 100;
    const uint32_t kept = compact_chunk_by_importance(gaussians, sh, stride, 0.4, budget);
    REQUIRE(kept == 2);
    CHECK(budget == 98); // budget decremented by the kept count
    REQUIRE(gaussians.size() == 2);
    REQUIRE(sh.size() == 2u * stride);

    // Ascending-index compaction => slot0 is source idx1, slot1 is source idx4.
    CHECK(gaussians[0].position.x == doctest::Approx(1.0f));
    CHECK(gaussians[1].position.x == doctest::Approx(4.0f));
    // SH block of each kept gaussian must match its original source block.
    CHECK(sh[0 * stride + 0].x == doctest::Approx(1.0f));
    CHECK(sh[0 * stride + 7].x == doctest::Approx(1.0f));
    CHECK(sh[1 * stride + 0].x == doctest::Approx(4.0f));
    CHECK(sh[1 * stride + 7].x == doctest::Approx(4.0f));
    CHECK(sh[1 * stride + 7].y == doctest::Approx(7.0f)); // coefficient slot preserved
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Compaction is a no-op when the ratio keeps everything") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;

    LocalVector<Gaussian> gaussians;
    for (uint32_t i = 0; i < 4; i++) {
        gaussians.push_back(make_g(0.5f, 1.0f, float(i)));
    }
    LocalVector<Vector3> sh; // no high-order SH (stride 0) is a valid path
    uint32_t budget = 100;
    const uint32_t kept = compact_chunk_by_importance(gaussians, sh, 0, 1.0, budget);
    CHECK(kept == 4);
    CHECK(budget == 96); // 4 kept
    REQUIRE(gaussians.size() == 4);
    CHECK(gaussians[0].position.x == doctest::Approx(0.0f));
    CHECK(gaussians[3].position.x == doctest::Approx(3.0f));
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Global budget caps cumulative kept across many tiny chunks") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;
    // Codex #420: many one-splat chunks each floor to 1 splat; without the global budget the
    // total would be the full count and defeat the VRAM cap. With a budget of 3, only 3 survive.
    uint32_t budget = 3;
    uint32_t total_kept = 0;
    const double tiny_ratio = 0.09; // round(1 * 0.09) == 0 -> floored to 1 per chunk
    for (uint32_t c = 0; c < 10; c++) {
        LocalVector<Gaussian> g;
        g.push_back(make_g(0.5f, 1.0f, float(c)));
        LocalVector<Vector3> sh;
        total_kept += compact_chunk_by_importance(g, sh, 0, tiny_ratio, budget);
    }
    CHECK(total_kept == 3); // never exceeds the budget
    CHECK(budget == 0);     // fully consumed, remaining chunks dropped to 0
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Importance uses scale magnitude (negative scales survive)") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;
    // A large negative-scale splat must out-rank a tiny positive-scale one (magnitude, not sign).
    Gaussian big_neg = make_g(1.0f, 0.0f, 0.0f);
    big_neg.scale = Vector3(-5.0f, -4.0f, -3.0f);
    Gaussian small_pos = make_g(1.0f, 0.1f, 1.0f); // max |scale| == 0.1
    CHECK(gaussian_importance(big_neg) > gaussian_importance(small_pos));

    // Under a keep-1-of-2 budget the big negative-scale splat is the survivor.
    LocalVector<Gaussian> g;
    g.push_back(small_pos); // index 0, marker 1.0
    g.push_back(big_neg);   // index 1, marker 0.0
    LocalVector<Vector3> sh;
    uint32_t budget = 1;
    const uint32_t kept = compact_chunk_by_importance(g, sh, 0, 0.5, budget);
    REQUIRE(kept == 1);
    CHECK(g[0].position.x == doctest::Approx(0.0f)); // big_neg's marker
    CHECK(budget == 0);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Single global ratio thins multiple chunks proportionally within cap") {
    using namespace ResidentAtlasBudget;
    // Two assets/chunks of different sizes share ONE global keep_ratio.
    const uint64_t cap = compute_effective_atlas_cap_bytes(ResidentAtlasBudgetTests::k1GB, 0);
    const SubsetPlan plan = compute_subset_plan(20000000, cap, 144);
    REQUIRE(plan.reduced);

    const uint32_t chunk_a = 1000;
    const uint32_t chunk_b = 250;
    const uint32_t keep_a = compute_chunk_keep_count(chunk_a, plan.keep_ratio);
    const uint32_t keep_b = compute_chunk_keep_count(chunk_b, plan.keep_ratio);
    // Same ratio applied to both -> proportional thinning, each >= 1, each < its source.
    CHECK(keep_a >= 1u);
    CHECK(keep_b >= 1u);
    CHECK(keep_a < chunk_a);
    CHECK(keep_b < chunk_b);
    CHECK(double(keep_a) / double(chunk_a) == doctest::Approx(double(keep_b) / double(chunk_b)).epsilon(0.05));
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Per-asset budget is proportional, floors at 1, and stays under the global cap") {
    using namespace ResidentAtlasBudget;
    // target_keep 100 split across two assets (300 + 100 = 400 total) -> 75 / 25.
    uint32_t global_remaining = 100;
    const uint32_t a = resident_asset_budget(100, 300, 400, global_remaining);
    global_remaining -= a;
    const uint32_t b = resident_asset_budget(100, 100, 400, global_remaining);
    CHECK(a == 75);
    CHECK(b == 25);
    CHECK(a + b <= 100); // the sum never exceeds the global budget

    // A later (e.g. visible) asset still gets its proportional share -- it is NOT starved to 0
    // by the earlier larger asset, which is the Codex #420 round-3 concern.
    CHECK(b >= 1u);

    // A tiny asset against a huge total still keeps at least one splat of budget.
    CHECK(resident_asset_budget(100, 1, 1000000, 100) == 1u);
    // The proportional share is capped by whatever budget is globally left.
    CHECK(resident_asset_budget(100, 300, 400, 10) == 10u);
    // Degenerate inputs are budget 0, never a divide-by-zero.
    CHECK(resident_asset_budget(100, 0, 400, 100) == 0u);
    CHECK(resident_asset_budget(100, 300, 0, 100) == 0u);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget] Importance is always finite (no selection UB on NaN/Inf data)") {
    using namespace ResidentAtlasBudget;
    using namespace ResidentAtlasBudgetTests;
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    Gaussian partial_nan = make_g(1.0f, 1.0f, 0.0f);
    partial_nan.scale = Vector3(qnan, 1.0f, 1.0f); // one axis NaN: MAX may mask it -> finite
    Gaussian all_nan = make_g(1.0f, 1.0f, 1.0f);
    all_nan.scale = Vector3(qnan, qnan, qnan); // size_factor NaN -> importance non-finite -> floored
    Gaussian inf_scale = make_g(1.0f, 1.0f, 2.0f);
    inf_scale.scale = Vector3(inf, 1.0f, 1.0f); // Inf -> non-finite -> floored
    Gaussian nan_opacity = make_g(qnan, 1.0f, 3.0f); // NaN opacity -> non-finite -> floored
    Gaussian normal = make_g(0.5f, 0.5f, 4.0f);

    // The core guarantee: importance is ALWAYS finite, so the strict-weak-ordering comparator
    // in select_top_k_indices never sees a non-finite value (which would be UB).
    CHECK(Math::is_finite(gaussian_importance(partial_nan)));
    CHECK(Math::is_finite(gaussian_importance(all_nan)));
    CHECK(Math::is_finite(gaussian_importance(inf_scale)));
    CHECK(Math::is_finite(gaussian_importance(nan_opacity)));
    // Inputs that actually compute a non-finite importance are floored to the minimum.
    CHECK(gaussian_importance(all_nan) == doctest::Approx(0.0001f));
    CHECK(gaussian_importance(inf_scale) == doctest::Approx(0.0001f));
    CHECK(gaussian_importance(nan_opacity) == doctest::Approx(0.0001f));
    CHECK(gaussian_importance(normal) > 0.0001f);

    // Selection over a mix of floored-degenerate + finite splats is deterministic and keeps
    // the finite one.
    LocalVector<Gaussian> g;
    g.push_back(all_nan);     // index 0, marker 1.0 -> floored
    g.push_back(normal);      // index 1, marker 4.0 -> highest finite importance
    g.push_back(nan_opacity); // index 2, marker 3.0 -> floored
    LocalVector<Vector3> sh;
    uint32_t budget = 1;
    const uint32_t kept = compact_chunk_by_importance(g, sh, 0, 1.0 / 3.0, budget); // keep 1 of 3
    REQUIRE(kept == 1);
    CHECK(g[0].position.x == doctest::Approx(4.0f)); // the finite splat survives
    CHECK(budget == 0);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget][HLOD] device ledger limit follows the heap fraction, fixed cap when untrusted (#1131)") {
    using namespace ResidentAtlasBudget;
    const uint64_t cap = uint64_t(512) << 20;
    // RTX 3090 heap (25,731,006,464 B): the canonical route alone holds 819 MB of buffers on the
    // 252k-leaf holzbank scan, so the old fixed 512 MiB ledger bound refused every real scan.
    const uint64_t rtx3090 = 25731006464ull;
    const uint64_t limit = compute_hlod_device_ledger_limit_bytes(rtx3090, cap);
    CHECK(limit == uint64_t(double(rtx3090) * kResidentAtlasHeadroomFraction));
    CHECK(limit > uint64_t(819250080) + uint64_t(43253760) + uint64_t(100105656));
    // Unknown (0) and untrusted (< 256 MiB) heap sizes stay fail closed at the fixed cap or below.
    CHECK(compute_hlod_device_ledger_limit_bytes(0, cap) == cap);
    CHECK(compute_hlod_device_ledger_limit_bytes(uint64_t(100) << 20, cap) == (uint64_t(100) << 20));
    CHECK(compute_hlod_device_ledger_limit_bytes(uint64_t(200) << 20, cap) == (uint64_t(200) << 20));
    CHECK(compute_hlod_device_ledger_limit_bytes(uint64_t(300) << 20, cap) == uint64_t(double(uint64_t(300) << 20) * kResidentAtlasHeadroomFraction));
    // Never above the heap itself.
    CHECK(compute_hlod_device_ledger_limit_bytes(ResidentAtlasBudgetTests::k8GB, cap) < ResidentAtlasBudgetTests::k8GB);
}

TEST_CASE("[GaussianSplatting][ResidentAtlasBudget][HLOD] ledger limit literals and HLOD-owned bound refusals (#1131)") {
    using namespace ResidentAtlasBudget;
    const uint64_t cap = uint64_t(512) << 20; // GaussianSplatHlodRuntimeData::GPU_BYTE_LIMIT
    // Literals, not the formula: 8 GiB heap -> 0.60 -> 5,153,960,755; unknown heap -> the 512 MiB cap.
    CHECK(compute_hlod_device_ledger_limit_bytes(8589934592ull, cap) == 5153960755ull);
    CHECK(compute_hlod_device_ledger_limit_bytes(0, cap) == 536870912ull);
    CHECK(compute_hlod_device_ledger_limit_bytes(268435455ull, cap) == 268435455ull); // 1 B under the 256 MiB floor
    CHECK(compute_hlod_device_ledger_limit_bytes(268435456ull, cap) == 161061273ull); // at the floor: trusted, 0.60
    // Candidate: exactly the HLOD-owned cap passes, one byte more refuses even with unlimited ledger room.
    const uint64_t big = uint64_t(1) << 40;
    CHECK(hlod_candidate_fits(cap, 0, big, cap));
    CHECK_FALSE(hlod_candidate_fits(cap + 1, 0, big, cap));
    // Ledger: device bytes + candidate exactly at the limit pass, one more refuses.
    CHECK(hlod_candidate_fits(100, 900, 1000, cap));
    CHECK_FALSE(hlod_candidate_fits(100, 901, 1000, cap));
    CHECK_FALSE(hlod_candidate_fits(1001, 0, 1000, cap));
    // Prepared bundle: owned bytes bound at cap + 1, ledger bound at limit + 1.
    CHECK(hlod_prepared_fits(cap, 1000, 1000, cap));
    CHECK_FALSE(hlod_prepared_fits(cap + 1, 1000, big, cap));
    CHECK_FALSE(hlod_prepared_fits(cap, 1001, 1000, cap));
}
