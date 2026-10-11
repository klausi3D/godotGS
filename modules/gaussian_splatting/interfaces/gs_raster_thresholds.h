#ifndef GS_RASTER_THRESHOLDS_H
#define GS_RASTER_THRESHOLDS_H

namespace gs {

// Host mirror of shaders/includes/gs_alpha_threshold.glsl GS_RASTER_ALPHA_THRESHOLD.
// Minimum visible per-splat contribution (tau): an 8-bit channel below this rounds
// to 0, so the splat cannot affect the framebuffer. This is the single host source
// of truth for that threshold. It is a parallel definition of the GLSL constant
// (GLSL cannot share a C++ constexpr) — keep the two numerically equal.
static constexpr float RASTER_ALPHA_THRESHOLD = 1.0f / 255.0f;

// Default screen-space low-pass (px^2 added to the cov2d diagonal in tile
// binning). 0.3 is what every reference adds: Inria forward.cu, gsplat `eps2d`
// (classic and antialiased modes) and aras-p. Classic-trained models were
// optimized against exactly this dilation (#1173). The shader clamps the
// runtime value to [0.05, 2.0]. Single host source for the struct defaults and
// the `rendering/gaussian_splatting/rasterization/low_pass_filter` default.
static constexpr float RASTER_LOW_PASS_FILTER_DEFAULT = 0.3f;

} // namespace gs

#endif // GS_RASTER_THRESHOLDS_H
