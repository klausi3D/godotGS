/**************************************************************************/
/*  test_gaussian_thumbnail_generator.h                                   */
/*  #798: a failed thumbnail canvas allocation must not be cached.        */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_THUMBNAIL_GENERATOR_H
#define GAUSSIAN_SPLATTING_TEST_THUMBNAIL_GENERATOR_H

#ifdef TOOLS_ENABLED

#include "test_macros.h"

#include "../core/gaussian_splat_asset.h"
#include "../core/gs_vector_alloc.h"
#include "../editor/gaussian_thumbnail_generator.h"

namespace TestGaussianThumbnailGenerator {

// A small, fully-populated asset. Colors vary per splat so a correctly generated
// COLOR thumbnail is distinguishable from the flat background fill.
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
	PackedColorArray colors;
	colors.resize(int(p_count));
	for (uint32_t i = 0; i < p_count; i++) {
		positions.set(int(i) * 3 + 0, float(i));
		positions.set(int(i) * 3 + 1, 0.0f);
		positions.set(int(i) * 3 + 2, float(i) * 0.5f);
		scales.set(int(i) * 3 + 0, 0.2f);
		scales.set(int(i) * 3 + 1, 0.2f);
		scales.set(int(i) * 3 + 2, 0.2f);
		colors.set(int(i), Color(1.0f, 0.25f, 0.75f, 1.0f));
	}
	asset->set_positions(positions);
	asset->set_scales(scales);
	asset->set_colors(colors);
	return asset;
}

} // namespace TestGaussianThumbnailGenerator

// ── #798 review round 3 ─────────────────────────────────────────────────
//
// _project_to_canvas() used to swallow its own allocation failure: it returned an
// empty projection, all four generators read that as "nothing to draw" and returned a
// perfectly valid FLAT image, and generate_thumbnail_image() cannot tell a flat image
// from a good one -- it caches any valid Ref<Image> in memory AND writes it to the disk
// cache keyed by the asset fingerprint. One transient allocation failure therefore
// pinned a blank preview for that asset, across editor sessions, with no path to a
// retry.
//
// The failure cannot be provoked by asset size (generate_thumbnail_image() validates
// p_size and the snapshot first), so this case arms the TESTS_ENABLED seam in
// core/gs_vector_alloc.h at the exact `p_where` label of the canvas allocation and
// asserts the seam actually fired -- otherwise the case would silently measure the
// success path, which is the class of vacuous test this PR is fixing elsewhere.
TEST_CASE("[GaussianSplatting][Editor] A failed thumbnail canvas allocation is not cached as a thumbnail") {
	Ref<GaussianThumbnailGenerator> generator;
	generator.instantiate();
	if (generator.is_null()) {
		FAIL("GaussianThumbnailGenerator must instantiate");
		return;
	}

	Ref<GaussianSplatAsset> asset = TestGaussianThumbnailGenerator::_make_asset(8);
	if (asset.is_null()) {
		FAIL("the fixture asset must instantiate");
		return;
	}

	const int size = 16;
	const GaussianThumbnailGenerator::ThumbnailStyle style = GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR;

	// Start from a known-cold state in BOTH caches; a leftover disk entry for this
	// fingerprint would short-circuit generation and make every assertion below vacuous.
	generator->clear_cache();
	generator->clear_disk_cache();

	gs_vector_alloc_force_failure_at("GaussianThumbnailGenerator::_project_to_canvas hits");
	Ref<Image> failed;
	{
		// The injected failure emits the expected ERR_PRINT; keep the log clean.
		ERR_PRINT_OFF;
		failed = generator->generate_thumbnail_image(asset, size, style);
		ERR_PRINT_ON;
	}
	const bool first_injection_fired = !gs_vector_alloc_forced_failure_is_armed();
	gs_vector_alloc_clear_forced_failure();
	if (!first_injection_fired) {
		FAIL("the injected canvas allocation failure never fired -- generate_thumbnail_image() did not reach _project_to_canvas, so this case proves nothing");
		generator->clear_cache();
		generator->clear_disk_cache();
		return;
	}

	CHECK_MESSAGE(failed.is_null(),
			"A failed canvas allocation must propagate as a null image, not a flat one.");
	CHECK_MESSAGE(generator->get_cache_entry_count() == 0,
			"A failed canvas allocation must leave no in-memory cache entry.");

	// Now the DISK half of the same contract. Drop the in-memory cache so the next call
	// must consult the disk cache, and arm the seam again: if the first failure had been
	// persisted as a png, this call would return that blank image from disk and never
	// reach _project_to_canvas -- so the injection would NOT fire.
	generator->clear_cache();
	gs_vector_alloc_force_failure_at("GaussianThumbnailGenerator::_project_to_canvas hits");
	Ref<Image> failed_again;
	{
		ERR_PRINT_OFF;
		failed_again = generator->generate_thumbnail_image(asset, size, style);
		ERR_PRINT_ON;
	}
	const bool second_injection_fired = !gs_vector_alloc_forced_failure_is_armed();
	gs_vector_alloc_clear_forced_failure();
	CHECK_MESSAGE(second_injection_fired,
			"A failed canvas allocation must leave no disk cache entry -- the retry must regenerate, not serve a persisted blank.");
	CHECK_MESSAGE(failed_again.is_null(),
			"The retry under the same injected failure must also fail closed.");

	// And the failure must be transient: once the allocation succeeds, the same asset
	// still produces a real thumbnail.
	generator->clear_cache();
	Ref<Image> recovered = generator->generate_thumbnail_image(asset, size, style);
	if (recovered.is_null()) {
		FAIL("after the injected failure is cleared, the same asset must still produce a thumbnail");
		generator->clear_cache();
		generator->clear_disk_cache();
		return;
	}
	CHECK_MESSAGE(recovered->get_width() == size, "the recovered thumbnail must have the requested width");
	CHECK_MESSAGE(recovered->get_height() == size, "the recovered thumbnail must have the requested height");
	CHECK_MESSAGE(generator->get_cache_entry_count() == 1,
			"a successful thumbnail must be cached");

	generator->clear_cache();
	generator->clear_disk_cache();
}

#endif // TOOLS_ENABLED

#endif // GAUSSIAN_SPLATTING_TEST_THUMBNAIL_GENERATOR_H
