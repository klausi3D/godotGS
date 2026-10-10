#include "gaussian_splat_world.h"

#include "../io/gaussian_splat_world_io.h"

void GaussianSplatWorld::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_gaussian_data", "data"), &GaussianSplatWorld::set_gaussian_data);
    ClassDB::bind_method(D_METHOD("get_gaussian_data"), &GaussianSplatWorld::get_gaussian_data);
    ClassDB::bind_method(D_METHOD("has_resident_gaussian_data"), &GaussianSplatWorld::has_resident_gaussian_data);
    ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &GaussianSplatWorld::set_bounds);
    ClassDB::bind_method(D_METHOD("get_bounds"), &GaussianSplatWorld::get_bounds);
    ClassDB::bind_method(D_METHOD("set_metadata", "metadata"), &GaussianSplatWorld::set_metadata);
    ClassDB::bind_method(D_METHOD("get_metadata"), &GaussianSplatWorld::get_metadata);
    ClassDB::bind_method(D_METHOD("has_chunk_payload_source"), &GaussianSplatWorld::has_chunk_payload_source);
    ClassDB::bind_method(D_METHOD("is_payload_source_backed"), &GaussianSplatWorld::is_payload_source_backed);
    ClassDB::bind_method(D_METHOD("has_renderable_payload"), &GaussianSplatWorld::has_renderable_payload);
    ClassDB::bind_method(D_METHOD("get_payload_mode"), &GaussianSplatWorld::get_payload_mode);
    ClassDB::bind_method(D_METHOD("is_streamable_payload"), &GaussianSplatWorld::is_streamable_payload);
    ClassDB::bind_method(D_METHOD("get_resident_only_reason"), &GaussianSplatWorld::get_resident_only_reason);
    ClassDB::bind_method(D_METHOD("get_splat_count"), &GaussianSplatWorld::get_splat_count);
    ClassDB::bind_method(D_METHOD("get_sh_degree"), &GaussianSplatWorld::get_sh_degree);
    ClassDB::bind_method(D_METHOD("get_sh_first_order_count"), &GaussianSplatWorld::get_sh_first_order_count);
    ClassDB::bind_method(D_METHOD("get_sh_high_order_count"), &GaussianSplatWorld::get_sh_high_order_count);
    ClassDB::bind_method(D_METHOD("get_2d_mode"), &GaussianSplatWorld::get_2d_mode);
    ClassDB::bind_method(D_METHOD("materialize_resident_gaussian_data"), &GaussianSplatWorld::materialize_resident_gaussian_data);
    ClassDB::bind_method(D_METHOD("get_chunk_count"), &GaussianSplatWorld::get_chunk_count);
    ClassDB::bind_method(D_METHOD("get_chunk_sizes"), &GaussianSplatWorld::get_chunk_sizes);
    ClassDB::bind_method(D_METHOD("get_chunk_aabbs"), &GaussianSplatWorld::get_chunk_aabbs);
    ClassDB::bind_method(D_METHOD("clear"), &GaussianSplatWorld::clear);
    ClassDB::bind_method(D_METHOD("save_to_file", "path"), &GaussianSplatWorld::save_to_file);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "gaussian_data", PROPERTY_HINT_RESOURCE_TYPE, "GaussianData"),
            "set_gaussian_data", "get_gaussian_data");
    ADD_PROPERTY(PropertyInfo(Variant::AABB, "bounds"), "set_bounds", "get_bounds");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "metadata"), "set_metadata", "get_metadata");
}

bool GaussianSplatWorld::_get(const StringName &p_name, Variant &r_ret) const {
    if (p_name == StringName("stats/total_splats")) {
        r_ret = get_splat_count();
        return true;
    }
    if (p_name == StringName("stats/chunk_count")) {
        r_ret = static_chunks.size();
        return true;
    }
    if (p_name == StringName("stats/lod_levels")) {
        int lod_levels = 0;
        if (metadata.has(StringName("lod_levels"))) {
            lod_levels = int(metadata[StringName("lod_levels")]);
        } else if (metadata.has(StringName("lod_level_count"))) {
            lod_levels = int(metadata[StringName("lod_level_count")]);
        }
        r_ret = lod_levels;
        return true;
    }
    if (p_name == StringName("stats/memory_mb")) {
        double bytes = 0.0;
        if (gaussian_data.is_valid()) {
            bytes += gaussian_data->get_memory_usage();
            const uint32_t high_order = gaussian_data->get_sh_high_order_count();
            if (high_order > 0) {
                bytes += double(gaussian_data->get_count()) * double(high_order) * sizeof(Vector3);
            }
        }
        for (int i = 0; i < static_chunks.size(); i++) {
            bytes += double(static_chunks[i].indices.size()) * sizeof(uint32_t);
        }
        r_ret = bytes / (1024.0 * 1024.0);
        return true;
    }
    if (p_name == StringName("payload/mode")) {
        r_ret = get_payload_mode();
        return true;
    }
    if (p_name == StringName("payload/streamable")) {
        r_ret = is_streamable_payload();
        return true;
    }
    if (p_name == StringName("payload/has_resident_data")) {
        r_ret = has_resident_gaussian_data();
        return true;
    }
    if (p_name == StringName("payload/has_chunk_source")) {
        r_ret = has_chunk_payload_source();
        return true;
    }
    if (p_name == StringName("payload/resident_only_reason")) {
        r_ret = get_resident_only_reason();
        return true;
    }
    return false;
}

void GaussianSplatWorld::_get_property_list(List<PropertyInfo> *p_list) const {
    const uint32_t usage = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
    p_list->push_back(PropertyInfo(Variant::INT, "stats/total_splats", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::INT, "stats/chunk_count", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::INT, "stats/lod_levels", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::FLOAT, "stats/memory_mb", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::STRING, "payload/mode", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::BOOL, "payload/streamable", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::BOOL, "payload/has_resident_data", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::BOOL, "payload/has_chunk_source", PROPERTY_HINT_NONE, "", usage));
    p_list->push_back(PropertyInfo(Variant::STRING, "payload/resident_only_reason", PROPERTY_HINT_NONE, "", usage));
}

void GaussianSplatWorld::set_gaussian_data(const Ref<GaussianData> &p_data) {
    gaussian_data = p_data;
    if (gaussian_data.is_valid()) {
        set_payload_metadata(gaussian_data->get_count(),
                gaussian_data->get_sh_degree(),
                gaussian_data->get_sh_first_order_count(),
                gaussian_data->get_sh_high_order_count(),
                gaussian_data->get_2d_mode());
        if (!bounds.has_volume()) {
            bounds = gaussian_data->get_aabb();
        }
    }
    notify_property_list_changed();
    emit_changed();
}

bool GaussianSplatWorld::has_resident_gaussian_data() const {
    return gaussian_data.is_valid() && gaussian_data->get_count() > 0;
}

void GaussianSplatWorld::set_bounds(const AABB &p_bounds) {
    bounds = p_bounds;
    emit_changed();
}

void GaussianSplatWorld::set_metadata(const Dictionary &p_metadata) {
    metadata = p_metadata;
    notify_property_list_changed();
    emit_changed();
}

void GaussianSplatWorld::set_static_chunks(const Vector<GaussianSplatRenderer::StaticChunk> &p_chunks) {
    static_chunks = p_chunks;
    notify_property_list_changed();
    emit_changed();
}

void GaussianSplatWorld::set_chunk_payload_source(const Ref<ChunkPayloadSource> &p_source) {
    chunk_payload_source = p_source;
    if (chunk_payload_source.is_valid() && chunk_payload_source->is_valid()) {
        if (splat_count_metadata == 0) {
            splat_count_metadata = chunk_payload_source->get_count();
        }
        if (sh_degree_metadata == 0) {
            sh_degree_metadata = chunk_payload_source->get_sh_degree();
        }
        if (!bounds.has_volume()) {
            bounds = chunk_payload_source->get_bounds();
        }
    }
    notify_property_list_changed();
    emit_changed();
}

bool GaussianSplatWorld::has_chunk_payload_source() const {
    return chunk_payload_source.is_valid() && chunk_payload_source->is_valid();
}

bool GaussianSplatWorld::is_payload_source_backed() const {
    return has_chunk_payload_source() && !has_resident_gaussian_data();
}

bool GaussianSplatWorld::has_renderable_payload() const {
    return has_resident_gaussian_data() || has_chunk_payload_source();
}

String GaussianSplatWorld::get_payload_mode() const {
    if (is_streamable_payload()) {
        return "streamable_uncompressed";
    }
    if (has_resident_gaussian_data()) {
        return "resident_only";
    }
    if (has_chunk_payload_source()) {
        return "streamable_uncompressed";
    }
    return "empty";
}

bool GaussianSplatWorld::is_streamable_payload() const {
    return has_chunk_payload_source() && !has_resident_gaussian_data();
}

String GaussianSplatWorld::get_resident_only_reason() const {
    if (is_streamable_payload()) {
        return String();
    }
    if (has_resident_gaussian_data() && has_chunk_payload_source()) {
        return "resident_payload_materialized";
    }
    if (has_resident_gaussian_data()) {
        return "resident_payload_no_file_source";
    }
    if (has_chunk_payload_source()) {
        return String();
    }
    return "no_renderable_payload";
}

void GaussianSplatWorld::set_payload_metadata(uint32_t p_splat_count, uint32_t p_sh_degree,
        uint32_t p_sh_first_order_count, uint32_t p_sh_high_order_count, bool p_is_2d) {
    splat_count_metadata = p_splat_count;
    sh_degree_metadata = p_sh_degree;
    sh_first_order_count_metadata = p_sh_first_order_count;
    sh_high_order_count_metadata = p_sh_high_order_count;
    is_2d_metadata = p_is_2d;
    notify_property_list_changed();
    emit_changed();
}

uint32_t GaussianSplatWorld::get_splat_count() const {
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_count();
    }
    if (chunk_payload_source.is_valid() && chunk_payload_source->is_valid()) {
        return chunk_payload_source->get_count();
    }
    return splat_count_metadata;
}

uint32_t GaussianSplatWorld::get_sh_degree() const {
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_sh_degree();
    }
    if (chunk_payload_source.is_valid() && chunk_payload_source->is_valid()) {
        return chunk_payload_source->get_sh_degree();
    }
    return sh_degree_metadata;
}

uint32_t GaussianSplatWorld::get_sh_first_order_count() const {
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_sh_first_order_count();
    }
    return sh_first_order_count_metadata;
}

uint32_t GaussianSplatWorld::get_sh_high_order_count() const {
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_sh_high_order_count();
    }
    return sh_high_order_count_metadata;
}

bool GaussianSplatWorld::get_2d_mode() const {
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_2d_mode();
    }
    return is_2d_metadata;
}

Error GaussianSplatWorld::materialize_resident_gaussian_data() {
    if (gaussian_data.is_valid()) {
        return OK;
    }
    if (!has_chunk_payload_source()) {
        return ERR_UNCONFIGURED;
    }

    LocalVector<Gaussian> gaussians;
    LocalVector<Vector3> sh_high_order;
    uint32_t sh_first_order = 0;
    uint32_t sh_high_order_count = 0;
    if (!chunk_payload_source->capture_chunk_snapshot(0, chunk_payload_source->get_count(),
                gaussians, sh_high_order, sh_first_order, sh_high_order_count)) {
        return ERR_FILE_CANT_READ;
    }

    Ref<GaussianData> materialized;
    materialized.instantiate();
    materialized->set_gaussian_payload(gaussians, sh_high_order,
            sh_first_order, sh_high_order_count, is_2d_metadata);
    set_gaussian_data(materialized);
    return OK;
}

int GaussianSplatWorld::get_chunk_count() const {
    return static_chunks.size();
}

PackedInt32Array GaussianSplatWorld::get_chunk_sizes() const {
    PackedInt32Array sizes;
    sizes.resize(static_chunks.size());
    for (int i = 0; i < static_chunks.size(); i++) {
        sizes.set(i, static_cast<int>(static_chunks[i].indices.size()));
    }
    return sizes;
}

Array GaussianSplatWorld::get_chunk_aabbs() const {
    Array aabbs;
    aabbs.resize(static_chunks.size());
    for (int i = 0; i < static_chunks.size(); i++) {
        aabbs.set(i, static_chunks[i].bounds);
    }
    return aabbs;
}

void GaussianSplatWorld::clear() {
    gaussian_data.unref();
    // Payload source lifetime is tied to world liveness; leaving it live
    // across clear() would retain file handles / in-memory splat data after
    // the world has been logically emptied.
    chunk_payload_source.unref();
    static_chunks.clear();
    bounds = AABB();
    metadata.clear();
    splat_count_metadata = 0;
    sh_degree_metadata = 0;
    sh_first_order_count_metadata = 0;
    sh_high_order_count_metadata = 0;
    is_2d_metadata = false;
    // Consumers must observe the completed reset, not retain their apply-time
    // payload/submission after this resource has become empty.
    emit_changed();
}

Error GaussianSplatWorld::save_to_file(const String &p_path) const {
    ResourceFormatSaverGaussianSplatWorld saver;
    return saver.save(Ref<Resource>(const_cast<GaussianSplatWorld *>(this)), p_path, 0);
}
