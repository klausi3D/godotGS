#include "ply_loader.h"
#include "gaussian_splat_world_io.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"
#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "io_settings_utils.h"
#include "../core/gaussian_splat_world.h"
#include "../logger/gs_logger.h"
#include "../logger/startup_trace.h"

#include <climits>

namespace {

// Bump this when the GaussianSplatWorld binary format, the metadata
// contract, OR the PLY decode semantics change.  try_load_cache() rejects
// caches with a different version (via _cache_version_matches) so stale data
// is never silently reused after a change.
//   v1: initial versioned PLY cache.
//   v2: read_float_property now decodes integer-typed PLY properties
//       (char/uchar/short/ushort/int/uint and int8..uint32 aliases) to their
//       real value instead of silently returning 0.0 (issue #465). Caches
//       written by v1 may hold zeroed positions/opacity/scales for such PLYs,
//       so a stale v1 .gsplatcache is rejected and the raw PLY is re-parsed
//       automatically — no manual cache deletion required.
//   v3: parse_header now rejects vertex-element `property list` declarations
//       (issue #511) and accounts for elements declared BEFORE `vertex`
//       (issue #512) instead of silently misaligning the per-vertex stride /
//       reading a preceding element's bytes as vertex data. A v1/v2 cache may
//       hold GaussianData that was decoded from a mis-parsed layout, so a stale
//       pre-v3 .gsplatcache is rejected and the raw PLY is re-parsed (which now
//       either loads correctly or fails loudly) — no manual cache deletion
//       required.
static constexpr int PLY_CACHE_VERSION = 3;

static constexpr int SH_DC_COMPONENTS = 3;
static constexpr int SH_REST_COMPONENTS = 45;
static constexpr int SH_FIRST_ORDER_COEFFS = 3;
static constexpr int SH_HIGH_ORDER_COEFFS = 12;
static constexpr float SH_C0 = 0.28209479177387814f;

// Byte size of a fixed-size (non-list) PLY scalar property type, or 0 for an
// unknown/unsupported type. Shared by the vertex-property decoder and the
// preceding-element stride accounting added for issue #512 so both use one
// authoritative type->size table.
static int _ply_scalar_type_size(const String &p_type) {
    if (p_type == "float" || p_type == "float32") {
        return 4;
    } else if (p_type == "double" || p_type == "float64") {
        return 8;
    } else if (p_type == "uchar" || p_type == "uint8") {
        return 1;
    } else if (p_type == "ushort" || p_type == "uint16") {
        return 2;
    } else if (p_type == "int" || p_type == "int32") {
        return 4;
    } else if (p_type == "short" || p_type == "int16") {
        return 2;
    } else if (p_type == "uint" || p_type == "uint32") {
        return 4;
    } else if (p_type == "char" || p_type == "int8") {
        return 1;
    }
    return 0;
}

static bool _variant_to_uint64(const Variant &p_value, uint64_t &r_out) {
    switch (p_value.get_type()) {
        case Variant::INT: {
            int64_t v = p_value.operator int64_t();
            if (v < 0) {
                return false;
            }
            r_out = static_cast<uint64_t>(v);
            return true;
        }
        case Variant::FLOAT: {
            double v = p_value.operator double();
            if (v < 0.0) {
                return false;
            }
            r_out = static_cast<uint64_t>(Math::round(v));
            return true;
        }
        default:
            return false;
    }
}

static bool _is_ply_cache_enabled() {
    if (ProjectSettings *ps = ProjectSettings::get_singleton()) {
        return GaussianSplattingIO::get_bool_setting(ps, "rendering/gaussian_splatting/import/use_gsplatworld_cache", true);
    }
    return true;
}

static String _get_ply_cache_path(const String &p_source_path) {
    return p_source_path.get_basename() + ".gsplatcache";
}

static Ref<GaussianSplatWorld> _load_ply_cache_world(const String &p_cache_path) {
    ResourceFormatLoaderGaussianSplatWorld format_loader;
    Error load_err = OK;
    return format_loader.load_resident(p_cache_path, &load_err);
}

static bool _cache_source_path_matches(const Dictionary &p_metadata, const String &p_source_path) {
    const Variant source_path_var = p_metadata.get(StringName("cache_source_path"), Variant());
    return source_path_var.get_type() == Variant::STRING && String(source_path_var) == p_source_path;
}

static bool _cache_source_stamp_matches(const Dictionary &p_metadata, uint64_t p_source_size, uint64_t p_source_mtime) {
    uint64_t cached_size = 0;
    uint64_t cached_mtime = 0;
    return _variant_to_uint64(p_metadata.get(StringName("cache_source_size"), Variant()), cached_size) &&
            _variant_to_uint64(p_metadata.get(StringName("cache_source_mtime"), Variant()), cached_mtime) &&
            cached_size == p_source_size &&
            cached_mtime == p_source_mtime;
}

static bool _cache_version_matches(const Dictionary &p_metadata) {
    const Variant version_var = p_metadata.get(StringName("cache_version"), Variant());
    const Variant::Type version_type = version_var.get_type();
    return (version_type == Variant::INT || version_type == Variant::FLOAT) &&
            int(version_var) == PLY_CACHE_VERSION;
}

static bool _cache_metadata_matches(const Dictionary &p_metadata,
        const String &p_source_path,
        uint64_t p_source_size,
        uint64_t p_source_mtime) {
    return _cache_source_path_matches(p_metadata, p_source_path) &&
            _cache_source_stamp_matches(p_metadata, p_source_size, p_source_mtime) &&
            _cache_version_matches(p_metadata);
}

static bool _cached_gaussian_data_matches(const Ref<::GaussianData> &p_cached_data, int p_expected_vertex_count) {
    if (p_cached_data.is_null() || p_cached_data->get_count() <= 0) {
        return false;
    }
    return p_expected_vertex_count <= 0 || p_cached_data->get_count() == p_expected_vertex_count;
}

} // namespace

PLYLoader::PLYLoader() {
    gaussian_data.instantiate();
}

PLYLoader::~PLYLoader() {
}

void PLYLoader::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_file", "path"), &PLYLoader::load_file);
    ClassDB::bind_method(D_METHOD("get_gaussian_data"), &PLYLoader::get_gaussian_data);
    ClassDB::bind_method(D_METHOD("get_splat_count"), &PLYLoader::get_splat_count);
    ClassDB::bind_method(D_METHOD("get_load_statistics"), &PLYLoader::get_load_statistics);
    ClassDB::bind_method(D_METHOD("has_property", "name"), &PLYLoader::has_property);
    ClassDB::bind_method(D_METHOD("get_property_names"), &PLYLoader::get_property_names);
}

Error PLYLoader::load_file(const String &p_path) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null()) {
        GS_LOG_ERROR_DEFAULT("Failed to open PLY file: " + p_path);
        return ERR_FILE_NOT_FOUND;
    }

    // Higher-level entry points (GaussianSplatAsset::load_from_file,
    // load_gaussian_data_from_file) own the begin_asset_open() call; nesting
    // one here would double-arm the pending-flush counter and split a single
    // asset load across multiple [StartupTrace] lines.

    last_cache_hit = false;
    last_header_time_us = 0;
    last_parse_time_us = 0;
    last_cache_time_us = 0;

    uint64_t start_time = OS::get_singleton()->get_ticks_usec();
    const int64_t source_size_signed = FileAccess::get_size(p_path);
    const uint64_t source_size = source_size_signed > 0
            ? static_cast<uint64_t>(source_size_signed)
            : file->get_length();
    const uint64_t source_mtime = FileAccess::get_modified_time(p_path);

    // Parse header
    uint64_t header_start = OS::get_singleton()->get_ticks_usec();
    Error err = parse_header(file);
    last_header_time_us = OS::get_singleton()->get_ticks_usec() - header_start;
    if (err != OK) {
        GS_LOG_ERROR_DEFAULT("Failed to parse PLY header");
        return err;
    }

    // Try cache before parsing large payloads
    if (_is_ply_cache_enabled()) {
        uint64_t cache_start = OS::get_singleton()->get_ticks_usec();
        if (try_load_cache(p_path, source_size, source_mtime)) {
            last_cache_time_us = OS::get_singleton()->get_ticks_usec() - cache_start;
            last_cache_hit = true;
            last_load_time_us = OS::get_singleton()->get_ticks_usec() - start_time;
            if (GSStartupTrace *trace = GSStartupTrace::get_singleton(); trace && trace->is_enabled()) {
                trace->record_subphase("ply_header_parse", last_header_time_us);
                trace->record_subphase("ply_cache_lookup", last_cache_time_us);
            }
            GS_LOG_STREAMING_INFO(vformat("PLY cache hit: %d splats in %.2f ms",
                    header.vertex_count, last_load_time_us / 1000.0));
            return OK;
        }
        last_cache_time_us = OS::get_singleton()->get_ticks_usec() - cache_start;
    }

    // Parse data based on format
    uint64_t parse_start = OS::get_singleton()->get_ticks_usec();
    {
        GS_STARTUP_SCOPE("ply_payload_parse");
        if (header.is_binary) {
            err = parse_binary_data(file);
        } else {
            err = parse_ascii_data(file);
        }
    }
    last_parse_time_us = OS::get_singleton()->get_ticks_usec() - parse_start;
    if (GSStartupTrace *trace = GSStartupTrace::get_singleton(); trace && trace->is_enabled()) {
        trace->record_subphase("ply_header_parse", last_header_time_us);
        trace->record_subphase("ply_cache_lookup", last_cache_time_us);
    }

    if (err != OK) {
        GS_LOG_ERROR_DEFAULT("Failed to parse PLY data");
        return err;
    }

    if (_is_ply_cache_enabled()) {
        write_cache(p_path, source_size, source_mtime);
    }

    last_load_time_us = OS::get_singleton()->get_ticks_usec() - start_time;
    GS_LOG_STREAMING_INFO(vformat("PLY loaded: %d splats in %.2f ms", header.vertex_count, last_load_time_us / 1000.0));

    return OK;
}

Error PLYLoader::parse_header(Ref<FileAccess> file) {
    header = PLYHeader(); // Reset header

    String line = file->get_line();
    if (line != "ply") {
        return ERR_FILE_UNRECOGNIZED;
    }

    String current_element = ""; // Track current element type being parsed
    int vertex_property_offset = 0;
    bool found_end_header = false;

    // Practical upper bound for splat counts: 2^30 entries.  At ~248 bytes per
    // raw vertex this already exceeds 256 GiB on disk, so anything larger is a
    // header that overflows int sizing in resize() / chunked parsing below.
    static constexpr int64_t PLY_MAX_VERTEX_COUNT = 1ll << 30;

    // Issue #512: a legal PLY may declare other elements (face/camera/...)
    // BEFORE `vertex`. In a binary PLY every element's data is stored back to
    // back in declaration order, so a preceding element's rows occupy bytes
    // ahead of the vertex block; assuming vertex data starts at header_size
    // reads those bytes as vertex data (silent misparse). Track the fixed-size
    // footprint of every element declared before `vertex` (binary: total bytes,
    // ASCII: total rows) so the data parsers can skip past it. A preceding
    // element whose rows are variable-length (`property list`) has no fixed
    // size, so the offset is undecidable and we reject rather than misparse.
    bool seen_vertex = false;
    bool current_element_is_preceding = false; // non-vertex element before vertex
    int64_t current_element_count = 0;
    int64_t current_element_stride = 0; // fixed-size bytes per row of current preceding element
    int64_t pre_vertex_data_bytes = 0; // binary: bytes of all elements before vertex
    int64_t pre_vertex_row_count = 0; // ASCII: rows of all elements before vertex

    while (!file->eof_reached()) {
        line = file->get_line().strip_edges();

        if (line.begins_with("format")) {
            Vector<String> parts = line.split(" ");
            if (parts.size() >= 2) {
                if (parts[1] == "binary_little_endian") {
                    header.is_binary = true;
                    header.is_little_endian = true;
                } else if (parts[1] == "binary_big_endian") {
                    header.is_binary = true;
                    header.is_little_endian = false;
                } else if (parts[1] == "ascii") {
                    header.is_binary = false;
                }
            }
        } else if (line.begins_with("element")) {
            // A new element closes the previous one: if that previous element
            // was a fixed-size element declared before `vertex`, fold its data
            // footprint into the pre-vertex accumulators now.
            if (current_element_is_preceding) {
                pre_vertex_data_bytes += current_element_count * current_element_stride;
                pre_vertex_row_count += current_element_count;
            }
            current_element_is_preceding = false;
            current_element_count = 0;
            current_element_stride = 0;

            Vector<String> parts = line.split(" ");
            if (parts.size() >= 2) {
                current_element = parts[1]; // Could be "vertex", "face", etc.
                const int64_t parsed_count = (parts.size() >= 3) ? parts[2].to_int() : -1;
                if (current_element == "vertex") {
                    seen_vertex = true;
                    if (parts.size() >= 3) {
                        if (parsed_count < 1 || parsed_count > PLY_MAX_VERTEX_COUNT) {
                            GS_LOG_ERROR_DEFAULT(vformat(
                                    "[PLY] vertex count out of range: %d (must be 1..%d)",
                                    parsed_count, PLY_MAX_VERTEX_COUNT));
                            return ERR_FILE_CORRUPT;
                        }
                        header.vertex_count = static_cast<int>(parsed_count);
                        vertex_property_offset = 0; // Reset offset for vertex properties
                        header.properties.clear();
                    }
                } else if (!seen_vertex) {
                    // Non-vertex element declared before `vertex`: its data
                    // precedes the vertex block. It must carry a valid row count
                    // or we cannot size the skip, so fail closed.
                    if (parsed_count < 0 || parsed_count > PLY_MAX_VERTEX_COUNT) {
                        GS_LOG_ERROR_DEFAULT(vformat(
                                "[PLY] element '%s' declared before 'vertex' has an invalid row "
                                "count; refusing to guess the vertex data offset",
                                current_element));
                        return ERR_FILE_CORRUPT;
                    }
                    current_element_is_preceding = true;
                    current_element_count = parsed_count;
                }
                // A non-vertex element declared AFTER `vertex` is trailing data
                // we never read, so it needs no tracking.
            }
        } else if (line.begins_with("property")) {
            Vector<String> parts = line.split(" ");
            if (parts.size() >= 3) {
                const bool is_list = (parts[1] == "list");
                if (current_element == "vertex") {
                    if (is_list) {
                        // Issue #511: a per-vertex list is variable length, so
                        // the vertex stride is not fixed. Dropping it (the old
                        // `continue`) misaligned every vertex after the first and
                        // still "succeeded" with garbage. Reject instead.
                        GS_LOG_ERROR_DEFAULT(vformat(
                                "[PLY] vertex element declares an unsupported variable-length "
                                "'property list' ('%s'); refusing to load rather than misalign the "
                                "per-vertex stride",
                                parts[parts.size() - 1]));
                        return ERR_FILE_CORRUPT;
                    }

                    PLYProperty prop;
                    prop.type = parts[1];
                    prop.name = parts[2];

                    const int size = _ply_scalar_type_size(prop.type);
                    if (size <= 0) {
                        GS_LOG_ERROR_DEFAULT(vformat(
                                "[PLY] unknown property type '%s' for property '%s'",
                                prop.type, prop.name));
                        return ERR_FILE_CORRUPT;
                    }
                    prop.size = size;
                    prop.offset = vertex_property_offset;
                    vertex_property_offset += prop.size;
                    header.properties.push_back(prop);
                } else if (current_element_is_preceding) {
                    if (is_list) {
                        // Issue #512: a variable-length row in a preceding
                        // element makes the vertex data offset undecidable
                        // without parsing every row. Reject rather than misparse.
                        GS_LOG_ERROR_DEFAULT(vformat(
                                "[PLY] element '%s' declared before 'vertex' uses an unsupported "
                                "variable-length 'property list'; cannot compute the vertex data offset",
                                current_element));
                        return ERR_FILE_CORRUPT;
                    }
                    const int size = _ply_scalar_type_size(parts[1]);
                    if (size <= 0) {
                        GS_LOG_ERROR_DEFAULT(vformat(
                                "[PLY] element '%s' declared before 'vertex' has a property of "
                                "unknown type '%s'; cannot compute the vertex data offset",
                                current_element, parts[1]));
                        return ERR_FILE_CORRUPT;
                    }
                    current_element_stride += size;
                }
                // Properties of a trailing (post-vertex) element are ignored.
            }
        } else if (line == "end_header") {
            // Fold a preceding element that ended immediately before end_header
            // (defensive: `vertex` normally follows, but stay consistent).
            if (current_element_is_preceding) {
                pre_vertex_data_bytes += current_element_count * current_element_stride;
                pre_vertex_row_count += current_element_count;
            }
            header.header_size = file->get_position();
            found_end_header = true;
            break;
        }
    }

    if (!found_end_header) {
        GS_LOG_ERROR_DEFAULT("[PLY] header truncated: missing end_header sentinel");
        return ERR_FILE_CORRUPT;
    }

    if (header.vertex_count == 0) {
        return ERR_FILE_CORRUPT;
    }

    // Vertex data begins after the header text plus any fixed-size elements
    // declared before `vertex` (issue #512). For the common vertex-first
    // layout pre_vertex_data_bytes/pre_vertex_row_count are 0 and this equals
    // header_size / skips no rows.
    header.vertex_data_offset = static_cast<uint64_t>(header.header_size) +
            static_cast<uint64_t>(pre_vertex_data_bytes);
    header.pre_vertex_row_count = pre_vertex_row_count;

    return OK;
}

bool PLYLoader::try_load_cache(const String &p_source_path, uint64_t p_source_size, uint64_t p_source_mtime) {
    if (p_source_path.is_empty()) {
        return false;
    }

    // Use .gsplatcache (not .gsplatworld) so Godot's filesystem scanner does
    // not treat PLY caches as standalone importable resources.
    const String cache_path = _get_ply_cache_path(p_source_path);
    if (!FileAccess::exists(cache_path)) {
        return false;
    }

    // Call the format loader directly with CACHE_MODE_IGNORE so we always
    // read from disk.  Going through ResourceLoader::load() with the default
    // CACHE_MODE_REUSE would return stale in-memory data when the cache file
    // has been rewritten during the same editor session.
    Ref<GaussianSplatWorld> world = _load_ply_cache_world(cache_path);
    if (world.is_null()) {
        return false;
    }

    Dictionary cache_metadata = world->get_metadata();
    if (!_cache_metadata_matches(cache_metadata, p_source_path, p_source_size, p_source_mtime)) {
        return false;
    }

    Ref<::GaussianData> cached_data = world->get_gaussian_data();
    if (!_cached_gaussian_data_matches(cached_data, header.vertex_count)) {
        return false;
    }

    gaussian_data = cached_data;

    return true;
}

void PLYLoader::write_cache(const String &p_source_path, uint64_t p_source_size, uint64_t p_source_mtime) const {
    if (p_source_path.is_empty()) {
        return;
    }
    if (gaussian_data.is_null() || gaussian_data->get_count() <= 0) {
        return;
    }

    const String cache_path = p_source_path.get_basename() + ".gsplatcache";

    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(gaussian_data);
    world->set_bounds(gaussian_data->get_aabb());

    Dictionary cache_metadata;
    cache_metadata[StringName("cache_source_path")] = p_source_path;
    cache_metadata[StringName("cache_source_size")] = static_cast<int64_t>(p_source_size);
    cache_metadata[StringName("cache_source_mtime")] = static_cast<int64_t>(p_source_mtime);
    cache_metadata[StringName("cache_version")] = PLY_CACHE_VERSION;
    world->set_metadata(cache_metadata);

    // Write through the format saver directly so .gsplatcache does not need
    // to be a globally recognised ResourceSaver extension.
    ResourceFormatSaverGaussianSplatWorld format_saver;
    Error err = format_saver.save(world, cache_path);
    if (err != OK && GaussianSplattingIO::is_data_log_enabled()) {
        GS_LOG_STREAMING_DEBUG(vformat("Failed to write PLY cache: %s (error %d)", cache_path, err));
    }
}

Error PLYLoader::parse_binary_data(Ref<FileAccess> file) {
    if (header.properties.size() == 0) {
        return ERR_FILE_CORRUPT;
    }

    // Calculate total size per vertex FIRST — before any allocation. resize()
    // both allocates AND per-record initializes header.vertex_count records, so
    // it must not run until we have proven the file actually holds that many
    // vertices.
    int vertex_size = 0;
    for (const PLYProperty &prop : header.properties) {
        vertex_size = MAX(vertex_size, prop.offset + prop.size);
    }

    if (vertex_size <= 0) {
        return ERR_FILE_CORRUPT;
    }

    // Issue #583: prove the payload extent BEFORE the large allocation. A
    // hostile or truncated PLY can declare a legal-under-cap vertex_count (up to
    // the 2^30 header cap) whose data does not exist in the file. The vertex
    // block occupies exactly vertex_size * vertex_count bytes starting at
    // vertex_data_offset; if fewer bytes remain, the file is truncated/corrupt.
    // Rejecting here turns what was a multi-GB resize() over-allocation plus a
    // ~100M-iteration init loop (reached before the per-chunk short read is ever
    // noticed) into a cheap, clean ERR_FILE_CORRUPT.
    const uint64_t required_payload_bytes = uint64_t(vertex_size) * uint64_t(header.vertex_count);
    const uint64_t file_length = file->get_length();
    const uint64_t available_payload_bytes = (file_length > header.vertex_data_offset)
            ? (file_length - header.vertex_data_offset)
            : 0;
    if (available_payload_bytes < required_payload_bytes) {
        GS_LOG_ERROR_DEFAULT(vformat(
                "[PLY] declared vertex_count %d needs %d payload bytes but only %d remain after the "
                "header (vertex data offset %d, file length %d); refusing to allocate for a "
                "truncated/hostile file",
                header.vertex_count,
                static_cast<int64_t>(required_payload_bytes),
                static_cast<int64_t>(available_payload_bytes),
                static_cast<int64_t>(header.vertex_data_offset),
                static_cast<int64_t>(file_length)));
        return ERR_FILE_CORRUPT;
    }

    gaussian_data->resize(header.vertex_count);

    // Precompute frequently used property indices (avoid per-vertex scans)
    const int x_idx = find_property_index("x");
    const int y_idx = find_property_index("y");
    const int z_idx = find_property_index("z");

    const int sx_idx = find_property_index("scale_0");
    const int sy_idx = find_property_index("scale_1");
    const int sz_idx = find_property_index("scale_2");

    const int rw_idx = find_property_index("rot_0");
    const int rx_idx = find_property_index("rot_1");
    const int ry_idx = find_property_index("rot_2");
    const int rz_idx = find_property_index("rot_3");

    const int opacity_idx = find_property_index("opacity");

    const int nx_idx = find_property_index("nx");
    const int ny_idx = find_property_index("ny");
    const int nz_idx = find_property_index("nz");

    // Check if normal properties exist to determine 2D mode
    const bool has_normals = (nx_idx >= 0 && ny_idx >= 0 && nz_idx >= 0);

    if (has_normals) {
        gaussian_data->set_2d_mode(true);
        GS_LOG_STREAMING_INFO("PLY contains normal vectors - enabling 2D mode");
    }

    int palette_idx = find_property_index("palette_id");
    int brush_override_idx = find_property_index("brush_override_id");
    int brush_u_idx = find_property_index("brush_axis_u");
    int brush_v_idx = find_property_index("brush_axis_v");
    int stroke_age_idx = find_property_index("stroke_age");

    int sh_dc_indices[SH_DC_COMPONENTS];
    bool dc_property_exists[SH_DC_COMPONENTS];
    for (int c = 0; c < SH_DC_COMPONENTS; c++) {
        String name = vformat("f_dc_%d", c);
        sh_dc_indices[c] = find_property_index(name);
        dc_property_exists[c] = sh_dc_indices[c] >= 0;
    }

    int sh_rest_indices[SH_REST_COMPONENTS];
    bool rest_property_exists[SH_REST_COMPONENTS];
    bool has_rest_properties = false;
    for (int idx = 0; idx < SH_REST_COMPONENTS; idx++) {
        String name = vformat("f_rest_%d", idx);
        sh_rest_indices[idx] = find_property_index(name);
        rest_property_exists[idx] = sh_rest_indices[idx] >= 0;
        if (rest_property_exists[idx]) {
            has_rest_properties = true;
        }
    }

    Gaussian default_gaussian{};
    default_gaussian.position = Vector3();
    default_gaussian.scale = Vector3(1, 1, 1);
    default_gaussian.rotation = Quaternion();
    default_gaussian.sh_dc = Color(1, 1, 1, 1);
    default_gaussian.opacity = 1.0f;
    default_gaussian.normal = Vector3(0, 0, 1);
    default_gaussian.area = 1.0f;
    default_gaussian.brush_axes = Vector2(1.0f, 1.0f);
    default_gaussian.stroke_age = 0.0f;
    default_gaussian.painterly_meta = gaussian_pack_painterly_meta(0);
    for (int j = 0; j < 3; j++) {
        default_gaussian.sh_1[j] = Vector3();
    }

    auto parse_vertex = [&](int p_index, const uint8_t *p_vertex) {
        Gaussian g = default_gaussian;

        if (x_idx >= 0 && y_idx >= 0 && z_idx >= 0) {
            g.position.x = read_float_property(p_vertex, header.properties[x_idx]);
            g.position.y = read_float_property(p_vertex, header.properties[y_idx]);
            g.position.z = read_float_property(p_vertex, header.properties[z_idx]);
        }

        float sh_dc_values[SH_DC_COMPONENTS] = {};
        bool sh_dc_present[SH_DC_COMPONENTS];
        for (int c = 0; c < SH_DC_COMPONENTS; c++) {
            sh_dc_present[c] = dc_property_exists[c];
            if (sh_dc_present[c]) {
                sh_dc_values[c] = read_float_property(p_vertex, header.properties[sh_dc_indices[c]]);
            }
        }

        float sh_rest_values[SH_REST_COMPONENTS] = {};
        bool sh_rest_present[SH_REST_COMPONENTS];
        if (has_rest_properties) {
            for (int idx = 0; idx < SH_REST_COMPONENTS; idx++) {
                sh_rest_present[idx] = rest_property_exists[idx];
                if (sh_rest_present[idx]) {
                    sh_rest_values[idx] = read_float_property(p_vertex, header.properties[sh_rest_indices[idx]]);
                }
            }
        } else {
            for (int idx = 0; idx < SH_REST_COMPONENTS; idx++) {
                sh_rest_present[idx] = false;
                sh_rest_values[idx] = 0.0f;
            }
        }

        float sh_coeffs[SH_DC_COMPONENTS + SH_REST_COMPONENTS];
        int sh_float_count = assemble_sh_coefficients(g,
                sh_dc_values,
                sh_dc_present,
                sh_rest_values,
                sh_rest_present,
                sh_coeffs);

        if (sx_idx >= 0 && sy_idx >= 0 && sz_idx >= 0) {
            g.scale.x = exp(read_float_property(p_vertex, header.properties[sx_idx]));
            g.scale.y = exp(read_float_property(p_vertex, header.properties[sy_idx]));
            g.scale.z = exp(read_float_property(p_vertex, header.properties[sz_idx]));
        }

        if (rw_idx >= 0 && rx_idx >= 0 && ry_idx >= 0 && rz_idx >= 0) {
            g.rotation.w = read_float_property(p_vertex, header.properties[rw_idx]);
            g.rotation.x = read_float_property(p_vertex, header.properties[rx_idx]);
            g.rotation.y = read_float_property(p_vertex, header.properties[ry_idx]);
            g.rotation.z = read_float_property(p_vertex, header.properties[rz_idx]);
            g.rotation.normalize();
        }

        if (opacity_idx >= 0) {
            float opacity = read_float_property(p_vertex, header.properties[opacity_idx]);
            g.opacity = 1.0f / (1.0f + exp(-opacity));
        }

        if (has_normals) {
            g.normal.x = read_float_property(p_vertex, header.properties[nx_idx]);
            g.normal.y = read_float_property(p_vertex, header.properties[ny_idx]);
            g.normal.z = read_float_property(p_vertex, header.properties[nz_idx]);
            g.normal.normalize();
        }

        if (palette_idx >= 0) {
            uint32_t palette_value = read_uint_property(p_vertex, header.properties[palette_idx]);
            uint16_t palette = (uint16_t)CLAMP(palette_value, 0u, 65535u);
            g.painterly_meta = gaussian_set_palette_id(g.painterly_meta, palette);
        }

        if (brush_override_idx >= 0) {
            uint32_t brush_override_value = read_uint_property(p_vertex, header.properties[brush_override_idx]);
            uint16_t brush_override_id = (uint16_t)CLAMP(brush_override_value, 0u, 65535u);
            g.painterly_meta = gaussian_set_brush_override_id(g.painterly_meta, brush_override_id);
        }

        if (brush_u_idx >= 0 && brush_v_idx >= 0) {
            g.brush_axes.x = read_float_property(p_vertex, header.properties[brush_u_idx]);
            g.brush_axes.y = read_float_property(p_vertex, header.properties[brush_v_idx]);
        }

        if (stroke_age_idx >= 0) {
            g.stroke_age = read_float_property(p_vertex, header.properties[stroke_age_idx]);
        }

        gaussian_data->set_gaussian(p_index, g);
        gaussian_data->set_spherical_harmonics(p_index, sh_coeffs, sh_float_count);

        if (GaussianSplattingIO::is_data_log_enabled() && p_index == 0) {
            GS_LOG_STREAMING_DEBUG(vformat("[PLY] Loaded %d SH floats for gaussian 0", sh_float_count));
            if (sh_float_count >= 6) {
                GS_LOG_STREAMING_DEBUG(vformat("[PLY] First 3 SH coeffs (after DC): [%f, %f, %f]",
                    sh_coeffs[3], sh_coeffs[4], sh_coeffs[5]));
            }
        }
    };

    // Seek to where the vertex element's data actually begins. This is
    // header_size for the common vertex-first layout, and skips past any
    // fixed-size elements declared before `vertex` (issue #512). parse_header
    // rejects layouts whose preceding elements are variable-length, so this
    // offset is always well defined here.
    file->seek(header.vertex_data_offset);

    const uint64_t total_bytes = uint64_t(vertex_size) * uint64_t(header.vertex_count);
    const uint64_t max_bulk_bytes = static_cast<uint64_t>(INT_MAX);
    const uint64_t target_chunk_bytes = 16ull * 1024ull * 1024ull;
    uint64_t max_vertices_per_chunk = target_chunk_bytes / uint64_t(vertex_size);
    if (max_vertices_per_chunk == 0) {
        max_vertices_per_chunk = 1;
    }

    if (total_bytes <= max_bulk_bytes) {
        Vector<uint8_t> bulk_buffer;
        bulk_buffer.resize(static_cast<int>(total_bytes));
        const uint64_t read = file->get_buffer(bulk_buffer.ptrw(), total_bytes);
        if (read != total_bytes) {
            return ERR_FILE_CORRUPT;
        }

        const uint8_t *data = bulk_buffer.ptr();
        for (int i = 0; i < header.vertex_count; i++) {
            const uint8_t *vertex = data + uint64_t(i) * uint64_t(vertex_size);
            parse_vertex(i, vertex);
        }
    } else {
        Vector<uint8_t> chunk_buffer;
        int remaining = header.vertex_count;
        int base_index = 0;
        while (remaining > 0) {
            const uint64_t batch_vertices_u = (max_vertices_per_chunk < static_cast<uint64_t>(remaining))
                    ? max_vertices_per_chunk
                    : static_cast<uint64_t>(remaining);
            const int batch_vertices = static_cast<int>(batch_vertices_u);
            const uint64_t batch_bytes = uint64_t(batch_vertices) * uint64_t(vertex_size);
            chunk_buffer.resize(static_cast<int>(batch_bytes));
            const uint64_t read = file->get_buffer(chunk_buffer.ptrw(), batch_bytes);
            if (read != batch_bytes) {
                return ERR_FILE_CORRUPT;
            }

            const uint8_t *data = chunk_buffer.ptr();
            for (int local = 0; local < batch_vertices; local++) {
                const uint8_t *vertex = data + uint64_t(local) * uint64_t(vertex_size);
                parse_vertex(base_index + local, vertex);
            }

            base_index += batch_vertices;
            remaining -= batch_vertices;
        }
    }

    return OK;
}

Error PLYLoader::parse_ascii_data(Ref<FileAccess> file) {
    // Skip the data rows of any elements declared before `vertex` (issue #512)
    // FIRST, so the payload-extent check below (issue #583) measures only the
    // bytes actually available to the vertex block — not the preceding element's
    // rows. parse_header rejects variable-length preceding elements, so each such
    // element contributes exactly its declared row count of lines here. If the
    // file runs out before those rows are consumed it is truncated; reject before
    // allocating anything.
    for (int64_t skip = 0; skip < header.pre_vertex_row_count; skip++) {
        if (file->eof_reached()) {
            return ERR_FILE_CORRUPT;
        }
        file->get_line();
    }

    // Issue #583: prove a plausible payload extent BEFORE resize() allocates and
    // per-record initializes header.vertex_count records. ASCII rows are
    // variable length, but every vertex row must carry at least one character
    // per property (a single digit is the smallest possible token), so
    // vertex_count rows need at least properties.size() * vertex_count bytes of
    // payload from the start of the vertex block — a strict lower bound that
    // never over-rejects a real file (a genuine row also needs separators and a
    // newline). Measure from the CURRENT position, i.e. after the preceding
    // elements' rows have been consumed above, so a hostile file cannot satisfy
    // the count by padding a large fixed-size element declared before `vertex`
    // (issue #583 follow-up): those bytes are no longer available to the vertex
    // block. A file that declares a legal-under-cap count with a truncated vertex
    // body has far fewer bytes remaining; reject before the large allocation
    // instead of resizing to a count the file cannot possibly contain.
    const int64_t min_bytes_per_vertex = MAX(header.properties.size(), 1);
    const uint64_t required_min_bytes = uint64_t(min_bytes_per_vertex) * uint64_t(header.vertex_count);
    const uint64_t file_length = file->get_length();
    const uint64_t vertex_block_start = file->get_position();
    const uint64_t available_bytes = (file_length > vertex_block_start) ? (file_length - vertex_block_start) : 0;
    if (available_bytes < required_min_bytes) {
        GS_LOG_ERROR_DEFAULT(vformat(
                "[PLY ASCII] declared vertex_count %d needs at least %d vertex-body bytes but only %d remain "
                "after the header and any preceding elements; refusing to allocate for a truncated/hostile file",
                header.vertex_count,
                static_cast<int64_t>(required_min_bytes),
                static_cast<int64_t>(available_bytes)));
        return ERR_FILE_CORRUPT;
    }

    // Issue #767 (hardened): the byte lower bound above proves enough BYTES
    // remain, and an earlier revision of this guard proved enough NEWLINES remain
    // -- but bytes are not rows and newlines are not FIELDS. A hostile file can
    // pack every required byte onto row 0 and then append `vertex_count - 1` BLANK
    // lines: each blank line contributes one '\n' byte, so the payload satisfies
    // the byte guard (blank lines are themselves the counted bytes) AND a bare
    // newline count, yet the row parser rejects the first blank row -- AFTER
    // resize(header.vertex_count) has already allocated and per-record initialized
    // the full declared count. Counting line terminators therefore preserves the
    // bounded over-allocation #767 set out to close (#583/#755 closed the
    // pre-vertex-element accounting hole, not this one).
    //
    // Prove the vertex ROWS actually BEAR FIELDS before the large allocation. The
    // row parser accepts a row iff it splits (split_spaces) into at least
    // properties.size() whitespace-separated tokens (see the values.size() check
    // in the parse loop below), so require that many field-bearing rows. Count
    // tokens per row in a cheap raw byte scan (no per-line String allocation),
    // bounded to the vertex block, then rewind so the parse loop reads from
    // vertex_block_start unchanged. This is still a strict lower bound that never
    // over-rejects a real file -- a genuine N-row body has N field-bearing rows
    // even when each row is as small as "0\n" -- but a blank/token-deficient row
    // no longer counts toward the gate. The fast binary path (row count == byte
    // count / stride, already exact) is untouched.
    {
        const int fields_per_row = MAX(header.properties.size(), 1);
        const uint64_t required_rows = uint64_t(header.vertex_count); // >= 1 (validated in parse_header)
        uint64_t field_bearing_rows = 0;
        uint64_t remaining_to_scan = available_bytes;
        uint8_t scan_buffer[4096];
        int row_tokens = 0;       // whitespace-separated tokens seen in the current row
        bool in_token = false;    // currently inside a run of non-whitespace
        bool row_counted = false; // current row already counted as field-bearing
        while (field_bearing_rows < required_rows && remaining_to_scan > 0) {
            const uint64_t want = MIN<uint64_t>(sizeof(scan_buffer), remaining_to_scan);
            const uint64_t read = file->get_buffer(scan_buffer, want);
            if (read == 0) {
                break; // short read: fewer bytes than the extent check promised
            }
            for (uint64_t b = 0; b < read; b++) {
                const uint8_t c = scan_buffer[b];
                if (c == '\n') {
                    // Row boundary: reset per-row token accounting.
                    row_tokens = 0;
                    in_token = false;
                    row_counted = false;
                } else if (c == ' ' || c == '\t' || c == '\r') {
                    in_token = false;
                } else if (!in_token) {
                    // Start of a new whitespace-delimited token (mirrors split_spaces).
                    in_token = true;
                    if (++row_tokens >= fields_per_row && !row_counted) {
                        field_bearing_rows++;
                        row_counted = true;
                    }
                }
            }
            remaining_to_scan -= read;
        }
        // Rewind to the vertex block start for the row parser below. seek() also
        // clears any EOF indicator set by scanning to the end of the payload.
        file->seek(vertex_block_start);
        if (field_bearing_rows < required_rows) {
            GS_LOG_ERROR_DEFAULT(vformat(
                    "[PLY ASCII] declared vertex_count %d needs %d field-bearing rows (>= %d tokens each) but "
                    "the vertex block proves only %d; refusing to allocate for a blank-padded/row-deficient "
                    "file (issue #767)",
                    header.vertex_count,
                    header.vertex_count,
                    fields_per_row,
                    static_cast<int64_t>(field_bearing_rows)));
            return ERR_FILE_CORRUPT;
        }
    }

    gaussian_data->resize(header.vertex_count);

    // Check if normal properties exist to determine 2D mode
    bool has_normals = (find_property_index("nx") >= 0 &&
                        find_property_index("ny") >= 0 &&
                        find_property_index("nz") >= 0);

    if (has_normals) {
        gaussian_data->set_2d_mode(true);
        GS_LOG_STREAMING_INFO("PLY contains normal vectors - enabling 2D mode");
    }

    int sh_dc_indices[SH_DC_COMPONENTS];
    bool dc_property_exists[SH_DC_COMPONENTS];
    for (int c = 0; c < SH_DC_COMPONENTS; c++) {
        String name = vformat("f_dc_%d", c);
        sh_dc_indices[c] = find_property_index(name);
        dc_property_exists[c] = sh_dc_indices[c] >= 0;
    }

    int sh_rest_indices[SH_REST_COMPONENTS];
    bool rest_property_exists[SH_REST_COMPONENTS];
    bool has_rest_properties = false;
    for (int idx = 0; idx < SH_REST_COMPONENTS; idx++) {
        String name = vformat("f_rest_%d", idx);
        sh_rest_indices[idx] = find_property_index(name);
        rest_property_exists[idx] = sh_rest_indices[idx] >= 0;
        if (rest_property_exists[idx]) {
            has_rest_properties = true;
        }
    }

    // ASCII parsing - simpler but slower
    for (int i = 0; i < header.vertex_count; i++) {
        if (file->eof_reached()) {
            return ERR_FILE_CORRUPT;
        }

        String line = file->get_line().strip_edges();
        // Split by any whitespace (spaces, tabs) for robust parsing
        Vector<String> values = line.split_spaces();

        if (values.size() < header.properties.size()) {
            GS_LOG_ERROR_DEFAULT(vformat(
                    "[PLY ASCII] Malformed row at vertex index %d: expected at least %d fields, got %d. Treating file as corrupt.",
                    i, header.properties.size(), values.size()));
            return ERR_FILE_CORRUPT;
        }

        Gaussian g;
        // Set defaults
        g.position = Vector3();
        g.scale = Vector3(1, 1, 1);
        g.rotation = Quaternion();
        g.sh_dc = Color(1, 1, 1, 1);
        g.opacity = 1.0f;
        g.normal = Vector3(0, 0, 1);
        g.area = 1.0f;
        g.brush_axes = Vector2(1.0f, 1.0f);
        g.stroke_age = 0.0f;
        g.painterly_meta = gaussian_pack_painterly_meta(0);
        for (int k = 0; k < 3; k++) {
            g.sh_1[k] = Vector3();
        }

        // Parse based on property order
        for (int j = 0; j < header.properties.size(); j++) {
            const PLYProperty &prop = header.properties[j];
            const String &token = values[j];
            if (!token.is_valid_float()) {
                GS_LOG_ERROR_DEFAULT(vformat("[PLY ASCII] Invalid numeric token at vertex index %d property '%s' (column %d): '%s'.",
                        i, prop.name, j, token));
                return ERR_FILE_CORRUPT;
            }
            float value = token.to_float();

            if (prop.name == "x") {
                g.position.x = value;
            } else if (prop.name == "y") {
                g.position.y = value;
            } else if (prop.name == "z") {
                g.position.z = value;
            } else if (prop.name == "scale_0") {
                g.scale.x = exp(value);
            } else if (prop.name == "scale_1") {
                g.scale.y = exp(value);
            } else if (prop.name == "scale_2") {
                g.scale.z = exp(value);
            } else if (prop.name == "rot_0") {
                g.rotation.w = value;
            } else if (prop.name == "rot_1") {
                g.rotation.x = value;
            } else if (prop.name == "rot_2") {
                g.rotation.y = value;
            } else if (prop.name == "rot_3") {
                g.rotation.z = value;
            } else if (prop.name == "opacity") {
                g.opacity = 1.0f / (1.0f + exp(-value));
            } else if (prop.name == "palette_id") {
                g.painterly_meta = gaussian_set_palette_id(g.painterly_meta, (uint16_t)CLAMP((int)value, 0, 65535));
            } else if (prop.name == "brush_override_id") {
                g.painterly_meta = gaussian_set_brush_override_id(g.painterly_meta, (uint16_t)CLAMP((int)value, 0, 65535));
            } else if (prop.name == "brush_axis_u") {
                g.brush_axes.x = value;
            } else if (prop.name == "brush_axis_v") {
                g.brush_axes.y = value;
            } else if (prop.name == "stroke_age") {
                g.stroke_age = value;
            } else if (prop.name == "nx") {
                g.normal.x = value;
            } else if (prop.name == "ny") {
                g.normal.y = value;
            } else if (prop.name == "nz") {
                g.normal.z = value;
            }
        }

        float sh_dc_values[SH_DC_COMPONENTS] = {};
        bool sh_dc_present[SH_DC_COMPONENTS];
        for (int c = 0; c < SH_DC_COMPONENTS; c++) {
            sh_dc_present[c] = dc_property_exists[c];
            if (sh_dc_present[c] && sh_dc_indices[c] < values.size()) {
                sh_dc_values[c] = values[sh_dc_indices[c]].to_float();
            }
        }

        float sh_rest_values[SH_REST_COMPONENTS] = {};
        bool sh_rest_present[SH_REST_COMPONENTS];
        if (has_rest_properties) {
            for (int idx = 0; idx < SH_REST_COMPONENTS; idx++) {
                sh_rest_present[idx] = rest_property_exists[idx];
                if (sh_rest_present[idx] && sh_rest_indices[idx] < values.size()) {
                    sh_rest_values[idx] = values[sh_rest_indices[idx]].to_float();
                }
            }
        } else {
            for (int idx = 0; idx < SH_REST_COMPONENTS; idx++) {
                sh_rest_present[idx] = false;
                sh_rest_values[idx] = 0.0f;
            }
        }

        float sh_coeffs[SH_DC_COMPONENTS + SH_REST_COMPONENTS];
        int sh_float_count = assemble_sh_coefficients(g,
                sh_dc_values,
                sh_dc_present,
                sh_rest_values,
                sh_rest_present,
                sh_coeffs);

        g.rotation.normalize();

        // Normalize the normal vector if it was loaded
        if (has_normals) {
            g.normal.normalize();
        }

        gaussian_data->set_gaussian(i, g);
        gaussian_data->set_spherical_harmonics(i, sh_coeffs, sh_float_count);

        // DEBUG: Log first gaussian SH data
        if (GaussianSplattingIO::is_data_log_enabled() && i == 0) {
            GS_LOG_STREAMING_DEBUG(vformat("[PLY ASCII] Loaded %d SH floats for gaussian 0", sh_float_count));
            if (sh_float_count >= 6) {
                GS_LOG_STREAMING_DEBUG(vformat("[PLY ASCII] First 3 SH coeffs (after DC): [%f, %f, %f]",
                    sh_coeffs[3], sh_coeffs[4], sh_coeffs[5]));
            }
        }
    }

    return OK;
}

int PLYLoader::assemble_sh_coefficients(Gaussian &r_gaussian,
        const float *p_dc_values,
        const bool *p_dc_present,
        const float *p_rest_values,
        const bool *p_rest_present,
        float *r_output) const {
    // -----------------------------------------------------------------
    // SH sign convention documentation (ISSUE-038)
    // -----------------------------------------------------------------
    // PLY files from 3DGS (Kerbl et al. 2023) store SH coefficients that
    // were trained with the Condon-Shortley (CS) phase baked into the
    // coefficient values, using real spherical harmonics.  Specifically:
    //
    //   Band 0 (l=0):  Y_0^0 = SH_C0                   (no sign issue)
    //   Band 1 (l=1):  Y_1^{-1}=-C1*y, Y_1^0=C1*z, Y_1^1=-C1*x
    //   Band 2 (l=2):  standard real SH with CS phase
    //   Band 3 (l=3):  standard real SH with CS phase
    //
    // The loader does NOT negate or transform any coefficients; it passes
    // them through as stored in the PLY (channel-major -> coeff-major
    // repack only).  The GPU shader (gaussian_splat_common_inc.glsl,
    // tile_binning.glsl) evaluates the SH basis with the same CS-phase
    // sign convention, so the two are consistent.
    //
    // If importing from a source that omits the CS phase, a sign flip on
    // odd-m coefficients would be needed here.
    // -----------------------------------------------------------------

    // DC term corresponds to spherical harmonics band l=0.
    // PLY stores DC as SH coefficients (scaled by SH_C0), centered around 0.
    // We add 0.5 here to convert to 0-1 color space, matching SPZ format.
    // This allows shaders to use DC directly without format-specific offsets.
    Color sh_dc = r_gaussian.sh_dc;
    if (p_dc_present[0]) {
        sh_dc.r = SH_C0 * p_dc_values[0];
    }
    if (p_dc_present[1]) {
        sh_dc.g = SH_C0 * p_dc_values[1];
    }
    if (p_dc_present[2]) {
        sh_dc.b = SH_C0 * p_dc_values[2];
    }
    sh_dc.a = 1.0f;
    r_gaussian.sh_dc = sh_dc;

    // PLY stores f_rest_0-44 in channel-major order (all R coeffs, then G, then B).
    // Repack into coefficient-major RGB triplets for the renderer.
    const int sh_coeffs_per_channel = SH_REST_COMPONENTS / 3;
    Vector3 sh_band1[SH_FIRST_ORDER_COEFFS];
    for (int i = 0; i < SH_FIRST_ORDER_COEFFS; i++) {
        sh_band1[i] = Vector3();
    }
    uint32_t first_order_count = 0;
    for (int coeff = 0; coeff < SH_FIRST_ORDER_COEFFS; coeff++) {
        Vector3 coeff_value;
        bool has_value = false;
        int base_r = coeff;
        int base_g = coeff + sh_coeffs_per_channel;
        int base_b = coeff + 2 * sh_coeffs_per_channel;
        if (base_r < SH_REST_COMPONENTS && p_rest_present[base_r]) {
            coeff_value.x = p_rest_values[base_r];
            has_value = true;
        }
        if (base_g < SH_REST_COMPONENTS && p_rest_present[base_g]) {
            coeff_value.y = p_rest_values[base_g];
            has_value = true;
        }
        if (base_b < SH_REST_COMPONENTS && p_rest_present[base_b]) {
            coeff_value.z = p_rest_values[base_b];
            has_value = true;
        }
        if (has_value) {
            first_order_count = coeff + 1;
        }
        sh_band1[coeff] = coeff_value;
    }

    // Higher order bands (l=2..3) occupy the remaining coefficients.
    Vector3 sh_high[SH_HIGH_ORDER_COEFFS];
    for (int i = 0; i < SH_HIGH_ORDER_COEFFS; i++) {
        sh_high[i] = Vector3();
    }
    uint32_t high_order_count = 0;
    for (int coeff = 0; coeff < SH_HIGH_ORDER_COEFFS; coeff++) {
        Vector3 coeff_value;
        bool has_value = false;
        int coeff_index = coeff + SH_FIRST_ORDER_COEFFS;
        int base_r = coeff_index;
        int base_g = coeff_index + sh_coeffs_per_channel;
        int base_b = coeff_index + 2 * sh_coeffs_per_channel;
        if (base_r < SH_REST_COMPONENTS && p_rest_present[base_r]) {
            coeff_value.x = p_rest_values[base_r];
            has_value = true;
        }
        if (base_g < SH_REST_COMPONENTS && p_rest_present[base_g]) {
            coeff_value.y = p_rest_values[base_g];
            has_value = true;
        }
        if (base_b < SH_REST_COMPONENTS && p_rest_present[base_b]) {
            coeff_value.z = p_rest_values[base_b];
            has_value = true;
        }
        if (has_value) {
            high_order_count = coeff + 1;
        }
        sh_high[coeff] = coeff_value;
    }

    for (int i = 0; i < SH_FIRST_ORDER_COEFFS; i++) {
        if ((uint32_t)i < first_order_count) {
            r_gaussian.sh_1[i] = sh_band1[i];
        } else {
            r_gaussian.sh_1[i] = Vector3();
        }
    }

    int out_index = 0;
    r_output[out_index++] = sh_dc.r;
    r_output[out_index++] = sh_dc.g;
    r_output[out_index++] = sh_dc.b;

    for (uint32_t i = 0; i < first_order_count; i++) {
        r_output[out_index++] = sh_band1[i].x;
        r_output[out_index++] = sh_band1[i].y;
        r_output[out_index++] = sh_band1[i].z;
    }

    for (uint32_t i = 0; i < high_order_count; i++) {
        r_output[out_index++] = sh_high[i].x;
        r_output[out_index++] = sh_high[i].y;
        r_output[out_index++] = sh_high[i].z;
    }

    return out_index;
}

int PLYLoader::find_property_index(const String &name) const {
    for (int i = 0; i < header.properties.size(); i++) {
        if (header.properties[i].name == name) {
            return i;
        }
    }
    return -1;
}

float PLYLoader::read_float_property(const uint8_t *data, const PLYProperty &prop) const {
    if (prop.type == "float" || prop.type == "float32") {
        uint32_t bits;
        memcpy(&bits, data + prop.offset, sizeof(uint32_t));
        if (!header.is_little_endian) {
            bits = BSWAP32(bits);
        }
        float value;
        memcpy(&value, &bits, sizeof(float));
        return value;
    } else if (prop.type == "double" || prop.type == "float64") {
        uint64_t bits;
        memcpy(&bits, data + prop.offset, sizeof(uint64_t));
        if (!header.is_little_endian) {
            bits = BSWAP64(bits);
        }
        double value;
        memcpy(&value, &bits, sizeof(double));
        return (float)value;
    } else if (prop.type == "uchar" || prop.type == "uint8") {
        // Single byte: endianness-independent.
        return (float)data[prop.offset];
    } else if (prop.type == "char" || prop.type == "int8") {
        // Single byte, signed: preserve negatives (unlike read_uint_property's >=0 clamp).
        int8_t value;
        memcpy(&value, data + prop.offset, sizeof(int8_t));
        return (float)value;
    } else if (prop.type == "ushort" || prop.type == "uint16") {
        uint16_t value;
        memcpy(&value, data + prop.offset, sizeof(uint16_t));
        if (!header.is_little_endian) {
            value = BSWAP16(value);
        }
        return (float)value;
    } else if (prop.type == "short" || prop.type == "int16") {
        uint16_t bits;
        memcpy(&bits, data + prop.offset, sizeof(uint16_t));
        if (!header.is_little_endian) {
            bits = BSWAP16(bits);
        }
        int16_t value;
        memcpy(&value, &bits, sizeof(int16_t));
        return (float)value;
    } else if (prop.type == "uint" || prop.type == "uint32") {
        uint32_t value;
        memcpy(&value, data + prop.offset, sizeof(uint32_t));
        if (!header.is_little_endian) {
            value = BSWAP32(value);
        }
        return (float)value;
    } else if (prop.type == "int" || prop.type == "int32") {
        uint32_t bits;
        memcpy(&bits, data + prop.offset, sizeof(uint32_t));
        if (!header.is_little_endian) {
            bits = BSWAP32(bits);
        }
        int32_t value;
        memcpy(&value, &bits, sizeof(int32_t));
        return (float)value;
    }
    // Genuinely unknown/unsupported type. parse_header() already rejects such
    // types up front, so this is a defensive fallback: warn once and read 0.0
    // instead of silently corrupting data or crashing.
    WARN_PRINT_ONCE(vformat("[PLY] unsupported property type '%s' for property '%s'; reading as 0.0",
            prop.type, prop.name));
    return 0.0f;
}

uint32_t PLYLoader::read_uint_property(const uint8_t *data, const PLYProperty &prop) const {
    if (prop.type == "uchar" || prop.type == "uint8") {
        return data[prop.offset];
    } else if (prop.type == "ushort" || prop.type == "uint16") {
        uint16_t value;
        memcpy(&value, data + prop.offset, sizeof(uint16_t));
        if (!header.is_little_endian) {
            value = (uint16_t)(((value & 0x00FF) << 8) | ((value & 0xFF00) >> 8));
        }
        return value;
    } else if (prop.type == "uint" || prop.type == "uint32") {
        uint32_t value;
        memcpy(&value, data + prop.offset, sizeof(uint32_t));
        if (!header.is_little_endian) {
            value = ((value & 0xFF000000) >> 24) |
                    ((value & 0x00FF0000) >> 8) |
                    ((value & 0x0000FF00) << 8) |
                    ((value & 0x000000FF) << 24);
        }
        return value;
    } else if (prop.type == "char" || prop.type == "int8") {
        int8_t value;
        memcpy(&value, data + prop.offset, sizeof(int8_t));
        return (uint32_t)MAX(value, (int8_t)0);
    } else if (prop.type == "short" || prop.type == "int16") {
        int16_t value;
        memcpy(&value, data + prop.offset, sizeof(int16_t));
        if (!header.is_little_endian) {
            value = (int16_t)(((value & 0x00FF) << 8) | ((value & 0xFF00) >> 8));
        }
        return (uint32_t)MAX(value, (int16_t)0);
    } else if (prop.type == "int" || prop.type == "int32") {
        uint32_t bits;
        memcpy(&bits, data + prop.offset, sizeof(uint32_t));
        if (!header.is_little_endian) {
            bits = BSWAP32(bits);
        }
        int32_t value;
        memcpy(&value, &bits, sizeof(int32_t));
        return (uint32_t)MAX(value, 0);
    }
    return 0;
}

int PLYLoader::get_splat_count() const {
    return gaussian_data.is_valid() ? gaussian_data->get_count() : 0;
}

Dictionary PLYLoader::get_load_statistics() const {
    Dictionary stats;
    stats["splat_count"] = get_splat_count();
    stats["format"] = header.is_binary ? "binary" : "ascii";
    stats["properties"] = header.properties.size();
    if (last_load_time_us > 0) {
        stats["load_time_ms"] = last_load_time_us / 1000.0;
        stats["header_time_ms"] = last_header_time_us / 1000.0;
        stats["parse_time_ms"] = last_parse_time_us / 1000.0;
        stats["cache_time_ms"] = last_cache_time_us / 1000.0;
        stats["cache_hit"] = last_cache_hit;
    }

    if (gaussian_data.is_valid()) {
        AABB aabb = gaussian_data->get_aabb();
        stats["bounds_min"] = aabb.position;
        stats["bounds_max"] = aabb.position + aabb.size;
    }

    return stats;
}

void PLYLoader::get_property_deficiencies(PackedStringArray &r_missing_required, PackedStringArray &r_missing_optional) const {
    r_missing_required = PackedStringArray();
    r_missing_optional = PackedStringArray();

    static const char *required_props[] = {
        "x", "y", "z",
        "f_dc_0", "f_dc_1", "f_dc_2",
        "scale_0", "scale_1", "scale_2",
        "rot_0", "rot_1", "rot_2", "rot_3",
        "opacity"
    };

    for (const char *prop : required_props) {
        if (!has_property(prop)) {
            r_missing_required.push_back(prop);
        }
    }

    static const char *optional_props[] = {
        "nx", "ny", "nz",
        "palette_id",
        "brush_override_id",
        "brush_axis_u", "brush_axis_v",
        "stroke_age",
        "f_rest_0", "f_rest_1", "f_rest_2"
    };

    for (const char *prop : optional_props) {
        if (!has_property(prop)) {
            r_missing_optional.push_back(prop);
        }
    }
}

Dictionary PLYLoader::get_property_summary() const {
    Dictionary summary;

    PackedStringArray property_names;
    for (const PLYProperty &prop : header.properties) {
        property_names.push_back(prop.name);
    }

    PackedStringArray missing_required;
    PackedStringArray missing_optional;
    get_property_deficiencies(missing_required, missing_optional);

    summary["property_names"] = property_names;
    summary["property_count"] = property_names.size();
    summary["missing_required"] = missing_required;
    summary["missing_optional"] = missing_optional;
    summary["has_normals"] = has_property("nx") && has_property("ny") && has_property("nz");
    summary["has_palette"] = has_property("palette_id");
    summary["has_brush_override_id"] = has_property("brush_override_id");

    return summary;
}

bool PLYLoader::has_property(const String &p_name) const {
    return find_property_index(p_name) >= 0;
}

Vector<String> PLYLoader::get_property_names() const {
    Vector<String> names;
    for (const PLYProperty &prop : header.properties) {
        names.push_back(prop.name);
    }
    return names;
}
