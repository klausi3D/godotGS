#ifndef GAUSSIAN_SCENE_PAYLOAD_H
#define GAUSSIAN_SCENE_PAYLOAD_H

#include "../core/gaussian_data.h"
#include "../core/gs_vector_alloc.h"
#include "core/io/marshalls.h"

namespace GaussianSplatting {
namespace ScenePayload {

constexpr uint32_t PREFIX_SIZE = 32;
constexpr uint32_t RECORD_SIZE = 132;
constexpr uint32_t MAX_HIGH_ORDER = 21;

inline uint32_t degree_for_layout(uint32_t p_first, uint32_t p_high) {
    const uint32_t total = 1 + p_first + p_high;
    uint32_t degree = 0;
    while ((degree + 2) * (degree + 2) <= total) {
        degree++;
    }
    return degree;
}

// Explicit float32 wire fields. Gaussian's ABI padding is deliberately excluded.
inline void encode_record(const Gaussian &g, uint8_t *r_bytes) {
    const float fields[] = {
        g.position.x, g.position.y, g.position.z, g.opacity,
        g.scale.x, g.scale.y, g.scale.z, g.area,
        g.rotation.x, g.rotation.y, g.rotation.z, g.rotation.w,
        g.sh_dc.r, g.sh_dc.g, g.sh_dc.b, g.sh_dc.a,
        g.sh_1[0].x, g.sh_1[0].y, g.sh_1[0].z,
        g.sh_1[1].x, g.sh_1[1].y, g.sh_1[1].z,
        g.sh_1[2].x, g.sh_1[2].y, g.sh_1[2].z,
        g.normal.x, g.normal.y, g.normal.z, g.stroke_age,
        g.brush_axes.x, g.brush_axes.y,
    };
    static_assert(sizeof(fields) == 124, "GSF v3 float field layout");
    for (uint32_t i = 0; i < 31; i++) {
        encode_float(fields[i], r_bytes + i * 4);
    }
    encode_uint32(g.painterly_meta, r_bytes + 124);
    encode_uint32(g.render_meta, r_bytes + 128);
}

inline Gaussian decode_record(const uint8_t *p_bytes) {
    float fields[31];
    for (uint32_t i = 0; i < 31; i++) {
        fields[i] = decode_float(p_bytes + i * 4);
    }
    Gaussian g;
    g.position = Vector3(fields[0], fields[1], fields[2]);
    g.opacity = fields[3];
    g.scale = Vector3(fields[4], fields[5], fields[6]);
    g.area = fields[7];
    g.rotation = Quaternion(fields[8], fields[9], fields[10], fields[11]);
    g.sh_dc = Color(fields[12], fields[13], fields[14], fields[15]);
    for (uint32_t i = 0; i < 3; i++) {
        g.sh_1[i] = Vector3(fields[16 + i * 3], fields[17 + i * 3], fields[18 + i * 3]);
    }
    g.normal = Vector3(fields[25], fields[26], fields[27]);
    g.stroke_age = fields[28];
    g.brush_axes = Vector2(fields[29], fields[30]);
    g.painterly_meta = decode_uint32(p_bytes + 124);
    g.render_meta = decode_uint32(p_bytes + 128);
    return g;
}

inline Error encode(const ::GaussianData::SaveSnapshot &p_snapshot, PackedByteArray &r_payload) {
    const Vector<Gaussian> &gaussians = p_snapshot.get_gaussians();
    const Vector<Vector3> &high_order = p_snapshot.get_sh_high_order();
    const uint32_t first = p_snapshot.get_sh_first_order_count();
    const uint32_t high = p_snapshot.get_sh_high_order_count();
    ERR_FAIL_COND_V(first > 3 || high > MAX_HIGH_ORDER || p_snapshot.get_sh_degree() != degree_for_layout(first, high), ERR_INVALID_DATA);
    const uint64_t high_elements = uint64_t(gaussians.size()) * high;
    ERR_FAIL_COND_V(uint64_t(high_order.size()) != high_elements, ERR_INVALID_DATA);
    const uint64_t bytes = PREFIX_SIZE + uint64_t(gaussians.size()) * RECORD_SIZE + high_elements * 12;
    ERR_FAIL_COND_V(bytes > UINT32_MAX, ERR_INVALID_DATA);
    if (!gs_resize_or_fail(r_payload, int64_t(bytes), "GaussianSceneSerializer::_write_gaussian_data_chunk payload")) {
        return ERR_OUT_OF_MEMORY;
    }
    uint8_t *w = r_payload.ptrw();
    encode_uint32(gaussians.size(), w);
    encode_uint32(1, w + 4);
    encode_uint32(RECORD_SIZE, w + 8);
    encode_uint32(p_snapshot.get_sh_degree(), w + 12);
    encode_uint32(first, w + 16);
    encode_uint32(high, w + 20);
    encode_uint32(uint32_t(p_snapshot.get_2d_mode()) | (uint32_t(p_snapshot.get_antialiased()) << 1), w + 24);
    encode_uint32(0, w + 28);
    uint64_t offset = PREFIX_SIZE;
    for (int i = 0; i < gaussians.size(); i++) {
        encode_record(gaussians[i], w + offset);
        offset += RECORD_SIZE;
    }
    for (int i = 0; i < high_order.size(); i++) {
        const Vector3 &coefficient = high_order[i];
        encode_float(coefficient.x, w + offset);
        encode_float(coefficient.y, w + offset + 4);
        encode_float(coefficient.z, w + offset + 8);
        offset += 12;
    }
    return OK;
}

// All metadata and the exact byte extent are validated before staging allocation.
inline Error decode(const PackedByteArray &p_payload, uint32_t p_declared_count,
        LocalVector<Gaussian> &r_gaussians, LocalVector<Vector3> &r_high_order,
        uint32_t &r_first, uint32_t &r_high, bool &r_mode_2d, bool &r_antialiased) {
    ERR_FAIL_COND_V(p_payload.size() < PREFIX_SIZE, ERR_FILE_CORRUPT);
    const uint8_t *r = p_payload.ptr();
    const uint32_t count = decode_uint32(r);
    const uint32_t degree = decode_uint32(r + 12);
    const uint32_t first = decode_uint32(r + 16);
    const uint32_t high = decode_uint32(r + 20);
    const uint32_t flags = decode_uint32(r + 24);
    ERR_FAIL_COND_V(count != p_declared_count || decode_uint32(r + 4) != 1 || decode_uint32(r + 8) != RECORD_SIZE, ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V(first > 3 || high > MAX_HIGH_ORDER || degree > 4 || degree != degree_for_layout(first, high), ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V((flags & ~3u) != 0 || decode_uint32(r + 28) != 0, ERR_FILE_CORRUPT);
    const uint64_t high_elements = uint64_t(count) * high;
    const uint64_t expected_size = PREFIX_SIZE + uint64_t(count) * RECORD_SIZE + high_elements * 12;
    ERR_FAIL_COND_V(expected_size != uint64_t(p_payload.size()), ERR_FILE_CORRUPT);
    r_gaussians.resize(count);
    r_high_order.resize(high_elements);
    uint64_t offset = PREFIX_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        r_gaussians[i] = decode_record(r + offset);
        offset += RECORD_SIZE;
    }
    for (uint32_t i = 0; i < r_high_order.size(); i++) {
        r_high_order[i] = Vector3(decode_float(r + offset), decode_float(r + offset + 4), decode_float(r + offset + 8));
        offset += 12;
    }
    r_first = first;
    r_high = high;
    r_mode_2d = flags & 1;
    r_antialiased = flags & 2;
    return OK;
}

} // namespace ScenePayload
} // namespace GaussianSplatting
#endif
