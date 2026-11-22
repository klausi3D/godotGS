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
    MutexLock lock(mutex);
    RegionOverrideGPU entry = {};
    entry.tint[0] = tint.r; entry.tint[1] = tint.g; entry.tint[2] = tint.b;
    entry.tint[3] = CLAMP(tint_strength, 0.0f, 1.0f);
    // Binary16 cannot represent a finite value above 65504. Keep authoring
    // floats intact and clamp only at the upload boundary.
    const uint32_t r = Math::make_half_float(CLAMP(emissive.r, 0.0f, 65504.0f));
    const uint32_t g = Math::make_half_float(CLAMP(emissive.g, 0.0f, 65504.0f));
    const uint32_t b = Math::make_half_float(CLAMP(emissive.b, 0.0f, 65504.0f));
    entry.emissive_opacity[0] = r | (g << 16);
    entry.emissive_opacity[1] = b | (uint32_t(Math::make_half_float(CLAMP(opacity, 0.0f, 1.0f))) << 16);
    entry.flags = (opacity <= 0 ? GS_REGION_HIDE : 0u) | (!wind_enabled ? GS_REGION_NO_WIND : 0u) |
            (highlight ? GS_REGION_HIGHLIGHT : 0u) | (!cast_shadow ? GS_REGION_NO_SHADOW : 0u);
    return entry;
}

int GSRegionOverrideState::region_id(const Dictionary &p_metadata, const StringName &p_name) {
    const Array regions = p_metadata.get("regions", Array());
    for (int i = 0; i < regions.size(); ++i) {
        if (regions[i].get_type() != Variant::DICTIONARY) { continue; }
        const Dictionary region = regions[i];
        const int id = region.get("id", -1);
        if (id >= 0 && id <= 65535 && StringName(String(region.get("name", String()))) == p_name) { return id; }
    }
    return -1;
}
PackedStringArray GSRegionOverrideState::region_names(const Dictionary &p_metadata) {
    PackedStringArray names;
    const Array regions = p_metadata.get("regions", Array());
    for (int i = 0; i < regions.size(); ++i) {
        if (regions[i].get_type() != Variant::DICTIONARY) { continue; }
        const Dictionary region = regions[i];
        const int id = region.get("id", -1);
        const String name = region.get("name", String());
        if (id >= 0 && id <= 65535 && !name.is_empty()) { names.push_back(name); }
    }
    return names;
}
void GSRegionOverrideState::set_manifest(const Dictionary &p_metadata) {
    MutexLock lock(mutex);
    segment_len = 1;
    const Array regions = p_metadata.get("regions", Array());
    for (int i = 0; i < regions.size(); ++i) {
        if (regions[i].get_type() != Variant::DICTIONARY) { continue; }
        const Dictionary region = regions[i];
        const int id = region.get("id", -1);
        if (id >= 0 && id <= 65535) { segment_len = MAX(segment_len, uint32_t(id + 1)); }
    }
}
void GSRegionOverrideState::set_override(int p_region, const Ref<GaussianSplatRegionOverride> &p_override) {
    ERR_FAIL_COND(p_region < 0 || p_region > 65535 || p_override.is_null());
    MutexLock lock(mutex); overrides[p_region] = p_override;
}
void GSRegionOverrideState::clear_override(int p_region) { MutexLock lock(mutex); overrides.erase(p_region); }
void GSRegionOverrideState::clear_all() { MutexLock lock(mutex); overrides.clear(); hidden = false; }
void GSRegionOverrideState::set_hidden(bool p_hidden) { MutexLock lock(mutex); hidden = p_hidden; }
Ref<GaussianSplatRegionOverride> GSRegionOverrideState::whole_asset() {
    MutexLock lock(mutex);
    if (!overrides.has(0)) { Ref<GaussianSplatRegionOverride> value; value.instantiate(); overrides[0] = value; }
    return overrides[0];
}
void GSRegionOverrideState::snapshot(LocalVector<RegionOverrideGPU> &r_entries) const {
    MutexLock lock(mutex);
    r_entries.clear();
    GaussianSplatRegionOverride neutral;
    const auto *whole = overrides.getptr(0);
    const RegionOverrideGPU fallback = whole ? (*whole)->pack_for_upload() : neutral.pack_for_upload();
    uint32_t length = segment_len;
    for (const auto &item : overrides) { length = MAX(length, uint32_t(item.key + 1)); }
    r_entries.resize(length);
    for (uint32_t region = 0; region < length; ++region) {
        const auto *value = overrides.getptr(int(region));
        r_entries[region] = value ? (*value)->pack_for_upload() : fallback;
        if (hidden) { r_entries[region].flags |= GS_REGION_HIDE; }
    }
}
