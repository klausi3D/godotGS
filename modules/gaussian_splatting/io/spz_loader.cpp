#include "spz_loader.h"
#include "../core/gs_vector_alloc.h"
#include "../logger/gs_logger.h"
#include "../thirdparty/spz/load-spz.h"
#include "../thirdparty/spz/splat-extensions.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"

#include <cmath>
#include <istream>
#include <streambuf>

// Pinned upstream's legacy stream decoder. Godot bounds gzip expansion before
// entering it; the public loadSpzPacked gzip path inflates without a caller cap.
namespace spz {
PackedGaussians deserializePackedGaussians(std::istream &p_stream);
}

namespace {
constexpr uint64_t MAX_SPZ_COMPRESSED_BYTES = 512ull * 1024 * 1024;
constexpr uint64_t MAX_SPZ_DECOMPRESSED_BYTES = 1024ull * 1024 * 1024;
constexpr uint32_t MAX_SPZ_POINTS = 32u * 1024 * 1024;
constexpr float SH_C0 = 0.28209479177387814f;
constexpr uint8_t FLAG_EXTENSIONS = 2;

class SpzMemoryBuffer : public std::streambuf {
public:
    SpzMemoryBuffer(const PackedByteArray &p_bytes) {
        char *begin = const_cast<char *>(reinterpret_cast<const char *>(p_bytes.ptr()));
        setg(begin, begin, begin + p_bytes.size());
    }
protected:
    pos_type seekoff(off_type p_offset, std::ios_base::seekdir p_dir, std::ios_base::openmode p_mode) override {
        if (!(p_mode & std::ios_base::in)) {
            return pos_type(off_type(-1));
        }
        const off_type size = egptr() - eback();
        const off_type base = p_dir == std::ios_base::beg ? 0 : p_dir == std::ios_base::cur ? gptr() - eback() : size;
        if (p_offset < -base || p_offset > size - base) {
            return pos_type(off_type(-1));
        }
        setg(eback(), eback() + base + p_offset, egptr());
        return pos_type(base + p_offset);
    }
    pos_type seekpos(pos_type p_position, std::ios_base::openmode p_mode) override {
        return seekoff(off_type(p_position), std::ios_base::beg, p_mode);
    }
};

struct SpzEnvelope {
    SPZLoader::SPZHeader header{};
    uint64_t attributes_bytes = 0;
    uint64_t strides[6]{};
    uint32_t toc_offset = 0;
    uint8_t stream_count = 0;
};

uint64_t spz_working_memory_budget() {
    uint64_t budget = 256ull * 1024 * 1024;
    if (OS::get_singleton()) {
        const Dictionary memory = OS::get_singleton()->get_memory_info();
        const Variant available = memory.get("available", Variant());
        if (available.get_type() == Variant::INT && int64_t(available) > 0) {
            budget = MAX(budget, uint64_t(int64_t(available)) / 2);
        }
    }
    return budget;
}

Error spz_validate_extensions(const uint8_t *p_bytes, uint64_t p_size, bool p_declared) {
    ERR_FAIL_COND_V(p_declared != (p_size > 0), ERR_FILE_CORRUPT);
    uint64_t offset = 0;
    uint32_t seen = 0;
    while (offset < p_size) {
        ERR_FAIL_COND_V(p_size - offset < 8, ERR_FILE_CORRUPT);
        const uint32_t type = decode_uint32(p_bytes + offset);
        const uint32_t length = decode_uint32(p_bytes + offset + 4);
        offset += 8;
        ERR_FAIL_COND_V(uint64_t(length) > p_size - offset, ERR_FILE_CORRUPT);
        if (type == uint32_t(spz::SpzExtensionType::SPZ_ADOBE_coordinate_system)) {
            ERR_FAIL_COND_V((seen & 1) || length != 4, ERR_FILE_CORRUPT);
            ERR_FAIL_COND_V(decode_uint32(p_bytes + offset) > 16, ERR_FILE_CORRUPT);
            seen |= 1;
        } else if (type == uint32_t(spz::SpzExtensionType::SPZ_ADOBE_safe_orbit_camera)) {
            ERR_FAIL_COND_V((seen & 2) || length != 12, ERR_FILE_CORRUPT);
            for (int field = 0; field < 3; field++) {
                ERR_FAIL_COND_V(!std::isfinite(decode_float(p_bytes + offset + field * 4)), ERR_FILE_CORRUPT);
            }
            seen |= 2;
        } else {
            ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "SPZ contains an unsupported extension; packing semantics cannot be guessed.");
        }
        offset += length;
    }
    return OK;
}

Error spz_parse_header(const PackedByteArray &p_bytes, SpzEnvelope &r_envelope, uint64_t p_input_bytes) {
    ERR_FAIL_COND_V(p_bytes.size() < 16, ERR_FILE_CORRUPT);
    const uint8_t *r = p_bytes.ptr();
    SPZLoader::SPZHeader &header = r_envelope.header;
    header.magic = decode_uint32(r);
    ERR_FAIL_COND_V(header.magic != SPZLoader::SPZ_MAGIC, ERR_FILE_UNRECOGNIZED);
    header.version = decode_uint32(r + 4);
    ERR_FAIL_COND_V(header.version < 1 || header.version > 4, ERR_FILE_UNRECOGNIZED);
    header.num_points = decode_uint32(r + 8);
    header.sh_degree = r[12];
    header.fractional_bits = r[13];
    header.flags = r[14];
    header.reserved = header.version < 4 ? r[15] : 0;
    ERR_FAIL_COND_V(header.num_points == 0 || header.num_points > MAX_SPZ_POINTS, ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V(header.sh_degree > 4 || header.fractional_bits > 24, ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V((header.flags & ~3u) != 0 || header.reserved != 0, ERR_FILE_CORRUPT);
    const uint32_t sh_terms = (header.sh_degree + 1) * (header.sh_degree + 1) - 1;
    r_envelope.strides[0] = header.version == 1 ? 6 : 9;
    r_envelope.strides[1] = 1;
    r_envelope.strides[2] = 3;
    r_envelope.strides[3] = 3;
    r_envelope.strides[4] = header.version >= 3 ? 4 : 3;
    r_envelope.strides[5] = sh_terms * 3;
    for (uint64_t stride : r_envelope.strides) {
        r_envelope.attributes_bytes += uint64_t(header.num_points) * stride;
    }
    ERR_FAIL_COND_V(r_envelope.attributes_bytes > MAX_SPZ_DECOMPRESSED_BYTES, ERR_FILE_CORRUPT);
    const uint64_t runtime_bytes = uint64_t(header.num_points) * (sizeof(Gaussian) + (sh_terms > 3 ? sh_terms - 3 : 0) * sizeof(Vector3));
    // Packed buffers coexist with staging and its final resource copy. Budget
    // the input/decode buffers too; genuine allocator exhaustion remains fatal.
    const uint64_t working_bytes = p_input_bytes + uint64_t(p_bytes.size()) + r_envelope.attributes_bytes * 2 + runtime_bytes * 2;
    ERR_FAIL_COND_V(working_bytes > spz_working_memory_budget(), ERR_FILE_CORRUPT);
    return OK;
}

Error spz_validate_legacy(const PackedByteArray &p_bytes, const SpzEnvelope &p_envelope) {
    const uint64_t payload_end = 16 + p_envelope.attributes_bytes;
    ERR_FAIL_COND_V(payload_end > uint64_t(p_bytes.size()), ERR_FILE_CORRUPT);
    return spz_validate_extensions(p_bytes.ptr() + payload_end, uint64_t(p_bytes.size()) - payload_end,
            p_envelope.header.flags & FLAG_EXTENSIONS);
}

Error spz_validate_v4(const PackedByteArray &p_bytes, SpzEnvelope &r_envelope) {
    ERR_FAIL_COND_V(p_bytes.size() < 32, ERR_FILE_CORRUPT);
    const uint8_t *r = p_bytes.ptr();
    for (uint32_t i = 20; i < 32; i++) {
        ERR_FAIL_COND_V(r[i] != 0, ERR_FILE_CORRUPT);
    }
    r_envelope.stream_count = r[15];
    r_envelope.toc_offset = decode_uint32(r + 16);
    ERR_FAIL_COND_V(r_envelope.toc_offset < 32 || uint64_t(r_envelope.toc_offset) > uint64_t(p_bytes.size()), ERR_FILE_CORRUPT);
    const Error extension_error = spz_validate_extensions(r + 32, r_envelope.toc_offset - 32, r_envelope.header.flags & FLAG_EXTENSIONS);
    if (extension_error != OK) {
        return extension_error;
    }
    const uint8_t expected_streams = r_envelope.header.sh_degree > 0 ? 6 : 5;
    ERR_FAIL_COND_V(r_envelope.stream_count != expected_streams, ERR_FILE_CORRUPT);
    uint64_t offset = uint64_t(r_envelope.toc_offset) + uint64_t(expected_streams) * 16;
    ERR_FAIL_COND_V(offset > uint64_t(p_bytes.size()), ERR_FILE_CORRUPT);
    for (uint32_t i = 0; i < expected_streams; i++) {
        const uint8_t *entry = r + r_envelope.toc_offset + i * 16;
        const uint64_t compressed_size = decode_uint64(entry);
        const uint64_t decoded_size = decode_uint64(entry + 8);
        const uint64_t expected_size = uint64_t(r_envelope.header.num_points) * r_envelope.strides[i];
        ERR_FAIL_COND_V(decoded_size != expected_size || compressed_size == 0, ERR_FILE_CORRUPT);
        ERR_FAIL_COND_V(compressed_size > uint64_t(p_bytes.size()) - offset, ERR_FILE_CORRUPT);
        offset += compressed_size;
    }
    ERR_FAIL_COND_V(offset != uint64_t(p_bytes.size()), ERR_FILE_CORRUPT);
    return OK;
}

Error spz_populate_runtime(const spz::PackedGaussians &p_packed, const SpzEnvelope &p_envelope, Ref<GaussianData> &r_data) {
    const uint32_t count = p_envelope.header.num_points;
    const uint32_t degree = p_envelope.header.sh_degree;
    ERR_FAIL_COND_V(p_packed.numPoints != int32_t(count) || p_packed.shDegree != int32_t(degree), ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V(p_packed.version != p_envelope.header.version || p_packed.fractionalBits != p_envelope.header.fractional_bits ||
            p_packed.antialiased != bool(p_envelope.header.flags & SPZLoader::SPZ_FLAG_ANTIALIASED) || p_packed.hadSkippedExtensions, ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V(p_packed.positions.size() != uint64_t(count) * p_envelope.strides[0] ||
            p_packed.alphas.size() != uint64_t(count) * p_envelope.strides[1] ||
            p_packed.colors.size() != uint64_t(count) * p_envelope.strides[2] ||
            p_packed.scales.size() != uint64_t(count) * p_envelope.strides[3] ||
            p_packed.rotations.size() != uint64_t(count) * p_envelope.strides[4] ||
            p_packed.sh.size() != uint64_t(count) * p_envelope.strides[5], ERR_FILE_CORRUPT);
    const uint32_t first = degree == 0 ? 0 : 3;
    const uint32_t high = (degree + 1) * (degree + 1) - 1 - first;
    const spz::CoordinateConverter converter = spz::coordinateConverter(spz::getPackedCoordinateSystem(p_packed.extensions), spz::CoordinateSystem::RUB, degree);
    LocalVector<Gaussian> gaussians;
    LocalVector<Vector3> sidecar;
    gaussians.resize(count);
    sidecar.resize(uint64_t(count) * high);
    for (uint32_t i = 0; i < count; i++) {
        const spz::UnpackedGaussian unpacked = p_packed.unpack(i, converter);
        Gaussian &g = gaussians[i];
        g.position = Vector3(unpacked.position[0], unpacked.position[1], unpacked.position[2]);
        g.scale = Vector3(std::exp(unpacked.scale[0]), std::exp(unpacked.scale[1]), std::exp(unpacked.scale[2]));
        g.opacity = 1.0f / (1.0f + std::exp(-unpacked.alpha));
        g.rotation = Quaternion(unpacked.rotation[0], unpacked.rotation[1], unpacked.rotation[2], unpacked.rotation[3]);
        ERR_FAIL_COND_V(!g.position.is_finite() || !g.scale.is_finite() || !g.rotation.is_finite() || g.rotation.length_squared() == 0, ERR_FILE_CORRUPT);
        g.rotation.normalize();
        g.sh_dc = Color(SH_C0 * unpacked.color[0], SH_C0 * unpacked.color[1], SH_C0 * unpacked.color[2], 1);
        g.render_meta = gaussian_set_dc_encoding(g.render_meta, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
        g.normal = Vector3(0, 0, 1);
        g.area = 1;
        g.brush_axes = Vector2(1, 1);
        for (uint32_t term = 0; term < first; term++) {
            g.sh_1[term] = Vector3(unpacked.shR[term], unpacked.shG[term], unpacked.shB[term]);
        }
        for (uint32_t term = 0; term < high; term++) {
            const uint32_t coefficient = first + term;
            sidecar[i * high + term] = Vector3(unpacked.shR[coefficient], unpacked.shG[coefficient], unpacked.shB[coefficient]);
        }
    }
    Ref<GaussianData> staged;
    staged.instantiate();
    staged->set_gaussian_payload(gaussians, sidecar, first, high, false, p_packed.antialiased);
    r_data = staged;
    return OK;
}
}

SPZLoader::SPZLoader() {
    gaussian_data.instantiate();
    header = SPZHeader{};
}
SPZLoader::~SPZLoader() {}

void SPZLoader::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_file", "path"), &SPZLoader::load_file);
    ClassDB::bind_method(D_METHOD("get_gaussian_data"), &SPZLoader::get_gaussian_data);
    ClassDB::bind_method(D_METHOD("get_splat_count"), &SPZLoader::get_splat_count);
    ClassDB::bind_method(D_METHOD("get_load_statistics"), &SPZLoader::get_load_statistics);
}

bool SPZLoader::is_spz_file(const String &p_path) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    if (file.is_null() || file->get_length() < 16) {
        return false;
    }
    const uint32_t magic = file->get_32();
    return magic == SPZ_MAGIC || ((magic & 0xffff) == 0x8b1f && p_path.to_lower().ends_with(".spz"));
}

Error SPZLoader::decompress_data(const PackedByteArray &p_compressed, PackedByteArray &r_decompressed,
        uint64_t p_max_decompressed_bytes, uint64_t p_expected_decompressed_bytes) {
    ERR_FAIL_COND_V(p_compressed.size() < 18 || uint64_t(p_compressed.size()) > MAX_SPZ_COMPRESSED_BYTES, ERR_FILE_CORRUPT);
    const uint8_t *r = p_compressed.ptr();
    ERR_FAIL_COND_V(r[0] != 0x1f || r[1] != 0x8b || r[2] != 8 || (r[3] & 0xe0), ERR_FILE_CORRUPT);
    const uint32_t bytes = decode_uint32(r + p_compressed.size() - 4);
    ERR_FAIL_COND_V(bytes == 0 || bytes > p_max_decompressed_bytes || bytes > MAX_SPZ_DECOMPRESSED_BYTES || uint64_t(bytes) + uint64_t(p_compressed.size()) > spz_working_memory_budget(), ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V(p_expected_decompressed_bytes > 0 && bytes != p_expected_decompressed_bytes, ERR_FILE_CORRUPT);
    if (!gs_resize_or_fail(r_decompressed, bytes, "SPZLoader::decompress_data output")) {
        return ERR_OUT_OF_MEMORY;
    }
    // Complete gzip decoding verifies its framing and CRC; a failed gzip must
    // not be accepted by retrying raw DEFLATE while ignoring the failed CRC.
    const int result = Compression::decompress(r_decompressed.ptrw(), bytes, r, p_compressed.size(), Compression::MODE_GZIP);
    ERR_FAIL_COND_V(result != int(bytes), ERR_FILE_CORRUPT);
    return OK;
}

Error SPZLoader::load_file(const String &p_path) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    ERR_FAIL_COND_V(file.is_null(), ERR_FILE_CANT_OPEN);
    ERR_FAIL_COND_V(file->get_length() < 16 || file->get_length() > MAX_SPZ_COMPRESSED_BYTES, ERR_FILE_CORRUPT);
    const PackedByteArray input = file->get_buffer(file->get_length());
    ERR_FAIL_COND_V(uint64_t(input.size()) != file->get_length(), ERR_FILE_CORRUPT);
    PackedByteArray bytes = input;
    const bool whole_gzip = input[0] == 0x1f && input[1] == 0x8b;
    if (whole_gzip) {
        const Error decompressed = decompress_data(input, bytes, MAX_SPZ_DECOMPRESSED_BYTES);
        if (decompressed != OK) {
            return decompressed;
        }
    }
    SpzEnvelope envelope;
    Error error = spz_parse_header(bytes, envelope, input.size());
    if (error != OK) {
        return error;
    }
    const bool v4 = envelope.header.version == 4;
    if (!v4 && !whole_gzip) {
        // Historical GodotGS header+gzip wrapper, retained as an explicit legacy
        // compatibility route rather than advertised as Niantic producer output.
        PackedByteArray payload;
        error = decompress_data(input.slice(16), payload, MAX_SPZ_DECOMPRESSED_BYTES, envelope.attributes_bytes);
        if (error != OK) {
            return error;
        }
        if (!gs_resize_or_fail(bytes, 16 + payload.size(), "SPZLoader::load_file legacy wrapper")) {
            return ERR_OUT_OF_MEMORY;
        }
        memcpy(bytes.ptrw(), input.ptr(), 16);
        memcpy(bytes.ptrw() + 16, payload.ptr(), payload.size());
        envelope = SpzEnvelope{};
        error = spz_parse_header(bytes, envelope, input.size());
        if (error != OK) {
            return error;
        }
    }
    error = v4 ? spz_validate_v4(bytes, envelope) : spz_validate_legacy(bytes, envelope);
    if (error != OK) {
        return error;
    }
    spz::PackedGaussians packed;
    if (v4) {
        packed = spz::loadSpzPacked(bytes.ptr(), bytes.size());
    } else {
        SpzMemoryBuffer buffer(bytes);
        std::istream stream(&buffer);
        packed = spz::deserializePackedGaussians(stream);
    }
    Ref<GaussianData> staged;
    error = spz_populate_runtime(packed, envelope, staged);
    if (error != OK) {
        return error;
    }
    gaussian_data = staged;
    header = envelope.header;
    container_stream_count = envelope.stream_count;
    return OK;
}

int SPZLoader::get_splat_count() const {
    return gaussian_data.is_valid() ? gaussian_data->get_count() : 0;
}
Dictionary SPZLoader::get_load_statistics() const {
    Dictionary stats;
    stats["splat_count"] = get_splat_count();
    stats["format"] = "spz";
    stats["version"] = int(header.version);
    stats["sh_degree"] = int(header.sh_degree);
    stats["fractional_bits"] = int(header.fractional_bits);
    stats["flags"] = int(header.flags);
    stats["antialiased"] = (header.flags & SPZ_FLAG_ANTIALIASED) != 0;
    stats["num_streams"] = int(container_stream_count);
    stats["decoder_commit"] = "affd0ecea7fbb4c265ee119475af7ee5b2997482";
    if (gaussian_data.is_valid()) {
        const AABB bounds = gaussian_data->get_aabb();
        stats["bounds_min"] = bounds.position;
        stats["bounds_max"] = bounds.position + bounds.size;
    }
    return stats;
}
