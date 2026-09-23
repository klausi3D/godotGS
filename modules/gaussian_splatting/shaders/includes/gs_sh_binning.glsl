// gs_sh_binning.glsl — Spherical harmonics evaluation for tile binning.
//
// Requires: Gaussian struct
// to be defined before inclusion.

#ifndef GS_SH_BINNING_GLSL_INCLUDED
#define GS_SH_BINNING_GLSL_INCLUDED

const uint SH_METADATA_FIRST_ORDER_MASK = 0x000000FFu;
const uint SH_METADATA_HIGH_ORDER_MASK = 0x0000FF00u;
const uint SH_METADATA_ENCODED_COUNT_MASK = 0x00FF0000u;
const uint SH_METADATA_ENCODING_MASK = 0x7F000000u;
const uint SH_METADATA_DC_ENCODING_MASK = 0x80000000u;
// #1054: the unsigned RGB9E5 id (1) is retired; it clamped negative SH to zero.
// Signed storage, mirrored by gs_decode_sh_snorm10() in renderer/gaussian_gpu_layout.h:
// three 10-bit two's-complement integers (bits 0-9 / 10-19 / 20-29) times scale / 511,
// with the per-splat scale in the w lane of the DC vec4 (g.sh_dc.w).
const uint SH_ENCODING_SNORM10_SPLAT_SCALE = 2u;
const float SH_SNORM10_MAX = 511.0;

// Read the number of first-order SH coefficients encoded in metadata.
uint gaussian_get_first_order_count(uint meta) {
    return meta & SH_METADATA_FIRST_ORDER_MASK;
}

// Read the number of higher-order SH coefficients encoded in metadata.
uint gaussian_get_high_order_count(uint meta) {
    return (meta & SH_METADATA_HIGH_ORDER_MASK) >> 8u;
}

// Read the total number of packed SH coefficients stored in metadata.
uint gaussian_get_encoded_count(uint meta) {
    return (meta & SH_METADATA_ENCODED_COUNT_MASK) >> 16u;
}

// Read the SH storage format identifier from metadata.
uint gaussian_get_sh_encoding(uint meta) {
    return (meta & SH_METADATA_ENCODING_MASK) >> 24u;
}

bool gaussian_get_dc_is_linear_rgb(uint meta) {
    return (meta & SH_METADATA_DC_ENCODING_MASK) != 0u;
}

// Decode one signed SH coefficient triplet (bitfieldExtract on a signed int sign-extends).
vec3 decode_sh_snorm10(uint packed, float splat_scale) {
    int word = int(packed);
    ivec3 q = ivec3(bitfieldExtract(word, 0, 10), bitfieldExtract(word, 10, 10), bitfieldExtract(word, 20, 10));
    return vec3(q) * (splat_scale * (1.0 / SH_SNORM10_MAX));
}

// True when the splat carries SH words in a format this decoder does not understand.
// evaluate_sh_with_bands() then falls back to DC only; callers count it (debug counter
// sh_unknown_encoding_count) so the fallback is observable rather than silent.
bool gaussian_sh_encoding_unsupported(uint meta) {
    uint encoding = gaussian_get_sh_encoding(meta);
    return encoding != 0u && encoding != SH_ENCODING_SNORM10_SPLAT_SCALE && gaussian_get_encoded_count(meta) > 0u;
}

// SH basis evaluation constants
const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;
const float SH_C2_0 = 1.0925484305920792;
const float SH_C2_1 = 0.31539156525252005;
const float SH_C2_2 = 0.5462742152960396;
const float SH_C3_0 = 0.5900435899266435;
const float SH_C3_1 = 2.890611442640554;
const float SH_C3_2 = 0.4570457994644658;
const float SH_C3_3 = 0.3731763325901154;

#ifndef GS_DC_LOGIT
#define GS_DC_LOGIT 0
#endif

// -----------------------------------------------------------------
// SH sign convention (ISSUE-038): Condon-Shortley phase included.
//
// This evaluation uses the real spherical harmonics basis with the
// Condon-Shortley (CS) phase factor.  Matches ply_loader.cpp import
// convention and gaussian_splat_common_inc.glsl.  See those files
// for full documentation.
// -----------------------------------------------------------------
//
// Compute SH basis functions up to the specified band level
// basis[0] = DC (l=0)
// basis[1-3] = 1st order (l=1)
// basis[4-8] = 2nd order (l=2)
// basis[9-15] = 3rd order (l=3)
void compute_sh_basis(vec3 dir, uint max_band, out float basis[16]) {
    // Initialize all to zero
    for (int i = 0; i < 16; i++) {
        basis[i] = 0.0;
    }

    float x = dir.x;
    float y = dir.y;
    float z = dir.z;

    // Band 0 (DC) - always computed
    basis[0] = SH_C0;

    if (max_band < 1u) return;

    // Band 1 (1st order)
    basis[1] = -SH_C1 * y;
    basis[2] = SH_C1 * z;
    basis[3] = -SH_C1 * x;

    if (max_band < 2u) return;

    // Band 2 (2nd order)
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;

    basis[4] = SH_C2_0 * x * y;
    basis[5] = -SH_C2_0 * y * z;
    basis[6] = SH_C2_1 * (3.0 * zz - 1.0);
    basis[7] = -SH_C2_0 * x * z;
    basis[8] = SH_C2_2 * (xx - yy);

    if (max_band < 3u) return;

    // Band 3 (3rd order)
    basis[9] = -SH_C3_0 * y * (3.0 * xx - yy);
    basis[10] = SH_C3_1 * x * y * z;
    basis[11] = -SH_C3_2 * y * (1.0 - 5.0 * zz);
    basis[12] = SH_C3_3 * z * (5.0 * zz - 3.0);
    basis[13] = -SH_C3_2 * x * (1.0 - 5.0 * zz);
    basis[14] = SH_C3_1 * z * (xx - yy);
    basis[15] = -SH_C3_0 * x * (xx - 3.0 * yy);
}

// Legacy 1st order basis for backwards compatibility
void compute_sh_basis_1st_order(vec3 dir, out float basis[4]) {
    basis[0] = SH_C0; // DC term
    basis[1] = -SH_C1 * dir.y;
    basis[2] = SH_C1 * dir.z;
    basis[3] = -SH_C1 * dir.x;
}

// Evaluate SH color with configurable band level
// sh_band_level: 0=DC only, 1=1st order, 2=2nd order, 3=3rd order
vec3 evaluate_sh_with_bands(Gaussian g, vec3 view_dir, uint sh_band_level) {
    // Decode DC term. The default path matches the Inria 3DGS convention
    // (`SH2RGB(sh) = sh * C0 + 0.5`), with the C0 factor pre-baked at load.
    // The legacy sigmoid path is retained only for assets explicitly tagged
    // `legacy_bias`; new imports default to `linear_rgb`.
    bool dc_logit = !gaussian_get_dc_is_linear_rgb(g.sh_metadata);
    vec3 color;
    if (dc_logit) {
        vec3 dc_logit_val = g.sh_dc.rgb;
        color = 1.5 * (1.0 / (1.0 + exp(-dc_logit_val))) - 0.25;
    } else {
        color = g.sh_dc.rgb + 0.5;
    }

    // If band level is 0 (DC only), return immediately
    if (sh_band_level == 0u) {
        return color;
    }

    // Check if SH encoding is present
    uint encoding = gaussian_get_sh_encoding(g.sh_metadata);
    if (encoding != SH_ENCODING_SNORM10_SPLAT_SCALE) {
        return color; // No SH data (or unsupported encoding, see gaussian_sh_encoding_unsupported), just use DC
    }
    float splat_scale = g.sh_dc.w;

    uint first_count = gaussian_get_first_order_count(g.sh_metadata);
    uint high_count = gaussian_get_high_order_count(g.sh_metadata);

    // Early exit if no coefficients available
    if (first_count == 0u && high_count == 0u) {
        return color;
    }

    // Compute SH basis for the requested band level
    float basis[16];
    compute_sh_basis(view_dir, sh_band_level, basis);

    // Add first-order SH terms (if available and band >= 1)
    if (sh_band_level >= 1u) {
        uint max_first = min(first_count, 3u);
        for (uint i = 0u; i < max_first; i++) {
            vec3 coeff = decode_sh_snorm10(floatBitsToUint(g.sh_encoded[i]), splat_scale);
            color += coeff * basis[1u + i];
        }
    }

    // Add higher-order SH terms (if available and band >= 2)
    if (sh_band_level >= 2u && high_count > 0u) {
        // Higher order coefficients start after first-order (index 3)
        // They map to basis indices 4+ (2nd order) and 9+ (3rd order)
        uint max_high = min(high_count, 12u);  // Max 12 higher-order coefficients (5 for l=2 + 7 for l=3)

        // Determine how many coefficients to use based on band level
        uint coeff_limit = (sh_band_level == 2u) ? 5u : 12u;  // l=2 has 5 coefficients, l=3 adds 7 more
        max_high = min(max_high, coeff_limit);

        for (uint i = 0u; i < max_high; i++) {
            vec3 coeff = decode_sh_snorm10(floatBitsToUint(g.sh_encoded[first_count + i]), splat_scale);
            color += coeff * basis[4u + i];  // Higher order starts at basis index 4
        }
    }

    return color;
}

// Legacy evaluate function that uses 1st order only (for backwards compatibility)
vec3 evaluate_sh_1st_order(Gaussian g, vec3 view_dir) {
    return evaluate_sh_with_bands(g, view_dir, 1u);
}

#endif // GS_SH_BINNING_GLSL_INCLUDED
