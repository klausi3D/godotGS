#ifndef SYNTHETIC_SPZ_WRITER_H
#define SYNTHETIC_SPZ_WRITER_H

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

namespace TestGaussianSplatting {

// Authoring values in Godot RUB coordinates. Color is actual RGB; the fixture
// producer converts it to raw SH-DC before calling the pinned Niantic writer.
struct SyntheticSpzSplat {
    Vector3 position;
    float opacity = 1.0f;
    Color color = Color(0.5f, 0.5f, 0.5f, 1.0f);
    Vector3 scale = Vector3(1, 1, 1);
    Quaternion rotation;
    Vector3 sh[24]{};
};

// Real producer output, not a handwritten imitation of the SPZ wire format.
// Niantic's producer uses 12 fractional bits. Versions 2/3 are whole-file gzip;
// version 4 uses independent Zstd streams. Test callers may select SH0-4 and AA.
bool write_synthetic_spz(const String &p_path, const LocalVector<SyntheticSpzSplat> &p_splats,
        uint8_t p_fractional_bits = 12, uint32_t p_version = 4,
        uint8_t p_degree = 0, bool p_antialiased = false, int p_storage_coordinates = -1);

} // namespace TestGaussianSplatting

#endif // SYNTHETIC_SPZ_WRITER_H
