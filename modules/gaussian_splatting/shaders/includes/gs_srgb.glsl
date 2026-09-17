#ifndef GS_SRGB_GLSL
#define GS_SRGB_GLSL

// Shared sRGB transfer functions for the composite paths.
//
// EXACT piecewise sRGB EOTF (IEC 61966-2-1). Used by every composite that
// injects the splat raster/painterly output -- premultiplied, display-referred,
// sRGB-encoded LDR (contract in interfaces/output_compositor_interfaces.h) --
// into the LINEAR pre-tonemap internal scene buffer. It must be the exact
// inverse of the engine tonemapper's linear_to_srgb encode (tonemap.glsl uses
// the exact piecewise OETF), so 8-bit source content round-trips bit-stably
// through decode -> linear tonemap -> encode. The fast polynomial
// srgb_to_linear in viewport_blit.glsl carries ~0.4% error (~1 LSB), which
// measurably eats 1-LSB margins on this round trip (QA tie-break margin), so
// the source decode must use this function and not the approximation.
//
// sRGB decode does NOT commute with alpha premultiplication: callers holding a
// premultiplied sample must unpremultiply, decode, then re-premultiply.
vec3 srgb_to_linear_exact(vec3 color) {
    vec3 srgb = clamp(color, vec3(0.0), vec3(1.0));
    vec3 low = srgb / 12.92;
    vec3 high = pow((srgb + vec3(0.055)) / 1.055, vec3(2.4));
    return mix(high, low, lessThan(srgb, vec3(0.04045)));
}

#endif // GS_SRGB_GLSL
