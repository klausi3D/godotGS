#pragma once

#include "scene/3d/node_3d.h"
#include "core/variant/dictionary.h"

class GaussianSplatBakeVolume3D : public Node3D {
    GDCLASS(GaussianSplatBakeVolume3D, Node3D);
    Vector3 volume_size = Vector3(1, 1, 1);
    PackedVector3Array hull_points;
protected:
    static void _bind_methods();
    virtual String volume_kind() const { return String(); }
    virtual String semantic_name() const { return get_name(); }
    virtual float volume_wind_weight() const { return 0; }
public:
    void set_volume_size(const Vector3 &p_size);
    Vector3 get_volume_size() const { return volume_size; }
    void set_hull_points(const PackedVector3Array &p_points) { hull_points = p_points; }
    PackedVector3Array get_hull_points() const { return hull_points; }
    Dictionary get_bake_descriptor(const Transform3D &p_bake_from_world = Transform3D()) const;
};

class GaussianSplatRegionVolume3D : public GaussianSplatBakeVolume3D {
    GDCLASS(GaussianSplatRegionVolume3D, GaussianSplatBakeVolume3D);
    String region_name;
protected:
    static void _bind_methods();
    String volume_kind() const override { return "region"; }
    String semantic_name() const override { return region_name.is_empty() ? String(get_name()) : region_name; }
public:
    void set_region_name(const String &p_name) { region_name = p_name; }
    String get_region_name() const { return region_name; }
};

class GaussianSplatWindVolume3D : public GaussianSplatBakeVolume3D {
    GDCLASS(GaussianSplatWindVolume3D, GaussianSplatBakeVolume3D);
    float wind_weight = 0;
protected:
    static void _bind_methods();
    String volume_kind() const override { return "wind"; }
    float volume_wind_weight() const override { return wind_weight; }
public:
    void set_wind_weight(float p_weight);
    float get_wind_weight() const { return wind_weight; }
};

class GaussianSplatDynamicIslandVolume3D : public GaussianSplatBakeVolume3D {
    GDCLASS(GaussianSplatDynamicIslandVolume3D, GaussianSplatBakeVolume3D);
    String asset_name;
protected:
    static void _bind_methods();
    String volume_kind() const override { return "island"; }
    String semantic_name() const override { return asset_name.is_empty() ? String(get_name()) : asset_name; }
public:
    void set_asset_name(const String &p_name) { asset_name = p_name; }
    String get_asset_name() const { return asset_name; }
};
