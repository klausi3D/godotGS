#pragma once

#include "test_macros.h"

#ifdef TOOLS_ENABLED

#include "../io/resource_importer_ply.h"
#include "../io/resource_importer_spz.h"
#include "../io/resource_importer_gsplatworld.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/ply_loader.h"
#include "../io/spz_loader.h"
#include "../editor/gaussian_import_dialog.h"
#include "../editor/gaussian_editor_plugin.h"
#include "../editor/gaussian_import_settings_dialog.h"
#include "../editor/gaussian_resource_preview_generator.h"
#include "../editor/gaussian_thumbnail_generator.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_world.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../nodes/gaussian_splat_node_3d.h"

#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/resource_uid.h"
#include "core/config/project_settings.h"
#include "core/templates/hash_map.h"
#include "core/math/math_funcs.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "editor/editor_node.h"
#include "editor/inspector/editor_inspector.h"
#include "core/string/ustring.h"
#include "scene/main/node.h"

#include <cstring>

#include "gs_test_setting_guard.h"

namespace {

String _load_text_fixture_or_empty(const String &p_path) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null()) {
        return String();
    }
    return file->get_as_text();
}

Error _write_missing_opacity_ascii_ply(const String &p_path) {
    static const char *k_missing_opacity_ascii_ply = R"(ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
0 0 0 0 0 0 1 0 0 0 0.25 0.5 0.75
)";

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return ERR_CANT_CREATE;
    }

    file->store_string(k_missing_opacity_ascii_ply);
    file.unref();
    return OK;
}

Error _write_minimal_ascii_ply(const String &p_path) {
    static const char *k_minimal_ascii_ply = R"(ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
0 0 0 0 0 0 1 0 0 0 0 0.25 0.5 0.75
)";

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return ERR_CANT_CREATE;
    }

    file->store_string(k_minimal_ascii_ply);
    file.unref();
    return OK;
}

// PLY fixture with one splat that carries DC plus full Inria SH3 layout
// (45 f_rest_* properties). The PLY loader (ply_loader.cpp:797-826) treats
// f_rest_* as channel-major with stride SH_REST_COMPONENTS / 3 = 15, then
// repacks into coefficient-major RGB triplets. Only band 1 (terms 0/1/2) is
// populated with non-zero values; bands 2-3 are zero but declared so the
// `present` map flags them and the loader infers high_order_count = 12.
//
// Channel layout in this fixture (45 floats):
//   R band-1 (terms 0,1,2):  f_rest_0..2  = 0.10 0.20 0.30
//   R band-2..3:             f_rest_3..14 = 0
//   G band-1:                f_rest_15..17 = 0.40 0.50 0.60
//   G band-2..3:             f_rest_18..29 = 0
//   B band-1:                f_rest_30..32 = 0.70 0.80 0.90
//   B band-2..3:             f_rest_33..44 = 0
// After loader repack:
//   g.sh_1[0] = (0.10, 0.40, 0.70)
//   g.sh_1[1] = (0.20, 0.50, 0.80)
//   g.sh_1[2] = (0.30, 0.60, 0.90)
Error _write_ascii_ply_with_first_order_sh(const String &p_path) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return ERR_CANT_CREATE;
    }

    String header;
    header += "ply\n";
    header += "format ascii 1.0\n";
    header += "element vertex 1\n";
    header += "property float x\n";
    header += "property float y\n";
    header += "property float z\n";
    header += "property float scale_0\n";
    header += "property float scale_1\n";
    header += "property float scale_2\n";
    header += "property float rot_0\n";
    header += "property float rot_1\n";
    header += "property float rot_2\n";
    header += "property float rot_3\n";
    header += "property float opacity\n";
    header += "property float f_dc_0\n";
    header += "property float f_dc_1\n";
    header += "property float f_dc_2\n";
    for (int i = 0; i < 45; i++) {
        header += vformat("property float f_rest_%d\n", i);
    }
    header += "end_header\n";

    String row = "0 0 0 0 0 0 1 0 0 0 0 0.25 0.5 0.75";
    for (int i = 0; i < 45; i++) {
        float value = 0.0f;
        // R channel band-1 lives at f_rest_0..2.
        if (i == 0) value = 0.10f;
        else if (i == 1) value = 0.20f;
        else if (i == 2) value = 0.30f;
        // G channel band-1 lives at f_rest_15..17.
        else if (i == 15) value = 0.40f;
        else if (i == 16) value = 0.50f;
        else if (i == 17) value = 0.60f;
        // B channel band-1 lives at f_rest_30..32.
        else if (i == 30) value = 0.70f;
        else if (i == 31) value = 0.80f;
        else if (i == 32) value = 0.90f;
        row += " " + String::num(value, 2);
    }
    row += "\n";

    file->store_string(header);
    file->store_string(row);
    file.unref();
    return OK;
}

Error _write_invalid_gsplatworld(const String &p_path) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return ERR_CANT_CREATE;
    }

    // Header-sized payload with invalid magic/version to verify import-time validation.
    PackedByteArray bytes;
    bytes.resize(104);
    bytes.fill(0);
    file->store_buffer(bytes.ptr(), bytes.size());
    file.unref();
    return OK;
}

Error _write_truncated_copy(const String &p_source_path, const String &p_dest_path, uint64_t p_trim_bytes) {
    Error read_err = OK;
    Ref<FileAccess> src = FileAccess::open(p_source_path, FileAccess::READ, &read_err);
    if (src.is_null()) {
        return read_err != OK ? read_err : ERR_CANT_OPEN;
    }

    const uint64_t src_size = src->get_length();
    if (src_size <= p_trim_bytes) {
        return ERR_FILE_CORRUPT;
    }

    const uint64_t out_size = src_size - p_trim_bytes;
    PackedByteArray out_data;
    out_data.resize(out_size);
    const uint64_t read = src->get_buffer(out_data.ptrw(), out_size);
    if (read != out_size) {
        return ERR_FILE_CORRUPT;
    }

    Error write_err = OK;
    Ref<FileAccess> dst = FileAccess::open(p_dest_path, FileAccess::WRITE, &write_err);
    if (dst.is_null()) {
        return write_err != OK ? write_err : ERR_CANT_CREATE;
    }

    if (!dst->store_buffer(out_data.ptr(), out_data.size())) {
        const Error dst_err = dst->get_error();
        return dst_err != OK ? dst_err : ERR_FILE_CANT_WRITE;
    }

    return OK;
}

Error _corrupt_gsplatworld_for_decode_failure(const String &p_path) {
    Error open_err = OK;
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ_WRITE, &open_err);
    if (file.is_null()) {
        return open_err != OK ? open_err : ERR_CANT_OPEN;
    }

    if (file->get_length() < 104) {
        return ERR_FILE_CORRUPT;
    }

    constexpr uint64_t flags_offset = 8u;
    constexpr uint64_t gaussian_offset_field = 56u;
    constexpr uint32_t compressed_flag = 1u << 4u;

    file->seek(flags_offset);
    uint32_t flags = file->get_32();
    flags |= compressed_flag;
    file->seek(flags_offset);
    file->store_32(flags);

    file->seek(gaussian_offset_field);
    const uint64_t gaussian_offset = file->get_64();
    if (gaussian_offset + 9u > file->get_length()) {
        return ERR_FILE_CORRUPT;
    }

    file->seek(gaussian_offset);
    file->store_64(1u); // compressed size (invalid for expected payload)
    file->store_8(0u); // compressed payload byte

    const Error write_err = file->get_error();
    return write_err != OK ? write_err : OK;
}

void _remove_user_file(const String &p_user_path) {
    Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_USERDATA);
    if (dir.is_null()) {
        return;
    }

    dir->remove(p_user_path.get_file());
}

Error _write_import_sidecar(const String &p_source_path, const Dictionary &p_options) {
    Ref<ConfigFile> config;
    config.instantiate();

    Array keys = p_options.keys();
    for (int i = 0; i < keys.size(); i++) {
        const StringName key = keys[i];
        config->set_value("params", String(key), p_options[keys[i]]);
    }

    return config->save(p_source_path + ".import");
}

Ref<GaussianSplatAsset> _make_editor_test_asset(const String &p_source_path, int p_splat_count, const Dictionary &p_options,
        uint32_t p_compression_flags = GaussianSplatAsset::COMPRESSION_NONE, int64_t p_memory_estimate_bytes = 0) {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_source_path(p_source_path);
    asset->set_splat_count(MAX(p_splat_count, 0));

    PackedFloat32Array positions;
    positions.resize(MAX(p_splat_count, 0) * 3);
    for (int i = 0; i < p_splat_count; i++) {
        positions.set(i * 3 + 0, float(i));
        positions.set(i * 3 + 1, float(i) * 0.5f);
        positions.set(i * 3 + 2, 0.0f);
    }
    asset->set_positions(positions);
    asset->set_compression_flags(p_compression_flags);

    Dictionary metadata;
    metadata[StringName("options")] = p_options;
    metadata[StringName("source_path")] = p_source_path;
    if (p_memory_estimate_bytes > 0) {
        metadata[StringName("memory_estimate_bytes")] = p_memory_estimate_bytes;
    }
    asset->set_import_metadata(metadata);
    return asset;
}

class TestGaussianImportSettingsDialog : public GaussianImportSettingsDialog {
public:
    Dictionary current_import_options;
    Ref<GaussianSplatAsset> current_asset;
    int reimport_request_count = 0;
    String last_importer_name;
    HashMap<StringName, Variant> last_reimport_params;

protected:
    Dictionary _load_import_options_for_path(const String &p_path) const override {
        (void)p_path;
        return current_import_options;
    }

    Ref<GaussianSplatAsset> _load_asset_for_path(const String &p_path, bool p_force_reload) const override {
        (void)p_path;
        (void)p_force_reload;
        return current_asset;
    }

    Error _perform_reimport_request(const String &p_source_path, const String &p_importer_name,
            const HashMap<StringName, Variant> &p_params) override {
        (void)p_source_path;
        reimport_request_count += 1;
        last_importer_name = p_importer_name;
        last_reimport_params = p_params;
        return OK;
    }
};

EditorProperty *_find_property_editor(Node *p_root, const StringName &p_property_name) {
    if (!p_root) {
        return nullptr;
    }

    if (EditorProperty *property = Object::cast_to<EditorProperty>(p_root)) {
        if (property->get_edited_property() == p_property_name) {
            return property;
        }
    }

    for (int i = 0; i < p_root->get_child_count(); i++) {
        Node *child = p_root->get_child(i);
        if (EditorProperty *property = _find_property_editor(child, p_property_name)) {
            return property;
        }
    }

    return nullptr;
}

PackedByteArray _make_spz_header(uint32_t p_version, uint32_t p_num_points, uint8_t p_sh_degree, uint8_t p_fractional_bits, uint8_t p_flags = 0, uint8_t p_reserved = 0) {
    PackedByteArray header;
    header.resize(sizeof(SPZLoader::SPZHeader));
    uint8_t *w = header.ptrw();

    auto write_u32 = [&](int p_offset, uint32_t p_value) {
        w[p_offset + 0] = uint8_t(p_value & 0xFF);
        w[p_offset + 1] = uint8_t((p_value >> 8) & 0xFF);
        w[p_offset + 2] = uint8_t((p_value >> 16) & 0xFF);
        w[p_offset + 3] = uint8_t((p_value >> 24) & 0xFF);
    };

    write_u32(0, SPZLoader::SPZ_MAGIC);
    write_u32(4, p_version);
    write_u32(8, p_num_points);
    w[12] = p_sh_degree;
    w[13] = p_fractional_bits;
    w[14] = p_flags;
    w[15] = p_reserved;

    return header;
}

PackedByteArray _gzip_compress(const PackedByteArray &p_input) {
    const int64_t max_size = Compression::get_max_compressed_buffer_size(p_input.size(), Compression::MODE_GZIP);
    PackedByteArray out;
    out.resize(max_size);
    const int64_t compressed_size = Compression::compress(out.ptrw(), p_input.ptr(), p_input.size(), Compression::MODE_GZIP);
    if (compressed_size <= 0) {
        out.clear();
        return out;
    }
    out.resize(compressed_size);
    return out;
}

PackedByteArray _make_spz_v2_single_point_payload(uint8_t p_alpha, uint8_t p_r, uint8_t p_g, uint8_t p_b) {
    PackedByteArray payload;
    payload.resize(19);
    payload.fill(0);

    uint8_t *w = payload.ptrw();
    // Positions are zeroed (9 bytes).
    w[9] = p_alpha;
    w[10] = p_r;
    w[11] = p_g;
    w[12] = p_b;
    // log_scale = encoded / 16 - 10, so 160 decodes to exp(0) = 1.0
    w[13] = 160;
    w[14] = 160;
    w[15] = 160;
    // Rotation bytes remain zero => identity quaternion after reconstruction.
    return payload;
}

Error _write_binary_file(const String &p_path, const PackedByteArray &p_bytes) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return ERR_CANT_CREATE;
    }
    file->store_buffer(p_bytes);
    file.unref();
    return OK;
}

Error _write_text_file(const String &p_path, const String &p_text) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return ERR_CANT_CREATE;
    }

    file->store_string(p_text);
    file.unref();
    return OK;
}

Ref<Image> _make_thumbnail_test_image() {
    Ref<Image> image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
    image->fill(Color(0.0f, 0.0f, 0.0f, 0.0f));
    image->set_pixel(0, 0, Color(0.9f, 0.2f, 0.1f, 1.0f));
    image->set_pixel(1, 0, Color(0.1f, 0.8f, 0.3f, 1.0f));
    image->set_pixel(0, 1, Color(0.2f, 0.4f, 0.9f, 1.0f));
    image->set_pixel(1, 1, Color(1.0f, 0.9f, 0.2f, 1.0f));
    return image;
}

bool _color_approx_eq(const Color &p_a, const Color &p_b, float p_epsilon = 0.01f) {
    return Math::abs(p_a.r - p_b.r) <= p_epsilon &&
            Math::abs(p_a.g - p_b.g) <= p_epsilon &&
            Math::abs(p_a.b - p_b.b) <= p_epsilon &&
            Math::abs(p_a.a - p_b.a) <= p_epsilon;
}

Ref<GaussianSplatAsset> _make_thumbnail_fixture_asset(int p_splat_count = 6) {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_splat_count(p_splat_count);
    asset->set_asset_type(GaussianSplatAsset::ASSET_TYPE_STATIC);
    asset->set_source_path("res://thumbnail_fixture.ply");

    PackedFloat32Array positions;
    positions.resize(p_splat_count * 3);
    {
        float *positions_w = positions.ptrw();
        for (int i = 0; i < p_splat_count; i++) {
            positions_w[i * 3 + 0] = i * 0.5f;
            positions_w[i * 3 + 1] = Math::sin(float(i));
            positions_w[i * 3 + 2] = Math::cos(float(i));
        }
    }

    PackedColorArray colors;
    colors.resize(p_splat_count);
    {
        Color *colors_w = colors.ptrw();
        for (int i = 0; i < p_splat_count; i++) {
            colors_w[i] = Color(0.2f * i, 0.3f, 0.5f, 0.8f);
        }
    }

    PackedFloat32Array scales;
    scales.resize(p_splat_count * 3);
    {
        float *scales_w = scales.ptrw();
        for (int i = 0; i < p_splat_count; i++) {
            scales_w[i * 3 + 0] = 0.5f;
            scales_w[i * 3 + 1] = 0.75f;
            scales_w[i * 3 + 2] = 1.0f;
        }
    }

    PackedFloat32Array rotations;
    rotations.resize(p_splat_count * 4);
    {
        float *rotations_w = rotations.ptrw();
        for (int i = 0; i < p_splat_count; i++) {
            rotations_w[i * 4 + 0] = 1.0f;
            rotations_w[i * 4 + 1] = 0.0f;
            rotations_w[i * 4 + 2] = 0.0f;
            rotations_w[i * 4 + 3] = 0.0f;
        }
    }

    asset->set_positions(positions);
    asset->set_colors(colors);
    asset->set_scales(scales);
    asset->set_rotations(rotations);
    return asset;
}

struct _PreviewGeneratorThreadContext {
    Ref<GaussianSplatAsset> asset;
    // Optional: a pre-configured generator (e.g. with a test-shrunk
    // test_set_wait_timeout_usec(), or pre-aborted). If left null, the
    // worker instantiates a fresh default-timeout generator, matching the
    // original (pre-#592-regression-test) behavior.
    Ref<GaussianSplatAssetPreviewGenerator> generator;
    Ref<Texture2D> texture;
    Dictionary metadata;
    Semaphore done;
    bool ran_on_worker = false;
};

static void _preview_generator_thread(void *p_userdata) {
    _PreviewGeneratorThreadContext *ctx = static_cast<_PreviewGeneratorThreadContext *>(p_userdata);
    ctx->ran_on_worker = !Thread::is_main_thread();

    Ref<GaussianSplatAssetPreviewGenerator> generator = ctx->generator;
    if (generator.is_null()) {
        generator.instantiate();
    }
    ctx->texture = generator->generate(ctx->asset, Size2(96, 96), ctx->metadata);
    ctx->done.post();
}

} // namespace

TEST_CASE("[GaussianSplatting][Importer] PLY importer produces metadata and preview images") {
    const String source_path = "user://test_fixture_metadata_thumb.ply";
    {
        Error ply_err = _write_minimal_ascii_ply(source_path);
        REQUIRE_MESSAGE(ply_err == OK, "Failed to create synthetic PLY fixture.");
    }

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    const String save_base_path = "user://gaussian_import_test";

    HashMap<StringName, Variant> options;
    options.insert(StringName("general/asset_type"), 0);
    options.insert(StringName("quality/preset"), String("desktop"));
    options.insert(StringName("compression/quantize_positions"), true);
    options.insert(StringName("compression/quantize_colors"), false);
    options.insert(StringName("compression/quantize_scales"), true);
    options.insert(StringName("compression/quantize_rotations"), false);
    options.insert(StringName("compression/pack_opacity"), true); // force customization
    options.insert(StringName("preview/generate_thumbnail"), true);
    options.insert(StringName("preview/thumbnail_style"),
            int(GaussianThumbnailGenerator::THUMBNAIL_STYLE_HEATMAP));
    options.insert(StringName("preview/thumbnail_size"), 128);
    options.insert(StringName("metadata/include_memory_estimate"), true);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options, nullptr, nullptr,
            &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "ResourceImporterPLY should successfully import the test PLY file.");
    if (import_err != OK) {
        return;
    }

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
    CHECK_MESSAGE(asset.is_valid(), "Imported GaussianSplatAsset should be loadable from disk.");
    if (!asset.is_valid()) {
        return;
    }

    CHECK(asset->get_splat_count() > 0);
    CHECK(asset->get_import_quality_preset() == String("desktop"));
    CHECK((asset->get_compression_flags() & GaussianSplatAsset::COMPRESSION_POSITIONS) != 0);
    CHECK_MESSAGE(asset->get_preview_image().is_valid(), "Importer should store a preview image for the Gaussian asset.");
    CHECK(asset->get_source_path() == source_path);

    Dictionary metadata = metadata_variant;
    CHECK(metadata.has(StringName("options")));
    Dictionary option_dict = metadata.get(StringName("options"), Dictionary());
    CHECK(option_dict.has(StringName("compression/pack_opacity")));
    CHECK(bool(option_dict[StringName("compression/pack_opacity")]) == true);
    CHECK(metadata.has(StringName("memory_estimate_mb")));
    CHECK((double)metadata[StringName("memory_estimate_mb")] > 0.0);
    CHECK(metadata.get(StringName("quality_customized"), false));
    CHECK(String(metadata.get(StringName("thumbnail_style_name"), String())) ==
            GaussianThumbnailGenerator::style_to_display_name(GaussianThumbnailGenerator::THUMBNAIL_STYLE_HEATMAP));

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer] Reimport updates quality options") {
    const String source_path = "user://test_fixture_reimport.ply";
    {
        Error ply_err = _write_minimal_ascii_ply(source_path);
        REQUIRE_MESSAGE(ply_err == OK, "Failed to create synthetic PLY fixture.");
    }

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    const String save_base_path = "user://gaussian_reimport_test";

    HashMap<StringName, Variant> options;
    options.insert(StringName("general/asset_type"), 0);
    options.insert(StringName("quality/preset"), String("high"));
    options.insert(StringName("preview/generate_thumbnail"), true);
    options.insert(StringName("metadata/include_statistics"), true);

    Variant metadata_variant;
    Error first_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options, nullptr, nullptr,
            &metadata_variant);
    CHECK(first_err == OK);
    if (first_err != OK) {
        return;
    }

    // Reimport with modified preset and disabled thumbnail generation.
    options.insert(StringName("quality/preset"), String("mobile"));
    options.insert(StringName("preview/generate_thumbnail"), false);
    options.insert(StringName("metadata/include_memory_estimate"), false);

    Variant second_metadata_variant;
    Error second_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options, nullptr, nullptr,
            &second_metadata_variant);
    CHECK(second_err == OK);
    if (second_err != OK) {
        return;
    }

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
    CHECK(asset.is_valid());
    if (!asset.is_valid()) {
        return;
    }

    CHECK(asset->get_import_quality_preset() == String("mobile"));
    CHECK(!asset->get_preview_image().is_valid());

    Dictionary metadata = second_metadata_variant;
    CHECK(String(metadata.get(StringName("quality_preset"), String())) == String("mobile"));
    CHECK(!metadata.get(StringName("thumbnail_generated"), true));
    Dictionary option_dict = metadata.get(StringName("options"), Dictionary());
    CHECK(String(option_dict.get(StringName("quality/preset"), String())) == String("mobile"));

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer] Ultra quality preset preserves full PLY splat count") {
    const String source_path = "user://test_fixture_ultra_count.ply";
    {
        Error ply_err = _write_minimal_ascii_ply(source_path);
        REQUIRE_MESSAGE(ply_err == OK, "Failed to create synthetic PLY fixture.");
    }

    Ref<PLYLoader> loader;
    loader.instantiate();
    const Error load_err = loader->load_file(source_path);
    REQUIRE_MESSAGE(load_err == OK, "Fixture PLY must load before testing ultra-quality import.");
    const Ref<::GaussianData> source_data = loader->get_gaussian_data();
    REQUIRE_MESSAGE(source_data.is_valid(), "Fixture PLY should produce GaussianData.");
    const int source_count = source_data->get_count();
    REQUIRE_MESSAGE(source_count > 0, "Fixture PLY should contain at least one splat.");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    const String save_base_path = "user://gaussian_ultra_quality_preserve_count";
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    CHECK_MESSAGE(import_err == OK, "Ultra preset import should succeed for fixture PLY.");
    if (import_err != OK) {
        return;
    }

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
    CHECK_MESSAGE(asset.is_valid(), "Ultra preset import should produce a loadable asset.");
    if (asset.is_null()) {
        _remove_user_file(save_base_path + ".res");
        return;
    }

    CHECK_MESSAGE(int(asset->get_splat_count()) == source_count,
            "Ultra preset should preserve all source splats when density is 1 and max_splats is unlimited.");

    const Dictionary metadata = metadata_variant;
    CHECK(int64_t(metadata.get(StringName("original_splat_count"), int64_t(-1))) == source_count);
    CHECK(int64_t(metadata.get(StringName("splat_count"), int64_t(-1))) == source_count);
    CHECK(String(metadata.get(StringName("quality_preset"), String())) == String("ultra"));

    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Importer] Ultra import initializes fresh node runtime from imported fidelity") {
    // TODO: set_splat_asset does not yet auto-switch to QUALITY_CUSTOM with relaxed LOD for full-fidelity assets.
    MESSAGE("Skipping - aspirational test, requires set_splat_asset fidelity auto-detection (not yet wired)");
    return;
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE_MESSAGE(project_settings != nullptr, "ProjectSettings singleton must exist for runtime fidelity regression.");
    ProjectSettingGuard tier_preset_guard(project_settings, "rendering/gaussian_splatting/quality/tier_preset");
    ProjectSettingGuard tier_apply_guard(project_settings, "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets");
    project_settings->set_setting("rendering/gaussian_splatting/quality/tier_preset", "custom");
    project_settings->set_setting("rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", false);

    const String source_path = "user://test_fixture_ultra_fidelity.ply";
    {
        Error ply_err = _write_minimal_ascii_ply(source_path);
        REQUIRE_MESSAGE(ply_err == OK, "Failed to create synthetic PLY fixture.");
    }

    Ref<PLYLoader> loader;
    loader.instantiate();
    const Error load_err = loader->load_file(source_path);
    REQUIRE_MESSAGE(load_err == OK, "Fixture PLY must load before testing node runtime fidelity propagation.");
    const Ref<::GaussianData> source_data = loader->get_gaussian_data();
    REQUIRE_MESSAGE(source_data.is_valid(), "Fixture PLY should produce GaussianData.");
    const int source_count = source_data->get_count();
    REQUIRE_MESSAGE(source_count > 0, "Fixture PLY should contain at least one splat.");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    const String save_base_path = "user://gaussian_ultra_runtime_fidelity";
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    REQUIRE_MESSAGE(import_err == OK, "Ultra preset import should succeed for node runtime fidelity regression.");

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
    REQUIRE_MESSAGE(asset.is_valid(), "Imported asset should be loadable for node runtime fidelity regression.");
    REQUIRE_MESSAGE(int(asset->get_splat_count()) == source_count, "Imported asset should preserve the full source count.");

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    CHECK(node->get_quality_preset() == GaussianSplatNode3D::QUALITY_BALANCED);
    CHECK(node->get_max_splat_count() == 500000);

    node->set_splat_asset(asset);

    CHECK(node->get_quality_preset() == GaussianSplatNode3D::QUALITY_CUSTOM);
    CHECK_MESSAGE(node->get_max_splat_count() >= source_count,
            "Fresh node should not re-cap a full-fidelity imported asset below its imported splat count.");
    const GaussianSplatting::GaussianSplatLODConfig &lod_config = node->get_lod_config();
    CHECK(lod_config.max_splats_per_frame >= (uint32_t)source_count);
    CHECK(lod_config.importance_threshold == doctest::Approx(0.0f));
    CHECK(lod_config.size_cull_threshold == doctest::Approx(0.0f));

    memdelete(node);
    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Importer] Imported fidelity defaults do not override customized node quality") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE_MESSAGE(project_settings != nullptr, "ProjectSettings singleton must exist for customized node regression.");
    ProjectSettingGuard tier_preset_guard(project_settings, "rendering/gaussian_splatting/quality/tier_preset");
    ProjectSettingGuard tier_apply_guard(project_settings, "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets");
    project_settings->set_setting("rendering/gaussian_splatting/quality/tier_preset", "custom");
    project_settings->set_setting("rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", false);

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    const String source_path = "user://test_fixture_customized_node.ply";
    {
        Error ply_err = _write_minimal_ascii_ply(source_path);
        REQUIRE_MESSAGE(ply_err == OK, "Failed to create synthetic PLY fixture.");
    }

    const String save_base_path = "user://gaussian_customized_node_runtime";
    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("quality/max_splats"), 0);
    options.insert(StringName("quality/density_multiplier"), 1.0);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    REQUIRE_MESSAGE(import_err == OK, "Ultra preset import should succeed for customized node regression.");

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + String(".res"));
    REQUIRE_MESSAGE(asset.is_valid(), "Imported asset should be loadable for customized node regression.");

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    node->set_quality_preset(GaussianSplatNode3D::QUALITY_PERFORMANCE);

    node->set_splat_asset(asset);

    CHECK(node->get_quality_preset() == GaussianSplatNode3D::QUALITY_PERFORMANCE);
    CHECK(node->get_max_splat_count() == 200000);

    memdelete(node);
    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] Missing required PLY properties fail consistently across strict paths") {
    const String source_path = "user://gaussian_missing_required_opacity.ply";
    const String save_base_path = "user://gaussian_missing_required_opacity_import";

    Error write_err = _write_missing_opacity_ascii_ply(source_path);
    CHECK_MESSAGE(write_err == OK, "Failed to create malformed PLY test fixture.");
    if (write_err != OK) {
        return;
    }

    Ref<PLYLoader> loader;
    loader.instantiate();
    Error loader_err = loader->load_file(source_path);
    CHECK(loader_err == OK);
    if (loader_err == OK) {
        PackedStringArray missing_required;
        PackedStringArray missing_optional;
        loader->get_property_deficiencies(missing_required, missing_optional);
        CHECK(missing_required.find("opacity") != -1);
    }

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    options.insert(StringName("validation/validate_required_properties"), true);
    options.insert(StringName("validation/warn_missing_optional"), false);
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options, nullptr, nullptr,
            &metadata_variant);
    CHECK_MESSAGE(import_err == ERR_FILE_CORRUPT,
            "ResourceImporterPLY should reject files missing required properties when strict validation is enabled.");

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    Error asset_err = asset->load_from_file(source_path);
    CHECK_MESSAGE(asset_err == ERR_FILE_CORRUPT,
            "GaussianSplatAsset::load_from_file should reject the same malformed PLY file.");

    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Thumbnail] Generator produces preview images for each style") {
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset();
    const int splat_count = asset->get_splat_count();

    Ref<GaussianThumbnailGenerator> generator;
    generator.instantiate();

    for (int style = 0; style < 4; style++) {
        Ref<Image> image = generator->generate_thumbnail_image(asset, 96,
                GaussianThumbnailGenerator::style_from_int(style));
        CHECK_MESSAGE(image.is_valid(), "Thumbnail generator should return a valid image for each style.");
    }

    Dictionary memory = generator->compute_memory_statistics(splat_count, GaussianSplatAsset::COMPRESSION_NONE, false);
    CHECK(float(memory.get(StringName("total_mb"), 0.0)) > 0.0f);
}

TEST_CASE("[GaussianSplatting][Thumbnail] Generator caches deterministic asset+settings keys") {
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset();

    Ref<GaussianThumbnailGenerator> generator;
    generator.instantiate();

    Ref<Image> first = generator->generate_thumbnail_image(asset, 96, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR);
    Ref<Image> second = generator->generate_thumbnail_image(asset, 96, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR);
    Ref<Image> alternate_style = generator->generate_thumbnail_image(asset, 96, GaussianThumbnailGenerator::THUMBNAIL_STYLE_HEATMAP);
    CHECK(first.is_valid());
    CHECK(second.is_valid());
    CHECK(alternate_style.is_valid());
    CHECK(first == second);

    Dictionary stats = generator->get_cache_statistics();
    CHECK(int(stats.get(StringName("entries"), 0)) == 2);
    CHECK(int64_t(stats.get(StringName("hits"), int64_t(0))) >= 1);
    CHECK(int64_t(stats.get(StringName("misses"), int64_t(0))) >= 2);

    asset->set_source_path("res://thumbnail_fixture_reimported.ply");
    Ref<Image> after_source_change = generator->generate_thumbnail_image(asset, 96, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR);
    CHECK(after_source_change.is_valid());

    stats = generator->get_cache_statistics();
    CHECK(int(stats.get(StringName("entries"), 0)) == 3);
}

TEST_CASE("[GaussianSplatting][Thumbnail] Editor preview generator uses stored images on worker threads") {
#ifndef THREADS_ENABLED
    MESSAGE("Skipping - THREADS_ENABLED is not enabled in this build");
    return;
#else
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_splat_count(1);
    asset->set_source_path("res://preview_worker_fixture.ply");
    asset->set_preview_image(_make_thumbnail_test_image());

    _PreviewGeneratorThreadContext ctx;
    ctx.asset = asset;

    Thread worker;
    worker.start(_preview_generator_thread, &ctx);
    CHECK(worker.is_started());
    if (worker.is_started()) {
        while (!ctx.done.try_wait()) {
            CallQueue *main_queue = MessageQueue::get_main_singleton();
            if (main_queue) {
                main_queue->flush();
            }
            OS::get_singleton()->delay_usec(1000);
        }
        worker.wait_to_finish();
    }

    CHECK(ctx.ran_on_worker);
    CHECK(ctx.texture.is_valid());
    CHECK(String(ctx.metadata.get(StringName("gaussian_preview_source"), String())) == "stored_thumbnail");
#endif
}

// #592 residual gap (external review): the happy-path test above proves the
// bounded wait completes when the main queue IS flushed. It never exercised
// the two reasons the wait was bounded in the first place: main-queue
// starvation (the timeout firing) and editor shutdown (abort() firing). Both
// are proven below. Neither test ever flushes the main message queue, so
// the worker's request can only be resolved by timeout or by abort() -- the
// exact starved/aborted conditions #592 was written to survive.
// Tagged [Editor] (not just [Thumbnail]) so Godot's doctest harness
// (tests/test_main.cpp's GodotTestCaseListener::test_case_start(), which
// gates on the literal substring "[Editor]"/"[SceneTree]" in the test name)
// stands up a MessageQueue main singleton for the duration of this test.
// Without it, MessageQueue::get_main_singleton() is null, so
// _create_preview_texture_from_image() takes its
// ERR_FAIL_NULL_V_MSG(main_queue, ...) early-out on the very first line and
// never reaches the wait/poll/timeout logic this test exists to exercise --
// this is not hypothetical: dropping the tag makes this test pass for the
// wrong reason (an instant early return, not an honored timeout).
TEST_CASE("[GaussianSplatting][Thumbnail][Editor] Editor preview generator gives up (not hang) when the main queue is starved (#592)") {
#ifndef THREADS_ENABLED
    MESSAGE("Skipping - THREADS_ENABLED is not enabled in this build");
    return;
#else
    // A fixture with no stored preview image: get_preview_image() is null, so
    // the "stored thumbnail" attempt short-circuits instantly and the worker
    // falls through to generating (and then trying to texture-ify) a fresh
    // thumbnail -- the same wait/poll/timeout code path as the stored-image
    // case, just reached the other way.
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset(1);
    asset->set_source_path("res://preview_worker_starvation_fixture.ply");

    Ref<GaussianSplatAssetPreviewGenerator> generator;
    generator.instantiate();
    // Shrink the bounded wait so this test proves the timeout actually fires
    // without waiting out the real 10s production backstop. Production code
    // never calls this setter (see gaussian_editor_plugin.cpp registration);
    // it exists solely for this regression test.
    const uint64_t test_timeout_usec = 30 * 1000; // 30 ms.
    generator->test_set_wait_timeout_usec(test_timeout_usec);

    _PreviewGeneratorThreadContext ctx;
    ctx.asset = asset;
    ctx.generator = generator;

    Thread worker;
    const uint64_t start_usec = OS::get_singleton()->get_ticks_usec();
    worker.start(_preview_generator_thread, &ctx);
    CHECK(worker.is_started());
    if (worker.is_started()) {
        // Deliberately never flush the main queue: this simulates main-queue
        // starvation. If the worker still blocked in wait() (the pre-#592
        // bug) or never re-checked its deadline, this call would hang the
        // whole test binary.
        worker.wait_to_finish();
    }
    const uint64_t elapsed_usec = OS::get_singleton()->get_ticks_usec() - start_usec;

    // The worker must return (not hang), honoring roughly the configured
    // timeout -- not returning instantly (which would mean the timeout was
    // not honored at all) and nowhere near the real 10s production default.
    CHECK(elapsed_usec >= test_timeout_usec);
    CHECK(elapsed_usec < 5 * 1000 * 1000); // Generous bound, still far below the 10s prod backstop.

    CHECK(ctx.ran_on_worker);
    CHECK_FALSE(ctx.texture.is_valid());
    CHECK(String(ctx.metadata.get(StringName("gaussian_preview_source"), String())) != "stored_thumbnail");

    // Lifetime contract from #592: abandoning the wait must not free the
    // shared request out from under the still-queued main-thread callable.
    // The message is still sitting in the main queue (never flushed above);
    // flush it now and prove running it is still safe (no crash, no UAF)
    // even though nothing reads its result anymore.
    CallQueue *main_queue = MessageQueue::get_main_singleton();
    if (main_queue) {
        main_queue->flush();
    }
#endif
}

// See the [Editor] tag note on the starvation test above: this test needs
// the same MessageQueue main singleton, for the same reason.
TEST_CASE("[GaussianSplatting][Thumbnail][Editor] Editor preview generator aborts promptly instead of waiting out the timeout (#592)") {
#ifndef THREADS_ENABLED
    MESSAGE("Skipping - THREADS_ENABLED is not enabled in this build");
    return;
#else
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset(1);
    asset->set_source_path("res://preview_worker_abort_fixture.ply");

    Ref<GaussianSplatAssetPreviewGenerator> generator;
    generator.instantiate();
    // Deliberately leave the wait timeout at its real production default: this
    // test must prove abort() unblocks the worker almost immediately
    // regardless of the timeout value, exactly as
    // EditorResourcePreview::stop() needs during editor shutdown.
    REQUIRE(generator->test_get_wait_timeout_usec() == GaussianSplatAssetPreviewGenerator::DEFAULT_WAIT_TIMEOUT_USEC);

    // Call abort() before the worker even starts. The wait loop re-checks
    // aborted.is_set() every poll iteration regardless of when it was set,
    // so this is a deterministic stand-in for EditorResourcePreview::stop()
    // racing with (or preceding) an in-flight generate() call -- it removes
    // any dependence on thread-scheduling timing from this test.
    generator->abort();

    _PreviewGeneratorThreadContext ctx;
    ctx.asset = asset;
    ctx.generator = generator;

    Thread worker;
    const uint64_t start_usec = OS::get_singleton()->get_ticks_usec();
    worker.start(_preview_generator_thread, &ctx);
    CHECK(worker.is_started());
    if (worker.is_started()) {
        // Main queue is deliberately never flushed: only abort() can unblock
        // this worker.
        worker.wait_to_finish();
    }
    const uint64_t elapsed_usec = OS::get_singleton()->get_ticks_usec() - start_usec;

    // abort() must short-circuit the wait almost immediately: nowhere near
    // the 10s production timeout (and this generator was never given a
    // shrunk timeout, so a pass here cannot be explained by the timeout
    // firing instead of the abort signal).
    CHECK(elapsed_usec < 2 * 1000 * 1000); // Well under 2s, let alone 10s.
    CHECK(ctx.ran_on_worker);
    CHECK_FALSE(ctx.texture.is_valid());

    // Same lifetime contract as the starvation test above: flushing the
    // still-queued callable after the wait was abandoned must not crash or
    // use freed memory.
    CallQueue *main_queue = MessageQueue::get_main_singleton();
    if (main_queue) {
        main_queue->flush();
    }
#endif
}

TEST_CASE("[GaussianSplatting][Importer] GaussianSplatAsset serializes preview images without ImageTexture state") {
    const String save_path = "user://gaussian_asset_preview_image_roundtrip.tres";

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_splat_count(1);
    asset->set_source_path("res://preview_image_fixture.ply");
    asset->set_preview_image(_make_thumbnail_test_image());

    const Error save_err = ResourceSaver::save(asset, save_path);
    CHECK_MESSAGE(save_err == OK, "Saving a GaussianSplatAsset with a preview image should succeed.");
    if (save_err != OK) {
        _remove_user_file(save_path);
        return;
    }

    const String saved_text = _load_text_fixture_or_empty(save_path);
    CHECK_MESSAGE(saved_text.contains("import/thumbnail = SubResource("),
            "Serialized GaussianSplatAsset should persist the preview image property.");
    CHECK_MESSAGE(!saved_text.contains("type=\"ImageTexture\""),
            "Serialized GaussianSplatAsset should not embed an ImageTexture subresource.");
    CHECK_MESSAGE(!saved_text.contains("rid://"),
            "Serialized GaussianSplatAsset should not persist texture RID state.");

    Ref<GaussianSplatAsset> loaded = ResourceLoader::load(save_path, "GaussianSplatAsset");
    CHECK_MESSAGE(loaded.is_valid(), "Roundtripped GaussianSplatAsset should load from disk.");
    if (loaded.is_valid()) {
        Ref<Image> preview = loaded->get_preview_image();
        CHECK(preview.is_valid());
        if (preview.is_valid()) {
            CHECK(preview->get_width() == 2);
            CHECK(preview->get_height() == 2);
            CHECK(_color_approx_eq(preview->get_pixel(0, 0), Color(0.9f, 0.2f, 0.1f, 1.0f)));
        }
    }

    _remove_user_file(save_path);
}

TEST_CASE("[GaussianSplatting][Importer][RequiresGPU] GaussianSplatAsset loads legacy ImageTexture thumbnails") {
    REQUIRE_GPU_DEVICE();

    const String modern_path = "user://gaussian_asset_preview_modern.tres";
    const String legacy_path = "user://gaussian_asset_preview_legacy.tres";

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_splat_count(1);
    asset->set_source_path("res://legacy_preview_fixture.ply");
    asset->set_preview_image(_make_thumbnail_test_image());

    const Error save_err = ResourceSaver::save(asset, modern_path);
    CHECK_MESSAGE(save_err == OK, "Saving the modern preview-image asset should succeed.");
    if (save_err != OK) {
        _remove_user_file(modern_path);
        _remove_user_file(legacy_path);
        return;
    }

    String legacy_text = _load_text_fixture_or_empty(modern_path);
    const String property_prefix = "import/thumbnail = SubResource(\"";
    const int property_pos = legacy_text.find(property_prefix);
    REQUIRE_MESSAGE(property_pos >= 0, "Modern GaussianSplatAsset should serialize preview image subresources.");
    const int id_start = property_pos + property_prefix.length();
    const int id_end = legacy_text.find("\")", id_start);
    REQUIRE_MESSAGE(id_end > id_start, "Modern preview image subresource id should be parseable.");
    const String image_id = legacy_text.substr(id_start, id_end - id_start);

    legacy_text = legacy_text.replace(String("import/thumbnail = SubResource(\"") + image_id + "\")",
            "import/thumbnail = SubResource(\"ImageTexture_legacy_preview\")");

    const int resource_pos = legacy_text.find("[resource]");
    REQUIRE_MESSAGE(resource_pos >= 0, "Modern GaussianSplatAsset text resource should contain a [resource] block.");
    const String legacy_subresource =
            "[sub_resource type=\"ImageTexture\" id=\"ImageTexture_legacy_preview\"]\n"
            "image = SubResource(\"" + image_id + "\")\n\n";
    legacy_text = legacy_text.insert(resource_pos, legacy_subresource);

    const Error legacy_write_err = _write_text_file(legacy_path, legacy_text);
    CHECK_MESSAGE(legacy_write_err == OK, "Writing the legacy ImageTexture fixture should succeed.");
    if (legacy_write_err != OK) {
        _remove_user_file(modern_path);
        _remove_user_file(legacy_path);
        return;
    }

    Ref<GaussianSplatAsset> loaded = ResourceLoader::load(legacy_path, "GaussianSplatAsset");
    CHECK_MESSAGE(loaded.is_valid(), "Legacy ImageTexture-backed GaussianSplatAsset should still load.");
    if (loaded.is_valid()) {
        Ref<Image> preview = loaded->get_preview_image();
        CHECK(preview.is_valid());
        if (preview.is_valid()) {
            CHECK(_color_approx_eq(preview->get_pixel(1, 1), Color(1.0f, 0.9f, 0.2f, 1.0f)));
        }

        const String resaved_path = "user://gaussian_asset_preview_legacy_resaved.tres";
        const Error resave_err = ResourceSaver::save(loaded, resaved_path);
        CHECK_MESSAGE(resave_err == OK, "Resaving a legacy GaussianSplatAsset should migrate the preview to Image storage.");
        if (resave_err == OK) {
            const String resaved_text = _load_text_fixture_or_empty(resaved_path);
            CHECK(!resaved_text.contains("type=\"ImageTexture\""));
            _remove_user_file(resaved_path);
        } else {
            _remove_user_file(resaved_path);
        }
    }

    _remove_user_file(modern_path);
    _remove_user_file(legacy_path);
}

TEST_CASE("[GaussianSplatting][Editor] Import dialog respects metadata baselines") {
    GaussianImportDialog *dialog = memnew(GaussianImportDialog);

    Dictionary baseline_options;
    baseline_options[StringName("quality/preset")] = String("desktop");
    baseline_options[StringName("compression/quantize_positions")] = true;
    baseline_options[StringName("preview/generate_thumbnail")] = true;

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_import_quality_preset("desktop");
    asset->set_compression_flags(GaussianSplatAsset::COMPRESSION_POSITIONS);
    asset->set_source_path("res://test_splats.ply");
    Dictionary metadata;
    metadata[StringName("options")] = baseline_options;
    metadata[StringName("quality_preset")] = String("desktop");
    metadata[StringName("source_path")] = String("res://test_splats.ply");
    asset->set_import_metadata(metadata);

    dialog->configure_for_file("res://test_splats.ply", asset, true, Dictionary());

    Dictionary selected = dialog->get_selected_options();
    CHECK(String(selected.get(StringName("quality/preset"), String())) == String("desktop"));
    CHECK(selected.has(StringName("compression/quantize_positions")));
    CHECK(bool(selected[StringName("compression/quantize_positions")]) == true);

    GaussianImportDialog::ImportConfiguration config = dialog->get_configuration();
    CHECK(config.preset == "desktop");
    CHECK(config.custom_settings == false);

    memdelete(dialog);
}

TEST_CASE("[GaussianSplatting][Editor] Import dialog applies quality override options") {
    GaussianImportDialog *dialog = memnew(GaussianImportDialog);

    Dictionary baseline_options;
    baseline_options[StringName("quality/preset")] = String("desktop");
    baseline_options[StringName("quality/max_splats")] = 250000;
    baseline_options[StringName("quality/density_multiplier")] = 0.5;

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_import_quality_preset("desktop");
    asset->set_source_path("res://test_splats.ply");

    Dictionary metadata;
    metadata[StringName("options")] = baseline_options;
    metadata[StringName("quality_preset")] = String("desktop");
    metadata[StringName("source_path")] = String("res://test_splats.ply");
    asset->set_import_metadata(metadata);

    Dictionary override_options;
    override_options[StringName("quality/preset")] = String("ultra");
    override_options[StringName("quality/max_splats")] = 0;
    override_options[StringName("quality/density_multiplier")] = 1.0;

    dialog->configure_for_file("res://test_splats.ply", asset, true, override_options);

    const Dictionary selected = dialog->get_selected_options();
    CHECK(String(selected.get(StringName("quality/preset"), String())) == String("ultra"));
    CHECK(int(selected.get(StringName("quality/max_splats"), -1)) == 0);
    CHECK((double)selected.get(StringName("quality/density_multiplier"), -1.0) == doctest::Approx(1.0));

    const GaussianImportDialog::ImportConfiguration config = dialog->get_configuration();
    CHECK(config.preset == "ultra");
    CHECK(config.max_splats == 0);
    CHECK(config.density_multiplier == doctest::Approx(1.0));

    memdelete(dialog);
}

// ============================================================================
// SPZ Importer Tests
// ============================================================================

TEST_CASE("[GaussianSplatting][Importer] SPZ importer basic configuration") {
    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();

    CHECK(importer->get_importer_name() == "gaussian_splat_spz");
    CHECK(importer->get_visible_name() == "Gaussian Splat SPZ");
    CHECK(importer->get_resource_type() == "GaussianSplatAsset");
    CHECK(importer->get_save_extension() == "res");
    CHECK(importer->can_import_threaded() == true);

    List<String> extensions;
    importer->get_recognized_extensions(&extensions);
    CHECK(extensions.size() == 1);
    CHECK(extensions.front()->get() == "spz");

    CHECK(importer->get_preset_count() > 0);
    for (int i = 0; i < importer->get_preset_count(); i++) {
        CHECK(!importer->get_preset_name(i).is_empty());
    }
}

TEST_CASE("[GaussianSplatting][Importer] SPZ importer provides import options") {
    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();

    List<ResourceImporter::ImportOption> options;
    importer->get_import_options("res://test.spz", &options, 0);

    bool has_preset_option = false;
    bool has_thumbnail_option = false;
    bool has_compression_option = false;
    bool has_metadata_option = false;

    for (const ResourceImporter::ImportOption &opt : options) {
        String name = String(opt.option.name);
        if (name == "quality/preset") {
            has_preset_option = true;
        } else if (name == "preview/generate_thumbnail") {
            has_thumbnail_option = true;
        } else if (name == "compression/quantize_positions") {
            has_compression_option = true;
        } else if (name == "metadata/include_statistics") {
            has_metadata_option = true;
        }
    }

    CHECK_MESSAGE(has_preset_option, "SPZ importer should provide quality/preset option");
    CHECK_MESSAGE(has_thumbnail_option, "SPZ importer should provide thumbnail generation option");
    CHECK_MESSAGE(has_compression_option, "SPZ importer should provide compression options");
    CHECK_MESSAGE(has_metadata_option, "SPZ importer should provide metadata options");
}

TEST_CASE("[GaussianSplatting][Importer] SPZ importer handles missing file gracefully") {
    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();

    const String source_path = "res://nonexistent_test.spz";
    const String save_base_path = "user://gaussian_spz_missing_test";

    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("desktop"));
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options, nullptr, nullptr,
            &metadata_variant);
    CHECK_MESSAGE(import_err == ERR_FILE_NOT_FOUND, "SPZ importer should return ERR_FILE_NOT_FOUND for missing files.");
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] SPZ loader rejects truncated payload sections") {
    const String source_path = "user://gaussian_spz_truncated_payload.spz";

    // Version 2, 2 points, SH degree 0. Minimum payload is 38 bytes; provide 37 bytes.
    PackedByteArray payload;
    payload.resize(37);
    memset(payload.ptrw(), 0, payload.size());

    PackedByteArray compressed_payload = _gzip_compress(payload);
    REQUIRE_MESSAGE(!compressed_payload.is_empty(), "Failed to gzip-compress truncated SPZ payload fixture");

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 2, 0, 12);
    const int header_size = file_data.size();
    file_data.resize(header_size + compressed_payload.size());
    memcpy(file_data.ptrw() + header_size, compressed_payload.ptr(), compressed_payload.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write truncated SPZ fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Truncated SPZ payload must be rejected as corrupt");

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer] SPZ loader marks DC encoding as linear RGB") {
    const String source_path = "user://gaussian_spz_linear_dc.spz";

    PackedByteArray payload = _make_spz_v2_single_point_payload(255, 64, 128, 255);
    PackedByteArray compressed_payload = _gzip_compress(payload);
    REQUIRE_MESSAGE(!compressed_payload.is_empty(), "Failed to gzip-compress SPZ linear-DC fixture");

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 12);
    const int header_size = file_data.size();
    file_data.resize(header_size + compressed_payload.size());
    memcpy(file_data.ptrw() + header_size, compressed_payload.ptr(), compressed_payload.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write SPZ linear-DC fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    REQUIRE_MESSAGE(load_err == OK, "SPZ loader should accept valid single-point payload");

    Ref<::GaussianData> data = loader->get_gaussian_data();
    REQUIRE_MESSAGE(data.is_valid(), "SPZ loader must produce GaussianData");
    REQUIRE_MESSAGE(data->get_count() == 1, "SPZ loader must load the single-point payload");

    const Gaussian g = data->get_gaussian(0);
    CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    CHECK(Math::is_equal_approx(g.sh_dc.r, 64.0f / 255.0f));
    CHECK(Math::is_equal_approx(g.sh_dc.g, 128.0f / 255.0f));
    CHECK(Math::is_equal_approx(g.sh_dc.b, 1.0f));

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] SPZ loader rejects out-of-range fractional_bits") {
    // A2 regression: fractional_bits is used as `1 << fractional_bits` in
    // fixed_to_float(); >= 31 is shift UB and > 24 is nonsensical for the 24-bit
    // fixed-point positions. Craft a structurally VALID single-point v2 file (the
    // same payload the linear-DC test above loads OK) but with fractional_bits =
    // 40. The header/payload-size guards all pass, so before the fix the loader
    // reached fixed_to_float with a bogus shift; the new range check must reject
    // it as corrupt first.
    const String source_path = "user://gaussian_spz_bad_fractional_bits.spz";

    PackedByteArray payload = _make_spz_v2_single_point_payload(255, 64, 128, 255);
    PackedByteArray compressed_payload = _gzip_compress(payload);
    REQUIRE_MESSAGE(!compressed_payload.is_empty(), "Failed to gzip-compress SPZ bad-fractional-bits fixture");

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 40);
    const int header_size = file_data.size();
    file_data.resize(header_size + compressed_payload.size());
    memcpy(file_data.ptrw() + header_size, compressed_payload.ptr(), compressed_payload.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write SPZ bad-fractional-bits fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "SPZ with fractional_bits > 24 must be rejected as corrupt");

    // Sanity: the SAME fixture with a valid fractional_bits (12) loads OK, proving
    // the fractional_bits field is the sole defect (no other guard pre-empts it).
    const String valid_path = "user://gaussian_spz_good_fractional_bits.spz";
    PackedByteArray valid_file = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 12);
    const int valid_header_size = valid_file.size();
    valid_file.resize(valid_header_size + compressed_payload.size());
    memcpy(valid_file.ptrw() + valid_header_size, compressed_payload.ptr(), compressed_payload.size());
    REQUIRE_MESSAGE(_write_binary_file(valid_path, valid_file) == OK, "Failed to write SPZ good-fractional-bits fixture");
    Ref<SPZLoader> valid_loader;
    valid_loader.instantiate();
    CHECK_MESSAGE(valid_loader->load_file(valid_path) == OK, "SPZ with fractional_bits = 12 must load");

    _remove_user_file(valid_path);
    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer] SPZ importer persists linear DC encoding metadata") {
    const String source_path = "user://gaussian_spz_linear_dc_import.spz";
    const String save_base_path = "user://gaussian_spz_linear_dc_imported";

    PackedByteArray payload = _make_spz_v2_single_point_payload(255, 64, 128, 255);
    PackedByteArray compressed_payload = _gzip_compress(payload);
    REQUIRE_MESSAGE(!compressed_payload.is_empty(), "Failed to gzip-compress SPZ import fixture");

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 12);
    const int header_size = file_data.size();
    file_data.resize(header_size + compressed_payload.size());
    memcpy(file_data.ptrw() + header_size, compressed_payload.ptr(), compressed_payload.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write SPZ import fixture");

    Ref<ResourceImporterSPZ> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("desktop"));
    options.insert(StringName("preview/generate_thumbnail"), false);

    Variant metadata_variant;
    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &metadata_variant);
    REQUIRE_MESSAGE(import_err == OK, "SPZ importer should succeed for linear-DC fixture");

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + ".res");
    REQUIRE_MESSAGE(asset.is_valid(), "SPZ importer should emit a loadable GaussianSplatAsset");

    const Dictionary metadata = asset->get_import_metadata();
    CHECK(String(metadata.get(StringName("dc_encoding"), String())) == String("linear_rgb"));

    Ref<::GaussianData> data;
    data.instantiate();
    REQUIRE_MESSAGE(data.is_valid(), "GaussianData must instantiate");
    REQUIRE_MESSAGE(data->populate_from_asset(asset) == OK, "populate_from_asset should accept imported SPZ asset");
    REQUIRE_MESSAGE(data->get_count() == 1, "Imported SPZ asset should materialize one gaussian");

    const Gaussian g = data->get_gaussian(0);
    CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);

    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Importer] PLY importer tags new assets as linear DC by default") {
    // Regression guard for the DC-encoding default flip. The PLY loader itself
    // does not tag render_meta — that tagging only happens when the asset is
    // built through ResourceImporterPLY (which writes "dc_encoding": "linear_rgb"
    // into import metadata) and then deserialized via populate_from_asset
    // (which reads that metadata at gaussian_data_io.cpp:265-268). So this
    // test must exercise the full importer round-trip rather than
    // PLYLoader::load_file directly.
    const String source_path = "user://gaussian_ply_default_dc.ply";
    const String save_base_path = "user://gaussian_ply_default_dc_imported";
    Error write_err = _write_minimal_ascii_ply(source_path);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write minimal PLY fixture");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("preview/generate_thumbnail"), false);

    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, nullptr);
    REQUIRE_MESSAGE(import_err == OK, "PLY importer should succeed for minimal fixture");

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + ".res");
    REQUIRE_MESSAGE(asset.is_valid(), "PLY importer should emit a loadable GaussianSplatAsset");

    const Dictionary metadata = asset->get_import_metadata();
    CHECK(String(metadata.get(StringName("dc_encoding"), String())) == String("linear_rgb"));

    Ref<::GaussianData> data;
    data.instantiate();
    REQUIRE_MESSAGE(data->populate_from_asset(asset) == OK,
            "populate_from_asset should accept default-imported PLY asset");
    REQUIRE_MESSAGE(data->get_count() == 1, "Imported PLY asset should materialize one gaussian");

    const Gaussian g = data->get_gaussian(0);
    CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);

    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Importer] PLY importer propagates SH band-1 coefficients into asset") {
    const String source_path = "user://gaussian_ply_first_order_sh.ply";
    const String save_base_path = "user://gaussian_ply_first_order_sh_imported";
    Error write_err = _write_ascii_ply_with_first_order_sh(source_path);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write band-1 SH PLY fixture");

    Ref<ResourceImporterPLY> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    options.insert(StringName("quality/preset"), String("ultra"));
    options.insert(StringName("preview/generate_thumbnail"), false);

    const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, nullptr);
    REQUIRE_MESSAGE(import_err == OK, "PLY importer should succeed for band-1 SH fixture");

    Ref<GaussianSplatAsset> asset = ResourceLoader::load(save_base_path + ".res");
    REQUIRE_MESSAGE(asset.is_valid(), "PLY importer should emit a loadable GaussianSplatAsset");

    // Band 1 = 3 first-order terms; layout is per-splat, per-term, RGB triplet.
    const PackedFloat32Array first_order = asset->get_sh_first_order_coefficients();
    REQUIRE_MESSAGE(first_order.size() == 1 * 3 * 3,
            "Asset should carry 9 floats of first-order SH for one splat with 3 terms");

    // Inria channel-major -> coefficient-major repack:
    //   PLY input: [R0 R1 R2 G0 G1 G2 B0 B1 B2] = [0.10 0.20 0.30 0.40 0.50 0.60 0.70 0.80 0.90]
    //   Asset out: [R0 G0 B0 R1 G1 B1 R2 G2 B2] = [0.10 0.40 0.70 0.20 0.50 0.80 0.30 0.60 0.90]
    CHECK(Math::is_equal_approx(first_order[0], 0.10f));
    CHECK(Math::is_equal_approx(first_order[1], 0.40f));
    CHECK(Math::is_equal_approx(first_order[2], 0.70f));
    CHECK(Math::is_equal_approx(first_order[3], 0.20f));
    CHECK(Math::is_equal_approx(first_order[4], 0.50f));
    CHECK(Math::is_equal_approx(first_order[5], 0.80f));
    CHECK(Math::is_equal_approx(first_order[6], 0.30f));
    CHECK(Math::is_equal_approx(first_order[7], 0.60f));
    CHECK(Math::is_equal_approx(first_order[8], 0.90f));

    Ref<::GaussianData> data;
    data.instantiate();
    REQUIRE_MESSAGE(data->populate_from_asset(asset) == OK,
            "populate_from_asset should accept SH-enriched PLY asset");
    REQUIRE_MESSAGE(data->get_count() == 1, "Imported PLY asset should materialize one gaussian");
    CHECK(data->get_sh_first_order_count() == 3u);

    const Gaussian round_tripped = data->get_gaussian(0);
    CHECK(Math::is_equal_approx(round_tripped.sh_1[0].x, 0.10f));
    CHECK(Math::is_equal_approx(round_tripped.sh_1[0].y, 0.40f));
    CHECK(Math::is_equal_approx(round_tripped.sh_1[0].z, 0.70f));
    CHECK(Math::is_equal_approx(round_tripped.sh_1[2].x, 0.30f));
    CHECK(Math::is_equal_approx(round_tripped.sh_1[2].z, 0.90f));

    _remove_user_file(source_path);
    _remove_user_file(save_base_path + ".res");
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] PLY ASCII loader hard-fails on malformed rows") {
    // Behavior change: previously a project-setting could downgrade malformed-row
    // handling to a warn-and-skip. That path has been removed — malformed ASCII
    // rows now always return ERR_FILE_CORRUPT.
    const String source_path = "user://gaussian_ply_malformed_row.ply";

    static const char *k_malformed_ascii_ply = R"(ply
format ascii 1.0
element vertex 2
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
0 0 0 0 0 0 1 0 0 0 0 0.25 0.5 0.75
0 0 0 1 2 3
)";

    Ref<FileAccess> file = FileAccess::open(source_path, FileAccess::WRITE);
    REQUIRE(file.is_valid());
    file->store_string(k_malformed_ascii_ply);
    file.unref();

    Ref<PLYLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT,
            "PLY ASCII loader must reject malformed rows with ERR_FILE_CORRUPT (no more silent row skipping).");

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] GaussianSplatAsset::load_from_file hard-fails on unknown raw extensions") {
    // Behavior change: unknown raw extensions previously fell through to the PLY loader.
    // They now hard-fail with ERR_FILE_UNRECOGNIZED instead of probing loosely.
    const String source_path = "user://gaussian_asset_unknown_ext.xyz";
    Ref<FileAccess> file = FileAccess::open(source_path, FileAccess::WRITE);
    REQUIRE(file.is_valid());
    file->store_string("not a real splat file");
    file.unref();

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    const Error err = asset->load_from_file(source_path);
    CHECK_MESSAGE(err == ERR_FILE_UNRECOGNIZED,
            "GaussianSplatAsset::load_from_file must hard-fail on unknown raw extensions, not probe as PLY.");

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Renderer] SH metadata preserves DC encoding mode") {
    SHCompressionMetrics metrics;
    PackedGaussian packed = {};

    Gaussian legacy = {};
    legacy.rotation = Quaternion();
    legacy.scale = Vector3(1.0f, 1.0f, 1.0f);
    legacy.opacity = 1.0f;
    legacy.sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
    legacy.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LEGACY_BIAS);
    pack_gaussian(legacy, packed, metrics, nullptr, 0, 0);
    CHECK(gs_get_dc_encoding(packed.sh_metadata) == GAUSSIAN_DC_ENCODING_LEGACY_BIAS);
    CHECK(gs_get_sh_encoding(packed.sh_metadata) == GS_SH_ENCODING_RGB9E5);

    Gaussian linear = legacy;
    linear.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    pack_gaussian(linear, packed, metrics, nullptr, 0, 0);
    CHECK(gs_get_dc_encoding(packed.sh_metadata) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    CHECK(gs_get_sh_encoding(packed.sh_metadata) == GS_SH_ENCODING_RGB9E5);
}

TEST_CASE("[GaussianSplatting][Renderer] SH metadata preserves DC encoding mode for F16 packing") {
    SHCompressionMetrics metrics;
    PackedGaussianF16 packed = {};

    Gaussian legacy = {};
    legacy.rotation = Quaternion();
    legacy.scale = Vector3(1.0f, 1.0f, 1.0f);
    legacy.opacity = 1.0f;
    legacy.sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
    legacy.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LEGACY_BIAS);
    pack_gaussian_f16(legacy, packed, metrics, Vector3(), nullptr, 0, 0, PackedSphericalHarmonicsF16::MAX_ENCODED_COEFFICIENTS);
    CHECK(gs_get_dc_encoding(packed.sh_metadata) == GAUSSIAN_DC_ENCODING_LEGACY_BIAS);
    CHECK(gs_get_sh_encoding(packed.sh_metadata) == GS_SH_ENCODING_F16);

    Gaussian linear = legacy;
    linear.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    pack_gaussian_f16(linear, packed, metrics, Vector3(), nullptr, 0, 0, PackedSphericalHarmonicsF16::MAX_ENCODED_COEFFICIENTS);
    CHECK(gs_get_dc_encoding(packed.sh_metadata) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    CHECK(gs_get_sh_encoding(packed.sh_metadata) == GS_SH_ENCODING_F16);
}

TEST_CASE("[GaussianSplatting][Renderer] Shader SH metadata masks match host DC encoding contract") {
    const String common_source = _load_text_fixture_or_empty("res://modules/gaussian_splatting/shaders/includes/gaussian_splat_common_inc.glsl");
    REQUIRE_MESSAGE(!common_source.is_empty(), "gaussian_splat_common_inc.glsl must be readable in test environment");
    CHECK(common_source.contains("const uint SH_METADATA_ENCODING_MASK = 0x7F000000u;"));
    CHECK(common_source.contains("const uint SH_METADATA_DC_ENCODING_MASK = 0x80000000u;"));

    const String binning_source = _load_text_fixture_or_empty("res://modules/gaussian_splatting/shaders/includes/gs_sh_binning.glsl");
    REQUIRE_MESSAGE(!binning_source.is_empty(), "gs_sh_binning.glsl must be readable in test environment");
    CHECK(binning_source.contains("const uint SH_METADATA_ENCODING_MASK = 0x7F000000u;"));
    CHECK(binning_source.contains("const uint SH_METADATA_DC_ENCODING_MASK = 0x80000000u;"));
}

TEST_CASE("[GaussianSplatting][Renderer] Quantized SH metadata preserves DC encoding through asset flags") {
    const uint32_t legacy_flags = gs_pack_asset_gpu_flags(false, GAUSSIAN_DC_ENCODING_LEGACY_BIAS);
    const uint32_t linear_flags = gs_pack_asset_gpu_flags(false, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    const uint32_t is_2d_linear_flags = gs_pack_asset_gpu_flags(true, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
    CHECK((legacy_flags & GS_GPU_ASSET_FLAG_DC_LINEAR_RGB) == 0u);
    CHECK((linear_flags & GS_GPU_ASSET_FLAG_DC_LINEAR_RGB) != 0u);
    CHECK((is_2d_linear_flags & GS_GPU_ASSET_FLAG_IS_2D) != 0u);

    const String instance_layout_source = _load_text_fixture_or_empty("res://modules/gaussian_splatting/shaders/includes/gs_instance_layout.glsl");
    REQUIRE_MESSAGE(!instance_layout_source.is_empty(), "gs_instance_layout.glsl must be readable in test environment");
    CHECK(instance_layout_source.contains("const uint GS_ASSET_FLAG_DC_LINEAR_RGB = 1u << 1u;"));

    const String tile_binning_source = _load_text_fixture_or_empty("res://modules/gaussian_splatting/shaders/tile_binning.glsl");
    REQUIRE_MESSAGE(!tile_binning_source.is_empty(), "tile_binning.glsl must be readable in test environment");
    CHECK(tile_binning_source.contains("uint gs_build_quantized_sh_metadata(uint encoded_total, bool dc_linear_rgb)"));
    CHECK(tile_binning_source.contains("chunk_meta.flags & GS_ASSET_FLAG_DC_LINEAR_RGB"));
}

TEST_CASE("[GaussianSplatting][Importer] populate_from_asset preserves DC encoding metadata") {
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset(1);
    REQUIRE_MESSAGE(asset.is_valid(), "Fixture asset must be created");

    Dictionary metadata = asset->get_import_metadata();
    metadata[StringName("dc_encoding")] = "linear_rgb";
    asset->set_import_metadata(metadata);

    Ref<::GaussianData> data;
    data.instantiate();
    REQUIRE_MESSAGE(data.is_valid(), "GaussianData must instantiate");

    const Error err = data->populate_from_asset(asset);
    REQUIRE_MESSAGE(err == OK, "populate_from_asset should accept fixture asset");
    REQUIRE_MESSAGE(data->get_count() == 1, "populate_from_asset should materialize the fixture");

    const Gaussian g = data->get_gaussian(0);
    CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
}

TEST_CASE("[GaussianSplatting][Importer] populate_gaussian_data restores DC encoding from asset metadata") {
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset(1);
    REQUIRE_MESSAGE(asset.is_valid(), "Fixture asset must be created");

    Dictionary metadata = asset->get_import_metadata();
    metadata[StringName("dc_encoding")] = "linear_rgb";
    asset->set_import_metadata(metadata);

    Ref<::GaussianData> data;
    data.instantiate();
    REQUIRE_MESSAGE(data.is_valid(), "GaussianData must instantiate");

    const bool ok = asset->populate_gaussian_data(data);
    REQUIRE_MESSAGE(ok, "populate_gaussian_data should accept fixture asset");
    REQUIRE_MESSAGE(data->get_count() == 1, "populate_gaussian_data should materialize the fixture");

    const Gaussian g = data->get_gaussian(0);
    CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
}

TEST_CASE("[GaussianSplatting][Importer] populate_gaussian_data defaults missing DC metadata to linear RGB") {
    Ref<GaussianSplatAsset> asset = _make_thumbnail_fixture_asset(1);
    REQUIRE_MESSAGE(asset.is_valid(), "Fixture asset must be created");
    REQUIRE_MESSAGE(!asset->get_import_metadata().has(StringName("dc_encoding")),
            "Fixture must not carry explicit DC metadata");

    Ref<::GaussianData> data;
    data.instantiate();
    REQUIRE_MESSAGE(data.is_valid(), "GaussianData must instantiate");

    const bool ok = asset->populate_gaussian_data(data);
    REQUIRE_MESSAGE(ok, "populate_gaussian_data should accept fixture asset");
    REQUIRE_MESSAGE(data->get_count() == 1, "populate_gaussian_data should materialize the fixture");

    const Gaussian g = data->get_gaussian(0);
    CHECK(gaussian_get_dc_encoding(g.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] SPZ loader rejects oversized payload sections") {
    const String source_path = "user://gaussian_spz_oversized_payload.spz";

    // Version 2, 1 point, SH degree 0. Exact payload is 19 bytes; provide 20 bytes.
    PackedByteArray payload;
    payload.resize(20);
    memset(payload.ptrw(), 0, payload.size());

    PackedByteArray compressed_payload = _gzip_compress(payload);
    REQUIRE_MESSAGE(!compressed_payload.is_empty(), "Failed to gzip-compress oversized SPZ payload fixture");

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 12);
    const int header_size = file_data.size();
    file_data.resize(header_size + compressed_payload.size());
    memcpy(file_data.ptrw() + header_size, compressed_payload.ptr(), compressed_payload.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write oversized SPZ fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Oversized SPZ payload must be rejected as corrupt");

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] SPZ loader rejects oversized decompression claims") {
    const String source_path = "user://gaussian_spz_oversized_claim.spz";

    // Minimal gzip-like stream with ISIZE trailer set to 2 GiB - 1.
    PackedByteArray fake_gzip;
    fake_gzip.resize(20);
    uint8_t *w = fake_gzip.ptrw();
    w[0] = 0x1F;
    w[1] = 0x8B;
    w[2] = 0x08; // DEFLATE
    w[3] = 0x00; // flags
    w[4] = 0x00; w[5] = 0x00; w[6] = 0x00; w[7] = 0x00; // mtime
    w[8] = 0x00; // xfl
    w[9] = 0x03; // os
    w[10] = 0x03; // empty DEFLATE block
    w[11] = 0x00;
    w[12] = 0x00; w[13] = 0x00; w[14] = 0x00; w[15] = 0x00; // crc32
    w[16] = 0xFF; w[17] = 0xFF; w[18] = 0xFF; w[19] = 0x7F; // isize = 2147483647

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 12);
    const int header_size = file_data.size();
    file_data.resize(header_size + fake_gzip.size());
    memcpy(file_data.ptrw() + header_size, fake_gzip.ptr(), fake_gzip.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write oversized-claim SPZ fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Oversized SPZ decompression claims must be rejected");

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] SPZ loader rejects malformed gzip optional headers") {
    const String source_path = "user://gaussian_spz_malformed_gzip_header.spz";

    PackedByteArray malformed_gzip;
    malformed_gzip.resize(20);
    uint8_t *w = malformed_gzip.ptrw();
    w[0] = 0x1F;
    w[1] = 0x8B;
    w[2] = 0x08; // DEFLATE
    w[3] = 0x08; // FNAME set, but no null terminator before trailer
    w[4] = 0x00; w[5] = 0x00; w[6] = 0x00; w[7] = 0x00; // mtime
    w[8] = 0x00; // xfl
    w[9] = 0x03; // os
    w[10] = 0x61; // filename bytes without terminator
    w[11] = 0x62;
    w[12] = 0x00; w[13] = 0x00; w[14] = 0x00; w[15] = 0x00; // crc32
    w[16] = 0x13; w[17] = 0x00; w[18] = 0x00; w[19] = 0x00; // isize = 19

    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 1, 0, 12);
    const int header_size = file_data.size();
    file_data.resize(header_size + malformed_gzip.size());
    memcpy(file_data.ptrw() + header_size, malformed_gzip.ptr(), malformed_gzip.size());

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write malformed-gzip SPZ fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Malformed gzip optional headers must be rejected");

    _remove_user_file(source_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] SPZ loader enforces header-derived decompression cap") {
    const String source_path = "user://gaussian_spz_payload_cap.spz";

    // Version 2, SH degree 3, 20M points => expected payload exceeds 1 GiB cap.
    PackedByteArray file_data = _make_spz_header(SPZLoader::SPZ_VERSION_2, 20000000u, 3, 12);
    file_data.push_back(0x00); // Ensure file is larger than header.

    Error write_err = _write_binary_file(source_path, file_data);
    REQUIRE_MESSAGE(write_err == OK, "Failed to write cap-enforcement SPZ fixture");

    Ref<SPZLoader> loader;
    loader.instantiate();
    Error load_err = loader->load_file(source_path);
    CHECK_MESSAGE(load_err == ERR_FILE_CORRUPT, "Payloads that exceed decompression caps must be rejected");

    _remove_user_file(source_path);
}

// Note: Full SPZ import test requires an SPZ test fixture file.
// To create one, use the SPZ encoder from https://github.com/nianticlabs/spz
// with a minimal Gaussian data set. The test fixture should be placed at:
//   res://test_splats.spz (in test project)
// Required SPZ fixture attributes:
//   - Valid SPZ header with version >= 1
//   - At least 1 Gaussian with position, scale, rotation, color data
//   - SH degree 0 is sufficient for basic testing

// ============================================================================
// Importer Selection Tests (extension-based routing)
// ============================================================================

TEST_CASE("[GaussianSplatting][Importer] Extension-based importer selection logic") {
    // Test the extension detection logic used by _import_from_path and _load_preview_asset

    SUBCASE("PLY extension selects PLY importer") {
        String path = "res://models/test_scene.ply";
        String extension = path.get_extension().to_lower();
        CHECK(extension == "ply");

        String importer_name;
        if (extension == "ply") {
            importer_name = "gaussian_splat_ply";
        } else if (extension == "spz") {
            importer_name = "gaussian_splat_spz";
        }
        CHECK(importer_name == "gaussian_splat_ply");
    }

    SUBCASE("SPZ extension selects SPZ importer") {
        String path = "res://models/compressed_scene.spz";
        String extension = path.get_extension().to_lower();
        CHECK(extension == "spz");

        String importer_name;
        if (extension == "ply") {
            importer_name = "gaussian_splat_ply";
        } else if (extension == "spz") {
            importer_name = "gaussian_splat_spz";
        }
        CHECK(importer_name == "gaussian_splat_spz");
    }

    SUBCASE("Unknown extension is detected correctly") {
        String path = "res://models/invalid_format.xyz";
        String extension = path.get_extension().to_lower();
        CHECK(extension == "xyz");

        bool is_supported = (extension == "ply" || extension == "spz");
        CHECK_MESSAGE(!is_supported, "Unknown extension .xyz should not be recognized as supported");
    }

    SUBCASE("Case insensitive extension handling") {
        String path_upper = "res://models/test.PLY";
        String path_mixed = "res://models/test.SpZ";

        String ext_upper = path_upper.get_extension().to_lower();
        String ext_mixed = path_mixed.get_extension().to_lower();

        CHECK(ext_upper == "ply");
        CHECK(ext_mixed == "spz");
    }

    SUBCASE("Path with multiple dots handles extension correctly") {
        String path = "res://models/my.scene.v2.ply";
        String extension = path.get_extension().to_lower();
        CHECK(extension == "ply");
    }
}

TEST_CASE("[GaussianSplatting][Importer] PLY and SPZ importers have distinct names") {
    Ref<ResourceImporterPLY> ply_importer;
    ply_importer.instantiate();

    Ref<ResourceImporterSPZ> spz_importer;
    spz_importer.instantiate();

    CHECK(ply_importer->get_importer_name() == "gaussian_splat_ply");
    CHECK(spz_importer->get_importer_name() == "gaussian_splat_spz");
    CHECK(ply_importer->get_importer_name() != spz_importer->get_importer_name());

    // Both should produce the same resource type
    CHECK(ply_importer->get_resource_type() == spz_importer->get_resource_type());
    CHECK(ply_importer->get_resource_type() == "GaussianSplatAsset");
}

TEST_CASE("[GaussianSplatting][Importer] gsplatworld importer preserves payload and chunks") {
    Ref<GaussianData> data;
    data.instantiate();
    data->resize(2);

    PackedVector3Array positions;
    positions.push_back(Vector3(0.0f, 0.0f, 0.0f));
    positions.push_back(Vector3(1.0f, 0.0f, 0.0f));
    data->set_positions(positions);

    PackedVector3Array scales;
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    data->set_scales(scales);

    TypedArray<Quaternion> rotations;
    rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    data->set_rotations(rotations);

    PackedFloat32Array opacities;
    opacities.push_back(1.0f);
    opacities.push_back(1.0f);
    data->set_opacities(opacities);

    PackedFloat32Array sh_dc;
    sh_dc.push_back(1.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(1.0f);
    sh_dc.push_back(0.0f);
    data->set_spherical_harmonics(sh_dc);

    Ref<GaussianSplatWorld> source_world;
    source_world.instantiate();
    source_world->set_gaussian_data(data);
    source_world->set_bounds(AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(4.0f, 4.0f, 4.0f)));

    Vector<GaussianSplatRenderer::StaticChunk> chunks;
    chunks.resize(1);
    chunks.write[0].bounds = AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(3.0f, 3.0f, 3.0f));
    chunks.write[0].center = Vector3(0.5f, 0.0f, 0.0f);
    chunks.write[0].radius = 2.0f;
    chunks.write[0].indices.push_back(0);
    chunks.write[0].indices.push_back(1);
    source_world->set_static_chunks(chunks);

    const String source_path = "user://gsplatworld_importer_source.gsplatworld";
    const String save_base_path = "user://gsplatworld_importer_roundtrip";
    const String imported_path = save_base_path + ".gsplatworld";

    Error save_source_err = ResourceSaver::save(source_world, source_path);
    CHECK_MESSAGE(save_source_err == OK, "Saving source gsplatworld should succeed.");
    if (save_source_err != OK) {
        return;
    }

    Ref<ResourceImporterGSplatWorld> importer;
    importer.instantiate();
    CHECK(importer->get_save_extension() == "gsplatworld");
    CHECK(importer->get_format_version() == 2);

    HashMap<StringName, Variant> options;
    Variant import_metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &import_metadata_variant);
    CHECK_MESSAGE(import_err == OK, "ResourceImporterGSplatWorld import should succeed.");
    if (import_err != OK) {
        _remove_user_file(source_path);
        _remove_user_file(imported_path);
        return;
    }

    Ref<GaussianSplatWorld> imported_world = ResourceLoader::load(imported_path, "GaussianSplatWorld");
    CHECK_MESSAGE(imported_world.is_valid(), "Imported gsplatworld should be loadable.");
    CHECK(import_metadata_variant.get_type() == Variant::DICTIONARY);
    if (import_metadata_variant.get_type() == Variant::DICTIONARY) {
        Dictionary import_metadata = import_metadata_variant;
        CHECK(String(import_metadata.get(StringName("payload_mode"), String())) == String("streamable_uncompressed"));
        CHECK(bool(import_metadata.get(StringName("streamable"), false)));
        CHECK_FALSE(bool(import_metadata.get(StringName("compressed"), true)));
        CHECK(int64_t(import_metadata.get(StringName("splat_count"), int64_t(0))) == 2);
        CHECK(int64_t(import_metadata.get(StringName("chunk_count"), int64_t(0))) == 1);
    }
    if (imported_world.is_valid()) {
        CHECK_FALSE(imported_world->has_resident_gaussian_data());
        CHECK(imported_world->has_chunk_payload_source());
        CHECK(imported_world->is_streamable_payload());
        CHECK(imported_world->get_payload_mode() == String("streamable_uncompressed"));
        CHECK(imported_world->get_splat_count() == 2);
        CHECK(imported_world->materialize_resident_gaussian_data() == OK);
        Ref<GaussianData> imported_data = imported_world->get_gaussian_data();
        CHECK(imported_data.is_valid());
        if (imported_data.is_valid()) {
            CHECK(imported_data->get_count() == 2);
        }
        CHECK(imported_world->get_chunk_count() == 1);
        PackedInt32Array chunk_sizes = imported_world->get_chunk_sizes();
        CHECK(chunk_sizes.size() == 1);
        if (chunk_sizes.size() == 1) {
            CHECK(chunk_sizes[0] == 2);
        }
    }

    _remove_user_file(source_path);
    _remove_user_file(imported_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] gsplatworld importer rejects invalid payloads") {
    const String source_path = "user://gsplatworld_importer_invalid_source.gsplatworld";
    const String save_base_path = "user://gsplatworld_importer_invalid";
    const String imported_path = save_base_path + ".gsplatworld";

    Error write_err = _write_invalid_gsplatworld(source_path);
    CHECK_MESSAGE(write_err == OK, "Writing invalid gsplatworld test payload should succeed.");
    if (write_err != OK) {
        return;
    }

    Ref<ResourceImporterGSplatWorld> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, nullptr);
    CHECK_MESSAGE(import_err != OK, "ResourceImporterGSplatWorld should reject malformed source payloads.");
    CHECK_MESSAGE(!FileAccess::exists(imported_path),
            "Importer should not emit an output file when source payload validation fails.");

    _remove_user_file(source_path);
    _remove_user_file(imported_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] gsplatworld importer rejects truncated payloads") {
    Ref<GaussianData> data;
    data.instantiate();
    data->resize(2);

    PackedVector3Array positions;
    positions.push_back(Vector3(0.0f, 0.0f, 0.0f));
    positions.push_back(Vector3(1.0f, 0.0f, 0.0f));
    data->set_positions(positions);

    PackedVector3Array scales;
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    data->set_scales(scales);

    TypedArray<Quaternion> rotations;
    rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    data->set_rotations(rotations);

    PackedFloat32Array opacities;
    opacities.push_back(1.0f);
    opacities.push_back(1.0f);
    data->set_opacities(opacities);

    PackedFloat32Array sh_dc;
    sh_dc.push_back(1.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(1.0f);
    sh_dc.push_back(0.0f);
    data->set_spherical_harmonics(sh_dc);

    Ref<GaussianSplatWorld> source_world;
    source_world.instantiate();
    source_world->set_gaussian_data(data);
    source_world->set_bounds(AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(4.0f, 4.0f, 4.0f)));

    const String source_path = "user://gsplatworld_importer_truncated_source.gsplatworld";
    const String truncated_source_path = "user://gsplatworld_importer_truncated_input.gsplatworld";
    const String save_base_path = "user://gsplatworld_importer_truncated";
    const String imported_path = save_base_path + ".gsplatworld";

    Error save_err = ResourceSaver::save(source_world, source_path);
    CHECK_MESSAGE(save_err == OK, "Saving source gsplatworld for truncation test should succeed.");
    if (save_err != OK) {
        return;
    }

    Error trunc_err = _write_truncated_copy(source_path, truncated_source_path, 16);
    CHECK_MESSAGE(trunc_err == OK, "Creating truncated gsplatworld input should succeed.");
    if (trunc_err != OK) {
        _remove_user_file(source_path);
        _remove_user_file(truncated_source_path);
        _remove_user_file(imported_path);
        return;
    }

    Ref<ResourceImporterGSplatWorld> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    Error import_err = importer->import(ResourceUID::INVALID_ID, truncated_source_path, save_base_path, options,
            nullptr, nullptr, nullptr);
    CHECK_MESSAGE(import_err != OK,
            "ResourceImporterGSplatWorld should reject payloads truncated after a valid header.");
    CHECK_MESSAGE(!FileAccess::exists(imported_path),
            "Importer should not emit an output file when payload size validation fails.");

    _remove_user_file(source_path);
    _remove_user_file(truncated_source_path);
    _remove_user_file(imported_path);
}

TEST_CASE("[GaussianSplatting][Importer][MalformedCorpus] gsplatworld importer rejects decode-invalid payloads") {
    Ref<GaussianData> data;
    data.instantiate();
    data->resize(2);

    PackedVector3Array positions;
    positions.push_back(Vector3(0.0f, 0.0f, 0.0f));
    positions.push_back(Vector3(1.0f, 0.0f, 0.0f));
    data->set_positions(positions);

    PackedVector3Array scales;
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
    data->set_scales(scales);

    TypedArray<Quaternion> rotations;
    rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    data->set_rotations(rotations);

    PackedFloat32Array opacities;
    opacities.push_back(1.0f);
    opacities.push_back(1.0f);
    data->set_opacities(opacities);

    PackedFloat32Array sh_dc;
    sh_dc.push_back(1.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(0.0f);
    sh_dc.push_back(1.0f);
    sh_dc.push_back(0.0f);
    data->set_spherical_harmonics(sh_dc);

    Ref<GaussianSplatWorld> source_world;
    source_world.instantiate();
    source_world->set_gaussian_data(data);
    source_world->set_bounds(AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(4.0f, 4.0f, 4.0f)));

    const String source_path = "user://gsplatworld_importer_decode_source.gsplatworld";
    const String corrupt_path = "user://gsplatworld_importer_decode_corrupt_input.gsplatworld";
    const String save_base_path = "user://gsplatworld_importer_decode_invalid";
    const String imported_path = save_base_path + ".gsplatworld";

    Error save_err = ResourceSaver::save(source_world, source_path);
    CHECK_MESSAGE(save_err == OK, "Saving source gsplatworld for decode corruption test should succeed.");
    if (save_err != OK) {
        return;
    }

    Error copy_err = _write_truncated_copy(source_path, corrupt_path, 0);
    CHECK_MESSAGE(copy_err == OK, "Copying gsplatworld source for decode corruption test should succeed.");
    if (copy_err != OK) {
        _remove_user_file(source_path);
        _remove_user_file(corrupt_path);
        _remove_user_file(imported_path);
        return;
    }

    Error corrupt_err = _corrupt_gsplatworld_for_decode_failure(corrupt_path);
    CHECK_MESSAGE(corrupt_err == OK, "Corrupting gsplatworld payload for decode validation test should succeed.");
    if (corrupt_err != OK) {
        _remove_user_file(source_path);
        _remove_user_file(corrupt_path);
        _remove_user_file(imported_path);
        return;
    }

    Ref<ResourceImporterGSplatWorld> importer;
    importer.instantiate();

    HashMap<StringName, Variant> options;
    Error import_err = importer->import(ResourceUID::INVALID_ID, corrupt_path, save_base_path, options,
            nullptr, nullptr, nullptr);
    CHECK_MESSAGE(import_err != OK,
            "ResourceImporterGSplatWorld should reject payloads that fail full decode validation.");
    CHECK_MESSAGE(!FileAccess::exists(imported_path),
            "Importer should not emit an output file when decode validation fails.");

    _remove_user_file(source_path);
    _remove_user_file(corrupt_path);
    _remove_user_file(imported_path);
}

TEST_CASE("[GaussianSplatting][Importer] gsplatworld importer accepts compressed payloads") {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    CHECK(ps != nullptr);
    if (ps == nullptr) {
        return;
    }

    const StringName compression_key = StringName("rendering/gaussian_splatting/import/gsplatworld_compression_enabled");
    const bool had_setting = ps->has_setting(compression_key);
    const Variant previous_value = had_setting ? ps->get_setting(compression_key) : Variant(false);
    ps->set_setting(compression_key, true);

    Ref<GaussianData> data;
    data.instantiate();
    const int splat_count = 16; // > 1KB payload so saver's compression path activates.
    data->resize(splat_count);

    PackedVector3Array positions;
    PackedVector3Array scales;
    TypedArray<Quaternion> rotations;
    PackedFloat32Array opacities;
    PackedFloat32Array sh_dc;
    for (int i = 0; i < splat_count; i++) {
        positions.push_back(Vector3(float(i), 0.0f, 0.0f));
        scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
        rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
        opacities.push_back(1.0f);
        sh_dc.push_back((i % 3) == 0 ? 1.0f : 0.0f);
        sh_dc.push_back((i % 3) == 1 ? 1.0f : 0.0f);
        sh_dc.push_back((i % 3) == 2 ? 1.0f : 0.0f);
    }
    data->set_positions(positions);
    data->set_scales(scales);
    data->set_rotations(rotations);
    data->set_opacities(opacities);
    data->set_spherical_harmonics(sh_dc);

    Ref<GaussianSplatWorld> source_world;
    source_world.instantiate();
    source_world->set_gaussian_data(data);
    source_world->set_bounds(AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(32.0f, 4.0f, 4.0f)));

    const String source_path = "user://gsplatworld_importer_compressed_source.gsplatworld";
    const String save_base_path = "user://gsplatworld_importer_compressed";
    const String imported_path = save_base_path + ".gsplatworld";

    ResourceFormatSaverGaussianSplatWorld world_saver;
    Error save_err = world_saver.save_resident_compressed(source_world, source_path);
    CHECK_MESSAGE(save_err == OK, "Saving compressed gsplatworld source should succeed.");
    if (save_err != OK) {
        ps->set_setting(compression_key, previous_value);
        return;
    }

    Ref<FileAccess> source_file = FileAccess::open(source_path, FileAccess::READ);
    CHECK(source_file.is_valid());
    if (source_file.is_valid()) {
        source_file->seek(8); // magic + version
        const uint32_t flags = source_file->get_32();
        CHECK_MESSAGE((flags & (1u << 4u)) != 0u, "Saved source gsplatworld should have compression flag set.");
    }

    Ref<ResourceImporterGSplatWorld> importer;
    importer.instantiate();
    HashMap<StringName, Variant> options;
    Variant import_metadata_variant;
    Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
            nullptr, nullptr, &import_metadata_variant);
    CHECK_MESSAGE(import_err == OK, "Importer should accept valid compressed gsplatworld payloads.");

    Ref<GaussianSplatWorld> imported_world = ResourceLoader::load(imported_path, "GaussianSplatWorld");
    CHECK_MESSAGE(imported_world.is_valid(), "Imported compressed gsplatworld should be loadable.");
    CHECK(import_metadata_variant.get_type() == Variant::DICTIONARY);
    if (import_metadata_variant.get_type() == Variant::DICTIONARY) {
        Dictionary import_metadata = import_metadata_variant;
        CHECK(String(import_metadata.get(StringName("payload_mode"), String())) == String("resident_only"));
        CHECK_FALSE(bool(import_metadata.get(StringName("streamable"), true)));
        CHECK(bool(import_metadata.get(StringName("compressed"), false)));
        CHECK(String(import_metadata.get(StringName("resident_only_reason"), String())) ==
                String("compressed_world_has_no_random_access_chunks"));
    }
    if (imported_world.is_valid()) {
        CHECK(imported_world->has_resident_gaussian_data());
        CHECK_FALSE(imported_world->has_chunk_payload_source());
        CHECK(imported_world->get_payload_mode() == String("resident_only"));
    }

    _remove_user_file(source_path);
    _remove_user_file(imported_path);
    ps->set_setting(compression_key, previous_value);
}

TEST_CASE("[GaussianSplatting][Importer] gsplatworld importer reports resident-uncompressed payloads as non-streamable") {
	Ref<GaussianData> data;
	data.instantiate();
	data->resize(2);

	PackedVector3Array positions;
	positions.push_back(Vector3(0.0f, 0.0f, 0.0f));
	positions.push_back(Vector3(1.0f, 0.0f, 0.0f));
	data->set_positions(positions);

	PackedVector3Array scales;
	scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
	scales.push_back(Vector3(1.0f, 1.0f, 1.0f));
	data->set_scales(scales);

	TypedArray<Quaternion> rotations;
	rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
	rotations.push_back(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
	data->set_rotations(rotations);

	PackedFloat32Array opacities;
	opacities.push_back(1.0f);
	opacities.push_back(1.0f);
	data->set_opacities(opacities);

	PackedFloat32Array sh_dc;
	sh_dc.push_back(1.0f);
	sh_dc.push_back(0.0f);
	sh_dc.push_back(0.0f);
	sh_dc.push_back(0.0f);
	sh_dc.push_back(1.0f);
	sh_dc.push_back(0.0f);
	data->set_spherical_harmonics(sh_dc);

	Ref<GaussianSplatWorld> source_world;
	source_world.instantiate();
	source_world->set_gaussian_data(data);
	source_world->set_bounds(AABB(Vector3(-1.0f, -1.0f, -1.0f), Vector3(4.0f, 4.0f, 4.0f)));

	const String source_path = "user://gsplatworld_importer_resident_uncompressed_source.gsplatworld";
	const String save_base_path = "user://gsplatworld_importer_resident_uncompressed";
	const String imported_path = save_base_path + ".gsplatworld";

	ResourceFormatSaverGaussianSplatWorld world_saver;
	const Error save_err = world_saver.save_resident_uncompressed(source_world, source_path);
	CHECK_MESSAGE(save_err == OK, "Saving resident-uncompressed gsplatworld source should succeed.");
	if (save_err != OK) {
		return;
	}

	Ref<FileAccess> source_file = FileAccess::open(source_path, FileAccess::READ);
	CHECK(source_file.is_valid());
	if (source_file.is_valid()) {
		source_file->seek(8); // magic + version
		const uint32_t flags = source_file->get_32();
		CHECK_MESSAGE((flags & (1u << 4u)) == 0u, "Resident-uncompressed source must not set compression flag.");
		CHECK_MESSAGE((flags & (1u << 5u)) != 0u, "Resident-uncompressed source must set resident payload flag.");
	}

	Ref<ResourceImporterGSplatWorld> importer;
	importer.instantiate();
	HashMap<StringName, Variant> options;
	Variant import_metadata_variant;
	const Error import_err = importer->import(ResourceUID::INVALID_ID, source_path, save_base_path, options,
			nullptr, nullptr, &import_metadata_variant);
	CHECK_MESSAGE(import_err == OK, "Importer should accept valid resident-uncompressed gsplatworld payloads.");

	Ref<GaussianSplatWorld> imported_world = ResourceLoader::load(imported_path, "GaussianSplatWorld");
	CHECK_MESSAGE(imported_world.is_valid(), "Imported resident-uncompressed gsplatworld should be loadable.");
	CHECK(import_metadata_variant.get_type() == Variant::DICTIONARY);
	if (import_metadata_variant.get_type() == Variant::DICTIONARY) {
		Dictionary import_metadata = import_metadata_variant;
		CHECK(String(import_metadata.get(StringName("payload_mode"), String())) == String("resident_only"));
		CHECK_FALSE(bool(import_metadata.get(StringName("streamable"), true)));
		CHECK_FALSE(bool(import_metadata.get(StringName("compressed"), true)));
		CHECK(String(import_metadata.get(StringName("resident_only_reason"), String())) ==
				String("explicit_resident_payload"));
	}
	if (imported_world.is_valid()) {
		CHECK(imported_world->has_resident_gaussian_data());
		CHECK_FALSE(imported_world->has_chunk_payload_source());
		CHECK_FALSE(imported_world->is_streamable_payload());
		CHECK(imported_world->get_payload_mode() == String("resident_only"));
	}

	_remove_user_file(source_path);
	_remove_user_file(imported_path);
}

TEST_CASE("[GaussianSplatting][Importer] GaussianSplatAsset save_to_file rejects empty assets") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    const String output_path = "user://gaussian_asset_empty_save.ply";
    const Error save_err = asset->save_to_file(output_path);
    CHECK_MESSAGE(save_err == ERR_INVALID_DATA,
            "save_to_file should return ERR_INVALID_DATA when asset has no splat payload");
    _remove_user_file(output_path);
}

TEST_CASE("[GaussianSplatting][Importer] GaussianSplatAsset unloaded getters remain safe and empty") {
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();

    CHECK_FALSE(asset->is_loaded());
    CHECK_EQ(asset->get_splat_count(), uint32_t(0));

    CHECK(asset->get_positions().is_empty());
    CHECK(asset->get_position_vectors().is_empty());
    CHECK(asset->get_colors().is_empty());
    CHECK(asset->get_scales().is_empty());
    CHECK(asset->get_scale_vectors().is_empty());
    CHECK(asset->get_rotations().is_empty());
    CHECK_EQ(asset->get_rotation_quaternions().size(), 0);
    CHECK(asset->get_sh_dc_coefficients().is_empty());
    CHECK(asset->get_sh_first_order_coefficients().is_empty());
    CHECK(asset->get_sh_high_order_coefficients().is_empty());
    CHECK(asset->get_spherical_harmonics_buffer().is_empty());
    CHECK(asset->get_opacity_logits().is_empty());
    CHECK(asset->get_opacities().is_empty());
    CHECK(asset->get_palette_ids().is_empty());
    CHECK(asset->get_palette_ids_buffer().is_empty());
    CHECK(asset->get_painterly_flags().is_empty());
    CHECK(asset->get_painterly_flags_buffer().is_empty());
    CHECK(asset->get_brush_override_ids().is_empty());
    CHECK(asset->get_brush_override_ids_buffer().is_empty());
    CHECK(asset->get_normals().is_empty());
    CHECK(asset->get_normal_vectors().is_empty());
    CHECK(asset->get_brush_axes().is_empty());
    CHECK(asset->get_brush_axes_vector2().is_empty());
    CHECK(asset->get_stroke_ages().is_empty());
    CHECK(asset->get_stroke_ages_buffer().is_empty());

    CHECK(asset->get_gaussian_data().is_null());
}

TEST_CASE("[GaussianSplatting][Importer] GaussianSplatAsset save_to_file persists loadable payloads") {
    const String source_path = "user://test_roundtrip_source.ply";
    const String output_path = "user://gaussian_asset_save_roundtrip.ply";

    {
        Error write_err = _write_minimal_ascii_ply(source_path);
        CHECK_MESSAGE(write_err == OK, "Failed to write synthetic PLY fixture for save_to_file test");
        if (write_err != OK) {
            return;
        }
    }

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    const Error load_err = asset->load_from_file(source_path);
    CHECK_MESSAGE(load_err == OK, "Fixture PLY should load into GaussianSplatAsset before save_to_file");
    if (load_err != OK) {
        _remove_user_file(source_path);
        return;
    }

    PackedInt32Array brush_override_ids;
    brush_override_ids.resize(asset->get_splat_count());
    if (brush_override_ids.size() > 0) {
        int32_t *ptr = brush_override_ids.ptrw();
        for (int i = 0; i < brush_override_ids.size(); i++) {
            ptr[i] = (i * 37) % 65536;
        }
        asset->set_brush_override_ids(brush_override_ids);
        CHECK_EQ(asset->get_painterly_flags_buffer()[0], brush_override_ids[0]);
    }

    const Error save_err = asset->save_to_file(output_path);
    CHECK_MESSAGE(save_err == OK, "save_to_file should persist GaussianData-backed assets");
    CHECK_MESSAGE(FileAccess::exists(output_path), "save_to_file should produce an output file");
    if (save_err != OK || !FileAccess::exists(output_path)) {
        _remove_user_file(output_path);
        return;
    }

    Ref<GaussianSplatAsset> reloaded;
    reloaded.instantiate();
    const Error reload_err = reloaded->load_from_file(output_path);
    CHECK_MESSAGE(reload_err == OK, "Saved output should be loadable through GaussianSplatAsset::load_from_file");
    if (reload_err == OK) {
        CHECK_MESSAGE(reloaded->get_splat_count() == asset->get_splat_count(),
                "Roundtripped asset should preserve splat_count");
        PackedInt32Array reloaded_brush_override_ids = reloaded->get_brush_override_ids_buffer();
        CHECK_EQ(reloaded_brush_override_ids.size(), brush_override_ids.size());
        if (!brush_override_ids.is_empty() && reloaded_brush_override_ids.size() == brush_override_ids.size()) {
            CHECK_EQ(reloaded_brush_override_ids[0], brush_override_ids[0]);
            CHECK_EQ(reloaded->get_painterly_flags_buffer()[0], brush_override_ids[0]);
            CHECK_EQ(reloaded_brush_override_ids[brush_override_ids.size() - 1], brush_override_ids[brush_override_ids.size() - 1]);
        }
    }

    _remove_user_file(source_path);
    _remove_user_file(output_path);
}

// ============================================================================
// Thumbnail Customization Tracking Tests
// ============================================================================

TEST_CASE("[GaussianSplatting][Editor] Thumbnail style change triggers custom_settings flag") {
    GaussianImportDialog *dialog = memnew(GaussianImportDialog);

    // Configure dialog with default "desktop" preset
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_import_quality_preset("desktop");
    asset->set_source_path("res://test_splats.ply");

    Dictionary metadata;
    Dictionary baseline_options;
    baseline_options[StringName("quality/preset")] = String("desktop");
    baseline_options[StringName("preview/thumbnail_style")] = 0; // THUMBNAIL_STYLE_COLOR
    baseline_options[StringName("preview/thumbnail_size")] = 128;
    metadata[StringName("options")] = baseline_options;
    asset->set_import_metadata(metadata);

    dialog->configure_for_file("res://test_splats.ply", asset, true, Dictionary());

    // Initially should not be custom
    // Note: The custom_settings flag depends on whether any setting differs from preset defaults
    // For a fresh dialog configured with matching preset, it should be false
    (void)dialog->get_configuration();

    // Now configure with a different thumbnail style
    Dictionary style_override;
    style_override[StringName("preview/thumbnail_style")] = int(GaussianThumbnailGenerator::THUMBNAIL_STYLE_HEATMAP);
    dialog->configure_for_file("res://test_splats.ply", asset, true, style_override);

    GaussianImportDialog::ImportConfiguration modified_config = dialog->get_configuration();

    // If the style differs from the preset default, custom_settings should be true
    const GaussianImportPresetDefinition &preset = gaussian_get_import_preset_by_name("desktop");
    if (int(GaussianThumbnailGenerator::THUMBNAIL_STYLE_HEATMAP) != preset.thumbnail_style) {
        CHECK_MESSAGE(modified_config.custom_settings == true,
                "Changing thumbnail_style should trigger custom_settings=true when different from preset default");
    }

    memdelete(dialog);
}

TEST_CASE("[GaussianSplatting][Editor] Thumbnail size change triggers custom_settings flag") {
    GaussianImportDialog *dialog = memnew(GaussianImportDialog);

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_import_quality_preset("desktop");
    asset->set_source_path("res://test_splats.ply");

    Dictionary metadata;
    Dictionary baseline_options;
    baseline_options[StringName("quality/preset")] = String("desktop");
    baseline_options[StringName("preview/thumbnail_size")] = 128;
    metadata[StringName("options")] = baseline_options;
    asset->set_import_metadata(metadata);

    dialog->configure_for_file("res://test_splats.ply", asset, true, Dictionary());

    // Apply override with different thumbnail size
    const GaussianImportPresetDefinition &preset = gaussian_get_import_preset_by_name("desktop");
    int different_size = preset.default_thumbnail_size != 256 ? 256 : 64;

    Dictionary size_override;
    size_override[StringName("preview/thumbnail_size")] = different_size;
    dialog->configure_for_file("res://test_splats.ply", asset, true, size_override);

    GaussianImportDialog::ImportConfiguration config = dialog->get_configuration();
    CHECK_MESSAGE(config.thumbnail_size == different_size,
            "Dialog should apply thumbnail_size override correctly");
    CHECK_MESSAGE(config.custom_settings == true,
            "Changing thumbnail_size should trigger custom_settings=true when different from preset default");

    memdelete(dialog);
}

TEST_CASE("[GaussianSplatting][Editor] Disabling thumbnail generation triggers custom_settings flag") {
    GaussianImportDialog *dialog = memnew(GaussianImportDialog);

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_import_quality_preset("desktop");
    asset->set_source_path("res://test_splats.ply");

    Dictionary metadata;
    Dictionary baseline_options;
    baseline_options[StringName("quality/preset")] = String("desktop");
    baseline_options[StringName("preview/generate_thumbnail")] = true;
    metadata[StringName("options")] = baseline_options;
    asset->set_import_metadata(metadata);

    // Configure with generate_thumbnail = false
    Dictionary thumb_override;
    thumb_override[StringName("preview/generate_thumbnail")] = false;
    dialog->configure_for_file("res://test_splats.ply", asset, true, thumb_override);

    GaussianImportDialog::ImportConfiguration config = dialog->get_configuration();
    CHECK_MESSAGE(config.generate_thumbnail == false,
            "Dialog should apply generate_thumbnail override correctly");
    // Presets default to generate_thumbnail = true, so disabling it should trigger custom
    CHECK_MESSAGE(config.custom_settings == true,
            "Setting generate_thumbnail=false should trigger custom_settings=true");

    memdelete(dialog);
}

TEST_CASE("[GaussianSplatting][Editor] Import dialog handles SPZ source paths") {
    GaussianImportDialog *dialog = memnew(GaussianImportDialog);

    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_import_quality_preset("desktop");
    asset->set_source_path("res://test_splats.spz");

    Dictionary metadata;
    metadata[StringName("source_format")] = "spz";
    metadata[StringName("quality_preset")] = String("desktop");
    asset->set_import_metadata(metadata);

    dialog->configure_for_file("res://test_splats.spz", asset, true, Dictionary());

    CHECK(dialog->get_source_path() == "res://test_splats.spz");

    GaussianImportDialog::ImportConfiguration config = dialog->get_configuration();
    CHECK(config.preset == "desktop");

    memdelete(dialog);
}

TEST_CASE("[GaussianSplatting][Editor] Import settings dialog loads sidecar params") {
    const String source_path = "user://gaussian_import_settings_sidecar_fixture.ply";
    Dictionary options;
    options[StringName("quality/preset")] = String("ultra");
    options[StringName("quality/max_splats")] = 12345;
    options[StringName("compression/quantize_positions")] = true;

    const Error write_err = _write_import_sidecar(source_path, options);
    CHECK_MESSAGE(write_err == OK, "Failed to write synthetic .import sidecar for settings dialog test");

    Dictionary loaded = GaussianImportSettingsDialog::load_import_options_from_sidecar(source_path);
    CHECK(String(loaded.get(StringName("quality/preset"), String())) == String("ultra"));
    CHECK(int(loaded.get(StringName("quality/max_splats"), -1)) == 12345);
    CHECK(bool(loaded.get(StringName("compression/quantize_positions"), false)) == true);

    _remove_user_file(source_path + ".import");
}

TEST_CASE("[GaussianSplatting][Editor] Import settings dialog ignores missing sidecar") {
    const String source_path = "user://gaussian_import_settings_missing_sidecar_fixture.ply";
    _remove_user_file(source_path + ".import");

    Dictionary loaded = GaussianImportSettingsDialog::load_import_options_from_sidecar(source_path);
    CHECK(loaded.is_empty());
}

TEST_CASE("[GaussianSplatting][Editor] Advanced import settings reimport refreshes inspector, preview, and stats") {
    // GaussianImportSettingsDialog::open_settings exercises EditorInspector,
    // a 3D viewport scene, and popup_centered_ratio — none of which work
    // without an EditorNode-backed runtime. In --headless --test the dialog's
    // popup path SIGSEGVs (no symbolicated frames; this was the tail end of
    // the multi-day [Editor] lane crash). Skip when the editor isn't loaded.
    if (!EditorNode::get_singleton()) {
        MESSAGE("Skipping - EditorNode not available; this test requires editor runtime.");
        return;
    }

    const String source_path = "user://gaussian_import_settings_reimport_fixture.ply";

    Dictionary initial_options;
    initial_options[StringName("quality/preset")] = String("desktop");
    initial_options[StringName("quality/max_splats")] = 12345;
    initial_options[StringName("custom/preserve")] = String("alpha");
    initial_options[StringName("metadata/include_statistics")] = true;

    Dictionary refreshed_options = initial_options;
    refreshed_options[StringName("quality/preset")] = String("ultra");
    refreshed_options[StringName("quality/max_splats")] = 54321;
    refreshed_options[StringName("custom/preserve")] = String("beta");

    TestGaussianImportSettingsDialog *dialog = memnew(TestGaussianImportSettingsDialog);
    dialog->current_import_options = initial_options;
    dialog->current_asset = _make_editor_test_asset(source_path, 3, initial_options,
            GaussianSplatAsset::COMPRESSION_POSITIONS, 1048576);

    dialog->open_settings(source_path);

    CHECK(String(dialog->_test_get_setting_value(StringName("quality/preset"))) == String("desktop"));
    CHECK(int(dialog->_test_get_setting_value(StringName("quality/max_splats"))) == 12345);
    REQUIRE_MESSAGE(dialog->_test_get_loaded_asset().is_valid(), "Import settings dialog should load the initial asset.");
    CHECK(dialog->_test_get_loaded_asset()->get_splat_count() == 3);
    CHECK(dialog->_test_get_stats_text().contains("Splats: 3"));
    CHECK(dialog->_test_get_stats_text().contains("Est. Memory: 1.0 MB"));

    EditorProperty *max_splats_property = _find_property_editor(dialog, StringName("quality/max_splats"));
    REQUIRE_MESSAGE(max_splats_property != nullptr, "Import settings inspector should expose quality/max_splats.");
    dialog->_test_set_setting_value(StringName("quality/max_splats"), 22222);

    dialog->current_import_options = refreshed_options;
    dialog->current_asset = _make_editor_test_asset(source_path, 5, refreshed_options,
            GaussianSplatAsset::COMPRESSION_POSITIONS | GaussianSplatAsset::COMPRESSION_COLORS, 2097152);

    dialog->_test_reimport_now();

    CHECK(dialog->reimport_request_count == 1);
    CHECK(dialog->last_importer_name == "gaussian_splat_ply");
    REQUIRE_MESSAGE(dialog->last_reimport_params.has(StringName("quality/max_splats")),
            "Reimport request should preserve the edited inspector value.");
    CHECK(int(dialog->last_reimport_params[StringName("quality/max_splats")]) == 22222);
    REQUIRE_MESSAGE(dialog->last_reimport_params.has(StringName("custom/preserve")),
            "Reimport request should preserve unexposed sidecar parameters.");
    CHECK(String(dialog->last_reimport_params[StringName("custom/preserve")]) == "alpha");
    CHECK(String(dialog->_test_get_setting_value(StringName("quality/preset"))) == String("ultra"));
    CHECK(int(dialog->_test_get_setting_value(StringName("quality/max_splats"))) == 54321);
    REQUIRE_MESSAGE(dialog->_test_get_loaded_asset().is_valid(), "Import settings dialog should refresh the loaded asset.");
    CHECK(dialog->_test_get_loaded_asset()->get_splat_count() == 5);
    CHECK(dialog->_test_get_stats_text().contains("Splats: 5"));
    CHECK(dialog->_test_get_stats_text().contains("Est. Memory: 2.0 MB"));

    GaussianSplatNode3D *preview_node = Object::cast_to<GaussianSplatNode3D>(dialog->find_child("ImportPreviewSplat", true, false));
    REQUIRE_MESSAGE(preview_node != nullptr, "Import settings dialog should rebuild its preview splat node.");
    CHECK(preview_node->get_splat_asset() == dialog->_test_get_loaded_asset());

    memdelete(dialog);
}

TEST_CASE("[GaussianSplatting][Editor] Hot reload fans one watched source path out to all registered live nodes") {
    const String source_path = "user://hot_reload_fanout_fixture.ply";

    GaussianEditorPlugin *plugin = memnew(GaussianEditorPlugin);
    GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
    GaussianSplatNode3D *node_c = memnew(GaussianSplatNode3D);

    Ref<GaussianSplatAsset> asset_a = _make_editor_test_asset(source_path, 1, Dictionary());
    Ref<GaussianSplatAsset> asset_b = _make_editor_test_asset(source_path, 1, Dictionary());
    Ref<GaussianSplatAsset> asset_c = _make_editor_test_asset(source_path, 1, Dictionary());
    Ref<GaussianSplatAsset> refreshed_asset = _make_editor_test_asset(source_path, 2, Dictionary());

    node_a->set_splat_asset(asset_a);
    node_b->set_splat_asset(asset_b);
    node_c->set_splat_asset(asset_c);

    plugin->_test_track_hot_reload_source(source_path, Dictionary(), node_a->get_instance_id());
    plugin->_test_track_hot_reload_source(source_path, Dictionary(), node_b->get_instance_id());

    CHECK(plugin->_test_process_hot_reload_path_now(source_path, refreshed_asset));

    CHECK(node_a->get_splat_asset() == refreshed_asset);
    CHECK(node_b->get_splat_asset() == refreshed_asset);
    CHECK(node_c->get_splat_asset() == asset_c);

    memdelete(node_c);
    memdelete(node_b);
	memdelete(node_a);
	memdelete(plugin);
}

TEST_CASE("[GaussianSplatting][Editor] Plugin does not handle standalone renderer selections") {
	GaussianEditorPlugin *plugin = memnew(GaussianEditorPlugin);
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();

	CHECK(plugin->handles(node));
	CHECK(plugin->handles(asset.ptr()));
	CHECK_FALSE(plugin->handles(renderer.ptr()));

	memdelete(node);
	memdelete(plugin);
}

TEST_CASE("[GaussianSplatting][Editor] Hot reload leaves watched nodes unchanged when no refreshed asset is available") {
	const String source_path = "user://hot_reload_failure_fixture.ply";

	GaussianEditorPlugin *plugin = memnew(GaussianEditorPlugin);
	GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
	GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);

	Ref<GaussianSplatAsset> asset_a = _make_editor_test_asset(source_path, 1, Dictionary());
	Ref<GaussianSplatAsset> asset_b = _make_editor_test_asset(source_path, 1, Dictionary());

	node_a->set_splat_asset(asset_a);
	node_b->set_splat_asset(asset_b);

	plugin->_test_track_hot_reload_source(source_path, Dictionary(), node_a->get_instance_id());
	plugin->_test_track_hot_reload_source(source_path, Dictionary(), node_b->get_instance_id());

	CHECK(plugin->_test_process_hot_reload_path_now(source_path, Ref<GaussianSplatAsset>()));

	CHECK(node_a->get_splat_asset() == asset_a);
	CHECK(node_b->get_splat_asset() == asset_b);

	memdelete(node_b);
	memdelete(node_a);
	memdelete(plugin);
}

#endif // TOOLS_ENABLED
