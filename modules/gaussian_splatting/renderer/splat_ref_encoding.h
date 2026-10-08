#ifndef GS_SPLAT_REF_ENCODING_H
#define GS_SPLAT_REF_ENCODING_H
#include <cstdint>
namespace gs_splat_ref {
using uint = uint32_t;
#define GS_REF_INLINE inline
#include "../shaders/includes/gs_splat_ref_encoding.glsl"
#undef GS_REF_INLINE
}
#endif
