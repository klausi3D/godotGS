#pragma once

// Safety regression tests for the animation state machine (audit #597/#598/#599).
//
// #597 - Every ClassDB-bound method that indexes `clips` from a script-supplied
//        index must return safely on an out-of-range index. Before the fix the
//        index check lived in a `void` helper whose `ERR_FAIL_INDEX` returned
//        only from the helper; each caller then fell through into
//        `clips[p_index]`, where `LocalVector::operator[]` fires
//        `CRASH_BAD_UNSIGNED_INDEX` -> `GENERATE_TRAP()` and ABORTS the whole
//        process (in every build, including template_release). A single call
//        such as `GaussianAnimationStateMachine.new().get_clip_name(0)` killed
//        the engine. The fix moves the bound-index check to a caller-local
//        `ERR_FAIL_INDEX[_V]` that both logs and returns BEFORE any indexing, so
//        the calls below now return their documented defaults instead of
//        trapping. The mere fact that these tests run to completion (rather than
//        aborting the test binary at the first bad-index call) is the proof.
//        Expected error spam is silenced with ERR_PRINT_OFF/ON.
//
// #598 - A looping clip with duration 0 reached `fmod(current_time, 0)` -> NaN,
//        which then poisoned every later comparison and froze playback. The fix
//        floors duration at all producers and guards the loop wrap.
//
// #598 (adjacent gap, closed in the same hardening pass) - the original fix
//        used `MAX(MIN_CLIP_DURATION, p_duration)` to floor a zero/negative
//        duration. MAX is `m_a > m_b ? m_a : m_b`, and any comparison against
//        NaN is false, so `MAX(MIN_CLIP_DURATION, NaN)` returned the NaN
//        argument unchanged (a +Infinity duration passed through the same
//        way). A non-finite clip.duration makes every later comparison
//        (`clip.duration > 0.0f`, `current_time >= clip.duration`) evaluate
//        false, so update() never wraps and never stops the clip: playback
//        silently freezes forever, with no crash and no error. The fix adds
//        an explicit `Math::is_finite()` check at every duration producer
//        (add_clip / set_clip_duration / from_dict) and at the analogous
//        switch-delay producer (switch_to_clip_delayed), which had the
//        identical MAX-vs-NaN gap in its own transition_duration.
//
// #599 - `blend_to_clip()` never cross-faded: it enqueues a target that, on
//        completion, HARD-SWITCHES `current_clip_index`. It is renamed to
//        `switch_to_clip_delayed()` (with `blend_to_clip` kept as a deprecated
//        alias) and documented truthfully. These tests pin the delayed-hard-cut
//        behavior: mid-transition output is 100% the source clip, never a blend.

#include "test_macros.h"
#include "../animation/animation_state_machine.h"
#include "core/math/math_funcs.h"

#include <limits>

TEST_CASE("[GaussianSplatting][Animation] bad clip index returns safely from every bound method (#597)") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine sm;
    sm.set_splat_count(4);
    const AnimationProperty prop = ANIMATION_PROPERTY_POSITION;

    // No clips exist: index 0 (and any negative index) is out of range. Pre-fix
    // each of these calls aborted the process; post-fix they return defaults.
    ERR_PRINT_OFF;

    // Value-returning methods must yield their documented default.
    CHECK(sm.get_clip_name(0) == String());
    CHECK(Math::is_equal_approx(sm.get_clip_duration(0), 0.0f));
    CHECK_FALSE(sm.get_clip_looping(0));
    CHECK_FALSE(sm.has_track(0, prop));
    CHECK_EQ(sm.get_keyframe_count(0, prop), 0);
    CHECK(Math::is_equal_approx(sm.get_keyframe_time(0, prop, 0), 0.0f));
    CHECK(sm.get_keyframe_value(0, prop, 0).get_type() == Variant::NIL);
    CHECK(Math::is_equal_approx(sm.get_clip_weight(0), 0.0f));

    // Negative indices are equally out of range.
    CHECK(sm.get_clip_name(-1) == String());
    CHECK(Math::is_equal_approx(sm.get_clip_duration(-3), 0.0f));
    CHECK_FALSE(sm.get_clip_looping(-1));
    CHECK(Math::is_equal_approx(sm.get_clip_weight(-2), 0.0f));

    // Void methods must not crash and must not mutate state on a bad index.
    sm.remove_clip(0);
    sm.remove_clip(-1);
    sm.set_clip_duration(0, 5.0f);
    sm.set_clip_looping(0, true);
    sm.add_track_to_clip(0, prop);
    sm.remove_track_from_clip(0, prop);
    sm.add_keyframe(0, prop, 0.0f, Vector3(1, 2, 3));
    sm.add_keyframe_bezier(0, prop, 0.0f, Vector3(1, 2, 3), Vector2(), Vector2());
    sm.remove_keyframe(0, prop, 0);
    sm.set_clip_weight(0, 0.5f);
    sm.switch_to_clip_delayed(0, 0.3f);
    sm.blend_to_clip(0, 0.3f); // deprecated alias
    sm.play(0); // index 0 is out of range on an empty machine -> must stay stopped

    ERR_PRINT_ON;

    // Reaching here proves no abort occurred, and state is unchanged.
    CHECK_EQ(sm.get_clip_count(), 0);
    CHECK_EQ(sm.get_current_clip(), -1);
    CHECK_EQ(sm.get_state(), ANIMATION_STATE_STOPPED);
    CHECK_FALSE(sm.is_playing());
}

TEST_CASE("[GaussianSplatting][Animation] bad clip index is rejected at the exact boundary (#597)") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine sm;
    sm.set_splat_count(1);
    const int idx = sm.add_clip("only", 1.0f); // valid indices are just {0}
    REQUIRE(idx == 0);
    const AnimationProperty prop = ANIMATION_PROPERTY_OPACITY;

    // Valid index still works.
    CHECK(sm.get_clip_name(0) == String("only"));

    ERR_PRINT_OFF;
    // One-past-the-end is out of range and must be rejected safely.
    CHECK(sm.get_clip_name(1) == String());
    CHECK(Math::is_equal_approx(sm.get_clip_duration(1), 0.0f));
    CHECK_FALSE(sm.has_track(1, prop));
    sm.add_keyframe(1, prop, 0.0f, 0.5f);
    sm.switch_to_clip_delayed(5, 0.3f);
    sm.play(5); // out of range -> must not start playback
    ERR_PRINT_ON;

    CHECK_EQ(sm.get_clip_count(), 1);
    CHECK_EQ(sm.get_current_clip(), -1);
    CHECK_EQ(sm.get_state(), ANIMATION_STATE_STOPPED);
}

TEST_CASE("[GaussianSplatting][Animation] clip-duration producers reject non-positive values (#598)") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine sm;

    // add_clip clamps a zero/negative duration to a positive floor.
    const int a = sm.add_clip("zero", 0.0f);
    REQUIRE(a == 0);
    CHECK(sm.get_clip_duration(0) > 0.0f);

    const int b = sm.add_clip("negative", -4.0f);
    REQUIRE(b == 1);
    CHECK(sm.get_clip_duration(1) > 0.0f);

    // set_clip_duration clamps too (previously used MAX(0, ...), which permitted 0).
    sm.set_clip_duration(0, 0.0f);
    CHECK(sm.get_clip_duration(0) > 0.0f);
    sm.set_clip_duration(0, -10.0f);
    CHECK(sm.get_clip_duration(0) > 0.0f);

    // from_dict clamps a deserialized non-positive duration.
    Dictionary clip_dict;
    clip_dict["name"] = "loaded";
    clip_dict["duration"] = 0.0f;
    clip_dict["looping"] = true;
    Array clips_array;
    clips_array.push_back(clip_dict);
    Dictionary root;
    root["clips"] = clips_array;
    sm.from_dict(root);
    REQUIRE(sm.get_clip_count() == 1);
    CHECK(sm.get_clip_duration(0) > 0.0f);
}

TEST_CASE("[GaussianSplatting][Animation] clip-duration producers reject NaN and +/-Infinity (#598 adjacent gap)") {
    using namespace GaussianSplatting;

    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    const float pos_inf = std::numeric_limits<float>::infinity();
    const float neg_inf = -std::numeric_limits<float>::infinity();

    GaussianAnimationStateMachine sm;

    // add_clip: MAX(floor, p_duration) alone would return NaN/+Inf unchanged
    // (any comparison against NaN is false, and MIN_CLIP_DURATION > +Inf is
    // also false), so add_clip must sanitize before storing.
    const int nan_clip = sm.add_clip("nan", nan_value);
    REQUIRE(nan_clip == 0);
    CHECK_FALSE(Math::is_nan(sm.get_clip_duration(nan_clip)));
    CHECK_FALSE(Math::is_inf(sm.get_clip_duration(nan_clip)));
    CHECK(sm.get_clip_duration(nan_clip) > 0.0f);

    const int pos_inf_clip = sm.add_clip("pos_inf", pos_inf);
    REQUIRE(pos_inf_clip == 1);
    CHECK_FALSE(Math::is_nan(sm.get_clip_duration(pos_inf_clip)));
    CHECK_FALSE(Math::is_inf(sm.get_clip_duration(pos_inf_clip)));
    CHECK(sm.get_clip_duration(pos_inf_clip) > 0.0f);

    const int neg_inf_clip = sm.add_clip("neg_inf", neg_inf);
    REQUIRE(neg_inf_clip == 2);
    CHECK_FALSE(Math::is_nan(sm.get_clip_duration(neg_inf_clip)));
    CHECK_FALSE(Math::is_inf(sm.get_clip_duration(neg_inf_clip)));
    CHECK(sm.get_clip_duration(neg_inf_clip) > 0.0f);

    // set_clip_duration: the same guard applies when overwriting a live clip.
    sm.set_clip_duration(0, nan_value);
    CHECK_FALSE(Math::is_nan(sm.get_clip_duration(0)));
    CHECK(sm.get_clip_duration(0) > 0.0f);

    sm.set_clip_duration(0, pos_inf);
    CHECK_FALSE(Math::is_inf(sm.get_clip_duration(0)));
    CHECK(sm.get_clip_duration(0) > 0.0f);

    sm.set_clip_duration(0, neg_inf);
    CHECK_FALSE(Math::is_inf(sm.get_clip_duration(0)));
    CHECK(sm.get_clip_duration(0) > 0.0f);

    // from_dict: a corrupt or hand-built save carrying a non-finite duration
    // must be sanitized on load too, not only through the live setters.
    const float bad_durations[] = { nan_value, pos_inf, neg_inf };
    for (float bad_duration : bad_durations) {
        Dictionary clip_dict;
        clip_dict["name"] = "loaded";
        clip_dict["duration"] = bad_duration;
        clip_dict["looping"] = true;
        Array clips_array;
        clips_array.push_back(clip_dict);
        Dictionary root;
        root["clips"] = clips_array;
        sm.from_dict(root);
        REQUIRE(sm.get_clip_count() == 1);
        CHECK_FALSE(Math::is_nan(sm.get_clip_duration(0)));
        CHECK_FALSE(Math::is_inf(sm.get_clip_duration(0)));
        CHECK(sm.get_clip_duration(0) > 0.0f);
    }
}

TEST_CASE("[GaussianSplatting][Animation] a NaN clip duration cannot freeze a looping clip via an always-false comparison (#598 adjacent gap)") {
    using namespace GaussianSplatting;

    const float nan_value = std::numeric_limits<float>::quiet_NaN();

    // Pre-fix, a NaN duration would pass MAX(floor, NaN) unchanged, so the
    // loop-wrap guard `clip.duration > 0.0f` (itself a comparison) is false
    // for NaN: the clip never wraps, current_time grows unbounded, and
    // sampling sticks at the final keyframe forever. Prove the sanitized
    // (finite, positive) duration wraps correctly instead.
    GaussianAnimationStateMachine sm;
    const int idx = sm.add_clip("loop_nan", nan_value);
    REQUIRE(idx == 0);
    const float duration = sm.get_clip_duration(0);
    REQUIRE(duration > 0.0f);
    REQUIRE_FALSE(Math::is_nan(duration));

    sm.set_clip_looping(0, true);
    sm.set_splat_count(1);
    sm.play(0);
    REQUIRE(sm.is_playing());

    for (int i = 0; i < 64; i++) {
        sm.update(0.5f);
        const float t = sm.get_current_time();
        CHECK_FALSE(Math::is_nan(t));
        CHECK_FALSE(Math::is_inf(t));
        CHECK(t >= 0.0f);
        CHECK(t < duration);
    }
    CHECK_EQ(sm.get_state(), ANIMATION_STATE_PLAYING);
}

TEST_CASE("[GaussianSplatting][Animation] a +Infinity clip duration cannot prevent a non-looping clip from stopping (#598 adjacent gap)") {
    using namespace GaussianSplatting;

    const float pos_inf = std::numeric_limits<float>::infinity();

    // Pre-fix, a +Infinity duration would pass MAX(floor, +Inf) unchanged
    // (MIN_CLIP_DURATION > +Inf is false), so `current_time >= clip.duration`
    // would never fire and the clip would never stop. Prove the sanitized
    // (finite, tiny) duration still lets a non-looping clip stop.
    GaussianAnimationStateMachine sm;
    const int idx = sm.add_clip("stop_inf", pos_inf);
    REQUIRE(idx == 0);
    const float duration = sm.get_clip_duration(0);
    REQUIRE(duration > 0.0f);
    REQUIRE_FALSE(Math::is_inf(duration));

    sm.set_splat_count(1);
    sm.play(0);
    REQUIRE(sm.is_playing());

    for (int i = 0; i < 8 && sm.is_playing(); i++) {
        sm.update(1.0f);
    }
    CHECK_EQ(sm.get_state(), ANIMATION_STATE_STOPPED);
    const float t = sm.get_current_time();
    CHECK_FALSE(Math::is_nan(t));
    CHECK_FALSE(Math::is_inf(t));
}

TEST_CASE("[GaussianSplatting][Animation] switch_to_clip_delayed rejects a NaN switch-delay (#598 adjacent gap, #599)") {
    using namespace GaussianSplatting;

    const float nan_value = std::numeric_limits<float>::quiet_NaN();

    GaussianAnimationStateMachine sm;
    sm.set_splat_count(1);
    const int a = sm.add_clip("A", 10.0f);
    const int b = sm.add_clip("B", 10.0f);
    sm.add_keyframe(a, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.25f);
    sm.add_keyframe(b, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.75f);
    sm.play(a);
    REQUIRE(sm.get_current_clip() == a);

    // Pre-fix, MAX(0.0f, NaN) left transition_duration == NaN, so
    // _update_blend_weights' `transition_duration <= 0.0f` check was false
    // and its CLAMP(...) also returned NaN (CLAMP's comparisons are equally
    // false against NaN), so `blend_progress >= 1.0f` never fired: the
    // switch was stuck forever. The sanitized delay must instead commit on
    // the very next update() (treated as an immediate switch).
    sm.switch_to_clip_delayed(b, nan_value);
    sm.update(0.0f);
    CHECK_EQ(sm.get_current_clip(), b);
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.75f));
}

TEST_CASE("[GaussianSplatting][Animation] zero-duration looping clip never produces NaN playback (#598)") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine sm;
    // Requested duration 0 is floored to a tiny positive value; looping playback
    // must still advance without ever hitting fmod(t, 0) -> NaN.
    const int idx = sm.add_clip("loop", 0.0f);
    REQUIRE(idx == 0);
    sm.set_clip_looping(0, true);
    sm.set_splat_count(1);
    sm.play(0);
    REQUIRE(sm.is_playing());

    const float duration = sm.get_clip_duration(0);
    REQUIRE(duration > 0.0f);

    // Large deltas relative to a tiny duration exercise the wrap every frame.
    for (int i = 0; i < 128; i++) {
        sm.update(0.5f);
        const float t = sm.get_current_time();
        CHECK_FALSE(Math::is_nan(t));
        CHECK_FALSE(Math::is_inf(t));
        CHECK(t >= 0.0f);
        CHECK(t < duration); // stays wrapped inside the loop, never runs away
    }
    // A looping clip never stops.
    CHECK_EQ(sm.get_state(), ANIMATION_STATE_PLAYING);
}

TEST_CASE("[GaussianSplatting][Animation] switch_to_clip_delayed is a delayed hard cut, not a cross-fade (#599)") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine sm;
    sm.set_splat_count(1);
    // Long, distinct constant-opacity clips so we can tell which clip is sampled.
    const int a = sm.add_clip("A", 10.0f);
    const int b = sm.add_clip("B", 10.0f);
    REQUIRE(a == 0);
    REQUIRE(b == 1);
    sm.add_keyframe(a, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.25f);
    sm.add_keyframe(b, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.75f);
    sm.play(a);

    REQUIRE(sm.get_current_clip() == a);
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.25f));

    // Schedule a switch over 1.0s. It must NOT switch immediately, and the source
    // clip must keep being sampled 1:1 (no weighting toward the target).
    sm.switch_to_clip_delayed(b, 1.0f);
    CHECK_EQ(sm.get_current_clip(), a);
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.25f));

    // Mid-transition: still the source clip. A real cross-fade would read ~0.5
    // here (lerp of 0.25 and 0.75); a hard cut stays at 0.25. This is the
    // discriminator between "delayed hard cut" and "cross-fade".
    sm.update(0.5f);
    CHECK_EQ(sm.get_current_clip(), a);
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.25f));
    CHECK(sm.sample_opacity(0, -1.0f) < 0.4f); // definitively not a 0.5 blend

    // Completing the delay commits the hard switch in a single step.
    sm.update(0.6f); // transition_time 1.1 >= 1.0
    CHECK_EQ(sm.get_current_clip(), b);
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.75f));
}

TEST_CASE("[GaussianSplatting][Animation] deprecated blend_to_clip alias forwards to the delayed switch (#599)") {
    using namespace GaussianSplatting;

    GaussianAnimationStateMachine sm;
    sm.set_splat_count(1);
    const int a = sm.add_clip("A", 10.0f);
    const int b = sm.add_clip("B", 10.0f);
    sm.add_keyframe(a, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.25f);
    sm.add_keyframe(b, ANIMATION_PROPERTY_OPACITY, 0.0f, 0.75f);
    sm.play(a);
    REQUIRE(sm.get_current_clip() == a);

    // The deprecated alias must behave identically to switch_to_clip_delayed.
    sm.blend_to_clip(b, 1.0f);
    CHECK_EQ(sm.get_current_clip(), a); // no immediate switch
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.25f));

    sm.update(0.5f);
    CHECK_EQ(sm.get_current_clip(), a); // still delayed, still source

    sm.update(0.6f);
    CHECK_EQ(sm.get_current_clip(), b); // committed hard switch
    CHECK(Math::is_equal_approx(sm.sample_opacity(0, -1.0f), 0.75f));
}
