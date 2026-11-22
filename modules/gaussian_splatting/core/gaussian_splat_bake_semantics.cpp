#include "gaussian_splat_bake_semantics.h"
#include "gaussian_splat_hlod_bake_bounds.h"
#include "gs_project_settings.h"

#include "../io/gs_world_writer.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/math/convex_hull.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gs_hlod {
namespace {
struct Cell {
    int64_t x = 0, z = 0;
    bool operator==(const Cell &p_other) const { return x == p_other.x && z == p_other.z; }
};
struct CellHasher {
    static uint32_t hash(const Cell &p_cell) { return hash_murmur3_one_64(uint64_t(p_cell.z), hash_murmur3_one_64(uint64_t(p_cell.x))); }
};
using Cells = HashSet<Cell, CellHasher>;
bool cell_at(const Vector3 &p, Cell &r_cell) {
    const double x = std::floor(double(p.x) / kIslandHoleCellSizeM);
    const double z = std::floor(double(p.z) / kIslandHoleCellSizeM);
    if (!std::isfinite(x) || !std::isfinite(z) || std::abs(x) >= 9.0e18 || std::abs(z) >= 9.0e18) { return false; }
    r_cell = {int64_t(x), int64_t(z)};
    return true;
}
bool vec3(const Variant &p, Vector3 &r) {
    if (p.get_type() == Variant::VECTOR3) { r = p; return r.is_finite(); }
    if (p.get_type() != Variant::ARRAY) { return false; }
    const Array a = p;
    if (a.size() != 3) { return false; }
    for (int i = 0; i < 3; ++i) {
        if (a[i].get_type() != Variant::INT && a[i].get_type() != Variant::FLOAT) { return false; }
        r[i] = double(a[i]);
    }
    return r.is_finite();
}
Array xyz(const Vector3 &p) {
    Array a;
    a.push_back(p.x);
    a.push_back(p.y);
    a.push_back(p.z);
    return a;
}
// Row-major 3x3 basis followed by the origin; the identity transform.
Array identity_transform_json() {
    Array a;
    const int values[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    for (int v : values) {
        a.push_back(v);
    }
    return a;
}
// TiledBakeSource contract: centre_bounds is closed and its upper face is
// AABB::get_end(), evaluated in real_t. AABB::expand_to stores a rounded
// end - begin, so position + size can land one ulp below the largest centre.
// Round each size up until position + size covers the exact maximum.
AABB closed_centre_bounds(const Vector3 &p_lo, const Vector3 &p_hi) {
    Vector3 size;
    for (int a = 0; a < 3; ++a) {
        real_t s = real_t(double(p_hi[a]) - double(p_lo[a]));
        while (std::isfinite(s) && real_t(p_lo[a] + s) < p_hi[a]) { s = std::nextafter(s, std::numeric_limits<real_t>::infinity()); }
        size[a] = s;
    }
    return AABB(p_lo, size);
}
Dictionary bounds_json(const AABB &p) { Dictionary d; d["position"] = xyz(p.position); d["size"] = xyz(p.size); return d; }
struct Volume {
    String kind, path, name;
    Transform3D inverse;
    Vector3 size;
    LocalVector<Plane> planes;
    uint32_t id = 0;
    uint8_t wind = 0;
    bool contains(const Vector3 &p) const {
        const Vector3 q = inverse.xform(p);
        if (planes.is_empty()) {
            return std::abs(q.x) <= size.x * 0.5 && std::abs(q.y) <= size.y * 0.5 && std::abs(q.z) <= size.z * 0.5;
        }
        for (const Plane &plane : planes) { if (plane.distance_to(q) > 0.000001) { return false; } }
        return true;
    }
};
struct Island {
    uint32_t id = 0, count = 0;
    String name, path;
    AABB bounds;
    bool has_bounds = false;
    Cells cells;
};
struct Chunk {
    uint32_t source = 0;
    TiledBakeChunkInfo info;
};
bool packed_ids(const Variant &p, PackedInt32Array &r_ids) {
    if (p.get_type() == Variant::PACKED_INT32_ARRAY) { r_ids = p; return true; }
    if (p.get_type() != Variant::ARRAY) { return false; }
    const Array a = p;
    if (r_ids.resize(a.size()) != OK) { return false; }
    for (int i = 0; i < a.size(); ++i) {
        if ((a[i].get_type() != Variant::INT && a[i].get_type() != Variant::FLOAT) || !std::isfinite(double(a[i])) || double(a[i]) < 0 || double(a[i]) > INT32_MAX || std::floor(double(a[i])) != double(a[i])) { return false; }
        r_ids.set(i, int32_t(a[i]));
    }
    return true;
}
class SpanSource : public TiledBakeSource {
    BakeInput input;
public:
    explicit SpanSource(const BakeInput &p) : input(p) {}
    TiledBakeSourceInfo get_info() const override { return {input.count, 1, input.sh_high_order_count, input.sh_first_order_count}; }
    Error get_chunk_info(uint32_t p_id, TiledBakeChunkInfo &r, String *) const override {
        if (p_id != 0) { return ERR_INVALID_PARAMETER; }
        r.first_splat = 0; r.splat_count = input.count;
        if (!input.count) { return OK; }
        Vector3 lo = input.gaussians[0].position, hi = lo;
        for (uint32_t i = 1; i < input.count; ++i) { lo = lo.min(input.gaussians[i].position); hi = hi.max(input.gaussians[i].position); }
        r.centre_bounds = closed_centre_bounds(lo, hi);
        return OK;
    }
    Error read_chunk(uint32_t p_id, TiledBakeChunk &r, String *) const override {
        if (p_id != 0) { return ERR_INVALID_PARAMETER; }
        if (!fallible_resize(r.gaussians, input.count, "semantic span records") ||
                !fallible_resize(r.sh_high_order, uint64_t(input.count) * input.sh_high_order_count, "semantic span SH")) { return ERR_OUT_OF_MEMORY; }
        if (input.count) { memcpy(r.gaussians.ptr(), input.gaussians, uint64_t(input.count) * sizeof(Gaussian)); }
        if (r.sh_high_order.size()) { memcpy(r.sh_high_order.ptr(), input.sh_high_order, uint64_t(r.sh_high_order.size()) * sizeof(Vector3)); }
        return OK;
    }
};
}

struct SemanticBakeSource::State {
    const TiledBakeSource *source = nullptr;
    TiledBakeSourceInfo original, info;
    LocalVector<Chunk> chunks;
    LocalVector<Volume> volumes;
    LocalVector<Island> islands;
    Dictionary metadata, report;
    PackedInt32Array ply_islands;
    uint32_t fields = 0;
    uint64_t reserved = 0, read_workspace = 0, output = 0, deadline = UINT64_MAX;
    String reason;
    Vector<String> created;
    bool committed = false, semantic_input = false;
    Error fail(const String &p_reason, Error p_error = ERR_INVALID_DATA) { reason = p_reason; return p_error; }
    Error check() { return OS::get_singleton()->get_ticks_usec() >= deadline ? fail("semantic_deadline", ERR_TIMEOUT) : OK; }
    uint32_t source_island(uint32_t p_index) const { return ply_islands.is_empty() ? 0u : uint32_t(ply_islands[p_index]); }
    uint32_t assign(Gaussian &r_g, uint32_t p_index, uint64_t *p_overrides = nullptr) const {
        uint16_t region = gaussian_semantic_region(r_g.semantic);
        uint8_t wind = gaussian_semantic_wind_weight(r_g.semantic);
        uint32_t island = source_island(p_index);
        bool matched[3] = {};
        for (const Volume &v : volumes) {
            const int field = v.kind == "region" ? 0 : (v.kind == "wind" ? 1 : 2);
            if (matched[field] || !v.contains(r_g.position)) { continue; }
            matched[field] = true;
            const uint32_t old = field == 0 ? region : (field == 1 ? wind : island);
            const uint32_t value = field == 1 ? v.wind : v.id;
            if (p_overrides && (fields & (1u << field)) && old != value) { ++p_overrides[field]; }
            if (field == 0) { region = uint16_t(value); }
            else if (field == 1) { wind = uint8_t(value); }
            else { island = value; }
        }
        if (semantic_input) { r_g.semantic = gaussian_pack_semantic(region, wind, true); }
        return island;
    }
    int island_index(uint32_t p_id) const {
        for (uint32_t i = 0; i < islands.size(); ++i) { if (islands[i].id == p_id) { return int(i); } }
        return -1;
    }
    Error read_filtered(uint32_t p_source, uint32_t p_island, TiledBakeChunk &r_out) const {
        TiledBakeChunk raw; TiledBakeChunkInfo descriptor;
        Error error = source->get_chunk_info(p_source, descriptor, nullptr);
        if (error == OK) { error = source->read_chunk(p_source, raw, nullptr); }
        if (error != OK) { return error; }
        if (raw.gaussians.size() != descriptor.splat_count || raw.sh_high_order.size() != uint64_t(descriptor.splat_count) * info.sh_high_order_count) { return ERR_INVALID_DATA; }
        if (!fallible_resize(r_out.gaussians, raw.gaussians.size(), "semantic filtered records") ||
                !fallible_resize(r_out.sh_high_order, raw.sh_high_order.size(), "semantic filtered SH")) { return ERR_OUT_OF_MEMORY; }
        uint32_t count = 0;
        for (uint32_t i = 0; i < raw.gaussians.size(); ++i) {
            Gaussian g = raw.gaussians[i];
            if (assign(g, descriptor.first_splat + i) != p_island) { continue; }
            r_out.gaussians[count] = g;
            for (uint32_t c = 0; c < info.sh_high_order_count; ++c) { r_out.sh_high_order[uint64_t(count) * info.sh_high_order_count + c] = raw.sh_high_order[uint64_t(i) * info.sh_high_order_count + c]; }
            ++count;
        }
        r_out.gaussians.resize(count); r_out.sh_high_order.resize(uint64_t(count) * info.sh_high_order_count);
        return OK;
    }
};

SemanticBakeSource::SemanticBakeSource() { state = memnew(State); }
SemanticBakeSource::~SemanticBakeSource() {
    if (!state->committed) { for (const String &path : state->created) {
        if (DirAccess::remove_absolute(path) != OK) { ERR_PRINT("semantic_island_cleanup_failed: " + path); }
    } }
    memdelete(state);
}
Error SemanticBakeSource::open(const TiledBakeSource &p_source, const Array &p_volumes, const Dictionary &p_metadata,
        const String &p_directory, uint64_t p_max_ram, uint64_t p_max_output, uint64_t p_deadline, uint32_t p_degree, bool p_2d) {
    State &s = *state;
    if (s.source) { return s.fail("semantic_source_already_open"); }
    s.source = &p_source; s.original = s.info = p_source.get_info(); s.metadata = p_metadata.duplicate(true); s.deadline = p_deadline;
    const Variant side_value = p_metadata.get("gs_ply_semantics", p_metadata.has("fields") ? Variant(p_metadata) : Variant(Dictionary()));
    if (side_value.get_type() != Variant::DICTIONARY) { return s.fail("semantic_ply_side_data"); }
    const Dictionary side = side_value;
    if ((p_metadata.has("regions") && p_metadata["regions"].get_type() != Variant::ARRAY) ||
            (p_metadata.has("islands") && p_metadata["islands"].get_type() != Variant::ARRAY)) { return s.fail("semantic_manifest_type"); }
    const Variant field_value = side.get("fields", 0);
    if ((field_value.get_type() != Variant::INT && field_value.get_type() != Variant::FLOAT) || !std::isfinite(double(field_value)) || double(field_value) < 0 || double(field_value) > 7 || std::floor(double(field_value)) != double(field_value)) { return s.fail("semantic_ply_fields"); }
    s.fields = int64_t(side.get("fields", 0));
    s.semantic_input = s.fields != 0 || !p_volumes.is_empty();
    if (s.fields & 4u) {
        if (!packed_ids(side.get("island_ids", Variant()), s.ply_islands) || uint64_t(s.ply_islands.size()) != s.original.splat_count) { return s.fail("semantic_ply_island_side_data"); }
        for (int32_t id : s.ply_islands) { if (id < 0) { return s.fail("semantic_ply_island_id_range"); } }
    }
    // Worst-case cell sets, volume descriptors, chunk maps and two decoded chunks.
    // Admission uses the caller's existing RAM ceiling, never a larger bake budget.
    s.reserved = uint64_t(s.original.splat_count) * 512u + uint64_t(s.original.chunk_count) * 256u + uint64_t(JSON::stringify(p_volumes).utf8().length()) * 128u + (1u << 20);
    if (s.reserved > p_max_ram || !bake_allocation_probe(s.reserved, "semantic source reservation")) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
    bool used_regions[65536] = {};
    uint32_t max_region = 0, max_island = 0;
    for (uint32_t c = 0; c < s.original.chunk_count; ++c) {
        if (s.check() != OK) { return ERR_TIMEOUT; }
        TiledBakeChunk chunk; TiledBakeChunkInfo descriptor;
        Error error = p_source.get_chunk_info(c, descriptor, nullptr);
        if (error != OK) { return s.fail("semantic_source_read", error); }
        if (uint64_t(descriptor.first_splat) + descriptor.splat_count > s.original.splat_count) { return s.fail("semantic_source_descriptor"); }
        // Raw and filtered vectors grow to ORIGINAL chunk size, even if only
        // one world splat survives. Includes <=1.5x capacity and growth probes.
        s.read_workspace = MAX(s.read_workspace, uint64_t(descriptor.splat_count) * 6u * (sizeof(Gaussian) + uint64_t(s.info.sh_high_order_count) * sizeof(Vector3)));
        if (s.read_workspace > p_max_ram - s.reserved) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
        error = p_source.read_chunk(c, chunk, nullptr);
        if (error != OK) { return s.fail("semantic_source_read", error); }
        if (chunk.gaussians.size() != descriptor.splat_count || chunk.sh_high_order.size() != uint64_t(descriptor.splat_count) * s.info.sh_high_order_count) { return s.fail("semantic_source_descriptor"); }
        for (uint32_t i = 0; i < chunk.gaussians.size(); ++i) {
            const Gaussian &g = chunk.gaussians[i];
            if (!gaussian_semantic_validate(g.semantic) || !g.position.is_finite()) { return s.fail("semantic_input_invalid"); }
            const uint32_t id = gaussian_semantic_region(g.semantic);
            if (id) { used_regions[id] = true; max_region = MAX(max_region, id); }
            max_island = MAX(max_island, s.source_island(descriptor.first_splat + i));
        }
    }
    Array regions = s.metadata.get("regions", Array());
    bool manifest_ids[65536] = {};
    HashSet<String> manifest_names;
    for (const Variant &entry : regions) {
        if (entry.get_type() != Variant::DICTIONARY) { return s.fail("semantic_region_manifest"); }
        const Dictionary d = entry;
        const Variant id_value = d.get("id", 0);
        if ((id_value.get_type() != Variant::INT && id_value.get_type() != Variant::FLOAT) || !std::isfinite(double(id_value)) || double(id_value) < 1 || double(id_value) > 65535 || std::floor(double(id_value)) != double(id_value) || d.get("name", "").get_type() != Variant::STRING) { return s.fail("semantic_region_manifest"); }
        const uint32_t id = int64_t(id_value); const String name = d["name"];
        if (manifest_ids[id] || name.is_empty() || manifest_names.has(name)) { return s.fail("semantic_region_manifest"); }
        manifest_ids[id] = true; manifest_names.insert(name); max_region = MAX(max_region, id);
    }
    for (uint32_t id = 1; id <= max_region; ++id) {
        if (!used_regions[id]) { continue; }
        bool found = false;
        for (const Variant &entry : regions) { const Dictionary d = entry; if (int64_t(d.get("id", -1)) == id) { found = true; break; } }
        if (!found) {
            const String name = "ply_" + itos(id);
            if (manifest_names.has(name)) { return s.fail("semantic_duplicate_region_name"); }
            manifest_names.insert(name);
            Dictionary d; d["id"] = id; d["name"] = name; regions.push_back(d);
        }
    }
    if (!fallible_resize(s.volumes, p_volumes.size(), "semantic volumes")) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
    for (int i = 0; i < p_volumes.size(); ++i) {
        if (p_volumes[i].get_type() != Variant::DICTIONARY) { return s.fail("semantic_volume_descriptor"); }
        const Dictionary d = p_volumes[i]; Volume &v = s.volumes[i];
        for (const Variant &key : d.keys()) {
            const String name = key;
            if (key.get_type() != Variant::STRING || (name != "kind" && name != "path" && name != "name" && name != "size" && name != "transform" && name != "hull" && name != gs::settings::WIND_WEIGHT_PROPERTY)) { return s.fail("semantic_volume_descriptor"); }
        }
        for (const char *name : { "kind", "path", "name" }) {
            if (d.has(name) && d[name].get_type() != Variant::STRING) { return s.fail("semantic_volume_descriptor"); }
        }
        v.kind = d.get("kind", ""); v.path = d.get("path", ""); v.name = d.get("name", "");
        if ((v.kind != "region" && v.kind != "wind" && v.kind != "island") || v.path.is_empty()) { return s.fail("semantic_volume_kind_or_path"); }
        Transform3D transform;
        const Variant encoded = d.get("transform", Transform3D());
        if (encoded.get_type() == Variant::TRANSFORM3D) { transform = encoded; }
        else if (encoded.get_type() == Variant::ARRAY) {
            const Array a = encoded;
            if (a.size() != 12) { return s.fail("semantic_volume_transform"); }
            for (int k = 0; k < 12; ++k) {
                if ((a[k].get_type() != Variant::INT && a[k].get_type() != Variant::FLOAT) || !std::isfinite(double(a[k]))) { return s.fail("semantic_volume_transform"); }
                if (k < 9) { transform.basis[k / 3][k % 3] = double(a[k]); } else { transform.origin[k - 9] = double(a[k]); }
            }
        } else { return s.fail("semantic_volume_transform"); }
        if (!transform.is_finite() || std::abs(transform.basis.determinant()) < 1e-12) { return s.fail("semantic_volume_transform"); }
        v.inverse = transform.affine_inverse();
        if (!vec3(d.get("size", Vector3(1,1,1)), v.size) || v.size.x <= 0 || v.size.y <= 0 || v.size.z <= 0) { return s.fail("semantic_volume_size"); }
        const Variant hull = d.get("hull", PackedVector3Array()); Vector<Vector3> points;
        if (hull.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
            const PackedVector3Array packed = hull;
            if (points.resize(packed.size()) != OK) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
            for (int k = 0; k < packed.size(); ++k) { points.write[k] = packed[k]; }
        } else if (hull.get_type() == Variant::ARRAY) {
            const Array packed = hull;
            if (points.resize(packed.size()) != OK) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
            for (int k = 0; k < packed.size(); ++k) { if (!vec3(packed[k], points.write[k])) { return s.fail("semantic_volume_hull"); } }
        } else { return s.fail("semantic_volume_hull"); }
        if (!points.is_empty()) {
            for (const Vector3 &point : points) { if (!point.is_finite()) { return s.fail("semantic_volume_hull"); } }
            Geometry3D::MeshData mesh;
            if (points.size() < 4 || ConvexHullComputer::convex_hull(points, mesh) != OK || mesh.faces.size() < 4) { return s.fail("semantic_volume_hull"); }
            if (!fallible_resize(v.planes, mesh.faces.size(), "semantic hull planes")) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
            for (uint32_t k = 0; k < mesh.faces.size(); ++k) { v.planes[k] = mesh.faces[k].plane; }
        }
        const Variant weight = d.get(gs::settings::WIND_WEIGHT_PROPERTY, 0.0);
        if ((weight.get_type() != Variant::FLOAT && weight.get_type() != Variant::INT) || !std::isfinite(double(weight)) || double(weight) < 0 || double(weight) > 1) { return s.fail("semantic_wind_weight_range"); }
        v.wind = uint8_t(std::floor(double(weight) * 255.0 + 0.5));
        if ((v.kind == "region" || v.kind == "island") && v.name.is_empty()) { v.name = v.path.get_file(); }
    }
    if (s.volumes.size() > 1) { std::sort(s.volumes.ptr(), s.volumes.ptr() + s.volumes.size(), [](const Volume &a, const Volume &b) { return a.path < b.path; }); }
    for (uint32_t i = 0; i < s.volumes.size(); ++i) {
        Volume &v = s.volumes[i];
        if (i && v.path == s.volumes[i-1].path) { return s.fail("semantic_duplicate_volume_path"); }
        if (v.kind == "region") {
            if (max_region == 65535) { return s.fail("semantic_region_id_exhausted", ERR_PARAMETER_RANGE_ERROR); }
            v.id = ++max_region;
            for (const Variant &entry : regions) { const Dictionary d = entry; if (String(d.get("name", "")) == v.name) { return s.fail("semantic_duplicate_region_name"); } }
            Dictionary d; d["id"] = v.id; d["name"] = v.name; regions.push_back(d);
        } else if (v.kind == "island") {
            if (max_island == INT32_MAX) { return s.fail("semantic_island_id_exhausted", ERR_PARAMETER_RANGE_ERROR); }
            v.id = ++max_island;
        }
    }
    Cells world_cells; uint64_t overrides[3] = {};
    s.info.splat_count = 0; s.info.chunk_count = 0;
    for (uint32_t c = 0; c < s.original.chunk_count; ++c) {
        if (s.check() != OK) { return ERR_TIMEOUT; }
        TiledBakeChunk raw; TiledBakeChunkInfo descriptor;
        Error error = p_source.get_chunk_info(c, descriptor, nullptr);
        if (error == OK) { error = p_source.read_chunk(c, raw, nullptr); }
        if (error != OK) { return s.fail("semantic_source_read", error); }
        Chunk mapped; mapped.source = c; mapped.info.first_splat = s.info.splat_count;
        Vector3 centre_lo, centre_hi;
        for (uint32_t i = 0; i < raw.gaussians.size(); ++i) {
            Gaussian g = raw.gaussians[i]; const uint32_t id = s.assign(g, descriptor.first_splat + i, overrides);
            Cell cell; if (!cell_at(g.position, cell)) { return s.fail("semantic_hole_grid_range"); }
            if (id == 0) {
                if (mapped.info.splat_count++ == 0) { centre_lo = centre_hi = g.position; }
                else { centre_lo = centre_lo.min(g.position); centre_hi = centre_hi.max(g.position); }
                world_cells.insert(cell);
            } else {
                int index = s.island_index(id);
                if (index < 0) {
                    const uint64_t metadata_charge = 32768u + uint64_t(p_directory.length() + 256) * 256u;
                    if (metadata_charge > p_max_ram - s.reserved || s.read_workspace > p_max_ram - s.reserved - metadata_charge ||
                            !bake_allocation_probe(metadata_charge, "semantic island metadata")) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
                    s.reserved += metadata_charge;
                    const uint32_t count = s.islands.size();
                    if (!fallible_resize(s.islands, uint64_t(count) + 1, "semantic islands")) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
                    index = int(count); Island &island = s.islands[index]; island.id = id; island.name = "ply_island_" + itos(id);
                    for (const Volume &v : s.volumes) { if (v.kind == "island" && v.id == id) { island.name = v.name; break; } }
                }
                Island &island = s.islands[index]; ++island.count; island.cells.insert(cell);
                if (!g.scale.is_finite() || !g.rotation.is_finite()) { return s.fail("semantic_island_nonfinite"); }
                bake_bounds::BoundsAccumulator support;
                const double shift[3] = {};
                bake_bounds::add_payload_bounds({&g, nullptr, 1, 0}, shift, support);
                Vector3 lo, hi;
                Vector3 size;
                for (int a = 0; a < 3; ++a) {
                    lo[a] = bake_bounds::round_down_offset(0, support.lo[a]); hi[a] = bake_bounds::round_up_offset(0, support.hi[a]);
                    size[a] = std::nextafter(real_t(double(hi[a]) - double(lo[a])), std::numeric_limits<real_t>::infinity());
                }
                if (!lo.is_finite() || !size.is_finite()) { return s.fail("semantic_island_bounds_range"); }
                const AABB bounds(lo, size);
                island.bounds = island.has_bounds ? island.bounds.merge(bounds) : bounds; island.has_bounds = true;
            }
        }
        if (mapped.info.splat_count) {
            mapped.info.centre_bounds = closed_centre_bounds(centre_lo, centre_hi);
            const uint32_t count = s.chunks.size();
            if (!fallible_resize(s.chunks, uint64_t(count) + 1, "semantic chunk map")) { return s.fail("semantic_limit_ram", ERR_OUT_OF_MEMORY); }
            s.chunks[count] = mapped; s.info.splat_count += mapped.info.splat_count; ++s.info.chunk_count;
        }
    }
    if (s.semantic_input || !regions.is_empty()) { s.metadata["regions"] = regions; }
    // Source-only PLY island indices no longer index the filtered payload.
    s.metadata.erase("gs_ply_semantics");
    Dictionary counts; counts["region"] = int64_t(overrides[0]); counts["wind"] = int64_t(overrides[1]); counts["island"] = int64_t(overrides[2]);
    s.report["semantic_ply_overridden_by_volume"] = counts;
    s.report["region_id_caveat"] = "Volume ids are NOT stable across edits. Game code must resolve regions by NAME through regions[], never hard-code numeric ids.";
    s.report["hole_metric"] = "Top-down XZ splat-centre grid only; holes in vertical surfaces behind an extracted island (walls, cliffs) are not measured. Report only, no gate.";
    s.report["island_asset_format"] = "plain_v3 (no existing V4 size threshold; all islands are always v3)";
    Array holes; Array manifest = s.metadata.get("islands", Array());
    HashSet<String> island_names;
    for (const Variant &entry : manifest) {
        if (entry.get_type() != Variant::DICTIONARY) { return s.fail("semantic_island_manifest"); }
        const Dictionary d = entry;
        if (d.get("name", Variant()).get_type() != Variant::STRING || d.get("asset_path", Variant()).get_type() != Variant::STRING ||
                d.get("bounds", Variant()).get_type() != Variant::DICTIONARY || d.get("rest_transform", Variant()).get_type() != Variant::ARRAY) { return s.fail("semantic_island_manifest"); }
        const String name = d["name"], path = d["asset_path"];
        const Dictionary bounds = d["bounds"]; Vector3 position, size;
        const Array rest = d["rest_transform"];
        if (name.is_empty() || path.is_empty() || island_names.has(name) || rest.size() != 12 ||
                !vec3(bounds.get("position", Variant()), position) || !vec3(bounds.get("size", Variant()), size) || size.x < 0 || size.y < 0 || size.z < 0) { return s.fail("semantic_island_manifest"); }
        for (const Variant &value : rest) {
            if ((value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) || !std::isfinite(double(value))) { return s.fail("semantic_island_manifest"); }
        }
        island_names.insert(name);
    }
    for (Island &island : s.islands) {
        if (p_directory.is_empty()) { return s.fail("semantic_island_output_directory_required", ERR_INVALID_PARAMETER); }
        if (island.name.is_empty() || island.name == "." || island.name == ".." || island.name.validate_filename() != island.name) { return s.fail("semantic_island_asset_name"); }
        if (island_names.has(island.name)) { return s.fail("semantic_duplicate_island_name"); }
        island_names.insert(island.name);
        island.path = p_directory.path_join(island.name + ".gsplatworld");
        if (FileAccess::exists(island.path)) { return s.fail("semantic_island_asset_exists", ERR_ALREADY_EXISTS); }
        for (const String &created : s.created) { if (created == island.path) { return s.fail("semantic_duplicate_island_asset"); } }
        if (DirAccess::make_dir_recursive_absolute(p_directory) != OK) { return s.fail("semantic_island_directory_io", ERR_CANT_CREATE); }
        gs_world_writer::PlainSpec spec; spec.splat_count = island.count; spec.chunk_count = s.original.chunk_count;
        spec.sh_degree = p_degree; spec.sh_first_order = s.info.sh_first_order_count; spec.sh_high_order = s.info.sh_high_order_count;
        spec.is_2d = p_2d; spec.bounds = island.bounds; spec.metadata["regions"] = regions;
        gs_world_writer::Writer writer;
        auto limits = gs_world_writer::Limits::unbounded(); limits.max_ram_bytes = p_max_ram - s.reserved - s.read_workspace;
        limits.max_output_bytes = p_max_output - s.output;
        limits.deadline_usec = p_deadline == UINT64_MAX ? UINT64_MAX : p_deadline - MIN(p_deadline, OS::get_singleton()->get_ticks_usec());
        Error error = writer.begin_plain(island.path, spec, gs_world_writer::CodecParams(), gs_world_writer::SourceMode::REITERABLE, limits);
        while (error == OK && writer.current_pass() != gs_world_writer::Writer::kNoPass) {
            for (uint32_t c = 0; c < s.original.chunk_count && error == OK; ++c) {
                if (s.check() != OK) { return ERR_TIMEOUT; }
                TiledBakeChunk filtered; error = s.read_filtered(c, island.id, filtered);
                gs_world_writer::ChunkDesc desc; desc.bounds = island.bounds; desc.center = island.bounds.get_center(); desc.radius = island.bounds.size.length() * 0.5;
                if (error == OK) { error = writer.append_chunk(desc, filtered.gaussians.ptr(), filtered.sh_high_order.ptr(), filtered.gaussians.size()); }
            }
        }
        if (error == OK) { error = writer.finalize(); }
        if (error != OK) { return s.fail("semantic_island_write: " + writer.get_report().reason, error); }
        s.created.push_back(island.path); s.output += writer.get_report().output_bytes;
        uint64_t uncovered = 0; for (const Cell &cell : island.cells) { if (!world_cells.has(cell)) { ++uncovered; } }
        Dictionary hole; hole["name"] = island.name; hole["hole_area_m2"] = double(uncovered) * kIslandHoleCellSizeM * kIslandHoleCellSizeM;
        hole["island_xz_footprint_m2"] = double(island.cells.size()) * kIslandHoleCellSizeM * kIslandHoleCellSizeM;
        hole["extracted_splats"] = island.count; hole["cell_size_m"] = kIslandHoleCellSizeM; holes.push_back(hole);
        Dictionary entry; entry["name"] = island.name; entry["asset_path"] = island.path; entry["bounds"] = bounds_json(island.bounds);
        // Payload remains in the bake frame, hence an identity rest transform.
        entry["rest_transform"] = identity_transform_json(); manifest.push_back(entry);
    }
    if (!manifest.is_empty()) { s.metadata["islands"] = manifest; }
    s.report["islands"] = holes;
    return OK;
}
TiledBakeSourceInfo SemanticBakeSource::get_info() const { return state->info; }
Error SemanticBakeSource::get_chunk_info(uint32_t p_id, TiledBakeChunkInfo &r_info, String *) const {
    if (p_id >= state->chunks.size()) { return ERR_INVALID_PARAMETER; }
    r_info = state->chunks[p_id].info; return OK;
}
Error SemanticBakeSource::read_chunk(uint32_t p_id, TiledBakeChunk &r_chunk, String *) const {
    if (p_id >= state->chunks.size()) { return ERR_INVALID_PARAMETER; }
    return state->read_filtered(state->chunks[p_id].source, 0, r_chunk);
}
Dictionary SemanticBakeSource::get_metadata() const { return state->metadata; }
Dictionary SemanticBakeSource::get_report() const { return state->report; }
String SemanticBakeSource::get_reason() const { return state->reason; }
uint64_t SemanticBakeSource::get_reserved_bytes() const { return state->reserved; }
uint64_t SemanticBakeSource::get_read_workspace_bytes() const { return state->read_workspace; }
uint64_t SemanticBakeSource::get_island_output_bytes() const { return state->output; }
void SemanticBakeSource::commit_islands() { state->committed = true; }

Error prepare_semantic_bake(const BakeInput &p_input, const Array &p_volumes, const Dictionary &p_source,
        const String &p_directory, SplatList &r_world, Dictionary &r_metadata, Dictionary &r_report, String &r_reason, bool p_is_2d) {
    r_world.clear(); r_world.sh_high_order_count = p_input.sh_high_order_count;
    if ((p_input.count && !p_input.gaussians) || (p_input.count && p_input.sh_high_order_count && !p_input.sh_high_order)) {
        r_reason = "semantic_input_missing_payload"; return ERR_INVALID_PARAMETER;
    }
    SpanSource source(p_input); SemanticBakeSource semantics;
    uint32_t degree = p_input.sh_high_order_count > 0 ? uint32_t(std::sqrt(4.0 + p_input.sh_high_order_count)) - 1 : (p_input.sh_first_order_count ? 1 : 0);
    Error error = semantics.open(source, p_volumes, p_source, p_directory, UINT64_MAX, UINT64_MAX, UINT64_MAX, degree, p_is_2d);
    if (error != OK) { r_reason = semantics.get_reason(); return error; }
    for (uint32_t c = 0; c < semantics.get_info().chunk_count; ++c) {
        TiledBakeChunk chunk; error = semantics.read_chunk(c, chunk, &r_reason);
        if (error == OK) { error = r_world.append({chunk.gaussians.ptr(), chunk.sh_high_order.ptr(), chunk.gaussians.size(), p_input.sh_high_order_count}, 0, chunk.gaussians.size()); }
        if (error != OK) { r_world.clear(); return error; }
    }
    r_metadata = semantics.get_metadata(); r_report = semantics.get_report(); semantics.commit_islands(); return OK;
}
}
