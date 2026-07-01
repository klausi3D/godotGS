#ifndef GS_RASTER_THRESHOLDS_H
#define GS_RASTER_THRESHOLDS_H

namespace gs {

// Host mirror of shaders/includes/gs_alpha_threshold.glsl GS_RASTER_ALPHA_THRESHOLD.
// Minimum visible per-splat contribution (tau): an 8-bit channel below this rounds
// to 0, so the splat cannot affect the framebuffer. This is the single host source
// of truth for that threshold. It is a parallel definition of the GLSL constant
// (GLSL cannot share a C++ constexpr) — keep the two numerically equal.
static constexpr float RASTER_ALPHA_THRESHOLD = 1.0f / 255.0f;

} // namespace gs

#endif // GS_RASTER_THRESHOLDS_H
