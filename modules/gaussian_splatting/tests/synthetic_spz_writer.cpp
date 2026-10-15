#include "synthetic_spz_writer.h"
#include "../thirdparty/spz/load-spz.h"
#include "../thirdparty/spz/coordinate-system-adobe.h"
#include "core/io/compression.h"
#include "core/io/file_access.h"
#include <cmath>
#include <limits>

namespace TestGaussianSplatting {
bool write_synthetic_spz(const String &p_path, const LocalVector<SyntheticSpzSplat> &p_splats,
        uint8_t p_fractional_bits, uint32_t p_version, uint8_t p_degree, bool p_antialiased, int p_storage_coordinates) {
    if (p_splats.is_empty() || p_fractional_bits != 12 || p_version < 2 || p_version > 4 || p_degree > 4) {
        return false;
    }
    spz::GaussianCloud cloud;
    cloud.numPoints = p_splats.size();
    cloud.shDegree = p_degree;
    cloud.antialiased = p_antialiased;
    const uint32_t terms = (p_degree + 1) * (p_degree + 1) - 1;
    for (uint32_t i = 0; i < p_splats.size(); i++) {
        const SyntheticSpzSplat &s = p_splats[i];
        for (int axis = 0; axis < 3; axis++) {
            cloud.positions.push_back(s.position[axis]);
            cloud.scales.push_back(std::log(s.scale[axis]));
            cloud.colors.push_back((s.color[axis] - 0.5f) / 0.28209479177387814f);
        }
        for (int axis = 0; axis < 4; axis++) {
            cloud.rotations.push_back(s.rotation[axis]);
        }
        cloud.alphas.push_back(s.opacity == 0 ? -std::numeric_limits<float>::infinity() :
                s.opacity == 1 ? std::numeric_limits<float>::infinity() : std::log(s.opacity / (1 - s.opacity)));
        for (uint32_t term = 0; term < terms; term++) {
            for (int axis = 0; axis < 3; axis++) {
                cloud.sh.push_back(s.sh[term][axis]);
            }
        }
    }
    if (p_storage_coordinates >= 0) {
        auto extension = std::make_shared<spz::SpzExtensionCoordinateSystemAdobe>();
        extension->coordinateSystem = spz::CoordinateSystem(p_storage_coordinates);
        cloud.extensions.push_back(extension);
    }
    spz::PackOptions options;
    options.version = p_version;
    options.from = spz::CoordinateSystem::RUB;
    std::vector<uint8_t> output;
    if (!spz::saveSpz(cloud, options, &output)) {
        return false;
    }
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
    if (file.is_null()) {
        return false;
    }
    file->store_buffer(output.data(), output.size());
    return file->get_error() == OK;
}

// Historical GodotGS SPZ v2 wrapper: a 16-byte uncompressed header followed by a
// gzip-compressed SoA payload built by the caller from reference bytes (#1154, #1056).
// SPZLoader keeps this route as an explicit legacy compatibility path.
bool write_spz_v2_payload(const String &p_path, uint32_t p_count, const LocalVector<uint8_t> &p_payload,
        uint8_t p_fractional_bits) {
    if (p_count == 0 || p_payload.is_empty()) {
        return false;
    }
    // gzip-compress the payload. The loader validates the gzip trailer's ISIZE
    // against num_points * 19, which Compression::MODE_GZIP writes correctly.
    const int64_t payload_size = int64_t(p_payload.size());
    const int64_t max_compressed = Compression::get_max_compressed_buffer_size(payload_size, Compression::MODE_GZIP);
    if (max_compressed <= 0) {
        return false;
    }
    LocalVector<uint8_t> compressed;
    compressed.resize(uint32_t(max_compressed));
    const int64_t compressed_size = Compression::compress(compressed.ptr(), p_payload.ptr(), payload_size, Compression::MODE_GZIP);
    if (compressed_size <= 0) {
        return false;
    }

    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
    if (f.is_null()) {
        return false;
    }
    f->set_big_endian(false);

    // 16-byte header (uncompressed). The first byte (0x4E) is NOT the GZIP magic
    // 0x1F, so the loader takes the standard "header uncompressed, payload gzip"
    // path rather than the fully-gzip-wrapped path.
    f->store_32(0x5053474E); // "NGSP" little-endian
    f->store_32(2);
    f->store_32(p_count);
    f->store_8(0); // sh_degree
    f->store_8(p_fractional_bits);
    f->store_8(0); // flags
    f->store_8(0); // reserved

    f->store_buffer(compressed.ptr(), compressed_size);
    return true;
}
} // namespace TestGaussianSplatting
