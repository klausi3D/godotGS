#pragma once

// Regression test for #1106: the GDScript-facing type of the
// GaussianAnimationStateMachine enums.
//
// AnimationProperty and AnimationState are declared at NAMESPACE scope
// (GaussianSplatting::AnimationProperty). VARIANT_ENUM_CAST stringifies its
// argument and GodotTypeInfo::Internal::enum_qualified_name_to_class_info_name()
// (core/variant/type_info.h) treats the second-to-last `::` part as the owning
// CLASS. Cast as `GaussianSplatting::AnimationProperty`, every bound parameter
// carried class_name "GaussianSplatting.AnimationProperty", while the bound
// constants are registered on GaussianAnimationStateMachine. The GDScript
// analyzer saw two different enum types and rejected the documented call
//     anim.add_track_to_clip(clip, GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION)
// with "Invalid argument for add_track_to_clip()", so the script failed to load.
//
// The test compiles a real GDScript that passes the class's own constants to
// all nine AnimationProperty-taking methods and stores get_state() into a
// variable typed with the class's AnimationState enum, then runs it. It asserts
// that the script parses (the user-visible property) and that the calls reach
// the C++ methods with the right values.

#include "test_macros.h"
#include "../animation/animation_state_machine.h"

#include "modules/modules_enabled.gen.h" // For gdscript.

#ifdef MODULE_GDSCRIPT_ENABLED

#include "modules/gdscript/gdscript.h"

namespace GaussianAnimationEnumBindingTests {

using GaussianSplatting::GaussianAnimationStateMachine;

TEST_CASE("[GaussianSplatting][Animation] GDScript passes the class's own enum constants to bound methods (#1106)") {
    GDScriptLanguage::get_singleton()->init();

    const String source = R"GDSCRIPT(
extends RefCounted

const P := GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION

func run() -> String:
	var anim := GaussianAnimationStateMachine.new()
	var clip := anim.add_clip("clip", 2.0)

	anim.add_track_to_clip(clip, GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION)
	if not anim.has_track(clip, GaussianAnimationStateMachine.ANIMATION_PROPERTY_POSITION):
		return "has_track false after add_track_to_clip"
	if anim.has_track(clip, GaussianAnimationStateMachine.ANIMATION_PROPERTY_COLOR):
		return "has_track true for a track that was never added"

	anim.add_keyframe(clip, P, 0.0, Vector3(1, 2, 3))
	anim.add_keyframe_bezier(clip, P, 1.0, Vector3(4, 5, 6), Vector2(-0.1, 0.0), Vector2(0.1, 0.0))
	if anim.get_keyframe_count(clip, P) != 2:
		return "keyframe count %d, expected 2" % anim.get_keyframe_count(clip, P)
	if not is_equal_approx(anim.get_keyframe_time(clip, P, 1), 1.0):
		return "keyframe time %f, expected 1.0" % anim.get_keyframe_time(clip, P, 1)
	if anim.get_keyframe_value(clip, P, 0) != Vector3(1, 2, 3):
		return "keyframe value %s, expected (1, 2, 3)" % str(anim.get_keyframe_value(clip, P, 0))

	anim.remove_keyframe(clip, P, 0)
	if anim.get_keyframe_count(clip, P) != 1:
		return "keyframe count %d after remove_keyframe, expected 1" % anim.get_keyframe_count(clip, P)
	anim.remove_track_from_clip(clip, P)
	if anim.has_track(clip, P):
		return "has_track true after remove_track_from_clip"

	var prop: GaussianAnimationStateMachine.AnimationProperty = GaussianAnimationStateMachine.ANIMATION_PROPERTY_OPACITY
	anim.add_track_to_clip(clip, prop)
	if not anim.has_track(clip, GaussianAnimationStateMachine.ANIMATION_PROPERTY_OPACITY):
		return "typed enum variable did not reach add_track_to_clip"

	var state: GaussianAnimationStateMachine.AnimationState = anim.get_state()
	if state != GaussianAnimationStateMachine.ANIMATION_STATE_STOPPED:
		return "state %d, expected ANIMATION_STATE_STOPPED" % state
	return "ok"
)GDSCRIPT";

    // The editor/--check-only validation path. Its error list carries the
    // analyzer message, so a regression reports WHY the script was rejected
    // (on the unfixed base: 'Invalid argument for "add_track_to_clip()"
    // function: argument 2 should be "GaussianSplatting.AnimationProperty" ...').
    List<ScriptLanguage::ScriptError> errors;
    GDScriptLanguage::get_singleton()->validate(source, "", nullptr, &errors);
    String error_text;
    for (const ScriptLanguage::ScriptError &e : errors) {
        error_text += vformat("\n  line %d: %s", e.line, e.message);
    }
    INFO(vformat("GDScript validation errors:%s", error_text));
    CHECK_EQ(errors.size(), 0);

    Ref<GDScript> gdscript;
    gdscript.instantiate();
    gdscript->set_source_code(source);

    // GDScript::reload() prints a spurious `Condition "err" is true` for a
    // path-less script even when parsing succeeds (see the upstream
    // "[Modules][GDScript] Load source code dynamically" case). A real parse
    // error is still reported through the returned Error.
    ERR_PRINT_OFF;
    const Error err = gdscript->reload();
    ERR_PRINT_ON;
    REQUIRE_MESSAGE(err == OK, "A script passing GaussianAnimationStateMachine.ANIMATION_PROPERTY_* to the class's own methods must parse.");

    Ref<RefCounted> runner;
    runner.instantiate();
    runner->set_script(gdscript);
    const Variant result = runner->call("run");
    CHECK_EQ(result.get_type(), Variant::STRING);
    CHECK_EQ(String(result), String("ok"));
}

TEST_CASE("[GaussianSplatting][Animation] bound enum argument types name GaussianAnimationStateMachine (#1106)") {
    // Every enum-typed argument and return of the class must name an enum the
    // class itself registers; a namespace-qualified name would resolve to no
    // registered enum (the #1106 shape).
    const StringName cls = GaussianAnimationStateMachine::get_class_static();
    List<MethodInfo> methods;
    ClassDB::get_method_list(cls, &methods, true);

    int enum_slots = 0;
    for (const MethodInfo &mi : methods) {
        LocalVector<PropertyInfo> slots;
        slots.push_back(mi.return_val);
        for (const PropertyInfo &arg : mi.arguments) {
            slots.push_back(arg);
        }
        for (const PropertyInfo &pi : slots) {
            if (!(pi.usage & PROPERTY_USAGE_CLASS_IS_ENUM)) {
                continue;
            }
            enum_slots++;
            const String qualified = pi.class_name;
            const int dot = qualified.rfind_char('.');
            INFO(vformat("method %s enum type %s", mi.name, qualified));
            REQUIRE(dot > 0);
            CHECK_EQ(qualified.substr(0, dot), String(cls));
            CHECK(ClassDB::has_enum(cls, StringName(qualified.substr(dot + 1)), true));
        }
    }
    // 9 AnimationProperty arguments + the AnimationState return of get_state().
    CHECK_EQ(enum_slots, 10);
}

} // namespace GaussianAnimationEnumBindingTests

#endif // MODULE_GDSCRIPT_ENABLED
