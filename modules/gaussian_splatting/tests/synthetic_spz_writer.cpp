/**************************************************************************/
/*  synthetic_spz_writer.cpp                                              */
/**************************************************************************/

#include "synthetic_spz_writer.h"

#include "core/io/compression.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"

#include <cmath>

namespace TestGaussianSplatting {

// Keep these in lockstep with io/spz_loader.{h,cpp}. Duplicated here (rather than
// including the loader) so the writer stays a self-contained fixture and cannot
// silently "agree with itself" if the loader constants ever drift.
static constexpr uint32_t SPZ_MAGIC = 0x5053474E; // "NGSP" little-endian
static constexpr uint32_t SPZ_VERSION_2 = 2;

static void _append_u24_le_signed(LocalVector<uint8_t> &r_bytes, int32_t p_value) {
    // 24-bit little-endian; the loader sign-extends bit 23 on read.
    const int32_t clamped = CLAMP(p_value, -0x800000, 0x7FFFFF);
    r_bytes.push_back(uint8_t(clamped & 0xFF));
    r_bytes.push_back(uint8_t((clamped >> 8) & 0xFF));
    r_bytes.push_back(uint8_t((clamped >> 16) & 0xFF));
}

static uint8_t _encode_alpha(float p_opacity) {
    // Inverse of SPZLoader::decode_alpha: alpha = byte / 255.
    const float a = CLAMP(p_opacity, 0.0f, 1.0f);
    return uint8_t(Math::round(a * 255.0f));
}

static uint8_t _encode_color_channel(float p_channel) {
    const float c = CLAMP(p_channel, 0.0f, 1.0f);
    return uint8_t(Math::round(c * 255.0f));
}

static uint8_t _encode_scale(float p_scale) {
    // Inverse of SPZLoader::decode_scale: scale = exp(byte/16 - 10)
    //   => byte = round((ln(scale) + 10) * 16), clamped to a byte.
    const float s = MAX(p_scale, 1e-9f);
    const float encoded = (std::log(s) + 10.0f) * 16.0f;
    const float clamped = CLAMP(encoded, 0.0f, 255.0f);
    return uint8_t(Math::round(clamped));
}

static int8_t _encode_quat_component(float p_value) {
    const float v = CLAMP(p_value, -1.0f, 1.0f);
    const float scaled = CLAMP(Math::round(v * 127.0f), -127.0f, 127.0f);
    return int8_t(scaled);
}

bool write_synthetic_spz(const String &p_path, const LocalVector<SyntheticSpzSplat> &p_splats,
        uint8_t p_fractional_bits) {
    const uint32_t count = p_splats.size();
    if (count == 0) {
        return false;
    }
    if (p_fractional_bits > 24) {
        return false;
    }

    const float fixed_scale = float(1u << p_fractional_bits);

    // Build the SoA payload EXACTLY in the loader's parse order:
    //   positions -> alphas -> colors -> scales -> rotations (v2).
    LocalVector<uint8_t> payload;
    payload.reserve(count * 19u);

    // Positions: 3 x int24 fixed-point.
    for (uint32_t i = 0; i < count; i++) {
        const Vector3 &pos = p_splats[i].position;
        _append_u24_le_signed(payload, int32_t(Math::round(pos.x * fixed_scale)));
        _append_u24_le_signed(payload, int32_t(Math::round(pos.y * fixed_scale)));
        _append_u24_le_signed(payload, int32_t(Math::round(pos.z * fixed_scale)));
    }

    // Alphas: 1 byte each.
    for (uint32_t i = 0; i < count; i++) {
        payload.push_back(_encode_alpha(p_splats[i].opacity));
    }

    // Colors: 3 bytes (RGB) each.
    for (uint32_t i = 0; i < count; i++) {
        const Color &c = p_splats[i].color;
        payload.push_back(_encode_color_channel(c.r));
        payload.push_back(_encode_color_channel(c.g));
        payload.push_back(_encode_color_channel(c.b));
    }

    // Scales: 3 log-encoded bytes each.
    for (uint32_t i = 0; i < count; i++) {
        const Vector3 &s = p_splats[i].scale;
        payload.push_back(_encode_scale(s.x));
        payload.push_back(_encode_scale(s.y));
        payload.push_back(_encode_scale(s.z));
    }

    // Rotations (v2): 3 x int8 (x,y,z); the loader reconstructs w >= 0.
    for (uint32_t i = 0; i < count; i++) {
        Quaternion q = p_splats[i].rotation;
        const real_t len = q.length();
        if (len > 0) {
            q = q / len;
        } else {
            q = Quaternion(); // identity fallback
        }
        // Store the hemisphere with w >= 0 so the loader's positive-w
        // reconstruction recovers the same orientation.
        if (q.w < 0) {
            q = Quaternion(-q.x, -q.y, -q.z, -q.w);
        }
        payload.push_back(uint8_t(_encode_quat_component(float(q.x))));
        payload.push_back(uint8_t(_encode_quat_component(float(q.y))));
        payload.push_back(uint8_t(_encode_quat_component(float(q.z))));
    }

    // gzip-compress the payload. The loader validates the gzip trailer's ISIZE
    // against num_points * 19, which Compression::MODE_GZIP writes correctly.
    const int64_t payload_size = int64_t(payload.size());
    const int64_t max_compressed = Compression::get_max_compressed_buffer_size(payload_size, Compression::MODE_GZIP);
    if (max_compressed <= 0) {
        return false;
    }
    LocalVector<uint8_t> compressed;
    compressed.resize(uint32_t(max_compressed));
    const int64_t compressed_size = Compression::compress(compressed.ptr(), payload.ptr(), payload_size, Compression::MODE_GZIP);
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
    f->store_32(SPZ_MAGIC);
    f->store_32(SPZ_VERSION_2);
    f->store_32(count);
    f->store_8(0); // sh_degree
    f->store_8(p_fractional_bits);
    f->store_8(0); // flags
    f->store_8(0); // reserved

    f->store_buffer(compressed.ptr(), compressed_size);
    return true;
}

} // namespace TestGaussianSplatting
