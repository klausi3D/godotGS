#ifndef SYNTHETIC_SPZ_WRITER_H
#define SYNTHETIC_SPZ_WRITER_H

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

namespace TestGaussianSplatting {

// One synthetic splat expressed in ACTIVATED / linear terms (the same space the
// SPZLoader hands back after decode), so a test can reason about round-tripped
// values directly.
struct SyntheticSpzSplat {
    Vector3 position;                  // world position (fixed-point encoded)
    float opacity = 1.0f;              // activated [0,1]; encoded as round(opacity*255)
    Color color = Color(0.5f, 0.5f, 0.5f, 1.0f); // linear RGB [0,1]; encoded as bytes
    Vector3 scale = Vector3(1, 1, 1);  // linear scale (log-encoded to a byte per axis)
    Quaternion rotation;               // identity by default
};

// Write a minimal but VALID Niantic-SPZ v2 (sh_degree 0) file that
// SPZLoader::load_file() accepts: a 16-byte uncompressed header followed by the
// gzip-compressed SoA payload (positions -> alphas -> colors -> scales ->
// rotations). Mirrors synthetic_ply_writer.{h,cpp} in style. Values are
// quantized exactly as the loader expects to decode them, so a handful of splats
// with distinct positions / scales round-trip predictably. Returns true on
// success.
bool write_synthetic_spz(const String &p_path, const LocalVector<SyntheticSpzSplat> &p_splats,
        uint8_t p_fractional_bits = 12);

} // namespace TestGaussianSplatting

#endif // SYNTHETIC_SPZ_WRITER_H
