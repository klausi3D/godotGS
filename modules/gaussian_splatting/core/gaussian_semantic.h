#pragma once

#include <cstdint>
#include "../shaders/includes/gs_semantic.glsl"

// Authoring currently writes wind_class = 0. Readers ignore that field.
constexpr uint32_t gaussian_pack_semantic(uint16_t p_region, uint8_t p_wind, bool p_valid = true) {
    return uint32_t(p_region) | (uint32_t(p_wind) << GS_SEMANTIC_WIND_SHIFT) |
            (p_valid ? GS_SEMANTIC_VALID_MASK : 0u);
}
constexpr uint16_t gaussian_semantic_region(uint32_t p_word) { return uint16_t(p_word & GS_SEMANTIC_REGION_MASK); }
constexpr uint8_t gaussian_semantic_wind_weight(uint32_t p_word) { return uint8_t((p_word & GS_SEMANTIC_WIND_MASK) >> GS_SEMANTIC_WIND_SHIFT); }
constexpr bool gaussian_semantic_valid(uint32_t p_word) { return (p_word & GS_SEMANTIC_VALID_MASK) != 0; }
constexpr bool gaussian_semantic_validate(uint32_t p_word) { return (p_word & GS_SEMANTIC_RESERVED_MASK) == 0; }
constexpr const char *GAUSSIAN_SEMANTIC_RESERVED_REASON = "semantic_reserved_bits";
