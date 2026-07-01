#pragma once

#include "test_macros.h"
#include "../animation/animation_state_machine.h"
#include "core/math/math_funcs.h"

TEST_CASE("[GaussianSplatting][Animation] linear keyframe interpolation") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("linear", 1.0f);
    state_machine.set_splat_count(1);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.0f);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 10.0f);
    state_machine.play(clip_index);

    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 0.0f), 0.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 0.5f), 5.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 1.0f), 10.0f));

    // Test extrapolation (should clamp)
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, -0.5f), 0.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 1.5f), 10.0f));
}

TEST_CASE("[GaussianSplatting][Animation] Vector3 keyframe interpolation") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("vector", 1.0f);
    state_machine.set_splat_count(1);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 0.0f, Vector3(0, 0, 0));
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 1.0f, Vector3(10, 20, 30));
    state_machine.play(clip_index);

    Vector3 mid = state_machine.sample_position(0, 0.5f);
    CHECK(mid.is_equal_approx(Vector3(5, 10, 15)));
}

TEST_CASE("[GaussianSplatting][Animation] splat-indexed keyframes sample per-splat values") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("per_splat_position", 1.0f);
    state_machine.set_splat_count(3);

    PackedVector3Array start_positions;
    start_positions.push_back(Vector3(0, 0, 0));
    start_positions.push_back(Vector3(10, 0, 0));
    start_positions.push_back(Vector3(20, 0, 0));

    PackedVector3Array end_positions;
    end_positions.push_back(Vector3(10, 0, 0));
    end_positions.push_back(Vector3(20, 0, 0));
    end_positions.push_back(Vector3(30, 0, 0));

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 0.0f, start_positions);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 1.0f, end_positions);
    state_machine.play(clip_index);

    CHECK(state_machine.sample_position(0, 0.5f).is_equal_approx(Vector3(5, 0, 0)));
    CHECK(state_machine.sample_position(1, 0.5f).is_equal_approx(Vector3(15, 0, 0)));
    CHECK(state_machine.sample_position(2, 0.5f).is_equal_approx(Vector3(25, 0, 0)));
}

TEST_CASE("[GaussianSplatting][Animation] mixed scalar and per-splat keyframes only reject missing indices in sampled window") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("mixed_window", 2.0f);
    state_machine.set_splat_count(2);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 0.0f, Vector3(0, 0, 0));
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 1.0f, Vector3(10, 0, 0));

    PackedVector3Array per_splat_tail;
    per_splat_tail.push_back(Vector3(30, 0, 0)); // Only splat 0 provided.
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_POSITION, 2.0f, per_splat_tail);
    state_machine.play(clip_index);

    // Sample window [0, 1] uses scalar keyframes; missing splat-1 entry at t=2 should not invalidate this.
    CHECK(state_machine.sample_position(1, 0.5f).is_equal_approx(Vector3(5, 0, 0)));
    CHECK(state_machine.sample_position(1, 1.0f).is_equal_approx(Vector3(10, 0, 0)));

    // Sample window [1, 2] includes a per-splat keyframe without splat-1 data and should fail gracefully.
    CHECK(state_machine.sample_position(1, 1.5f).is_equal_approx(Vector3()));
}

TEST_CASE("[GaussianSplatting][Animation] global track sampling stays index-agnostic in batch APIs") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("batch", 1.0f);
    state_machine.set_splat_count(4);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.0f);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 1.0f);
    state_machine.play(clip_index);

    const float sample_time = 0.25f;
    const float expected = state_machine.sample_opacity(0, sample_time);
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(3, sample_time), expected));

    LocalVector<float> opacities;
    state_machine.sample_opacities_batch(opacities, sample_time);

    REQUIRE(opacities.size() == 4);
    for (uint32_t i = 0; i < opacities.size(); i++) {
        CHECK(Math::is_equal_approx(opacities[i], expected));
    }
}

TEST_CASE("[GaussianSplatting][Animation] integer opacity keyframes sample consistently across scalar and batch APIs") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("int_opacity", 1.0f);
    state_machine.set_splat_count(3);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 2);
    state_machine.play(clip_index);

    const float sample_time = 0.5f;
    const float expected = state_machine.sample_opacity(0, sample_time);
    CHECK(Math::is_equal_approx(expected, 1.0f));
    CHECK(Math::is_equal_approx(state_machine.sample_opacity(2, sample_time), expected));

    LocalVector<float> opacities;
    state_machine.sample_opacities_batch(opacities, sample_time);
    REQUIRE(opacities.size() == 3);
    for (uint32_t i = 0; i < opacities.size(); i++) {
        CHECK(Math::is_equal_approx(opacities[i], expected));
    }
}

TEST_CASE("[GaussianSplatting][Animation] mixed numeric opacity keyframes interpolate linearly") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("mixed_numeric_opacity", 1.0f);
    state_machine.set_splat_count(1);

    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 0.0f, 0);
    state_machine.add_keyframe(clip_index, ANIMATION_PROPERTY_OPACITY, 1.0f, 2.0f);
    state_machine.play(clip_index);

    CHECK(Math::is_equal_approx(state_machine.sample_opacity(0, 0.5f), 1.0f));
}
TEST_CASE("[GaussianSplatting][Animation] state machine transitions") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine state_machine;
    int clip_index = state_machine.add_clip("idle", 1.0f);
    state_machine.set_splat_count(1);

    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_STOPPED);

    state_machine.play(clip_index);
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_PLAYING);

    state_machine.pause();
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_PAUSED);

    state_machine.stop();
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_STOPPED);

    state_machine.seek(0.25f);
    CHECK_EQ(state_machine.get_state(), ANIMATION_STATE_SEEKING);
}

TEST_CASE("[GaussianSplatting][Animation] cubic-bezier easing uses key-relative handles (regression)") {
    using namespace GaussianSplatting;

    // Drive the private easing through the PUBLIC sampling path, feeding handles
    // in the DOCUMENTED Godot key-relative convention (see docs/features/
    // animation.md): handle.x is an offset in SECONDS from the owning key's time,
    // handle.y an offset on the normalized-progress line (every documented
    // example uses handle.y = 0.0). With keyframes at t=0 and t=1 the segment
    // duration is 1.0s, so handle.x is already in normalized units.
    //
    // add_keyframe_bezier(clip, prop, time, value, in_handle, out_handle):
    //   keyframe-A's out_handle and keyframe-B's in_handle bound the segment, and
    //   interpolate() converts them to absolute control points P1/P2:
    //     P1 = (out_handle.x / time_diff,     out_handle.y)
    //     P2 = (1 + in_handle.x / time_diff, 1 + in_handle.y)
    //   With a=0, b=1 the sampled value is exactly the easing curve's Y output.
    auto eval_bezier = [](const Vector2& out_handle_a, const Vector2& in_handle_b, float a, float b, float t) -> float {
        GaussianAnimationStateMachine sm;
        int clip = sm.add_clip("bezier", 1.0f);
        sm.set_splat_count(1);
        // A (t=0): its out_handle bounds the segment; A.in_handle is unused here.
        sm.add_keyframe_bezier(clip, ANIMATION_PROPERTY_OPACITY, 0.0f, a, Vector2(0, 0), out_handle_a);
        // B (t=1): its in_handle bounds the segment; B.out_handle is unused here.
        sm.add_keyframe_bezier(clip, ANIMATION_PROPERTY_OPACITY, 1.0f, b, in_handle_b, Vector2(0, 0));
        sm.play(clip);
        return sm.sample_opacity(0, t);
    };

    // 1) SYMMETRIC ease-in-out: out_handle=(0.3,0), in_handle=(-0.3,0)
    //    -> P1=(0.3,0), P2=(0.7,1). NOTE the NEGATIVE in_handle.x, impossible for
    //    an absolute control point -- this is what proves the relative convention.
    const Vector2 sym_out(0.3f, 0.0f);
    const Vector2 sym_in(-0.3f, 0.0f);
    // By symmetry the eased midpoint is exactly the linear midpoint.
    CHECK(Math::abs(eval_bezier(sym_out, sym_in, 0.0f, 1.0f, 0.5f) - 0.5f) < 0.02f);
    // Proper S-curve: slow at the start (below the diagonal), slow at the end
    // (above the diagonal).
    CHECK(eval_bezier(sym_out, sym_in, 0.0f, 1.0f, 0.25f) < 0.25f);
    CHECK(eval_bezier(sym_out, sym_in, 0.0f, 1.0f, 0.75f) > 0.75f);

    // 2) ENDPOINTS map exactly to the keyframe values.
    CHECK(Math::is_equal_approx(eval_bezier(sym_out, sym_in, 2.0f, 8.0f, 0.0f), 2.0f));
    CHECK(Math::is_equal_approx(eval_bezier(sym_out, sym_in, 2.0f, 8.0f, 1.0f), 8.0f));

    // 3) DISCRIMINATOR -- asymmetric ease-in: out_handle=(0.3,0), in_handle=(0,0)
    //    -> P1=(0.3,0), P2=(1,1) (CSS-style cubic-bezier(0.3,0,1,1)), a slow start
    //    whose midpoint (~0.38) sits BELOW the linear midpoint 0.5. Removing the
    //    relative->absolute conversion feeds the raw handles as control points
    //    (P2=(0,0)) and the solved Y jumps to ~0.87, so the strict "< 0.45" check
    //    pins the conversion.
    const Vector2 in_zero(0.0f, 0.0f);
    const float eased_mid = eval_bezier(sym_out, in_zero, 0.0f, 1.0f, 0.5f);
    CHECK(eased_mid < 0.45f);

    // 4) MONOTONIC: an ease curve never decreases across the timeline.
    float prev = eval_bezier(sym_out, in_zero, 0.0f, 1.0f, 0.0f);
    for (int i = 1; i <= 10; i++) {
        const float t = static_cast<float>(i) / 10.0f;
        const float cur = eval_bezier(sym_out, in_zero, 0.0f, 1.0f, t);
        CHECK(cur >= prev - 1e-4f);
        prev = cur;
    }

    // 5) NON-UNIT VALUE RANGE, handle.y = 0: the symmetric S-curve must still
    //    land its midpoint at the linear midpoint, now 5.0 on a 0 -> 10 segment
    //    (value delta 10). This guards that scaling handle.y by the value delta
    //    leaves the handle.y == 0 case untouched.
    CHECK(Math::abs(eval_bezier(sym_out, sym_in, 0.0f, 10.0f, 0.5f) - 5.0f) < 0.2f);

    // 6) DISCRIMINATOR for VALUE-DELTA normalization of handle.y on a NON-UNIT
    //    range. out_handle=(0.0, 2.5), in_handle=(0,0) on a 0 -> 10 segment
    //    (value delta 10). Godot treats handle.y as a VALUE offset, so the
    //    normalized control point is P1.y = 2.5 / 10 = 0.25, giving
    //    P1=(0,0.25), P2=(1,1) -- an X(s)=smoothstep curve whose Y at the t=0.5
    //    midpoint is 0.59375, i.e. an eased value of 5.9375 (a mild, in-range
    //    overshoot above the linear 5.0).
    //
    //    If handle.y were left UN-normalized (the bug under review), P1.y would be
    //    the raw 2.5, the curve would balloon, and the eased value at t=0.5 jumps
    //    to ~14.375 -- a gross overshoot well past the segment's own 10.0 ceiling.
    //    The tight band below therefore FAILS unless handle.y is divided by the
    //    value delta.
    const Vector2 lift_out(0.0f, 2.5f);
    const float lifted_mid = eval_bezier(lift_out, in_zero, 0.0f, 10.0f, 0.5f);
    CHECK(lifted_mid < 10.0f);                       // no out-of-range overshoot
    CHECK(lifted_mid > 5.0f);                         // lifted above the linear midpoint
    CHECK(Math::abs(lifted_mid - 5.9375f) < 0.1f);    // value-normalized expectation
}
