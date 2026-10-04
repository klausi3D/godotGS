#ifndef SYNTHETIC_SPZ_WRITER_H
#define SYNTHETIC_SPZ_WRITER_H

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

namespace TestGaussianSplatting {

// One synthetic splat expressed in the reference encoder's input terms
// (activated opacity, linear scale, raw SH DC coefficient), so a test can reason
// about round-tripped values directly.
struct SyntheticSpzSplat {
    Vector3 position;                  // world position (fixed-point encoded)
    float opacity = 1.0f;              // activated [0,1]; encoded as round(opacity*255)
    // Raw SH DC coefficient per channel, the value a 3DGS PLY stores as f_dc_*
    // (alpha unused). Encoded as the reference does: byte = 255 * (0.5 + 0.15 * f_dc);
    // 0 is mid-grey.
    Color f_dc = Color(0.0f, 0.0f, 0.0f, 1.0f);
    Vector3 scale = Vector3(1, 1, 1);  // linear scale (log-encoded to a byte per axis)
    Quaternion rotation;               // identity by default
};

// Write a minimal but VALID Niantic-SPZ v2 (sh_degree 0) file that
// SPZLoader::load_file() accepts: a 16-byte uncompressed header followed by the
// gzip-compressed SoA payload (positions -> alphas -> colors -> scales ->
// rotations). Mirrors synthetic_ply_writer.{h,cpp} in style. Values are
// quantized the way the Niantic reference encoder (nianticlabs/spz load-spz.cc)
// packs them -- NOT as the inverse of SPZLoader -- so a loader decode that
// disagrees with the format fails the round trip. Returns true on success.
bool write_synthetic_spz(const String &p_path, const LocalVector<SyntheticSpzSplat> &p_splats,
        uint8_t p_fractional_bits = 12);

// Write an SPZ v2 (sh_degree 0) file around a caller-built, UNCOMPRESSED SoA
// payload of p_count splats (19 bytes each: positions, alphas, colours, scales,
// rotations). Lets a test pin exact bytes taken from the reference encoding
// instead of going through write_synthetic_spz's quantizers.
bool write_spz_v2_payload(const String &p_path, uint32_t p_count, const LocalVector<uint8_t> &p_payload,
        uint8_t p_fractional_bits = 12);

} // namespace TestGaussianSplatting

#endif // SYNTHETIC_SPZ_WRITER_H
