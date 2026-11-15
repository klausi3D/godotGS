#ifndef GS_OVERLAP_DROP_ACCOUNTING_H
#define GS_OVERLAP_DROP_ACCOUNTING_H
// Host view of shaders/includes/gs_overlap_drop_accounting.glsl: the same source text the tile
// rasterizers compile (#1457, #1461), so host tests run the shaders' drop arithmetic.
#include <cstdint>
namespace gs_overlap_drop {
using uint = uint32_t;
#define GS_DROP_INLINE inline
#include "../shaders/includes/gs_overlap_drop_accounting.glsl"
#undef GS_DROP_INLINE
} // namespace gs_overlap_drop
#endif
