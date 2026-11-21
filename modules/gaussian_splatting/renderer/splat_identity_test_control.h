#ifndef GS_SPLAT_IDENTITY_TEST_CONTROL_H
#define GS_SPLAT_IDENTITY_TEST_CONTROL_H

#ifdef TESTS_ENABLED
#include <atomic>
#include <cstdint>

// Change only between render submissions. Shader variants sample this on creation.
namespace GSSplatIdentityTestControl {
inline std::atomic<bool> force_address_keying{ false };
inline void set_force_address_keying(bool p_enabled) { force_address_keying.store(p_enabled); }
inline bool get_force_address_keying() { return force_address_keying.load(); }
inline const char *shader_defines() {
	return get_force_address_keying() ? "#define GS_TESTS_ENABLED 1\n#define GS_TEST_ADDRESS_IDENTITY 1\n" : "";
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

// Host mirror of gs_splat_identity.glsl; Python also executes the actual GLSL.
inline Keys keys(uint32_t p_node, uint32_t p_local, uint64_t p_placement, uint32_t p_atlas, uint32_t p_history_size, uint32_t p_row = 0) {
	// Parent-compatible seam: intentionally retain legacy address keys in both modes.
	(void)p_node;
	(void)p_local;
	(void)p_placement;
	const uint32_t size = p_history_size > 0 ? p_history_size : 1u;
	const uint32_t slot = p_row == 0 ? p_atlas % size : hash(p_atlas ^ (p_row * 0x9e3779b9u)) % size;
	const uint32_t key = hash(p_atlas ^ (p_row * 0x85ebca6bu)) & 0x7fffffffu;
	return { p_atlas, slot, key };
}
}
#endif

#endif
