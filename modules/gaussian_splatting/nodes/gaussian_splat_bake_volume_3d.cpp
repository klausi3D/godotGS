#include "gaussian_splat_bake_volume_3d.h"
#include "../core/gs_project_settings.h"
#include <cmath>

void GaussianSplatBakeVolume3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_volume_size", "size"), &GaussianSplatBakeVolume3D::set_volume_size);
    ClassDB::bind_method(D_METHOD("get_volume_size"), &GaussianSplatBakeVolume3D::get_volume_size);
    ClassDB::bind_method(D_METHOD("set_hull_points", "points"), &GaussianSplatBakeVolume3D::set_hull_points);
    ClassDB::bind_method(D_METHOD("get_hull_points"), &GaussianSplatBakeVolume3D::get_hull_points);
    ClassDB::bind_method(D_METHOD("get_bake_descriptor", "bake_from_world"), &GaussianSplatBakeVolume3D::get_bake_descriptor, DEFVAL(Transform3D()));
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size"), "set_volume_size", "get_volume_size");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "hull_points"), "set_hull_points", "get_hull_points");
}
void GaussianSplatBakeVolume3D::set_volume_size(const Vector3 &p_size) {
    ERR_FAIL_COND(!p_size.is_finite() || p_size.x <= 0 || p_size.y <= 0 || p_size.z <= 0);
    volume_size = p_size;
}
Dictionary GaussianSplatBakeVolume3D::get_bake_descriptor(const Transform3D &p_bake_from_world) const {
    Dictionary d;
    d["kind"] = volume_kind(); d["path"] = String(get_path()); d["name"] = semantic_name();
    d["transform"] = p_bake_from_world * get_global_transform();
    d["size"] = volume_size; d["hull"] = hull_points; d[gs::settings::WIND_WEIGHT_PROPERTY] = volume_wind_weight();
    return d;
}
void GaussianSplatRegionVolume3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_region_name", "name"), &GaussianSplatRegionVolume3D::set_region_name);
    ClassDB::bind_method(D_METHOD("get_region_name"), &GaussianSplatRegionVolume3D::get_region_name);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "region_name"), "set_region_name", "get_region_name");
}
void GaussianSplatWindVolume3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_wind_weight", "weight"), &GaussianSplatWindVolume3D::set_wind_weight);
    ClassDB::bind_method(D_METHOD("get_wind_weight"), &GaussianSplatWindVolume3D::get_wind_weight);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, gs::settings::WIND_WEIGHT_PROPERTY, PROPERTY_HINT_RANGE, "0,1,0.001"), "set_wind_weight", "get_wind_weight");
}
void GaussianSplatWindVolume3D::set_wind_weight(float p_weight) {
    ERR_FAIL_COND(!std::isfinite(p_weight) || p_weight < 0 || p_weight > 1);
    wind_weight = p_weight;
}
void GaussianSplatDynamicIslandVolume3D::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_asset_name", "name"), &GaussianSplatDynamicIslandVolume3D::set_asset_name);
    ClassDB::bind_method(D_METHOD("get_asset_name"), &GaussianSplatDynamicIslandVolume3D::get_asset_name);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "asset_name"), "set_asset_name", "get_asset_name");
}
