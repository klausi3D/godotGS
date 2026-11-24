#ifndef GS_SPLAT_IDENTITY_TEST_CONTROL_H
#define GS_SPLAT_IDENTITY_TEST_CONTROL_H

#ifdef TESTS_ENABLED
#include "core/string/ustring.h"
#include "splat_identity.h"
#include "core/templates/rid.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

// GS-GAME-1 readback ABI. The existing layout guard compares this test-only
// host declaration with every GLSL mirror, including offsets and scalar kinds.
struct alignas(16) GSGame1Sample {
    uint32_t key[4];
    uint32_t meta[4];
    float rest[4];
    float colour[4];
    float screen[4];
    float tint[4];
    float shadow[4];
    float shadow_anchor[4];
    float light_0[4];
    float light_1[4];
    float light_2[4];
    float light_3[4];
    float viewport[4];
    float local_rest[4];
};
static_assert(sizeof(GSGame1Sample) == 224, "GS-GAME-1 capture stride");
static_assert(offsetof(GSGame1Sample, meta) == 16, "GS-GAME-1 epoch offsets");
static_assert(offsetof(GSGame1Sample, local_rest) == 208, "GS-GAME-1 local position offset");

// Change only between render submissions. Shader variants sample this on creation.
namespace GSSplatIdentityTestControl {
inline std::atomic<bool> force_address_keying{ false };
inline void set_force_address_keying(bool p_enabled) { force_address_keying.store(p_enabled); }
inline bool get_force_address_keying() { return force_address_keying.load(); }
// All fields are off by default. The dispatch lane changes them only on the
// render thread between submissions; atomics also make diagnostics race-free.
// Borrowed test capture RID; one renderer owns it. Only the render thread may
// publish/consume this handle; its owner clears it before freeing the resource.
inline constexpr uint32_t capture_header_bytes = 16;
inline constexpr uint32_t capture_max_samples = 64;
inline constexpr uint32_t capture_colour_frame_offset = capture_header_bytes + 3 * sizeof(uint32_t);
inline constexpr uint32_t capture_shadow_frame_offset = capture_header_bytes + sizeof(GSGame1Sample) + 3 * sizeof(uint32_t);
inline RID capture_buffer;
inline uint64_t capture_device_id = 0;
inline std::atomic<bool> force_address_wind_phase{ false };
inline std::atomic<bool> force_coarse_fade_zero{ false };
inline std::atomic<bool> force_caster_without_deformation{ false };
inline std::atomic<bool> force_legacy_wind_weight{ false };
inline std::atomic<bool> capture_enabled{ false };
inline std::atomic<bool> expected_region_key{ false };
inline std::atomic<bool> force_nonresident_draw{ false };
inline std::atomic<bool> disable_cache_invalidation{ false };
inline std::atomic<bool> override_by_address{ false };
inline std::atomic<bool> hide_only_in_raster{ false };
inline std::atomic<bool> asset_set_reset{ false };
inline std::atomic<bool> rows_after_shadow{ false };
// Per-view caster lists (hlod_shadow_face_cull.h): draw every view from the whole list (the reference), or cull with
// zero guard band and centre-only boxes (a deliberately unsafe negative control a depth comparison must detect).
// GS_TEST_SHADOW_VIEW_LISTS=whole starts a test binary on the whole-list reference, so lanes without the GS-GAME-1
// bridge (GS-COMBINED-1, runtime profiles) can be compared against it in one binary.
inline bool shadow_view_lists_env_whole() {
    const char *value = std::getenv("GS_TEST_SHADOW_VIEW_LISTS");
    return value != nullptr && std::strcmp(value, "whole") == 0;
}
inline std::atomic<bool> shadow_view_lists_disabled{ shadow_view_lists_env_whole() };
inline std::atomic<bool> shadow_view_lists_unsafe{ false };
inline void set_force_address_wind_phase(bool p_enabled) { force_address_wind_phase.store(p_enabled); }
inline bool get_force_address_wind_phase() { return force_address_wind_phase.load(); }
inline uint32_t shader_variant() {
    return uint32_t(get_force_address_keying()) | (uint32_t(get_force_address_wind_phase()) << 1) |
            (uint32_t(force_coarse_fade_zero.load()) << 2) | (uint32_t(force_caster_without_deformation.load()) << 3) |
            (uint32_t(force_legacy_wind_weight.load()) << 4) | (uint32_t(capture_enabled.load()) << 5) |
            (uint32_t(expected_region_key.load()) << 6);
}
inline String shader_defines() {
    String result = "#define GS_TESTS_ENABLED 1\n#define GS_TEST_REGION_CONTROLS 1\n";
    if (get_force_address_keying()) { result += "#define GS_TEST_ADDRESS_IDENTITY 1\n"; }
    if (get_force_address_wind_phase()) { result += "#define GS_TEST_ADDRESS_WIND_PHASE 1\n"; }
    if (force_coarse_fade_zero.load()) { result += "#define GS_TEST_COARSE_FADE_ZERO 1\n"; }
    if (force_caster_without_deformation.load()) { result += "#define GS_TEST_CASTER_WITHOUT_DEFORMATION 1\n"; }
    if (force_legacy_wind_weight.load()) { result += "#define GS_TEST_LEGACY_WIND_WEIGHT 1\n"; }
    if (capture_enabled.load()) { result += "#define GS_GAME_1_CAPTURE 1\n"; }
    if (expected_region_key.load()) { result += "#define GS_GAME_1_EXPECTED_REGION_KEY 1\n"; }
    return result;
}

struct Keys {
	uint32_t tie_break;
	uint32_t history_slot;
	uint32_t history_key;
};

inline uint32_t hash(uint32_t p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	return p_value ^ (p_value >> 16u);
}

inline String world_layout_refusal(uint32_t p_nodes, uint32_t p_splats) { return gs_splat_identity::world_refusal(p_nodes, p_splats); }
inline String layout_refusal(uint32_t p_rows, uint32_t p_chunks, uint32_t p_splats) {
    return gs_splat_identity::instance_layout(p_rows, p_chunks, p_splats).refusal();
}

// Host mirror of gs_splat_identity.glsl; Python also executes the actual GLSL.
inline Keys keys(uint32_t p_node, uint32_t p_local, uint64_t p_placement, uint32_t p_atlas, uint32_t p_history_size, uint32_t p_row = 0, uint32_t p_prefix = 0, uint32_t p_bits = 14, bool p_single = false) {

	(void)p_history_size; // Production rejects an out-of-range atlas slot before reading.
	if (get_force_address_keying()) {
		const uint32_t slot = p_atlas;
		const uint32_t key = hash(p_atlas ^ (p_row * 0x85ebca6bu)) & 0x7fffffffu;
		return { p_atlas, slot, key };
	}
	uint32_t h = hash(p_node ^ 0x9e3779b9u);
	h = hash(h ^ p_local);
	h = hash(h ^ uint32_t(p_placement));
	h = hash(h ^ uint32_t(p_placement >> 32u));
	return { p_single ? p_atlas : (p_prefix | (p_node << p_bits) | p_local), p_atlas, h & 0x7fffffffu };
}
}
#endif

#endif
