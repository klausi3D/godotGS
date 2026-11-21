#ifndef GAUSSIAN_SPLAT_REGION_OVERRIDE_H
#define GAUSSIAN_SPLAT_REGION_OVERRIDE_H

#include "core/io/resource.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "../renderer/gaussian_gpu_layout.h"

// Float authoring state; binary16 conversion happens only in pack_for_upload().
class GaussianSplatRegionOverride : public Resource {
    GDCLASS(GaussianSplatRegionOverride, Resource);
    mutable Mutex mutex;
    Color tint = Color(1, 1, 1);
    float tint_strength = 0;
    Color emissive = Color(0, 0, 0);
    float opacity = 1;
    bool wind_enabled = true;
    bool cast_shadow = true;
    bool highlight = false;
protected:
    static void _bind_methods();
public:
    void set_tint(const Color &p_value);
    Color get_tint() const;
    void set_tint_strength(float p_value);
    float get_tint_strength() const;
    void set_emissive(const Color &p_value);
    Color get_emissive() const;
    void set_opacity(float p_value);
    float get_opacity() const;
    void set_wind_enabled(bool p_value);
    bool get_wind_enabled() const;
    void set_cast_shadow(bool p_value);
    bool get_cast_shadow() const;
    void set_highlight(bool p_value);
    bool get_highlight() const;
    RegionOverrideGPU pack_for_upload() const;
};

// One placement's authored overrides. The director retains this ref, so the render
// thread never reads a Node. Its own mutex protects refs/hidden/segment length;
// each Resource separately locks its float snapshot. No lock is held during RD work.
class GSRegionOverrideState : public RefCounted {
    mutable Mutex mutex;
    HashMap<int, Ref<GaussianSplatRegionOverride>> overrides;
    uint32_t segment_len = 1;
    bool hidden = false;
public:
    static int region_id(const Dictionary &p_metadata, const StringName &p_name);
    static PackedStringArray region_names(const Dictionary &p_metadata);
    void set_manifest(const Dictionary &p_metadata);
    void set_override(int p_region, const Ref<GaussianSplatRegionOverride> &p_override);
    void clear_override(int p_region);
    void clear_all();
    void set_hidden(bool p_hidden);
    Ref<GaussianSplatRegionOverride> whole_asset();
    void snapshot(LocalVector<RegionOverrideGPU> &r_entries) const;
};

#endif
