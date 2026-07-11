/**************************************************************************/
/*  test_gaussian_splat_asset_prune.h                                     */
/*  Survivor-fidelity tests for GaussianSplatAsset::prune_by_importance   */
/*  (GS-PERF-PRUNE slice 2b). Guards that pruning is LOSSLESS for the      */
/*  splats it keeps -- it compacts the raw SoA arrays directly rather than */
/*  round-tripping through the AoS/opacity-activation path (which clamps   */
/*  extreme opacity logits and is not byte-preserving).                   */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_ASSET_PRUNE_H
#define GAUSSIAN_SPLATTING_TEST_ASSET_PRUNE_H

#include "test_macros.h"

#include "../core/gaussian_splat_asset.h"

namespace TestGaussianSplatAssetPrune {

// Build an asset of N splats whose importance (opacity*max|scale|) is strictly increasing
// in the source index (scale grows with i; opacity is saturated), so a ratio prune keeps a
// known high-index suffix. Opacity logits are set to EXTREME values (>> logit(0.9999)~=9.21)
// so the old sigmoid->logit write-back would have clamped survivors -- this fixture makes
// that corruption observable.
inline Ref<GaussianSplatAsset> _make_asset(uint32_t p_count) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	if (asset.is_null()) {
		return asset;
	}
	asset->set_splat_count(p_count);

	PackedFloat32Array positions;
	positions.resize(int(p_count) * 3);
	PackedFloat32Array scales;
	scales.resize(int(p_count) * 3);
	PackedFloat32Array opacity_logits;
	opacity_logits.resize(int(p_count));
	PackedColorArray colors;
	colors.resize(int(p_count));
	for (uint32_t i = 0; i < p_count; i++) {
		positions.set(int(i) * 3 + 0, float(i));
		positions.set(int(i) * 3 + 1, float(i) + 0.25f);
		positions.set(int(i) * 3 + 2, float(i) + 0.5f);
		const float s = 0.1f + 0.1f * float(i); // strictly increasing max|scale| -> importance
		scales.set(int(i) * 3 + 0, s);
		scales.set(int(i) * 3 + 1, s);
		scales.set(int(i) * 3 + 2, s);
		opacity_logits.set(int(i), 15.0f + float(i)); // 15..; all far above the 9.21 clamp point
		colors.set(int(i), Color(float(i) * 0.01f, 0.5f, 0.25f, 1.0f));
	}
	asset->set_positions(positions);
	asset->set_scales(scales);
	asset->set_opacity_logits(opacity_logits);
	asset->set_colors(colors);
	return asset;
}

} // namespace TestGaussianSplatAssetPrune

TEST_CASE("[GaussianSplatting][Prune] Asset prune keeps survivors byte-identical across every lane") {
	const uint32_t kCount = 8;
	Ref<GaussianSplatAsset> asset = TestGaussianSplatAssetPrune::_make_asset(kCount);
	REQUIRE(asset.is_valid());
	REQUIRE_EQ(asset->get_splat_count(), kCount);

	// Ratio 0.5 keeps the 4 highest-importance splats = the high-scale suffix, source indices 4..7.
	const uint32_t kept = asset->prune_by_importance(0.5, 0.0f);
	CHECK_EQ(kept, 4u);
	CHECK_EQ(asset->get_splat_count(), 4u);

	const PackedFloat32Array out_logits = asset->get_opacity_logits();
	const PackedFloat32Array out_positions = asset->get_positions();
	const PackedFloat32Array out_scales = asset->get_scales();
	const PackedColorArray out_colors = asset->get_colors();
	REQUIRE_EQ(out_logits.size(), 4);
	REQUIRE_EQ(out_positions.size(), 12);
	REQUIRE_EQ(out_scales.size(), 12);
	REQUIRE_EQ(out_colors.size(), 4);

	for (uint32_t j = 0; j < 4; j++) {
		const uint32_t src = 4u + j; // survivors are the ascending high-importance suffix
		// CRITICAL: the raw opacity logit must be preserved EXACTLY. The pre-fix AoS write-back
		// sigmoid-decoded then re-logit-clamped survivors to logit(0.9999)~=9.21, corrupting these.
		CHECK_MESSAGE(out_logits[int(j)] == float(15.0f + float(src)),
				vformat("survivor %d opacity_logit=%f; expected %f (a value near 9.21 means the "
						"lossy round-trip clamp regressed)",
						j, out_logits[int(j)], 15.0f + float(src)));
		// Every other lane byte-identical to the source splat too.
		CHECK(out_positions[int(j) * 3 + 0] == float(src));
		CHECK(out_positions[int(j) * 3 + 1] == float(src) + 0.25f);
		CHECK(out_positions[int(j) * 3 + 2] == float(src) + 0.5f);
		CHECK(out_scales[int(j) * 3 + 0] == 0.1f + 0.1f * float(src));
		CHECK(out_colors[int(j)].r == float(src) * 0.01f);
	}
}

TEST_CASE("[GaussianSplatting][Prune] Asset prune no-op default is byte-identical") {
	const uint32_t kCount = 6;
	Ref<GaussianSplatAsset> asset = TestGaussianSplatAssetPrune::_make_asset(kCount);
	REQUIRE(asset.is_valid());

	const PackedFloat32Array before_logits = asset->get_opacity_logits();
	const uint32_t kept = asset->prune_by_importance(1.0, 0.0f); // default -> no-op
	CHECK_EQ(kept, kCount);
	CHECK_EQ(asset->get_splat_count(), kCount);
	const PackedFloat32Array after_logits = asset->get_opacity_logits();
	REQUIRE_EQ(after_logits.size(), before_logits.size());
	for (int i = 0; i < before_logits.size(); i++) {
		CHECK(after_logits[i] == before_logits[i]); // untouched, incl. the extreme logits
	}
}

#endif // GAUSSIAN_SPLATTING_TEST_ASSET_PRUNE_H
