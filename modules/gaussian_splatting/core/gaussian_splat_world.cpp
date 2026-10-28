#include "gaussian_splat_world.h"

#include "../io/gaussian_splat_world_io.h"
#include "../io/gs_atomic_file_writer.h"
#include "core/io/dir_access.h"
#include "gaussian_splat_hlod_bake.h"
#include "gaussian_splat_hlod_job.h"
#include "gs_vector_alloc.h"
#include "core/os/os.h"
#include "../renderer/gaussian_gpu_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Live AABBs must enclose both the root and the requested box in double AND in
// real_t arithmetic. Unlike the file-header helper, never drop an unrepresentable
// requested union. No state is published until all three axes have been checked.
bool hlod_live_bounds(const GaussianSplatHlodTree &p_tree, const AABB &p_requested, AABB &r_bounds) {
    if (p_tree.nodes.is_empty()) {
        return false;
    }
    const GaussianSplatHlodNode &root = p_tree.nodes[0];
    double center[3];
    p_tree.node_cell_center(root, center);
    AABB candidate;
    for (int a = 0; a < 3; ++a) {
        const real_t requested_end = p_requested.position[a] + p_requested.size[a];
        const double exact_end = double(p_requested.position[a]) + double(p_requested.size[a]);
        const double root_lo = center[a] + double(root.aabb_min[a]);
        const double root_hi = center[a] + double(root.aabb_max[a]);
        if (!Math::is_finite(p_requested.position[a]) || !Math::is_finite(p_requested.size[a]) ||
                p_requested.size[a] < real_t(0) || !Math::is_finite(requested_end) || !Math::is_finite(exact_end) ||
                !Math::is_finite(root_lo) || !Math::is_finite(root_hi) || root_lo > root_hi) {
            return false;
        }
        // Zero extents are points/planes: retain them too, even if has_surface() is false.
        const double lo = MIN(root_lo, double(p_requested.position[a]));
        const double hi = MAX(root_hi, MAX(exact_end, double(requested_end)));
        if (lo < -double(std::numeric_limits<real_t>::max()) || hi > double(std::numeric_limits<real_t>::max())) {
            return false;
        }
        real_t begin = real_t(lo);
        if (double(begin) > lo) {
            begin = std::nextafter(begin, real_t(-INFINITY));
        }
        real_t upper = real_t(hi);
        if (double(upper) < hi) {
            upper = std::nextafter(upper, real_t(INFINITY));
        }
        const double exact_extent = double(upper) - double(begin);
        if (!Math::is_finite(exact_extent) || exact_extent > double(std::numeric_limits<real_t>::max())) {
            return false;
        }
        real_t extent = real_t(exact_extent);
        if (double(extent) < exact_extent) {
            extent = std::nextafter(extent, real_t(INFINITY));
        }
        // Cancellation in the double subtraction can lose a small positive upper
        // endpoint. One outward step repairs it; final checks remain fail-closed.
        if (double(begin) + double(extent) < hi || double(real_t(begin + extent)) < hi) {
            extent = std::nextafter(extent, real_t(INFINITY));
        }
        real_t end = begin + extent;
        if ((!Math::is_finite(end) || double(begin) + double(extent) > double(std::numeric_limits<real_t>::max())) && lo >= 0.0) {
            // The rounded extent can exceed real_t_max in double even while its
            // runtime end stays finite (2^102..FLT_MAX), or overflow that end
            // (2^103..FLT_MAX). Both endpoints must survive later revalidation.
            // Since 0 <= lo <= hi <= upper <= real_t_max, [0, upper] is a finite
            // conservative union, with an exactly represented runtime endpoint.
            begin = real_t(0);
            extent = upper;
            end = begin + extent;
        }
        if (!Math::is_finite(begin) || !Math::is_finite(extent) || !Math::is_finite(end) || extent < real_t(0) ||
                double(begin) > lo || double(begin) + double(extent) < hi || double(end) < hi) {
            return false;
        }
        candidate.position[a] = begin;
        candidate.size[a] = extent;
    }
    r_bounds = candidate;
    return true;
}

} // namespace

#ifdef TESTS_ENABLED
GaussianSplatWorld::HlodBakeTestHook GaussianSplatWorld::hlod_bake_test_hook = nullptr;
void *GaussianSplatWorld::hlod_bake_test_userdata = nullptr;
#endif

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
    ClassDB::bind_method(D_METHOD("bake_hlod"), &GaussianSplatWorld::bake_hlod);
	ClassDB::bind_method(D_METHOD("bake_hlod_offline", "limits", "candidate_path"),
	                     &GaussianSplatWorld::bake_hlod_offline);
	ClassDB::bind_method(D_METHOD("has_hlod_tree"), &GaussianSplatWorld::has_hlod_tree);
    ClassDB::bind_method(D_METHOD("get_hlod_info"), &GaussianSplatWorld::get_hlod_info);

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
    {
        MutexLock lock(render_input_mutex);
        _assign_gaussian_data_locked(p_data, false);
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
}

void GaussianSplatWorld::_assign_gaussian_data_locked(const Ref<GaussianData> &p_data, bool p_keep_hlod) {
    if (!p_keep_hlod) {
        if (!hlod_tree.is_empty()) {
            // The leaf chunks are built from the tree and index the old payload, like the tree.
            static_chunks.clear();
        }
        hlod_tree.clear();
    } else if (!hlod_tree.is_empty() && p_data.is_valid()) {
        // Same payload (materialized from the file the tree was loaded with).
        hlod_tree.leaf_payload_revision = p_data->get_content_revision();
        hlod_tree.leaf_payload_revision_valid = true;
    }
    gaussian_data = p_data;
    if (gaussian_data.is_valid()) {
        RWLockRead data_lock(gaussian_data->data_rwlock);
        splat_count_metadata = gaussian_data->get_count();
        sh_degree_metadata = gaussian_data->get_sh_degree();
        sh_first_order_count_metadata = gaussian_data->get_sh_first_order_count();
        sh_high_order_count_metadata = gaussian_data->get_sh_high_order_count();
        is_2d_metadata = gaussian_data->get_2d_mode();
        antialiased_metadata = gaussian_data->is_antialiased;
        if (!bounds.has_volume()) {
            bounds = gaussian_data->get_aabb();
        }
    }
}

bool GaussianSplatWorld::has_resident_gaussian_data() const {
    MutexLock lock(render_input_mutex);
    return gaussian_data.is_valid() && gaussian_data->get_count() > 0;
}

void GaussianSplatWorld::set_bounds(const AABB &p_bounds) {
    {
        MutexLock lock(render_input_mutex);
    AABB next_bounds = p_bounds;
    ERR_FAIL_COND_MSG(!hlod_tree.is_empty() && !hlod_live_bounds(hlod_tree, p_bounds, next_bounds),
            "GaussianSplatWorld.set_bounds(): invalid or unrepresentable HLOD bounds; previous bounds retained.");
    bounds = next_bounds;
        ++render_input_epoch;
    }
    emit_changed();
}

void GaussianSplatWorld::set_metadata(const Dictionary &p_metadata) {
    {
        MutexLock lock(render_input_mutex);
    metadata = p_metadata.duplicate(true);
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
}

void GaussianSplatWorld::set_static_chunks(const Vector<GaussianSplatRenderer::StaticChunk> &p_chunks) {
    {
        MutexLock lock(render_input_mutex);
    static_chunks = p_chunks;
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
}

void GaussianSplatWorld::set_chunk_payload_source(const Ref<ChunkPayloadSource> &p_source) {
    {
        MutexLock lock(render_input_mutex);
    if (chunk_payload_source != p_source && gaussian_data.is_null() && !hlod_tree.is_empty()) {
        // Both the interior representations and leaf chunk addresses describe the old source.
        hlod_tree.clear();
        static_chunks.clear();
    }
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
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
}

bool GaussianSplatWorld::has_chunk_payload_source() const {
    MutexLock lock(render_input_mutex);
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
    {
        MutexLock lock(render_input_mutex);
    splat_count_metadata = p_splat_count;
    sh_degree_metadata = p_sh_degree;
    sh_first_order_count_metadata = p_sh_first_order_count;
    sh_high_order_count_metadata = p_sh_high_order_count;
    is_2d_metadata = p_is_2d;
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
}

uint32_t GaussianSplatWorld::get_splat_count() const {
    MutexLock lock(render_input_mutex);
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_count();
    }
    if (chunk_payload_source.is_valid() && chunk_payload_source->is_valid()) {
        return chunk_payload_source->get_count();
    }
    return splat_count_metadata;
}

uint32_t GaussianSplatWorld::get_sh_degree() const {
    MutexLock lock(render_input_mutex);
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_sh_degree();
    }
    if (chunk_payload_source.is_valid() && chunk_payload_source->is_valid()) {
        return chunk_payload_source->get_sh_degree();
    }
    return sh_degree_metadata;
}

uint32_t GaussianSplatWorld::get_sh_first_order_count() const {
    MutexLock lock(render_input_mutex);
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_sh_first_order_count();
    }
    return sh_first_order_count_metadata;
}

uint32_t GaussianSplatWorld::get_sh_high_order_count() const {
    MutexLock lock(render_input_mutex);
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_sh_high_order_count();
    }
    return sh_high_order_count_metadata;
}

bool GaussianSplatWorld::get_2d_mode() const {
    MutexLock lock(render_input_mutex);
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_2d_mode();
    }
    return is_2d_metadata;
}

bool GaussianSplatWorld::get_antialiased() const {
    MutexLock lock(render_input_mutex);
    if (gaussian_data.is_valid()) {
        return gaussian_data->get_antialiased();
    }
    return antialiased_metadata;
}

Error GaussianSplatWorld::materialize_resident_gaussian_data() {
    Ref<ChunkPayloadSource> source;
    uint64_t epoch;
    bool mode_2d, aa;
    {
        MutexLock lock(render_input_mutex);
        if (gaussian_data.is_valid()) {
            return OK;
        }
        source = chunk_payload_source;
        epoch = render_input_epoch;
        mode_2d = is_2d_metadata;
        aa = antialiased_metadata;
    }
    if (source.is_null() || !source->is_valid()) {
        return ERR_UNCONFIGURED;
    }
    LocalVector<Gaussian> gaussians;
    LocalVector<Vector3> sh_high_order;
    uint32_t sh_first_order = 0, sh_high_order_count = 0;
    if (!source->capture_chunk_snapshot(0, source->get_count(), gaussians, sh_high_order,
                sh_first_order, sh_high_order_count)) {
        return ERR_FILE_CANT_READ;
    }
    Ref<GaussianData> materialized;
    materialized.instantiate();
    materialized->set_gaussian_payload(gaussians, sh_high_order, sh_first_order,
            sh_high_order_count, mode_2d, aa);
    {
        MutexLock lock(render_input_mutex);
        if (render_input_epoch != epoch || chunk_payload_source != source || gaussian_data.is_valid()) {
            return ERR_BUSY;
        }
        _assign_gaussian_data_locked(materialized, true);
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
    return OK;
}

int GaussianSplatWorld::get_chunk_count() const {
    MutexLock lock(render_input_mutex);
    return static_chunks.size();
}

PackedInt32Array GaussianSplatWorld::get_chunk_sizes() const {
    MutexLock lock(render_input_mutex);
    PackedInt32Array sizes;
    sizes.resize(static_chunks.size());
    for (int i = 0; i < static_chunks.size(); i++) {
        sizes.set(i, static_cast<int>(static_chunks[i].indices.size()));
    }
    return sizes;
}

Array GaussianSplatWorld::get_chunk_aabbs() const {
    MutexLock lock(render_input_mutex);
    Array aabbs;
    aabbs.resize(static_chunks.size());
    for (int i = 0; i < static_chunks.size(); i++) {
        aabbs.set(i, static_chunks[i].bounds);
    }
    return aabbs;
}

void GaussianSplatWorld::clear() {
    {
        MutexLock lock(render_input_mutex);
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
    antialiased_metadata = false;
    hlod_tree.clear();
    // Consumers must observe the completed reset, not retain their apply-time
    // payload/submission after this resource has become empty.
        ++render_input_epoch;
    }
    emit_changed();
}

Error GaussianSplatWorld::copy_from(const Ref<Resource> &p_resource) {
    const Ref<GaussianSplatWorld> other = p_resource;
    if (other.is_null() || other.ptr() == this || get_class() != p_resource->get_class()) {
        return Resource::copy_from(p_resource);
    }
    // One changed notification, after the complete state is in place.
    _block_emit_changed();
    reset_state();
    // Base Resource storage properties (name, local_to_scene, script, ...). The world's own
    // properties are assigned below directly: the gaussian_data setter would clear the tree.
    List<PropertyInfo> properties;
    p_resource->get_property_list(&properties);
    for (const PropertyInfo &property : properties) {
        if (!(property.usage & PROPERTY_USAGE_STORAGE) || property.name == "resource_path" ||
                property.name == "gaussian_data" || property.name == "bounds" || property.name == "metadata") {
            continue;
        }
        set(property.name, p_resource->get(property.name));
    }
    // Everything the v1/v2 loaders install: payload or file source, leaf chunks, bounds,
    // metadata, payload metadata and the tree. The tree's leaf revision refers to the shared
    // GaussianData, so it stays valid.
    {
        // Stable object order excludes AB/BA locking when two worlds are copied concurrently.
        const bool self_first = get_instance_id() < other->get_instance_id();
        MutexLock first(self_first ? render_input_mutex : other->render_input_mutex);
        MutexLock second(self_first ? other->render_input_mutex : render_input_mutex);
    gaussian_data = other->gaussian_data;
    chunk_payload_source = other->chunk_payload_source;
    static_chunks = other->static_chunks;
    bounds = other->bounds;
    metadata = other->metadata.duplicate(true);
    splat_count_metadata = other->splat_count_metadata;
    sh_degree_metadata = other->sh_degree_metadata;
    sh_first_order_count_metadata = other->sh_first_order_count_metadata;
    sh_high_order_count_metadata = other->sh_high_order_count_metadata;
    is_2d_metadata = other->is_2d_metadata;
    antialiased_metadata = other->antialiased_metadata;
    hlod_tree = other->hlod_tree;
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
    _unblock_emit_changed();
    return OK;
}

Error GaussianSplatWorld::save_to_file(const String &p_path) const {
    ResourceFormatSaverGaussianSplatWorld saver;
    return saver.save(Ref<Resource>(const_cast<GaussianSplatWorld *>(this)), p_path, 0);
}

void GaussianSplatWorld::set_hlod_tree(GaussianSplatHlodTree &&p_tree) {
    {
        MutexLock lock(render_input_mutex);
    AABB next_bounds = bounds;
    ERR_FAIL_COND_MSG(!p_tree.is_empty() && !hlod_live_bounds(p_tree, bounds, next_bounds),
            "GaussianSplatWorld.set_hlod_tree(): invalid or unrepresentable HLOD bounds; previous state retained.");
    hlod_tree = std::move(p_tree);
    bounds = next_bounds;
    hlod_tree.leaf_payload_revision_valid = gaussian_data.is_valid();
    hlod_tree.leaf_payload_revision = gaussian_data.is_valid() ? gaussian_data->get_content_revision() : 0u;
        ++render_input_epoch;
    }
    notify_property_list_changed();
    emit_changed();
}

bool GaussianSplatWorld::build_hlod_leaf_chunks(const GaussianSplatHlodTree &p_tree, Vector<GaussianSplatRenderer::StaticChunk> &r_chunks) {
    r_chunks.clear();
    // Leaves sorted by payload start (depth-first order).
    LocalVector<uint32_t> leaves;
    for (uint32_t i = 0; i < p_tree.nodes.size(); i++) {
        if (p_tree.nodes[i].child_count == 0u) {
            leaves.push_back(i);
        }
    }
    const LocalVector<GaussianSplatHlodNode> &nodes = p_tree.nodes;
    std::sort(leaves.ptr(), leaves.ptr() + leaves.size(), [&nodes](uint32_t p_a, uint32_t p_b) {
        return nodes[p_a].payload_first < nodes[p_b].payload_first;
    });
    Vector<GaussianSplatRenderer::StaticChunk> chunks;
    if (!gs_resize_or_fail(chunks, int64_t(leaves.size()), "GaussianSplatWorld::build_hlod_leaf_chunks chunks")) {
        return false;
    }
    for (uint32_t k = 0; k < leaves.size(); k++) {
        const GaussianSplatHlodNode &leaf = nodes[leaves[k]];
        GaussianSplatRenderer::StaticChunk chunk;
        chunk.bounds = p_tree.node_world_aabb(leaf);
        chunk.center = chunk.bounds.get_center();
        // The culler uses a float center; bound exact stored and runtime AABB corners about
        // that center, and pad the serialized sphere for its midpoint's rounding displacement.
        double cell_center[3];
        p_tree.node_cell_center(leaf, cell_center);
        double shift_squared = 0.0;
        double extent_squared = 0.0;
        for (int a = 0; a < 3; a++) {
            const double lo = cell_center[a] + double(leaf.aabb_min[a]);
            const double hi = cell_center[a] + double(leaf.aabb_max[a]);
            const double midpoint = 0.5 * (lo + hi);
            const double center = double(chunk.center[a]);
            const double shift = midpoint - center;
            shift_squared += shift * shift;
            const double runtime_lo = double(chunk.bounds.position[a]);
            const double runtime_hi = runtime_lo + double(chunk.bounds.size[a]);
            const double extent = MAX(MAX(std::abs(lo - center), std::abs(hi - center)),
                    MAX(std::abs(runtime_lo - center), std::abs(runtime_hi - center)));
            extent_squared += extent * extent;
        }
        const double radius = MAX(double(leaf.radius) + std::sqrt(shift_squared), std::sqrt(extent_squared));
        chunk.radius = float(radius);
        if (double(chunk.radius) < radius) {
            chunk.radius = std::nextafter(chunk.radius, std::numeric_limits<float>::infinity());
        }
        if (!gs_resize_or_fail(chunk.indices, int64_t(leaf.payload_count), "GaussianSplatWorld::build_hlod_leaf_chunks indices")) {
            return false;
        }
        uint32_t *dst = chunk.indices.ptrw();
        for (uint32_t i = 0; i < leaf.payload_count; i++) {
            dst[i] = uint32_t(leaf.payload_first) + i;
        }
        chunks.write[k] = chunk;
    }
    r_chunks = chunks;
    return true;
}

Dictionary GaussianSplatWorld::get_hlod_info() const {
    MutexLock lock(render_input_mutex);
    return hlod_tree.get_info();
}

// Probes the two allocations a payload copy makes (gaussians, then high-order SH).
static bool _probe_bake_payload(uint64_t p_gaussian_bytes, uint64_t p_sh_bytes, const char *p_where_gaussians,
        const char *p_where_sh) {
    return gs_hlod::bake_allocation_probe(p_gaussian_bytes, p_where_gaussians) &&
            gs_hlod::bake_allocation_probe(p_sh_bytes, p_where_sh);
}

Error GaussianSplatWorld::bake_hlod() {
    return _bake_hlod(false);
}

Dictionary GaussianSplatWorld::bake_hlod_offline(const Dictionary &p_limits, const String &p_candidate_path) const
{
	Dictionary report;
	report["error"] = ERR_INVALID_PARAMETER;
	gs_hlod::OfflineJob job;
	// Snapshot every source input under one render_input_mutex scope (World -> GaussianData order via the
	// getters; the mutex is recursive) so the pre-checks and the candidate copy describe one epoch.
	// Released before any bake, job check or file I/O.
	// Path/file-system checks stay outside the lock (file I/O).
	if (p_candidate_path.get_extension().to_lower() != "gsplatworld" || FileAccess::exists(p_candidate_path) ||
			DirAccess::dir_exists_absolute(p_candidate_path)) {
		return report;
	}
	Ref<GaussianSplatWorld> candidate;
	candidate.instantiate();
	uint32_t src_splats = 0, src_high = 0;
	{
		MutexLock lock(render_input_mutex);
		src_splats = get_splat_count();
		src_high = get_sh_high_order_count();
		// The first route has no arbitrary metadata/instances, compression or checkpoints.
		// Refuse those before serialization can allocate an unaccounted JSON/blob copy.
		if (!job.configure(p_limits) || !metadata.is_empty() || get_antialiased() || !hlod_tree.instances.is_empty() ||
				!hlod_tree.top_level_nodes.is_empty() || hlod_tree.nodes.size() > job.nodes ||
				uint64_t(src_splats) + hlod_tree.interior_splat_count > job.prepared) {
			return report;
		}
		const uint64_t old_interior = uint64_t(hlod_tree.interior_gaussians.get_capacity()) * sizeof(Gaussian) +
				uint64_t(hlod_tree.interior_sh_high_order.get_capacity()) * sizeof(Vector3);
		if (!job.reserve(src_splats, src_high, old_interior)) {
			report["error"] = ERR_OUT_OF_MEMORY;
			return report;
		}
		// Borrow the source owner; _bake_hlod snapshots and publishes only into candidate.
		candidate->gaussian_data = gaussian_data;
		candidate->chunk_payload_source = chunk_payload_source;
		candidate->splat_count_metadata = splat_count_metadata;
		candidate->sh_degree_metadata = sh_degree_metadata;
		candidate->sh_first_order_count_metadata = sh_first_order_count_metadata;
		candidate->sh_high_order_count_metadata = sh_high_order_count_metadata;
		candidate->is_2d_metadata = is_2d_metadata;
		candidate->antialiased_metadata = antialiased_metadata;
		candidate->bounds = bounds;
	}
	report["reserved_scratch_bytes"] = int64_t(job.reserved);
	gs_hlod::OfflineJobScope scope(job);
	if (!scope.acquired) {
		report["error"] = ERR_BUSY;
		return report;
	}
	if (job.check() != OK) {
		report["error"] = job.check();
		return report;
	}
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	Error error = candidate->_bake_hlod(true);
	report["bake_ms"] = double(OS::get_singleton()->get_ticks_usec() - started) / 1000.0;
	if (error == OK) {
		error = job.check();
	}
	if (error == OK) {
		ResourceFormatSaverGaussianSplatWorld saver;
		const uint64_t save_started = OS::get_singleton()->get_ticks_usec();
		error = saver.save_offline_candidate(candidate, p_candidate_path, job.output);
		report["save_ms"] = double(OS::get_singleton()->get_ticks_usec() - save_started) / 1000.0;
	}
	if (error == OK) {
		error = job.check();
	}
	if (error == OK) {
		// Direct strict loader, bypassing ResourceLoader caches/import remaps.
		ResourceFormatLoaderGaussianSplatWorld loader;
		const uint64_t validate_started = OS::get_singleton()->get_ticks_usec();
		Ref<GaussianSplatWorld> validated = loader.load_resident(p_candidate_path, &error);
		report["validate_ms"] = double(OS::get_singleton()->get_ticks_usec() - validate_started) / 1000.0;
		if (error == OK && (validated.is_null() || !validated->has_hlod_tree() ||
		                    validated->get_hlod_tree().bake_rule_version != gs_hlod::kQualityBakeRuleVersion ||
		                    validated->get_splat_count() != src_splats ||
		                    validated->get_sh_first_order_count() != get_sh_first_order_count() ||
		                    validated->get_sh_high_order_count() != src_high)) {
			error = ERR_FILE_CORRUPT;
		}
		if (error == OK) {
			report["hlod_info"] = validated->get_hlod_info();
		}
	}
	if (error == OK) {
		error = job.check();
	}
	if (error != OK) {
		_gs_atomic_remove_temp(p_candidate_path);
	}
	report["error"] = error;
	report["total_ms"] = double(OS::get_singleton()->get_ticks_usec() - started) / 1000.0;
	report["bake_rule_version"] = gs_hlod::kQualityBakeRuleVersion;
	return report;
}

Error GaussianSplatWorld::_bake_hlod(bool p_quality_selection) {
    // The 2D flag is carried through, not refused: the PLY loader sets it for any PLY with
    // nx/ny/nz (io/ply_loader.cpp, "has_normals"), which standard 3DGS exports carry, and no
    // shader reads it (GS_ASSET_FLAG_IS_2D / GS_INSTANCE_FLAG_IS_2D in gs_instance_layout.glsl
    // have no consumer), so refusing it would refuse almost every real scan.
    Ref<GaussianData> original_data;
    Ref<ChunkPayloadSource> original_source;
    uint64_t original_epoch;
    AABB original_bounds;
    bool original_is_2d, original_antialiased;
    uint32_t original_high_order_count;
    {
        MutexLock lock(render_input_mutex);
        original_data = gaussian_data;
        original_source = chunk_payload_source;
        original_epoch = render_input_epoch;
        original_bounds = bounds;
        original_is_2d = is_2d_metadata;
        original_antialiased = antialiased_metadata;
        original_high_order_count = sh_high_order_count_metadata;
    }
    ERR_FAIL_COND_V_MSG(original_data.is_null() && (original_source.is_null() || !original_source->is_valid()),
            ERR_UNCONFIGURED, "GaussianSplatWorld.bake_hlod(): no resident or file-backed payload to bake.");
    gs_hlod::BakeResult result;
    uint32_t sh_first_order = 0;
    uint32_t sh_high_order_count = 0;
    GaussianData::ChunkSnapshotMetadata snapshot_metadata;
    bool is_2d = original_is_2d;
    bool antialiased = original_antialiased;
    {
        // Scoped so the snapshot is freed as soon as the bake has consumed it (peak memory).
        LocalVector<Gaussian> gaussians;
        LocalVector<Vector3> sh_high_order;
        // File-backed input stays file-backed until every fallible bake step succeeds.
        // The snapshot and the baked copy are splat-count-sized LocalVector allocations, which abort
        // on failure; probe them first so memory pressure returns ERR_OUT_OF_MEMORY (and the
        // importer reaches its plain-copy fallback) instead of terminating the process.
        bool captured;
        bool snapshot_fits;
        if (original_data.is_valid()) {
            RWLockRead lock(original_data->data_rwlock);
			ERR_FAIL_COND_V (
			        gs_hlod::active_offline_job &&
			                (original_data->get_count() != gs_hlod::active_offline_job->admitted_count ||
			                 original_data->get_sh_high_order_count() != gs_hlod::active_offline_job->admitted_high),
			        ERR_INVALID_DATA)
				;
			snapshot_fits = _probe_bake_payload(uint64_t(original_data->get_count()) * sizeof(Gaussian),
                    uint64_t(original_data->get_count()) * original_data->get_sh_high_order_count() * sizeof(Vector3),
                    "GaussianSplatWorld::bake_hlod snapshot_gaussians", "GaussianSplatWorld::bake_hlod snapshot_sh_high_order");
            captured = snapshot_fits && original_data->_capture_chunk_snapshot_locked(0, original_data->get_count(),
                    gaussians, sh_high_order, sh_first_order, sh_high_order_count, &snapshot_metadata);
            is_2d = snapshot_metadata.is_2d_mode;
            antialiased = snapshot_metadata.is_antialiased;
        } else {
			ERR_FAIL_COND_V (gs_hlod::active_offline_job &&
			                         (original_source->get_count() != gs_hlod::active_offline_job->admitted_count ||
			                          original_high_order_count != gs_hlod::active_offline_job->admitted_high),
			                 ERR_INVALID_DATA)
				;
			snapshot_fits = _probe_bake_payload(uint64_t(original_source->get_count()) * sizeof(Gaussian),
                    uint64_t(original_source->get_count()) * original_high_order_count * sizeof(Vector3),
                    "GaussianSplatWorld::bake_hlod snapshot_gaussians", "GaussianSplatWorld::bake_hlod snapshot_sh_high_order");
            captured = snapshot_fits && original_source->capture_chunk_snapshot(0, original_source->get_count(),
                    gaussians, sh_high_order, sh_first_order, sh_high_order_count);
        }
        ERR_FAIL_COND_V_MSG(!snapshot_fits, ERR_OUT_OF_MEMORY, "GaussianSplatWorld.bake_hlod(): cannot allocate the payload snapshot.");
        ERR_FAIL_COND_V(!captured, ERR_CANT_ACQUIRE_RESOURCE);
        ERR_FAIL_COND_V_MSG(gaussians.is_empty(), ERR_UNCONFIGURED,
                "GaussianSplatWorld.bake_hlod(): the world has no splats.");
#ifdef TESTS_ENABLED
        if (hlod_bake_test_hook) {
            hlod_bake_test_hook(hlod_bake_test_userdata, this);
        }
#endif
        gs_hlod::BakeInput input;
        input.gaussians = gaussians.ptr();
        input.count = gaussians.size();
        input.sh_first_order_count = sh_first_order;
        input.sh_high_order_count = sh_high_order_count;
        input.sh_high_order = sh_high_order.is_empty() ? nullptr : sh_high_order.ptr();
        gs_hlod::BakeParams params;
        params.mode = p_quality_selection ? gs_hlod::BakeParams::QUALITY_SELECTION : gs_hlod::BakeParams::MERGE_ONLY;
        String error;
        Error bake_err = ERR_INVALID_DATA;
        if (!gs_hlod::bake_world(input, params, result, &error, &bake_err)) {
            ERR_FAIL_V_MSG(bake_err, vformat("GaussianSplatWorld.bake_hlod(): %s", error));
        }
    }

    // Build every fallible piece of the replacement before touching the world, so a failure
    // leaves the world exactly as it was (payload, chunks, source, tree).
    Vector<GaussianSplatRenderer::StaticChunk> leaf_chunks;
    if (!build_hlod_leaf_chunks(result.tree, leaf_chunks)) {
        ERR_FAIL_V_MSG(ERR_OUT_OF_MEMORY, "GaussianSplatWorld.bake_hlod(): cannot allocate the leaf chunks.");
    }
    result.leaf_source_index.reset();

    if (!_probe_bake_payload(uint64_t(result.leaf_gaussians.size()) * sizeof(Gaussian),
                uint64_t(result.leaf_sh_high_order.size()) * sizeof(Vector3),
                "GaussianSplatWorld::bake_hlod baked_gaussians", "GaussianSplatWorld::bake_hlod baked_sh_high_order")) {
        ERR_FAIL_V_MSG(ERR_OUT_OF_MEMORY, "GaussianSplatWorld.bake_hlod(): cannot allocate the baked payload.");
    }

    // Retain the original until publication so an edit cannot be silently overwritten.
    Ref<GaussianData> baked;
    baked.instantiate();
    baked->set_gaussian_payload(result.leaf_gaussians, result.leaf_sh_high_order, sh_first_order, sh_high_order_count, is_2d,
            antialiased);
    result.leaf_gaussians.reset();
    result.leaf_sh_high_order.reset();
    // The bounds become the RenderingServer custom AABB (GaussianSplatWorld3D::_update_bounds()),
    // so they must enclose the root's covariance-aware support, as the v2 saver's header does.
    AABB baked_bounds;
    if (!hlod_live_bounds(result.tree, original_bounds.has_volume() ? original_bounds : baked->get_aabb(), baked_bounds)) {
        ERR_FAIL_V_MSG(ERR_INVALID_DATA, "GaussianSplatWorld.bake_hlod(): the HLOD root bounds do not fit runtime coordinates.");
    }
    auto publish = [&]() {
        gaussian_data = baked;
        chunk_payload_source.unref(); // it indexes the old order; the baked payload is resident
        splat_count_metadata = baked->get_count();
        sh_degree_metadata = baked->get_sh_degree();
        sh_first_order_count_metadata = sh_first_order;
        sh_high_order_count_metadata = sh_high_order_count;
        is_2d_metadata = is_2d;
        antialiased_metadata = antialiased;
        bounds = baked_bounds;
        hlod_tree = std::move(result.tree);
        hlod_tree.leaf_payload_revision = baked->get_content_revision();
        hlod_tree.leaf_payload_revision_valid = true;
        static_chunks = leaf_chunks;
    };
    {
        MutexLock world_lock(render_input_mutex);
    ERR_FAIL_COND_V_MSG(render_input_epoch != original_epoch || gaussian_data != original_data || chunk_payload_source != original_source,
            ERR_INVALID_DATA, "GaussianSplatWorld.bake_hlod(): payload source changed during the bake.");
    if (original_data.is_valid()) {
        // Revalidate and publish in one read-lock interval, excluding concurrent payload/2D edits.
        RWLockRead lock(original_data->data_rwlock);
        ERR_FAIL_COND_V_MSG(original_data->get_content_revision() != snapshot_metadata.content_revision,
                ERR_INVALID_DATA, "GaussianSplatWorld.bake_hlod(): payload changed during the bake.");
        publish();
    } else {
        ERR_FAIL_COND_V_MSG(is_2d_metadata != is_2d, ERR_INVALID_DATA,
                "GaussianSplatWorld.bake_hlod(): payload metadata changed during the bake.");
        publish();
    }
        ++render_input_epoch;
    }
    // Notifications must run after the original data lock has been released.
    notify_property_list_changed();
    emit_changed();
    return OK;
}

bool GaussianSplatWorld::is_hlod_payload_current() const {
    MutexLock lock(render_input_mutex);
    if (hlod_tree.is_empty() || gaussian_data.is_null()) {
        return true; // no tree, or a file-backed payload (immutable on disk)
    }
    return hlod_tree.leaf_payload_revision_valid && hlod_tree.leaf_payload_revision == gaussian_data->get_content_revision();
}

bool GaussianSplatWorld::is_hlod_runtime_snapshot_current(const GaussianSplatHlodRuntimeData &p_snapshot) const {
    MutexLock world_lock(render_input_mutex);
    if (render_input_epoch != p_snapshot.source_epoch || gaussian_data != p_snapshot.source_data ||
            gaussian_data.is_null() || !hlod_tree.leaf_payload_revision_valid) {
        return false;
    }
    RWLockRead data_lock(gaussian_data->data_rwlock);
    return gaussian_data->get_content_revision() == p_snapshot.source_revision &&
            hlod_tree.leaf_payload_revision == p_snapshot.source_revision &&
            gaussian_data->animation_state_machine.is_null() && !gaussian_data->edit_state.has_runtime_modifications;
}

Error GaussianSplatWorld::capture_hlod_runtime_snapshot(Ref<GaussianSplatHlodRuntimeData> &r_snapshot, String &r_reason) const {
    const uint64_t started = OS::get_singleton()->get_ticks_usec();
    Ref<GaussianSplatHlodRuntimeData> next;
    next.instantiate();
    auto fail = [&](Error p_error, const char *p_reason) {
        r_reason = p_reason;
        return p_error;
    };
    uint64_t bytes = 0;
    auto charge = [&](uint64_t p_count, uint64_t p_stride) {
        uint64_t amount;
        if (!gs_hlod_runtime::allocation_bytes(p_count, p_stride, amount) ||
                amount > GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT - bytes) {
            return false;
        }
        bytes += amount;
        return true;
    };
    {
        MutexLock world_lock(render_input_mutex);
        if (gaussian_data.is_null() || hlod_tree.nodes.is_empty() || !hlod_tree.interior_resident ||
                !hlod_tree.instances.is_empty() || !hlod_tree.top_level_nodes.is_empty()) {
            return fail(ERR_UNAVAILABLE, "hlod_requires_resident_complete_uninstanced_tree");
        }
        const uint64_t total = uint64_t(hlod_tree.leaf_splat_count) + hlod_tree.interior_splat_count;
        if (hlod_tree.leaf_splat_count > GaussianSplatHlodRuntimeData::MAX_LEAVES ||
                total > GaussianSplatHlodRuntimeData::MAX_PAYLOAD ||
                hlod_tree.nodes.size() > GaussianSplatHlodRuntimeData::MAX_NODES) {
            return fail(ERR_PARAMETER_RANGE_ERROR, "hlod_source_admission_limit");
        }
        Error validation_error = OK;
        String validation_reason;
        if (!gs_hlod_validate_tree(hlod_tree, &validation_reason, &validation_error)) {
            r_reason = "hlod_invalid_tree: " + validation_reason;
            return validation_error;
        }
        next->source_data = gaussian_data;
        next->source_epoch = render_input_epoch;
        next->leaf_count = hlod_tree.leaf_splat_count;
        next->parent_count = hlod_tree.interior_splat_count;
        next->high_order_count = hlod_tree.sh_high_order_count;
        if (next->high_order_count > 21 || hlod_tree.interior_gaussians.size() != next->parent_count ||
                uint64_t(hlod_tree.interior_sh_high_order.size()) != uint64_t(next->parent_count) * next->high_order_count) {
            return fail(ERR_INVALID_DATA, "hlod_parent_extent_invalid");
        }
        RWLockRead data_lock(gaussian_data->data_rwlock);
        if (gaussian_data->get_count() != next->leaf_count || gaussian_data->get_sh_high_order_count() != next->high_order_count ||
                gaussian_data->get_sh_degree() > 4 || gaussian_data->animation_state_machine.is_valid() ||
                gaussian_data->edit_state.has_runtime_modifications) {
            return fail(ERR_UNAVAILABLE, "hlod_payload_layout_or_deformation_unsupported");
        }
        // Retained source, detached snapshots, tree/chunk tables, four complete histories and
        // conservative simultaneous raw+quantized packing scratch. All counts are bounded above.
        if (!charge(gaussian_data->gaussians.get_capacity(), sizeof(Gaussian)) ||
                !charge(gaussian_data->sh_high_order_coefficients.get_capacity(), sizeof(Vector3)) ||
                !charge(total, sizeof(Gaussian)) || !charge(total * next->high_order_count, sizeof(Vector3)) ||
                !charge(hlod_tree.nodes.get_capacity(), sizeof(GaussianSplatHlodNode)) ||
                !charge(hlod_tree.interior_gaussians.get_capacity(), sizeof(Gaussian)) ||
                !charge(hlod_tree.interior_sh_high_order.get_capacity(), sizeof(Vector3)) ||
                !charge(hlod_tree.nodes.size(), sizeof(GaussianSplatHlodRuntimeData::Node)) ||
                !charge(static_chunks.size(), sizeof(GaussianSplatRenderer::StaticChunk)) ||
                !charge(total, 2 * MAX(sizeof(PackedGaussian), sizeof(PackedGaussianQuantized))) ||
                !charge(uint64_t(GaussianSplatHlodRuntimeData::MAX_NODES) * 4, 64)) {
            return fail(ERR_OUT_OF_MEMORY, "hlod_cpu_admission_limit");
        }
        for (const GaussianSplatRenderer::StaticChunk &chunk : static_chunks) {
            if (!charge(chunk.indices.size(), sizeof(uint32_t))) {
                return fail(ERR_OUT_OF_MEMORY, "hlod_cpu_chunk_limit");
            }
        }
    }
    // Never hold World while acquiring the potentially large detached GaussianData snapshot.
    Error capture_error = next->source_data->capture_save_snapshot(next->leaves);
    if (capture_error != OK) {
        return fail(capture_error, "hlod_leaf_snapshot_failed");
    }
    next->source_revision = next->leaves.get_content_revision();
    next->sh_degree = next->leaves.get_sh_degree();
    next->mode_2d = next->leaves.get_2d_mode();
    next->antialiased = next->leaves.get_antialiased();
    {
        MutexLock world_lock(render_input_mutex);
        {
            RWLockRead data_lock(next->source_data->data_rwlock);
            if (render_input_epoch != next->source_epoch || gaussian_data != next->source_data ||
                    next->source_data->get_content_revision() != next->source_revision ||
                    !hlod_tree.leaf_payload_revision_valid || hlod_tree.leaf_payload_revision != next->source_revision ||
                    next->source_data->animation_state_machine.is_valid() || next->source_data->edit_state.has_runtime_modifications) {
                return fail(ERR_BUSY, "hlod_source_changed_during_capture");
            }
        }
        if (!gs_resize_or_fail(next->nodes, hlod_tree.nodes.size(), "HLOD runtime nodes") ||
                !gs_resize_or_fail(next->parents, next->parent_count, "HLOD runtime parents") ||
                !gs_resize_or_fail(next->parent_sh, uint64_t(next->parent_count) * next->high_order_count, "HLOD runtime parent SH")) {
            return fail(ERR_OUT_OF_MEMORY, "hlod_parent_snapshot_failed");
        }
        for (uint32_t i = 0; i < next->parent_count; ++i) {
            next->parents.write[i] = hlod_tree.interior_gaussians[i];
        }
        for (int64_t i = 0; i < next->parent_sh.size(); ++i) {
            next->parent_sh.write[i] = hlod_tree.interior_sh_high_order[i];
        }
        for (int64_t i = 0; i < next->nodes.size(); ++i) {
            GaussianSplatHlodRuntimeData::Node &out = next->nodes.write[i];
            out.topology = hlod_tree.nodes[i];
            const GaussianSplatHlodNode &node = out.topology;
            const uint64_t total = uint64_t(next->leaf_count) + next->parent_count;
            if (node.payload_first > total || node.payload_count > total - node.payload_first ||
                    node.payload_count == 0 || node.first_child > uint32_t(next->nodes.size()) ||
                    node.child_count > uint32_t(next->nodes.size()) - node.first_child) {
                return fail(ERR_INVALID_DATA, "hlod_node_range_invalid");
            }
            double center[3];
            hlod_tree.node_cell_center(node, center);
            double bounds_center_shift_squared = 0.0;
            for (int a = 0; a < 3; ++a) {
                out.exact_cell_center[a] = center[a];
                out.payload_center[a] = float(center[a]);
                const double exact_bounds_center = center[a] + (double(node.aabb_min[a]) + node.aabb_max[a]) * 0.5;
                out.bounds_center[a] = float(exact_bounds_center);
                if (!Math::is_finite(out.payload_center[a]) || !Math::is_finite(out.bounds_center[a])) {
                    return fail(ERR_INVALID_DATA, "hlod_center_unrepresentable");
                }
                const double shift = exact_bounds_center - double(out.bounds_center[a]);
                bounds_center_shift_squared += shift * shift;
            }
            // Moving the sphere center to float32 must never narrow coverage.
            // Enlarge only this detached runtime bound by the center displacement,
            // then round outwards; the baked tree and its format stay unchanged.
            const double runtime_radius = double(node.radius) + std::sqrt(bounds_center_shift_squared);
            out.topology.radius = std::nextafter(float(runtime_radius), std::numeric_limits<float>::infinity());
            if (!std::isfinite(runtime_radius) || runtime_radius < 0.0 || !std::isfinite(out.topology.radius)) {
                return fail(ERR_INVALID_DATA, "hlod_bounds_radius_unrepresentable");
            }
            for (uint32_t j = 0; j < node.payload_count; ++j) {
                const uint64_t index = node.payload_first + j;
                const Gaussian &g = next->gaussian(index);
                for (int a = 0; a < 3; ++a) {
                    const float canonical = index < next->leaf_count ? float(g.position[a]) : float(center[a] + double(g.position[a]));
                    const float relative = index < next->leaf_count ? float(double(g.position[a]) - center[a]) : float(g.position[a]);
                    const float reconstructed = float(float(out.payload_center[a]) + relative);
                    if (!Math::is_finite(canonical) || !Math::is_finite(relative) || reconstructed != canonical) {
                        return fail(ERR_UNAVAILABLE, "hlod_center_reconstruction_not_exact");
                    }
                }
                double scales[3] = { double(g.scale.x), double(g.scale.y), double(g.scale.z) };
                for (double scale : scales) {
                    if (!Math::is_finite(scale) || scale < 0.0) {
                        return fail(ERR_INVALID_DATA, "hlod_footprint_scale_invalid");
                    }
                }
                std::sort(scales, scales + 3);
                out.footprint += 36.0 * scales[1] * scales[2];
                if (!Math::is_finite(out.footprint)) {
                    return fail(ERR_INVALID_DATA, "hlod_footprint_overflow");
                }
                const uint32_t dc = uint32_t(gaussian_get_dc_encoding(g.render_meta));
                if (j == 0) {
                    out.dc_encoding = dc;
                } else if (dc != out.dc_encoding) {
                    out.homogeneous_dc = false;
                    next->quantized_eligible = false;
                }
            }
        }
    }
    next->source_world = Ref<Resource>(const_cast<GaussianSplatWorld *>(this));
    next->owner_key = get_instance_id();
    next->owned_cpu_bytes = bytes;
    next->preparation_usec = OS::get_singleton()->get_ticks_usec() - started;
    r_reason = String();
    r_snapshot = next;
    return OK;
}

uint64_t GaussianSplatWorld::capture_render_input(Ref<GaussianData> &r_data, Ref<ChunkPayloadSource> &r_source,
        Vector<GaussianSplatRenderer::StaticChunk> &r_chunks, AABB &r_bounds, Dictionary &r_metadata) const {
    MutexLock lock(render_input_mutex);
    r_data = gaussian_data;
    r_source = chunk_payload_source;
    r_chunks = static_chunks;
    r_bounds = bounds;
    r_metadata = metadata.duplicate(true);
    return render_input_epoch;
}
