/**************************************************************************/
/*  test_data_authority_hardening.h                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

// Tests pinning the Wave 2 data authority hardening rules:
//   - GaussianData is the only mutable runtime truth.
//   - Its bulk mutators invalidate derived state (octree, animation cache).
//   - apply_animation_at_time() is gone; animation stays a non-destructive
//     view through the get_animated_*() accessors.
//   - GaussianSplatAsset packed setters are not a runtime mutation path.
//     Once the asset has handed out a GaussianData the payload is sealed.
//   - Raw storage accessors document their thread contract and fire a
//     dev-build diagnostic for obvious misuse.

#include "../animation/animation_state_machine.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_merge_utils.h"
#include "core/io/resource.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "tests/test_macros.h"

#include <atomic>
#include <type_traits>
#include <utility>

namespace TestGaussianSplatting {

// Namespace-scope SFINAE probe used to pin apply_animation_at_time() as
// permanently removed. MSVC disallows template members inside a local
// struct, so the detector lives at namespace scope.
template <typename T, typename = void>
struct _HasApplyAnimationAtTime : std::false_type {};

template <typename T>
struct _HasApplyAnimationAtTime<T,
        decltype((void)std::declval<T *>()->apply_animation_at_time(0.0f))>
        : std::true_type {};

namespace {

Gaussian _make_authority_test_splat(const Vector3 &p_position,
        const Color &p_color = Color(1, 1, 1, 1),
        const Vector3 &p_scale = Vector3(1, 1, 1)) {
    Gaussian g;
    g.position = p_position;
    g.scale = p_scale;
    g.rotation = Quaternion();
    g.opacity = p_color.a;
    g.sh_dc = p_color;
    g.normal = Vector3(0, 1, 0);
    g.area = 1.0f;
    g.brush_axes = Vector2(1.0f, 1.0f);
    g.stroke_age = 0.0f;
    g.painterly_meta = 0;
    g.render_meta = 0;
    return g;
}

LocalVector<Gaussian> _make_authority_test_cloud() {
    LocalVector<Gaussian> splats;
    splats.resize(4);
    splats[0] = _make_authority_test_splat(Vector3(-2, 0, 0), Color(1, 0, 0, 1));
    splats[1] = _make_authority_test_splat(Vector3(2, 0, 0), Color(0, 1, 0, 1));
    splats[2] = _make_authority_test_splat(Vector3(0, -2, 0), Color(0, 0, 1, 1));
    splats[3] = _make_authority_test_splat(Vector3(0, 2, 0), Color(1, 1, 0, 1));
    return splats;
}

#ifdef THREADS_ENABLED
struct _AnimatedAccessorRaceContext {
    Ref<::GaussianData> data;
    uint32_t splat_count = 0;
    std::atomic<bool> stop{ false };
    std::atomic<int> invalid_samples{ 0 };
};

static bool _is_finite_color(const Color &p_color) {
    return Math::is_finite(p_color.r) &&
            Math::is_finite(p_color.g) &&
            Math::is_finite(p_color.b) &&
            Math::is_finite(p_color.a);
}

static void _animated_accessor_reader_thread(void *p_userdata) {
    _AnimatedAccessorRaceContext *ctx = static_cast<_AnimatedAccessorRaceContext *>(p_userdata);
    if (!ctx || !ctx->data.is_valid()) {
        return;
    }

    while (!ctx->stop.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < ctx->splat_count; i++) {
            const int index = static_cast<int>(i);
            const Vector3 position = ctx->data->get_animated_position(index, -1.0f);
            const Color color = ctx->data->get_animated_color(index, -1.0f);
            const float opacity = ctx->data->get_animated_opacity(index, -1.0f);
            const Vector3 scale = ctx->data->get_animated_scale(index, -1.0f);
            const Quaternion rotation = ctx->data->get_animated_rotation(index, -1.0f);
            if (!position.is_finite() || !_is_finite_color(color) ||
                    !Math::is_finite(opacity) || !scale.is_finite() ||
                    !rotation.is_finite()) {
                ctx->invalid_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (ctx->data->get_animated_position(-1, 0.0f) != Vector3() ||
                ctx->data->get_animated_color(-1, 0.0f) != Color() ||
                ctx->data->get_animated_opacity(-1, 0.0f) != 1.0f ||
                ctx->data->get_animated_scale(-1, 0.0f) != Vector3(1, 1, 1) ||
                ctx->data->get_animated_rotation(-1, 0.0f) != Quaternion()) {
            ctx->invalid_samples.fetch_add(1, std::memory_order_relaxed);
        }

        Thread::yield();
    }
}
#endif

static Ref<GaussianSplatAsset> _make_asset_snapshot_pattern(float p_marker, const Color &p_color) {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    constexpr int splat_count = 8;
    asset->set_splat_count(splat_count);

    PackedFloat32Array positions;
    positions.resize(splat_count * 3);
    PackedColorArray colors;
    colors.resize(splat_count);
    PackedFloat32Array scales;
    scales.resize(splat_count * 3);
    PackedFloat32Array rotations;
    rotations.resize(splat_count * 4);
    PackedFloat32Array opacity_logits;
    opacity_logits.resize(splat_count);
    PackedInt32Array palette_ids;
    palette_ids.resize(splat_count);
    PackedInt32Array painterly_flags;
    painterly_flags.resize(splat_count);
    PackedFloat32Array normals;
    normals.resize(splat_count * 3);
    PackedFloat32Array brush_axes;
    brush_axes.resize(splat_count * 2);
    PackedFloat32Array stroke_ages;
    stroke_ages.resize(splat_count);

    for (int i = 0; i < splat_count; i++) {
        positions.set(i * 3 + 0, p_marker);
        positions.set(i * 3 + 1, p_marker + float(i));
        positions.set(i * 3 + 2, -p_marker);
        colors.set(i, p_color);
        scales.set(i * 3 + 0, Math::abs(p_marker) + 1.0f);
        scales.set(i * 3 + 1, Math::abs(p_marker) + 2.0f);
        scales.set(i * 3 + 2, Math::abs(p_marker) + 3.0f);
        rotations.set(i * 4 + 0, 1.0f);
        rotations.set(i * 4 + 1, 0.0f);
        rotations.set(i * 4 + 2, 0.0f);
        rotations.set(i * 4 + 3, 0.0f);
        opacity_logits.set(i, p_marker);
        palette_ids.set(i, int(p_marker) & 0xffff);
        painterly_flags.set(i, (int(p_marker) + 7) & 0xffff);
        normals.set(i * 3 + 0, 0.0f);
        normals.set(i * 3 + 1, p_marker > 0.0f ? 1.0f : -1.0f);
        normals.set(i * 3 + 2, 0.0f);
        brush_axes.set(i * 2 + 0, p_marker);
        brush_axes.set(i * 2 + 1, p_marker + 0.5f);
        stroke_ages.set(i, p_marker + float(i) * 0.25f);
    }

    asset->set_positions(positions);
    asset->set_colors(colors);
    asset->set_scales(scales);
    asset->set_rotations(rotations);
    asset->set_opacity_logits(opacity_logits);
    asset->set_palette_ids(palette_ids);
    asset->set_painterly_flags(painterly_flags);
    asset->set_normals(normals);
    asset->set_brush_axes(brush_axes);
    asset->set_stroke_ages(stroke_ages);
    asset->set_source_path(vformat("res://snapshot_pattern_%s.ply", p_marker > 0.0f ? "a" : "b"));
    return asset;
}

static bool _snapshot_has_expected_core_sizes(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, int p_splat_count) {
    if (p_snapshot.splat_count != uint32_t(p_splat_count)) {
        return false;
    }
    if (p_snapshot.positions.size() != p_splat_count * 3 || p_snapshot.colors.size() != p_splat_count) {
        return false;
    }
    if (p_snapshot.scales.size() != p_splat_count * 3 || p_snapshot.rotations.size() != p_splat_count * 4) {
        return false;
    }
    return p_snapshot.opacity_logits.size() == p_splat_count;
}

static bool _snapshot_has_expected_painterly_sizes(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, int p_splat_count) {
    if (p_snapshot.palette_ids.size() != p_splat_count || p_snapshot.painterly_flags.size() != p_splat_count) {
        return false;
    }
    if (p_snapshot.normals.size() != p_splat_count * 3 || p_snapshot.brush_axes.size() != p_splat_count * 2) {
        return false;
    }
    return p_snapshot.stroke_ages.size() == p_splat_count;
}

static bool _snapshot_entry_core_matches(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, int p_index, float p_marker, const Color &p_color) {
    if (!Math::is_equal_approx(p_snapshot.positions[p_index * 3 + 0], p_marker)) {
        return false;
    }
    if (!Math::is_equal_approx(p_snapshot.positions[p_index * 3 + 1], p_marker + float(p_index))) {
        return false;
    }
    if (!Math::is_equal_approx(p_snapshot.positions[p_index * 3 + 2], -p_marker)) {
        return false;
    }
    if (p_snapshot.colors[p_index] != p_color) {
        return false;
    }
    return Math::is_equal_approx(p_snapshot.scales[p_index * 3 + 0], Math::abs(p_marker) + 1.0f);
}

static bool _snapshot_entry_metadata_matches(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, int p_index, float p_marker) {
    if (!Math::is_equal_approx(p_snapshot.opacity_logits[p_index], p_marker)) {
        return false;
    }
    if (p_snapshot.palette_ids[p_index] != (int(p_marker) & 0xffff)) {
        return false;
    }
    return p_snapshot.painterly_flags[p_index] == ((int(p_marker) + 7) & 0xffff);
}

static bool _snapshot_entry_painterly_matches(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, int p_index, float p_marker) {
    const float expected_normal_y = p_marker > 0.0f ? 1.0f : -1.0f;
    if (!Math::is_equal_approx(p_snapshot.normals[p_index * 3 + 1], expected_normal_y)) {
        return false;
    }
    if (!Math::is_equal_approx(p_snapshot.brush_axes[p_index * 2 + 0], p_marker)) {
        return false;
    }
    return Math::is_equal_approx(p_snapshot.stroke_ages[p_index], p_marker + float(p_index) * 0.25f);
}

static bool _snapshot_entry_matches_pattern(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, int p_index, float p_marker, const Color &p_color) {
    return _snapshot_entry_core_matches(p_snapshot, p_index, p_marker, p_color) &&
            _snapshot_entry_metadata_matches(p_snapshot, p_index, p_marker) &&
            _snapshot_entry_painterly_matches(p_snapshot, p_index, p_marker);
}

static bool _snapshot_matches_pattern(const GaussianSplatAsset::PayloadSnapshot &p_snapshot, float p_marker, const Color &p_color) {
    constexpr int splat_count = 8;
    if (!_snapshot_has_expected_core_sizes(p_snapshot, splat_count)) {
        return false;
    }
    if (!_snapshot_has_expected_painterly_sizes(p_snapshot, splat_count)) {
        return false;
    }

    for (int i = 0; i < splat_count; i++) {
        if (!_snapshot_entry_matches_pattern(p_snapshot, i, p_marker, p_color)) {
            return false;
        }
    }
    return true;
}

#ifdef THREADS_ENABLED
struct _AssetSnapshotCopyRaceContext {
    Ref<GaussianSplatAsset> target;
    Ref<GaussianSplatAsset> asset_a;
    Ref<GaussianSplatAsset> asset_b;
    std::atomic<bool> stop{ false };
    std::atomic<int> invalid_snapshots{ 0 };
    Semaphore writer_begin;
    Semaphore writer_done;
    Semaphore reader_begin;
    Semaphore reader_done;
};

static void _asset_snapshot_writer_thread(void *p_userdata) {
    _AssetSnapshotCopyRaceContext *ctx = static_cast<_AssetSnapshotCopyRaceContext *>(p_userdata);
    if (!ctx || ctx->target.is_null() || ctx->asset_a.is_null() || ctx->asset_b.is_null()) {
        return;
    }

    int iteration = 0;
    while (true) {
        ctx->writer_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        Ref<Resource> source = (iteration % 2) == 0 ? ctx->asset_a : ctx->asset_b;
        ctx->target->copy_from(source);
        iteration++;
        ctx->writer_done.post();
    }
}

static void _asset_snapshot_reader_thread(void *p_userdata) {
    _AssetSnapshotCopyRaceContext *ctx = static_cast<_AssetSnapshotCopyRaceContext *>(p_userdata);
    if (!ctx || ctx->target.is_null()) {
        return;
    }

    while (true) {
        ctx->reader_begin.wait();
        if (ctx->stop.load(std::memory_order_acquire)) {
            break;
        }

        const GaussianSplatAsset::PayloadSnapshot snapshot = ctx->target->capture_payload_snapshot();
        const bool matches_a = _snapshot_matches_pattern(snapshot, 11.0f, Color(1, 0, 0, 1));
        const bool matches_b = _snapshot_matches_pattern(snapshot, -23.0f, Color(0, 1, 0, 1));
        if (!matches_a && !matches_b) {
            ctx->invalid_snapshots.fetch_add(1, std::memory_order_relaxed);
        }
        ctx->reader_done.post();
    }
}
#endif

} // namespace

TEST_CASE("[GaussianSplatting][DataAuthority] Bulk setters invalidate octree and animation caches") {
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());

    // Build an octree anchored to the initial positions and query a cell that
    // matches splat[0] at (-2, 0, 0). Query should return at least one hit.
    data->build_octree();
    TypedArray<int> hits_before = data->query_octree(AABB(Vector3(-3, -1, -1), Vector3(2, 2, 2)));
    CHECK_MESSAGE(hits_before.size() > 0, "Octree should index splat 0 at (-2,0,0) prior to bulk mutation.");

    // Move every splat far away. Under the pre-fix behavior this did not
    // invalidate the octree, so query_octree() would still report hits near
    // the old positions. The hardening flushes the octree on bulk set_positions.
    PackedVector3Array far_positions;
    far_positions.resize(4);
    far_positions.set(0, Vector3(500, 500, 500));
    far_positions.set(1, Vector3(501, 500, 500));
    far_positions.set(2, Vector3(500, 501, 500));
    far_positions.set(3, Vector3(500, 500, 501));
    data->set_positions(far_positions);

    TypedArray<int> hits_after_move = data->query_octree(AABB(Vector3(-3, -1, -1), Vector3(2, 2, 2)));
    CHECK_MESSAGE(hits_after_move.size() == 0,
            "set_positions() must invalidate the octree so old positions do not leak.");

    // Rebuild octree at the new location; the new query should now find splats.
    data->build_octree();
    TypedArray<int> hits_new = data->query_octree(AABB(Vector3(499, 499, 499), Vector3(3, 3, 3)));
    CHECK(hits_new.size() > 0);

    // set_scales/set_rotations/set_opacities all feed derived bounds+caches
    // too; verify they at least bump the content revision so dependent
    // consumers re-read the payload.
    const uint64_t revision_before_scales = data->get_content_revision();
    PackedVector3Array scales;
    scales.resize(4);
    for (int i = 0; i < 4; i++) {
        scales.set(i, Vector3(2.0f, 2.0f, 2.0f));
    }
    data->set_scales(scales);
    CHECK(data->get_content_revision() > revision_before_scales);

    const uint64_t revision_before_opacities = data->get_content_revision();
    PackedFloat32Array opacities;
    opacities.resize(4);
    for (int i = 0; i < 4; i++) {
        opacities.set(i, 0.25f);
    }
    data->set_opacities(opacities);
    CHECK(data->get_content_revision() > revision_before_opacities);

    const uint64_t revision_before_rotations = data->get_content_revision();
    TypedArray<Quaternion> rotations;
    rotations.resize(4);
    for (int i = 0; i < 4; i++) {
        rotations[i] = Quaternion(Vector3(0, 1, 0), Math::deg_to_rad(45.0f));
    }
    data->set_rotations(rotations);
    CHECK(data->get_content_revision() > revision_before_rotations);
}

TEST_CASE("[GaussianSplatting][DataAuthority] Animation sampling is non-destructive") {
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());

    // Snapshot the base positions so we can verify they never shift under us.
    Vector<Vector3> baseline_positions;
    {
        const Gaussian *g = data->get_gaussians();
        REQUIRE(g != nullptr);
        baseline_positions.resize(data->get_count());
        for (int i = 0; i < data->get_count(); i++) {
            baseline_positions.write[i] = g[i].position;
        }
    }

    Ref<GaussianSplatting::GaussianAnimationStateMachine> anim;
    anim.instantiate();
    anim->set_splat_count(data->get_count());
    data->set_animation_state_machine(anim);

    // With no animated tracks, get_animated_position() is a pass-through.
    for (int i = 0; i < data->get_count(); i++) {
        const Vector3 sampled = data->get_animated_position(i, 0.5f);
        CHECK(sampled.is_equal_approx(baseline_positions[i]));
    }

    // Core invariant: calling get_animated_* must NOT mutate the base payload.
    const Gaussian *after_sample = data->get_gaussians();
    REQUIRE(after_sample != nullptr);
    for (int i = 0; i < data->get_count(); i++) {
        CHECK_MESSAGE(after_sample[i].position.is_equal_approx(baseline_positions[i]),
                "Sampling animation must leave the base GaussianData position untouched.");
    }

    // apply_animation_at_time() has been removed from the public API entirely.
    // This compile-time check pins that removal in place: if a future change
    // adds the symbol back, this static_assert stops being well-formed.
    static_assert(!_HasApplyAnimationAtTime<::GaussianData>::value,
            "GaussianData::apply_animation_at_time() must stay removed -- animation is a non-destructive view.");
}

TEST_CASE("[GaussianSplatting][DataAuthority] Animated accessors tolerate concurrent payload mutation") {
#ifndef THREADS_ENABLED
    MESSAGE("Skipping - THREADS_ENABLED is not enabled in this build");
    return;
#else
    constexpr uint32_t splat_count = 64;
    LocalVector<Gaussian> splats;
    splats.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        splats[i] = _make_authority_test_splat(Vector3(float(i), 0.0f, 0.0f),
                Color(float(i % 7) / 6.0f, 0.25f, 0.75f, 0.8f),
                Vector3(1.0f, 1.0f, 1.0f));
    }

    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(splats);

    Ref<GaussianSplatting::GaussianAnimationStateMachine> animation;
    animation.instantiate();
    animation->set_splat_count(splat_count);
    data->set_animation_state_machine(animation);

    PackedVector3Array positions_a;
    PackedVector3Array positions_b;
    PackedVector3Array scales_a;
    PackedVector3Array scales_b;
    PackedFloat32Array opacities_a;
    PackedFloat32Array opacities_b;
    TypedArray<Quaternion> rotations_a;
    TypedArray<Quaternion> rotations_b;
    positions_a.resize(splat_count);
    positions_b.resize(splat_count);
    scales_a.resize(splat_count);
    scales_b.resize(splat_count);
    opacities_a.resize(splat_count);
    opacities_b.resize(splat_count);
    rotations_a.resize(splat_count);
    rotations_b.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        const int index = static_cast<int>(i);
        positions_a.set(index, Vector3(float(i), 1.0f, 2.0f));
        positions_b.set(index, Vector3(-float(i), 3.0f, 4.0f));
        scales_a.set(index, Vector3(1.0f, 1.5f, 2.0f));
        scales_b.set(index, Vector3(2.0f, 1.0f, 0.5f));
        opacities_a.set(index, 0.25f);
        opacities_b.set(index, 0.85f);
        rotations_a[index] = Quaternion();
        rotations_b[index] = Quaternion(Vector3(0.0f, 1.0f, 0.0f), Math::deg_to_rad(30.0f));
    }

    _AnimatedAccessorRaceContext ctx;
    ctx.data = data;
    ctx.splat_count = splat_count;

    Thread reader_thread;
    const Thread::ID reader_id = reader_thread.start(_animated_accessor_reader_thread, &ctx);
    REQUIRE(reader_id != Thread::UNASSIGNED_ID);

    for (uint32_t iteration = 0; iteration < 96; iteration++) {
        const bool use_a = (iteration % 2) == 0;
        data->set_animation_enabled(use_a);
        data->set_animation_state_machine((iteration % 8) == 0 ? Ref<GaussianSplatting::GaussianAnimationStateMachine>() : animation);
        data->set_positions(use_a ? positions_a : positions_b);
        data->set_scales(use_a ? scales_a : scales_b);
        data->set_opacities(use_a ? opacities_a : opacities_b);
        data->set_rotations(use_a ? rotations_a : rotations_b);
        CHECK(data->get_gaussian(0).position.is_finite());
        Thread::yield();
    }

    ctx.stop.store(true, std::memory_order_release);
    reader_thread.wait_to_finish();

    CHECK(ctx.invalid_samples.load(std::memory_order_relaxed) == 0);
#endif
}

TEST_CASE("[GaussianSplatting][DataAuthority] GaussianSplatAsset packed setters are sealed at runtime") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    // Fresh assets still populate through the public property surface used by
    // import/deserialization before first runtime promotion.
    PackedFloat32Array bootstrap_positions;
    bootstrap_positions.resize(12);
    for (int i = 0; i < bootstrap_positions.size(); i++) {
        bootstrap_positions.set(i, float(i));
    }
    asset->set_splat_count(4);
    asset->set_positions(bootstrap_positions);
    CHECK(asset->get_positions().size() == bootstrap_positions.size());

    // Populate the asset through the legitimate persistence boundary.
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());
    const int splat_count = data->get_count();
    REQUIRE(splat_count > 0);
    REQUIRE(asset->populate_from_gaussian_data(data) == OK);
    CHECK(asset->get_splat_count() == (uint32_t)splat_count);

    // Asset is now sealed: handing out the runtime GaussianData keeps the seal.
    Ref<::GaussianData> runtime = asset->get_gaussian_data();
    REQUIRE(runtime.is_valid());
    CHECK(runtime->get_count() == splat_count);

    // Capture the asset positions buffer before attempting a sealed mutation.
    PackedFloat32Array before_positions = asset->get_positions();

    // Attempting an asset packed setter while sealed must NOT change the
    // underlying payload. We accept either a hard-fail or a silent no-op --
    // the required outcome is that the asset does not treat itself as a live
    // mutable runtime authority.
    PackedFloat32Array mutated;
    mutated.resize(before_positions.size());
    for (int i = 0; i < mutated.size(); i++) {
        mutated.set(i, 999.0f);
    }
    {
        // Suppress the expected ERR_PRINT so the test log stays clean.
        ERR_PRINT_OFF;
        asset->set_positions(mutated);
        ERR_PRINT_ON;
    }
    PackedFloat32Array after_positions = asset->get_positions();
    REQUIRE(after_positions.size() == before_positions.size());
    bool positions_unchanged = true;
    for (int i = 0; i < after_positions.size(); i++) {
        if (!Math::is_equal_approx(after_positions[i], before_positions[i])) {
            positions_unchanged = false;
            break;
        }
    }
    CHECK_MESSAGE(positions_unchanged,
            "Sealed GaussianSplatAsset must reject runtime set_positions() writes.");

    // Public same-count "reseat" must stay rejected once runtime authority
    // has been handed out. Reopening the setters here would recreate split
    // authority between the asset arrays and the live GaussianData.
    {
        ERR_PRINT_OFF;
        asset->set_splat_count(splat_count);
        asset->set_positions(mutated);
        ERR_PRINT_ON;
    }
    PackedFloat32Array after_rejected_reseat = asset->get_positions();
    REQUIRE(after_rejected_reseat.size() == before_positions.size());
    bool reseat_rejected = true;
    for (int i = 0; i < after_rejected_reseat.size(); i++) {
        if (!Math::is_equal_approx(after_rejected_reseat[i], before_positions[i])) {
            reseat_rejected = false;
            break;
        }
    }
    CHECK_MESSAGE(reseat_rejected,
            "Sealed GaussianSplatAsset must reject same-count set_splat_count() reopen attempts.");

    const Gaussian *runtime_ptr = runtime->get_gaussians();
    REQUIRE(runtime_ptr != nullptr);
    CHECK(runtime_ptr[0].position.is_equal_approx(Vector3(-2, 0, 0)));

    // populate_from_gaussian_data() is the legitimate override path and must
    // work regardless of prior seal state.
    Ref<::GaussianData> rewrite_data;
    rewrite_data.instantiate();
    rewrite_data->set_gaussians(_make_authority_test_cloud());
    REQUIRE(asset->populate_from_gaussian_data(rewrite_data) == OK);
}

TEST_CASE("[GaussianSplatting][DataAuthority] GaussianSplatAsset payload snapshots stay coherent across copy_from reloads") {
#ifndef THREADS_ENABLED
    MESSAGE("Skipping - THREADS_ENABLED is not enabled in this build");
    return;
#else
    Ref<GaussianSplatAsset> asset_a = _make_asset_snapshot_pattern(11.0f, Color(1, 0, 0, 1));
    Ref<GaussianSplatAsset> asset_b = _make_asset_snapshot_pattern(-23.0f, Color(0, 1, 0, 1));
    Ref<GaussianSplatAsset> target = _make_asset_snapshot_pattern(11.0f, Color(1, 0, 0, 1));

    // Seal the target first so copy_from() also proves the hot-reload unseal
    // path is serialized against snapshot reads.
    Ref<::GaussianData> runtime = target->get_gaussian_data();
    REQUIRE(runtime.is_valid());

    _AssetSnapshotCopyRaceContext ctx;
    ctx.target = target;
    ctx.asset_a = asset_a;
    ctx.asset_b = asset_b;

    Thread writer_thread;
    Thread reader_thread;
    writer_thread.start(_asset_snapshot_writer_thread, &ctx);
    reader_thread.start(_asset_snapshot_reader_thread, &ctx);

    constexpr uint32_t iterations = 128;
    for (uint32_t i = 0; i < iterations; i++) {
        if ((i % 2) == 0) {
            ctx.writer_begin.post();
            Thread::yield();
            ctx.reader_begin.post();
        } else {
            ctx.reader_begin.post();
            Thread::yield();
            ctx.writer_begin.post();
        }
        ctx.writer_done.wait();
        ctx.reader_done.wait();
    }

    ctx.stop.store(true, std::memory_order_release);
    ctx.writer_begin.post();
    ctx.reader_begin.post();
    writer_thread.wait_to_finish();
    reader_thread.wait_to_finish();

    CHECK_MESSAGE(ctx.invalid_snapshots.load(std::memory_order_acquire) == 0,
            "Payload snapshots must observe one complete asset generation, never mixed arrays/metadata during copy_from().");
#endif
}

TEST_CASE("[GaussianSplatting][DataAuthority] Raw storage accessors compile-survivable and main-thread safe") {
    // This test merely exercises the accessor to confirm the debug diagnostic
    // is a no-op on the main thread (fires at most once off-main-thread in
    // dev builds). The important outcome is that legitimate main-thread
    // consumers keep working after the hardening.
    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(_make_authority_test_cloud());

    const LocalVector<Gaussian> &storage = data->get_gaussian_storage();
    CHECK(storage.size() == 4u);

    const Gaussian *ptr = data->get_gaussians();
    REQUIRE(ptr != nullptr);
    CHECK(ptr[0].position.is_equal_approx(Vector3(-2, 0, 0)));
}

// ---------------------------------------------------------------------------
// #798 review round 2 -- failure-path contracts.
//
// Both cases below sit on branches that are reachable ONLY when an allocation
// fails, so they are written as CONTRACTS with two arms rather than as tests
// that assert a failure they cannot provoke. Each arm carries a real assertion,
// so neither is vacuous: the success arm verifies the normal payload, and the
// failure arm verifies the fail-closed state. The failure arm is what a narrow
// mutation (forcing the corresponding allocation to fail) exercises, exactly as
// #799 mutation-proved build_hierarchy().
//
// NOTE: guard-and-return, never REQUIRE-then-use. disable_exceptions=True means
// DOCTEST_CONFIG_NO_EXCEPTIONS, so REQUIRE does NOT abort the case; #799 hit
// 602 CrashHandlerExceptions that way.
// ---------------------------------------------------------------------------

namespace {

// Distinct from _make_authority_test_splat(): the normal is (1,0,0), NOT the
// (0,1,0) that gaussian_splat_merge_sources() substitutes when a source's
// normal lane is short. That difference is the whole point -- it is what makes
// "the merge silently defaulted this lane" observable instead of invisible.
LocalVector<Gaussian> _make_failure_contract_cloud(int p_count) {
    LocalVector<Gaussian> splats;
    splats.resize(p_count);
    for (int i = 0; i < p_count; i++) {
        Gaussian g;
        g.position = Vector3(float(i), 0, 0);
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1, 1, 1, 1);
        g.normal = Vector3(1, 0, 0);
        g.area = 1.0f;
        g.brush_axes = Vector2(1.0f, 1.0f);
        g.stroke_age = 0.0f;
        g.painterly_meta = 0;
        g.render_meta = 0;
        splats[i] = g;
    }
    return splats;
}

} // namespace

TEST_CASE("[GaussianSplatting][DataAuthority] Failed populate_from_gaussian_data leaves a coherent empty asset, never a mixed one") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    // Seed a payload first, so a failed REWRITE has a previous state it could
    // leak. This is the case that matters: populate_from_gaussian_data() has
    // already replaced splat_count and the SH term counts and re-run
    // _ensure_buffer_sizes() over every lane before the lane pointers are taken.
    Ref<::GaussianData> seed;
    seed.instantiate();
    seed->set_gaussians(_make_failure_contract_cloud(4));
    if (asset->populate_from_gaussian_data(seed) != OK) {
        FAIL("seed populate_from_gaussian_data() must succeed");
        return;
    }
    if (asset->get_splat_count() != 4u) {
        FAIL("seed populate must yield 4 splats");
        return;
    }

    // Rewrite with a DIFFERENT count, so a surviving "new count + unwritten
    // lanes" mixed state is distinguishable from both the old and the new one.
    Ref<::GaussianData> rewrite;
    rewrite.instantiate();
    rewrite->set_gaussians(_make_failure_contract_cloud(6));

    Error err;
    {
        ERR_PRINT_OFF;
        err = asset->populate_from_gaussian_data(rewrite);
        ERR_PRINT_ON;
    }

    if (err == OK) {
        // Success arm: the rewrite is fully applied.
        CHECK(asset->get_splat_count() == 6u);
        CHECK(asset->get_positions().size() == 6 * 3);
        CHECK(asset->get_opacities().size() == 6);
    } else {
        // Failure arm -- the round-2 contract. Restoring payload_sealed alone
        // used to leave splat_count == 6 with lanes at a mixture of old, newly
        // sized and empty lengths, and callers such as
        // _register_instance_in_director() log the Error and register the asset
        // anyway. The asset must instead be coherently EMPTY: splat_count == 0
        // is the shape every getter already early-outs on.
        CHECK_MESSAGE(asset->get_splat_count() == 0u,
                "A failed populate_from_gaussian_data() must reset to an empty asset, not leave a mixed one.");
        CHECK(asset->get_positions().is_empty());
        CHECK(asset->get_opacities().is_empty());
        CHECK(asset->get_spherical_harmonics_buffer().is_empty());
    }
}

TEST_CASE("[GaussianSplatting][DataAuthority] Merge either preserves every source lane or refuses the merge") {
    Vector<GaussianSplatMergeSource> sources;
    for (int s = 0; s < 2; s++) {
        Ref<GaussianSplatAsset> asset;
        asset.instantiate();
        Ref<::GaussianData> data;
        data.instantiate();
        data->set_gaussians(_make_failure_contract_cloud(3));
        if (asset->populate_from_gaussian_data(data) != OK) {
            FAIL("merge source populate_from_gaussian_data() must succeed");
            return;
        }
        GaussianSplatMergeSource src;
        src.asset = asset;
        src.transform = Transform3D(); // identity: world normal == local normal
        sources.push_back(src);
    }

    GaussianSplatMergeResult out;
    bool merged;
    {
        ERR_PRINT_OFF;
        merged = gaussian_splat_merge_sources(sources, 10.0f, out);
        ERR_PRINT_ON;
    }

    if (merged) {
        // Success arm: a merge that reports success must carry the SOURCE data
        // through, not per-lane defaults. Under a mutation that makes one
        // source's normal getter fail, the pre-round-2 guard (positions and
        // scales only) let the merge report success while substituting the
        // (0,1,0) fallback for every splat -- which is precisely what this
        // assertion catches.
        if (out.data.is_null()) {
            FAIL("a successful merge must produce GaussianData");
            return;
        }
        if (out.data->get_count() != 6) {
            FAIL("a successful merge of 2x3 splats must produce 6 splats");
            return;
        }
        const Gaussian *merged_splats = out.data->get_gaussians();
        if (merged_splats == nullptr) {
            FAIL("a successful merge must expose splat storage");
            return;
        }
        bool normals_preserved = true;
        for (int i = 0; i < 6; i++) {
            if (!merged_splats[i].normal.is_equal_approx(Vector3(1, 0, 0))) {
                normals_preserved = false;
                break;
            }
        }
        CHECK_MESSAGE(normals_preserved,
                "A merge reporting success must carry each source's normals through, "
                "not substitute the (0,1,0) default for a lane whose allocation failed.");
    } else {
        // Failure arm -- the round-2 contract: refuse the merge outright rather
        // than emitting geometry built from defaults.
        // Extra parens are load-bearing: a top-level || inside CHECK trips doctest's
        // expression decomposition ("Expression Too Complex", doctest.h:1545).
        CHECK_MESSAGE((out.data.is_null() || out.data->get_count() == 0),
                "A refused merge must not hand back partially-defaulted geometry.");
    }
}

} // namespace TestGaussianSplatting
