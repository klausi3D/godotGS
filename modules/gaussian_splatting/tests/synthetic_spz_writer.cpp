/**************************************************************************/
/*  synthetic_spz_writer.cpp                                              */
/**************************************************************************/

#include "synthetic_spz_writer.h"

#include "core/io/compression.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"

#include <cmath>

namespace TestGaussianSplatting {

// Encodings follow the Niantic reference encoder (nianticlabs/spz load-spz.cc),
// not SPZLoader. Duplicated here (rather than including the loader) so the
// writer stays a self-contained fixture and cannot silently "agree with itself"
// if the loader's decode drifts from the format.
static constexpr uint32_t SPZ_MAGIC = 0x5053474E; // "NGSP" little-endian
static constexpr uint32_t SPZ_VERSION_2 = 2;
static constexpr float SPZ_COLOR_SCALE = 0.15f; // splat-utils.h colorScale

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


static uint8_t _encode_scale(float p_scale) {
    // Inverse of SPZLoader::decode_scale: scale = exp(byte/16 - 10)
    //   => byte = round((ln(scale) + 10) * 16), clamped to a byte.
    const float s = MAX(p_scale, 1e-9f);
    const float encoded = (std::log(s) + 10.0f) * 16.0f;
    const float clamped = CLAMP(encoded, 0.0f, 255.0f);
    return uint8_t(Math::round(clamped));
}

// Niantic reference toUint8 (nianticlabs/spz splat-utils): round half away from
// zero, then clamp to a byte. Math::round is roundf, which rounds the same way.
static uint8_t _to_uint8(float p_value) {
    return uint8_t(CLAMP(Math::round(p_value), 0.0f, 255.0f));
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

    // Colors: 3 bytes (RGB) each. The reference packs the SH DC coefficient as
    // toUint8(f_dc * (0.15 * 255) + 0.5 * 255) (load-spz.cc, colorScale = 0.15),
    // not a display colour (#1056).
    for (uint32_t i = 0; i < count; i++) {
        const Color &dc = p_splats[i].f_dc;
        payload.push_back(_to_uint8(dc.r * (SPZ_COLOR_SCALE * 255.0f) + 0.5f * 255.0f));
        payload.push_back(_to_uint8(dc.g * (SPZ_COLOR_SCALE * 255.0f) + 0.5f * 255.0f));
        payload.push_back(_to_uint8(dc.b * (SPZ_COLOR_SCALE * 255.0f) + 0.5f * 255.0f));
    }

    // Scales: 3 log-encoded bytes each.
    for (uint32_t i = 0; i < count; i++) {
        const Vector3 &s = p_splats[i].scale;
        payload.push_back(_encode_scale(s.x));
        payload.push_back(_encode_scale(s.y));
        payload.push_back(_encode_scale(s.z));
    }

    // Rotations (v2): the reference packQuaternionFirstThree (nianticlabs/spz
    // load-spz.cc). Normalise, scale by -127.5 when w < 0 (which moves the
    // quaternion into the w >= 0 hemisphere) or +127.5 otherwise, add 127.5 and
    // store x, y, z as UNSIGNED bytes: a zero component is byte 128 (#1154).
    // This is written from the reference, not as the inverse of SPZLoader, so a
    // loader that misreads the bytes cannot agree with it.
    for (uint32_t i = 0; i < count; i++) {
        Quaternion q = p_splats[i].rotation;
        const real_t len = q.length();
        if (len > 0) {
            q = q / len;
        } else {
            q = Quaternion(); // identity fallback
        }
        const float sign_scale = q.w < 0 ? -127.5f : 127.5f;
        payload.push_back(_to_uint8(float(q.x) * sign_scale + 127.5f));
        payload.push_back(_to_uint8(float(q.y) * sign_scale + 127.5f));
        payload.push_back(_to_uint8(float(q.z) * sign_scale + 127.5f));
    }

    return write_spz_v2_payload(p_path, count, payload, p_fractional_bits);
}

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
    f->store_32(SPZ_MAGIC);
    f->store_32(SPZ_VERSION_2);
    f->store_32(p_count);
    f->store_8(0); // sh_degree
    f->store_8(p_fractional_bits);
    f->store_8(0); // flags
    f->store_8(0); // reserved

    f->store_buffer(compressed.ptr(), compressed_size);
    return true;
}

} // namespace TestGaussianSplatting
