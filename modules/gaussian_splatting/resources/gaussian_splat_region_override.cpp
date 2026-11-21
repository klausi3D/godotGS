#include "gaussian_splat_region_override.h"
#include "../core/gs_project_settings.h"
#include "core/object/class_db.h"

#define GS_REGION_PROPERTY(type, name) \
    void GaussianSplatRegionOverride::set_##name(type p_value) { { MutexLock lock(mutex); name = p_value; } emit_changed(); } \
    type GaussianSplatRegionOverride::get_##name() const { MutexLock lock(mutex); return name; }
// Color getters return by value; setters take const references.
void GaussianSplatRegionOverride::set_tint(const Color &p_value) { { MutexLock lock(mutex); tint = p_value; } emit_changed(); }
Color GaussianSplatRegionOverride::get_tint() const { MutexLock lock(mutex); return tint; }
void GaussianSplatRegionOverride::set_emissive(const Color &p_value) { { MutexLock lock(mutex); emissive = p_value; } emit_changed(); }
Color GaussianSplatRegionOverride::get_emissive() const { MutexLock lock(mutex); return emissive; }
GS_REGION_PROPERTY(float, tint_strength)
GS_REGION_PROPERTY(float, opacity)
GS_REGION_PROPERTY(bool, wind_enabled)
GS_REGION_PROPERTY(bool, cast_shadow)
GS_REGION_PROPERTY(bool, highlight)
#undef GS_REGION_PROPERTY

void GaussianSplatRegionOverride::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_tint", "value"), &GaussianSplatRegionOverride::set_tint);
    ClassDB::bind_method(D_METHOD("get_tint"), &GaussianSplatRegionOverride::get_tint);
    ClassDB::bind_method(D_METHOD("set_tint_strength", "value"), &GaussianSplatRegionOverride::set_tint_strength);
    ClassDB::bind_method(D_METHOD("get_tint_strength"), &GaussianSplatRegionOverride::get_tint_strength);
    ClassDB::bind_method(D_METHOD("set_emissive", "value"), &GaussianSplatRegionOverride::set_emissive);
    ClassDB::bind_method(D_METHOD("get_emissive"), &GaussianSplatRegionOverride::get_emissive);
    ClassDB::bind_method(D_METHOD("set_opacity", "value"), &GaussianSplatRegionOverride::set_opacity);
    ClassDB::bind_method(D_METHOD("get_opacity"), &GaussianSplatRegionOverride::get_opacity);
    ClassDB::bind_method(D_METHOD("set_wind_enabled", "value"), &GaussianSplatRegionOverride::set_wind_enabled);
    ClassDB::bind_method(D_METHOD("get_wind_enabled"), &GaussianSplatRegionOverride::get_wind_enabled);
    ClassDB::bind_method(D_METHOD("set_cast_shadow", "value"), &GaussianSplatRegionOverride::set_cast_shadow);
    ClassDB::bind_method(D_METHOD("get_cast_shadow"), &GaussianSplatRegionOverride::get_cast_shadow);
    ClassDB::bind_method(D_METHOD("set_highlight", "value"), &GaussianSplatRegionOverride::set_highlight);
    ClassDB::bind_method(D_METHOD("get_highlight"), &GaussianSplatRegionOverride::get_highlight);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tint"), "set_tint", "get_tint");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tint_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_tint_strength", "get_tint_strength");
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "emissive", PROPERTY_HINT_COLOR_NO_ALPHA), "set_emissive", "get_emissive");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "opacity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_opacity", "get_opacity");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, gs::settings::REGION_WIND_ENABLED_PROPERTY), "set_wind_enabled", "get_wind_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadow"), "set_cast_shadow", "get_cast_shadow");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "highlight"), "set_highlight", "get_highlight");
}

RegionOverrideGPU GaussianSplatRegionOverride::pack_for_upload() const {
    RegionOverrideGPU entry = {};
    entry.tint[0] = entry.tint[1] = entry.tint[2] = 1.0f;
    entry.emissive_opacity[1] = uint32_t(Math::make_half_float(1.0f)) << 16;
    return entry; // Parent-compatible inert seam; C's fix packs the float state.
}

int GSRegionOverrideState::region_id(const Dictionary &, const StringName &) { return -1; }
PackedStringArray GSRegionOverrideState::region_names(const Dictionary &) { return {}; }
void GSRegionOverrideState::set_manifest(const Dictionary &) {}
void GSRegionOverrideState::set_override(int, const Ref<GaussianSplatRegionOverride> &) {}
void GSRegionOverrideState::clear_override(int) {}
void GSRegionOverrideState::clear_all() {}
void GSRegionOverrideState::set_hidden(bool) {}
Ref<GaussianSplatRegionOverride> GSRegionOverrideState::whole_asset() { Ref<GaussianSplatRegionOverride> value; value.instantiate(); return value; }
void GSRegionOverrideState::snapshot(LocalVector<RegionOverrideGPU> &r_entries) const {
    r_entries.clear();
    GaussianSplatRegionOverride neutral;
    r_entries.push_back(neutral.pack_for_upload());
}
