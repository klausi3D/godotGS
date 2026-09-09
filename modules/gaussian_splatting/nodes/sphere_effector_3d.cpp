#include "sphere_effector_3d.h"

#include "../core/gaussian_splat_scene_director.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/string/ustring.h"

namespace {
static float _sanitize_finite_or_default(float p_value, float p_default, const char *p_property_name) {
    if (Math::is_finite(p_value)) {
        return p_value;
    }

    WARN_PRINT(vformat("[SphereEffector3D] Ignoring non-finite %s; resetting to %f.", p_property_name, p_default));
    return p_default;
}
} // namespace

void SphereEffector3D::_bind_methods() {
    ADD_GROUP("Sphere Effector", "");

    ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SphereEffector3D::set_enabled);
    ClassDB::bind_method(D_METHOD("is_enabled"), &SphereEffector3D::is_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");

    ClassDB::bind_method(D_METHOD("set_radius", "radius"), &SphereEffector3D::set_radius);
    ClassDB::bind_method(D_METHOD("get_radius"), &SphereEffector3D::get_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.0,1000.0,0.01,or_greater,suffix:m"), "set_radius", "get_radius");

    ClassDB::bind_method(D_METHOD("set_strength", "strength"), &SphereEffector3D::set_strength);
    ClassDB::bind_method(D_METHOD("get_strength"), &SphereEffector3D::get_strength);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "-10.0,10.0,0.01,or_greater,or_less"), "set_strength", "get_strength");

    ClassDB::bind_method(D_METHOD("set_falloff", "falloff"), &SphereEffector3D::set_falloff);
    ClassDB::bind_method(D_METHOD("get_falloff"), &SphereEffector3D::get_falloff);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "falloff", PROPERTY_HINT_RANGE, "0.001,16.0,0.01,or_greater"), "set_falloff", "get_falloff");

    ClassDB::bind_method(D_METHOD("set_frequency", "frequency"), &SphereEffector3D::set_frequency);
    ClassDB::bind_method(D_METHOD("get_frequency"), &SphereEffector3D::get_frequency);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "frequency", PROPERTY_HINT_RANGE, "0.1,16.0,0.01,or_greater,suffix:Hz"), "set_frequency", "get_frequency");

    ClassDB::bind_method(D_METHOD("set_affect_position", "affect_position"), &SphereEffector3D::set_affect_position);
    ClassDB::bind_method(D_METHOD("is_affecting_position"), &SphereEffector3D::is_affecting_position);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "affect_position"), "set_affect_position", "is_affecting_position");

    ClassDB::bind_method(D_METHOD("set_affect_opacity", "affect_opacity"), &SphereEffector3D::set_affect_opacity);
    ClassDB::bind_method(D_METHOD("is_affecting_opacity"), &SphereEffector3D::is_affecting_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "affect_opacity"), "set_affect_opacity", "is_affecting_opacity");

    ClassDB::bind_method(D_METHOD("set_opacity_strength", "opacity_strength"), &SphereEffector3D::set_opacity_strength);
    ClassDB::bind_method(D_METHOD("get_opacity_strength"), &SphereEffector3D::get_opacity_strength);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "opacity_strength", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_opacity_strength", "get_opacity_strength");

    ClassDB::bind_method(D_METHOD("set_target_opacity", "target_opacity"), &SphereEffector3D::set_target_opacity);
    ClassDB::bind_method(D_METHOD("get_target_opacity"), &SphereEffector3D::get_target_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "target_opacity", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_target_opacity", "get_target_opacity");

    ClassDB::bind_method(D_METHOD("set_layer_mask", "layer_mask"), &SphereEffector3D::set_layer_mask);
    ClassDB::bind_method(D_METHOD("get_layer_mask"), &SphereEffector3D::get_layer_mask);
    // Matches VisualInstance3D::layers (scene/3d/visual_instance_3d.cpp:188): the
    // compact 4x5 layer grid instead of a 16-row vertical checkbox list. Must stay in
    // sync with GaussianSplatNode3D::rendering/scene_effector_layer_mask, which this
    // mask is AND-ed against in scene_director_sphere_effectors.cpp:255.
    ADD_PROPERTY(PropertyInfo(Variant::INT, "layer_mask", PROPERTY_HINT_LAYERS_3D_RENDER),
            "set_layer_mask", "get_layer_mask");

    ClassDB::bind_method(D_METHOD("set_scope_mode", "scope_mode"), &SphereEffector3D::set_scope_mode);
    ClassDB::bind_method(D_METHOD("get_scope_mode"), &SphereEffector3D::get_scope_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "scope_mode", PROPERTY_HINT_ENUM, "World,Parent Subtree,Explicit Root"),
            "set_scope_mode", "get_scope_mode");

    ClassDB::bind_method(D_METHOD("set_scope_root", "scope_root"), &SphereEffector3D::set_scope_root);
    ClassDB::bind_method(D_METHOD("get_scope_root"), &SphereEffector3D::get_scope_root);
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "scope_root", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node"),
            "set_scope_root", "get_scope_root");

    ClassDB::bind_method(D_METHOD("set_priority", "priority"), &SphereEffector3D::set_priority);
    ClassDB::bind_method(D_METHOD("get_priority"), &SphereEffector3D::get_priority);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-32,32,1,or_greater,or_less"),
            "set_priority", "get_priority");

    ClassDB::bind_method(D_METHOD("get_configuration_warnings"), &SphereEffector3D::get_configuration_warnings);

    BIND_ENUM_CONSTANT(SCOPE_WORLD);
    BIND_ENUM_CONSTANT(SCOPE_SUBTREE);
    BIND_ENUM_CONSTANT(SCOPE_EXPLICIT_ROOT);
}

void SphereEffector3D::_validate_property(PropertyInfo &p_property) const {
    if (!affect_opacity && (p_property.name == "opacity_strength" || p_property.name == "target_opacity")) {
        p_property.usage = PROPERTY_USAGE_NO_EDITOR;
    }
    if (scope_mode != SCOPE_EXPLICIT_ROOT && p_property.name == "scope_root") {
        p_property.usage = PROPERTY_USAGE_NO_EDITOR;
    }
}

void SphereEffector3D::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_ENTER_TREE: {
            update_configuration_warnings();
            _sync_with_director();
        } break;

        case NOTIFICATION_ENTER_WORLD: {
            // Godot emits ENTER_WORLD on tree entry AND whenever a Node3D's
            // resolved World3D changes without a tree removal (world-switch
            // scenarios). Re-sync so the director's SharedWorld lookup picks
            // up the new scenario — a plain ENTER_TREE wouldn't cover mid-tree
            // world swaps and the effector could stay registered in the old
            // SharedWorld.
            _sync_with_director();
        } break;

        case NOTIFICATION_EXIT_WORLD: {
            // Mirror ENTER_WORLD: evict from the previous SharedWorld before
            // the new world's ENTER_WORLD re-registers. Without this, a world
            // switch leaks the effector into the old world's effector list
            // until that world's pruning pass eventually cleans up.
            _unregister_from_director();
        } break;

        case NOTIFICATION_TRANSFORM_CHANGED: {
            update_configuration_warnings();
            _sync_with_director();
        } break;

        case NOTIFICATION_EXIT_TREE: {
            _unregister_from_director();
        } break;

        default:
            break;
    }
}

void SphereEffector3D::_sync_with_director() {
    if (!is_inside_tree()) {
        return;
    }
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        return;
    }

    ObjectID scope_root_id;
    if (scope_mode == SCOPE_SUBTREE) {
        Node *parent = get_parent();
        if (parent) {
            scope_root_id = parent->get_instance_id();
        }
    } else if (scope_mode == SCOPE_EXPLICIT_ROOT && !scope_root.is_empty()) {
        Node *resolved = get_node_or_null(scope_root);
        if (resolved) {
            scope_root_id = resolved->get_instance_id();
        }
    }

    director->register_sphere_effector(
            get_instance_id(),
            get_global_transform(),
            radius,
            strength,
            falloff,
            frequency,
            enabled,
            affect_position,
            affect_opacity,
            opacity_strength,
            target_opacity,
            layer_mask,
            uint32_t(scope_mode),
            scope_root_id,
            priority);
}

void SphereEffector3D::_unregister_from_director() {
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (director) {
        director->unregister_sphere_effector(get_instance_id());
    }
}

SphereEffector3D::SphereEffector3D() {
    set_notify_transform(true);
}

void SphereEffector3D::set_enabled(bool p_enabled) {
    if (enabled == p_enabled) {
        return;
    }

    enabled = p_enabled;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_radius(float p_radius) {
    const float sanitized = MAX(_sanitize_finite_or_default(p_radius, 0.0f, "radius"), 0.0f);
    if (Math::is_equal_approx(radius, sanitized)) {
        return;
    }

    radius = sanitized;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_strength(float p_strength) {
    const float sanitized = _sanitize_finite_or_default(p_strength, 0.0f, "strength");
    if (Math::is_equal_approx(strength, sanitized)) {
        return;
    }

    strength = sanitized;
    _sync_with_director();
}

void SphereEffector3D::set_falloff(float p_falloff) {
    const float sanitized = MAX(_sanitize_finite_or_default(p_falloff, 2.0f, "falloff"), 0.001f);
    if (Math::is_equal_approx(falloff, sanitized)) {
        return;
    }

    falloff = sanitized;
    _sync_with_director();
}

void SphereEffector3D::set_frequency(float p_frequency) {
    const float sanitized = MAX(_sanitize_finite_or_default(p_frequency, 2.0f, "frequency"), 0.1f);
    if (Math::is_equal_approx(frequency, sanitized)) {
        return;
    }

    frequency = sanitized;
    _sync_with_director();
}

void SphereEffector3D::set_affect_position(bool p_affect_position) {
    if (affect_position == p_affect_position) {
        return;
    }

    affect_position = p_affect_position;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_affect_opacity(bool p_affect_opacity) {
    if (affect_opacity == p_affect_opacity) {
        return;
    }

    affect_opacity = p_affect_opacity;
    notify_property_list_changed();
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_opacity_strength(float p_opacity_strength) {
    const float sanitized = CLAMP(_sanitize_finite_or_default(p_opacity_strength, 1.0f, "opacity_strength"), 0.0f, 1.0f);
    if (Math::is_equal_approx(opacity_strength, sanitized)) {
        return;
    }

    opacity_strength = sanitized;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_target_opacity(float p_target_opacity) {
    const float sanitized = CLAMP(_sanitize_finite_or_default(p_target_opacity, 0.0f, "target_opacity"), 0.0f, 1.0f);
    if (Math::is_equal_approx(target_opacity, sanitized)) {
        return;
    }

    target_opacity = sanitized;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_layer_mask(uint32_t p_layer_mask) {
    if (layer_mask == p_layer_mask) {
        return;
    }

    layer_mask = p_layer_mask;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_scope_mode(int p_scope_mode) {
    const int sanitized = CLAMP(p_scope_mode, int(SCOPE_WORLD), int(SCOPE_EXPLICIT_ROOT));
    if (scope_mode == sanitized) {
        return;
    }

    scope_mode = sanitized;
    notify_property_list_changed();
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_scope_root(const NodePath &p_scope_root) {
    if (scope_root == p_scope_root) {
        return;
    }

    scope_root = p_scope_root;
    update_configuration_warnings();
    _sync_with_director();
}

void SphereEffector3D::set_priority(int p_priority) {
    if (priority == p_priority) {
        return;
    }

    priority = p_priority;
    _sync_with_director();
}

PackedStringArray SphereEffector3D::get_configuration_warnings() const {
    PackedStringArray warnings = Node3D::get_configuration_warnings();

    if (!enabled) {
        return warnings;
    }

    if (radius <= 0.0f) {
        warnings.push_back("Sphere effector radius is 0. The effector will not influence any splats.");
    }

    if (!affect_position && !affect_opacity) {
        warnings.push_back("Sphere effector is enabled but affects neither position nor opacity.");
    }

    if (affect_opacity && opacity_strength <= 0.0f) {
        warnings.push_back("Opacity modulation is enabled but opacity_strength is 0.");
    }

    if (affect_opacity && Math::is_equal_approx(target_opacity, 1.0f)) {
        warnings.push_back("Opacity modulation is enabled but target_opacity is 1.0. This effector will not visibly change opacity.");
    }

    if (layer_mask == 0u) {
        warnings.push_back("Layer mask is 0. No GaussianSplatNode3D can match this effector.");
    }

    if (scope_mode == SCOPE_SUBTREE && get_parent() == nullptr) {
        warnings.push_back("Parent Subtree scope needs a parent node. At the scene root this effector will not match nodes.");
    }

    if (scope_mode == SCOPE_EXPLICIT_ROOT && scope_root.is_empty()) {
        warnings.push_back("Explicit Root scope requires a scope_root path.");
    }

    return warnings;
}
