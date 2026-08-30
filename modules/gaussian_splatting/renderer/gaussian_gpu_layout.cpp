#include "gaussian_gpu_layout.h"
#include "float16_utils.h"
#include "gpu_debug_utils.h"

#include "../core/streaming_quantization.h"

#include "../core/gs_project_settings.h"
#include "../logger/gs_logger.h"
#include "../logger/gs_debug_trace.h"
#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"
#include <algorithm>
#include <cstring>

namespace {
static bool _is_data_log_enabled() { return gs::settings::is_data_log_enabled(); }

static float bits_to_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(uint32_t));
    return value;
}

static uint32_t encode_rgb9e5(const Vector3 &value) {
    const float max_channel = 65408.0f; // Maximum representable value in RGB9E5
    float r = CLAMP(value.x, 0.0f, max_channel);
    float g = CLAMP(value.y, 0.0f, max_channel);
    float b = CLAMP(value.z, 0.0f, max_channel);

    float max_component = MAX(MAX(r, g), b);
    if (max_component < 1.5258789e-5f) { // 2^-16
        return 0;
    }

    float exponent = Math::floor(Math::log(max_component) / Math::log(2.0f));
    int exp_shared = int(exponent) + 1 + 15; // Bias of 15
    exp_shared = CLAMP(exp_shared, 0, 31);

    float denom = Math::pow(2.0f, float(exp_shared - 15 - 9));
    int rm = int(Math::round(r / denom));
    int gm = int(Math::round(g / denom));
    int bm = int(Math::round(b / denom));

    if (rm > 511 || gm > 511 || bm > 511) {
        exp_shared = MIN(exp_shared + 1, 31);
        denom = Math::pow(2.0f, float(exp_shared - 15 - 9));
        rm = int(Math::round(r / denom));
        gm = int(Math::round(g / denom));
        bm = int(Math::round(b / denom));
    }

    rm = CLAMP(rm, 0, 511);
    gm = CLAMP(gm, 0, 511);
    bm = CLAMP(bm, 0, 511);

    return (uint32_t(exp_shared) << 27) | (uint32_t(bm) << 18) | (uint32_t(gm) << 9) | uint32_t(rm);
}

} // namespace

void PackedSphericalHarmonics::clear() {
    for (int i = 0; i < 4; i++) {
        dc[i] = 0.0f;
    }
    for (uint32_t i = 0; i < MAX_ENCODED_COEFFICIENTS; i++) {
        encoded[i] = 0.0f;
    }
}

void pack_gaussian(const Gaussian &src,
        PackedGaussian &dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        uint32_t coefficient_limit) {
    dst.position[0] = src.position.x;
    dst.position[1] = src.position.y;
    dst.position[2] = src.position.z;
    dst.opacity = src.opacity;

    dst.scale[0] = src.scale.x;
    dst.scale[1] = src.scale.y;
    dst.scale[2] = src.scale.z;

    // Zero the alignment padding EXPLICITLY. PackedGaussian is trivially
    // constructible and these records are produced through Vector::resize, so
    // the storage arrives uninitialized. `dst.area` used to write offset 28 as a
    // side effect; with the lane removed nothing did, leaving an indeterminate
    // word that (a) is hashed by _packed_gaussian_payload_checksum() in both
    // gaussian_streaming.cpp and streaming_upload_pipeline.cpp -- where it gates
    // upload verification -- making identical payloads compare unequal, and
    // (b) is memcpy'd into the GPU buffer on every unquantized upload.
    dst._pad_rotation_align = 0u;

    dst.rotation[0] = src.rotation.x;
    dst.rotation[1] = src.rotation.y;
    dst.rotation[2] = src.rotation.z;
    dst.rotation[3] = src.rotation.w;
    // Sample a few SH DC values for debug trace (avoids per-gaussian logging).
    static int pack_debug_count = 0;
    if (++pack_debug_count <= 5) {
        GaussianSplatting::debug_trace_record_pack_sh(src.sh_dc, src.opacity);
    }

    dst.sh.clear();
    dst.sh.dc[0] = src.sh_dc.r;
    dst.sh.dc[1] = src.sh_dc.g;
    dst.sh.dc[2] = src.sh_dc.b;
    dst.sh.dc[3] = src.sh_dc.a;

    uint32_t encoded_capacity = MIN<uint32_t>(coefficient_limit, PackedSphericalHarmonics::MAX_ENCODED_COEFFICIENTS);
    uint32_t first_count = MIN<uint32_t>(first_order_count, 3u);
    uint32_t stored_first = MIN<uint32_t>(first_count, encoded_capacity);
    uint32_t encoded_total = 0;

    for (uint32_t i = 0; i < stored_first; i++) {
        uint32_t packed = encode_rgb9e5(src.sh_1[i]);
        dst.sh.encoded[encoded_total++] = bits_to_float(packed);
    }

    uint32_t stored_high = 0;
    if (higher_order_count > 0 && encoded_total < encoded_capacity) {
        stored_high = MIN<uint32_t>(higher_order_count, encoded_capacity - encoded_total);
        for (uint32_t i = 0; i < stored_high; i++) {
            Vector3 coeff = higher_order_coeffs ? higher_order_coeffs[i] : Vector3();
            uint32_t packed = encode_rgb9e5(coeff);
            dst.sh.encoded[encoded_total++] = bits_to_float(packed);
        }
    }

    dst.normal[0] = src.normal.x;
    dst.normal[1] = src.normal.y;
    dst.normal[2] = src.normal.z;


    // DEBUG: Log first gaussian packing.
    static bool logged_once = false;
    if (_is_data_log_enabled() && !logged_once) {
        GS_LOG_RENDERER_DEBUG(vformat("[GPU Pack] First gaussian: first_order_count=%d, stored_first=%d, stored_high=%d, encoded_total=%d",
            first_order_count, stored_first, stored_high, encoded_total));
        GS_LOG_RENDERER_DEBUG(vformat("[GPU Pack] First gaussian: src.sh_1[0] = (%f, %f, %f)",
            src.sh_1[0].x, src.sh_1[0].y, src.sh_1[0].z));
    }

    dst.sh_metadata = gs_pack_sh_metadata(
            stored_first,
            stored_high,
            encoded_total,
            gaussian_get_dc_encoding(src.render_meta),
            GS_SH_ENCODING_RGB9E5);

    if (_is_data_log_enabled() && !logged_once) {
        GS_LOG_RENDERER_DEBUG(vformat("[GPU Pack] sh_metadata = 0x%08X", dst.sh_metadata));
        logged_once = true;
    }

    metrics.raw_bytes += sizeof(Color);
    if (encoded_total > 0) {
        metrics.raw_bytes += sizeof(Vector3) * encoded_total;
    }
    metrics.compressed_bytes += sizeof(dst.sh.dc) + sizeof(float) * encoded_total;
    metrics.coefficient_count += encoded_total;
}

namespace {
// Non-finite inputs must never poison the quantized packer: quantize_position/scale
// pass NaN straight through CLAMP (NaN compares false against both bounds), and a
// subsequent NaN->uint32 cast is UB; a NaN half-float rotation also becomes a NaN
// after the shader's normalize(). Floor every non-finite component to a deterministic
// fallback, mirroring gaussian_importance()'s flooring semantics.
static inline float sanitize_finite(float v, float fallback) {
    return Math::is_finite(v) ? v : fallback;
}
static inline Vector3 sanitize_finite_vec3(const Vector3 &v, const Vector3 &fallback) {
    return Vector3(sanitize_finite(v.x, fallback.x),
            sanitize_finite(v.y, fallback.y),
            sanitize_finite(v.z, fallback.z));
}
} // namespace

// Number of RGB9E5 higher-order SH slots in PackedGaussianQuantized.sh_encoded.
// The GLSL side synthesizes sh_metadata as gs_build_quantized_sh_metadata(6u, ...),
// so the layout is a fixed 6-slot array; unused slots are zeroed (RGB9E5(0) decodes
// to vec3(0), contributing nothing, and the per-chunk sh_limit still gates bands).
static constexpr uint32_t GS_QUANTIZED_SH_ENCODED_SLOTS = 6u;

void pack_gaussian_quantized(const Gaussian &src,
        const ChunkQuantizationInfo &chunk_quant,
        uint16_t chunk_id,
        PackedGaussianQuantized &dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        uint32_t coefficient_limit) {
    // Position: per-chunk quantized to position_bits. Must bit-match the GLSL
    // dequantize_position (min + q * (1/((1<<bits)-1)) * range). quantize_position()
    // owns the normalization + rounding; we only sanitize and narrow to uint16.
    const Vector3 safe_position = sanitize_finite_vec3(src.position, chunk_quant.position_min);
    uint32_t qx = 0, qy = 0, qz = 0;
    chunk_quant.quantize_position(safe_position, qx, qy, qz);
    dst.quantized_position[0] = uint16_t(qx);
    dst.quantized_position[1] = uint16_t(qy);
    dst.quantized_position[2] = uint16_t(qz);
    dst.chunk_id = chunk_id;

    // Opacity: FP32, kept exact (precision-critical; never quantized).
    dst.opacity = sanitize_finite(src.opacity, 0.0f);

    // Scale: per-chunk quantized when the chunk enables it, else zeros (the GLSL
    // returns vec3(1.0) when scale_bits==0, so the stored value is irrelevant then).
    const Vector3 safe_scale = sanitize_finite_vec3(src.scale, chunk_quant.scale_min);
    uint32_t sx = 0, sy = 0, sz = 0;
    chunk_quant.quantize_scale(safe_scale, sx, sy, sz);
    dst.quantized_scale[0] = uint16_t(sx);
    dst.quantized_scale[1] = uint16_t(sy);
    dst.quantized_scale[2] = uint16_t(sz);

    // Area: single FP16 (GLSL extract_area unpacks the high half-word of scale_area_hi).
    dst.area_fp16 = Float16Utils::float_to_half(sanitize_finite(src.area, 0.0f));

    // Rotation quaternion: 4x FP16 (GLSL normalizes on read). A non-finite quaternion
    // floors to identity so the shader's normalize() cannot produce NaN.
    Quaternion safe_rotation = src.rotation;
    if (!(Math::is_finite(safe_rotation.x) && Math::is_finite(safe_rotation.y) &&
                Math::is_finite(safe_rotation.z) && Math::is_finite(safe_rotation.w))) {
        safe_rotation = Quaternion();
    }
    dst.rotation[0] = Float16Utils::float_to_half(safe_rotation.x);
    dst.rotation[1] = Float16Utils::float_to_half(safe_rotation.y);
    dst.rotation[2] = Float16Utils::float_to_half(safe_rotation.z);
    dst.rotation[3] = Float16Utils::float_to_half(safe_rotation.w);

    dst._pre_sh_padding[0] = 0;
    dst._pre_sh_padding[1] = 0;

    // SH DC: FP32, kept exact (precision-critical; matches the shader's direct read).
    dst.sh_dc[0] = sanitize_finite(src.sh_dc.r, 0.0f);
    dst.sh_dc[1] = sanitize_finite(src.sh_dc.g, 0.0f);
    dst.sh_dc[2] = sanitize_finite(src.sh_dc.b, 0.0f);
    dst.sh_dc[3] = sanitize_finite(src.sh_dc.a, 0.0f);

    // Higher-order SH: RGB9E5 into the fixed 6-slot array, same selection order as
    // pack_gaussian (first-order coeffs then higher-order), stored as raw uint32 (the
    // shader bitcasts these back via uintBitsToFloat). encode_rgb9e5 clamps internally.
    for (uint32_t i = 0; i < GS_QUANTIZED_SH_ENCODED_SLOTS; i++) {
        dst.sh_encoded[i] = 0u;
    }
    const uint32_t encoded_capacity = MIN<uint32_t>(coefficient_limit, GS_QUANTIZED_SH_ENCODED_SLOTS);
    const uint32_t first_count = MIN<uint32_t>(first_order_count, 3u);
    const uint32_t stored_first = MIN<uint32_t>(first_count, encoded_capacity);
    uint32_t encoded_total = 0;
    for (uint32_t i = 0; i < stored_first; i++) {
        dst.sh_encoded[encoded_total++] = encode_rgb9e5(src.sh_1[i]);
    }
    uint32_t stored_high = 0;
    if (higher_order_count > 0 && encoded_total < encoded_capacity) {
        stored_high = MIN<uint32_t>(higher_order_count, encoded_capacity - encoded_total);
        for (uint32_t i = 0; i < stored_high; i++) {
            const Vector3 coeff = higher_order_coeffs ? higher_order_coeffs[i] : Vector3();
            dst.sh_encoded[encoded_total++] = encode_rgb9e5(coeff);
        }
    }

    // Normal + stroke_age: two half2 words (GLSL extract_normal / extract_stroke_age).
    const Vector3 safe_normal = sanitize_finite_vec3(src.normal, Vector3());
    dst.normal_xy = Float16Utils::pack_half2(safe_normal.x, safe_normal.y);
    dst.normal_z_stroke = Float16Utils::pack_half2(safe_normal.z, sanitize_finite(src.stroke_age, 0.0f));

    metrics.raw_bytes += sizeof(Gaussian);
    metrics.compressed_bytes += sizeof(PackedGaussianQuantized);
    metrics.coefficient_count += encoded_total;
}

void pack_gaussians_range_quantized(const LocalVector<Gaussian> &src,
        uint32_t start,
        uint32_t count,
        const ChunkQuantizationInfo &chunk_quant,
        uint16_t chunk_id,
        PackedGaussianQuantized *dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        uint32_t coefficient_limit) {
    ERR_FAIL_NULL_MSG(dst, "pack_gaussians_range_quantized destination is null");
    ERR_FAIL_COND_MSG((uint64_t)start + (uint64_t)count > (uint64_t)src.size(), "pack_gaussians_range_quantized out of bounds");
    for (uint32_t i = 0; i < count; i++) {
        // Match the sibling range packers: the per-splat higher-order SH block is indexed
        // by the SOURCE splat (start + i), not the destination slot i.
        const Vector3 *coeff_ptr = higher_order_coeffs
                ? higher_order_coeffs + size_t(start + i) * higher_order_count
                : nullptr;
        pack_gaussian_quantized(src[start + i], chunk_quant, chunk_id, dst[i], metrics,
                coeff_ptr, first_order_count, higher_order_count, coefficient_limit);
    }
}

// #787: Vector::resize() reports allocation failure through its return value and leaves the
// vector at its previous (smaller) size -- see CowData::resize()/_fork_allocate(), which return
// ERR_OUT_OF_MEMORY before touching the size. Ignoring that return and then writing dst.write[i]
// walks straight off the end, and CRASH_BAD_INDEX in core/templates/vector.h traps the whole
// process via __fastfail(7). That is exactly how the nightly GPU streaming lane died
// (0xC0000409, subcode 7, symbolized to VectorWriteProxy<PackedGaussian>::operator[]).
//
// Fail closed instead: report the sizes that failed -- which is the diagnostic the crash itself
// could never deliver -- and leave the output EMPTY, matching the existing "no payload" failure
// contract of the pack callers, so nobody consumes a partially packed chunk.
template <typename T>
static bool _reserve_pack_output(Vector<T> &p_dst, uint32_t p_count, const char *p_where) {
    if (p_dst.resize(p_count) == OK) {
        return true;
    }
    p_dst.clear();
    ERR_FAIL_V_MSG(false,
            vformat("%s: failed to allocate %d entries of %d bytes (%d bytes total); "
                    "leaving the output empty instead of writing out of bounds.",
                    String(p_where), p_count, (int64_t)sizeof(T),
                    (int64_t)p_count * (int64_t)sizeof(T)));
}

bool pack_gaussians_range(const LocalVector<Gaussian> &src,
        uint32_t start,
        uint32_t count,
        Vector<PackedGaussian> &dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        uint32_t coefficient_limit) {
    static int pack_range_call_count = 0;
    if (++pack_range_call_count <= 3) {
        if (src.size() > 0 && start < src.size()) {
            const Gaussian &g0 = src[start];
            GaussianSplatting::debug_trace_record_pack_range(count, start, src.size(), g0.sh_dc, g0.opacity);
        } else {
            GaussianSplatting::debug_trace_record_pack_range(count, start, src.size(), Color(), 0.0f);
        }
    }
    if (count == 0) {
        dst.clear();
        return true;
    }

    ERR_FAIL_COND_V_MSG((uint64_t)start + (uint64_t)count > (uint64_t)src.size(), false, "pack_gaussians_range out of bounds");

    if (!_reserve_pack_output(dst, count, "pack_gaussians_range")) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const Vector3 *coeff_ptr = nullptr;
        if (higher_order_coeffs && higher_order_count > 0) {
            coeff_ptr = higher_order_coeffs + (size_t)(start + i) * higher_order_count;
        }
        pack_gaussian(src[start + i], dst.write[i], metrics, coeff_ptr, first_order_count, higher_order_count, coefficient_limit);
    }
    return true;
}

void pack_gaussians_range_raw(const LocalVector<Gaussian> &src,
        uint32_t start,
        uint32_t count,
        PackedGaussian *dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        uint32_t coefficient_limit) {
    if (count == 0 || dst == nullptr) {
        return;
    }

    ERR_FAIL_COND_MSG((uint64_t)start + (uint64_t)count > (uint64_t)src.size(), "pack_gaussians_range_raw out of bounds");

    for (uint32_t i = 0; i < count; i++) {
        const Vector3 *coeff_ptr = nullptr;
        if (higher_order_coeffs && higher_order_count > 0) {
            coeff_ptr = higher_order_coeffs + (size_t)(start + i) * higher_order_count;
        }
        pack_gaussian(src[start + i], dst[i], metrics, coeff_ptr, first_order_count, higher_order_count, coefficient_limit);
    }
}

void pack_gaussians_range_raw(const Gaussian *src,
        uint32_t start,
        uint32_t count,
        PackedGaussian *dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        uint32_t coefficient_limit) {
    if (count == 0 || dst == nullptr || src == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const Vector3 *coeff_ptr = nullptr;
        if (higher_order_coeffs && higher_order_count > 0) {
            coeff_ptr = higher_order_coeffs + (size_t)(start + i) * higher_order_count;
        }
        pack_gaussian(src[start + i], dst[i], metrics, coeff_ptr, first_order_count, higher_order_count, coefficient_limit);
    }
}

bool pack_gaussians_range_limited(const LocalVector<Gaussian> &src,
        uint32_t start,
        uint32_t count,
        Vector<PackedGaussian> &dst,
        SHCompressionMetrics &metrics,
        const Vector3 *higher_order_coeffs,
        uint32_t first_order_count,
        uint32_t higher_order_count,
        const uint8_t *coefficient_limits,
        uint32_t coefficient_limit) {
    if (count == 0) {
        dst.clear();
        return true;
    }

    ERR_FAIL_COND_V_MSG((uint64_t)start + (uint64_t)count > (uint64_t)src.size(), false, "pack_gaussians_range_limited out of bounds");

    if (!_reserve_pack_output(dst, count, "pack_gaussians_range_limited")) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const Vector3 *coeff_ptr = nullptr;
        if (higher_order_coeffs && higher_order_count > 0) {
            coeff_ptr = higher_order_coeffs + (size_t)(start + i) * higher_order_count;
        }
        uint32_t limit = coefficient_limit;
        if (coefficient_limits) {
            limit = MIN<uint32_t>(limit, coefficient_limits[i]);
        }
        pack_gaussian(src[start + i], dst.write[i], metrics, coeff_ptr, first_order_count, higher_order_count, limit);
    }
    return true;
}
