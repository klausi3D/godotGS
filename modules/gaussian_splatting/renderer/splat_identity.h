#ifndef GS_SPLAT_IDENTITY_H
#define GS_SPLAT_IDENTITY_H

#include "gaussian_gpu_layout.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// The high word of the sort key remains the full sortable fp32 depth. Only
// its existing low uint32 is partitioned. Counts are immutable bake/registered
// topology counts, never the visible set or the resident page allocation.
namespace gs_splat_identity {
constexpr uint32_t MIN_LOCAL_BITS = 14;
inline uint32_t bits_for_count(uint64_t p_count) {
	uint32_t bits = 0;
	for (uint64_t maximum = p_count > 0 ? p_count - 1 : 0; maximum; maximum >>= 1) { ++bits; }
	return bits;
}
struct Layout {
	uint32_t r = 0, c = 0, l = MIN_LOCAL_BITS;
	bool fits() const { return 1u + r + c + l <= 32u; }
	String refusal() const {
		return fits() ? String() : vformat("splat_identity_instance_tie_overflow:R=%d,C=%d,L=%d", r, c, l);
	}
};
inline Layout instance_layout(uint32_t p_registered_rows, uint32_t p_max_chunks, uint32_t p_max_chunk_splats) {
	Layout layout;
	layout.r = bits_for_count(uint64_t(p_registered_rows) + 1u); // ranks 1..rows; 0 is reserved.
	layout.c = bits_for_count(p_max_chunks);
	layout.l = MAX(MIN_LOCAL_BITS, bits_for_count(p_max_chunk_splats));
	return layout;
}
inline uint32_t world_local_bits(uint32_t p_max_node_splats) {
	return MAX(MIN_LOCAL_BITS, bits_for_count(p_max_node_splats));
}
struct WorldLayout {
	uint32_t l = MIN_LOCAL_BITS;
	bool node_id_folded = false;
	bool fits() const { return l <= 31u; }
	uint32_t node_mask() const { return fits() ? (0x7fffffffu >> l) : 0u; }
	// Mirror of the world branch of gs_splat_composite_key. At L=14, node
	// IDs differing by 2^17 collide; the local field and zero prefix survive.
	uint32_t sort_key(uint32_t p_node, uint32_t p_local) const {
		return fits() ? ((p_node & node_mask()) << l) | p_local : 0u;
	}
	String refusal() const {
		return fits() ? String() : vformat("splat_identity_world_local_tie_overflow:L=%d", l);
	}
	void append_stats(Dictionary &r_stats) const {
		r_stats["tie_world_node_folded"] = node_id_folded;
	}
};
inline WorldLayout world_layout(uint32_t p_nodes, uint32_t p_max_node_splats) {
	WorldLayout layout;
	layout.l = world_local_bits(p_max_node_splats);
	layout.node_id_folded = layout.fits() && uint64_t(p_nodes) > uint64_t(layout.node_mask()) + 1u;
	return layout;
}
// Unfolded budget diagnostic retained for existing test vectors. Production
// admission uses WorldLayout: node-count overflow folds rather than refusing.
inline String world_refusal(uint32_t p_nodes, uint32_t p_max_node_splats) {
	const uint32_t n = bits_for_count(p_nodes), l = world_local_bits(p_max_node_splats);
	return 1u + n + l <= 32u ? String() : vformat("splat_identity_world_tie_overflow:N=%d,L=%d", n, l);
}
// Low six bits retain the instance flags. The local-width/singleton fields fit
// below bit 14; the ordered prefix starts at C+L >= 14. No ABI stride change.
inline uint32_t instance_sort_flags(uint32_t p_rank, const Layout &p_layout, bool p_single_chunk) {
	if (!p_layout.fits()) { return 0; } // Refused rows are omitted before publication.
	return 0x80000000u | (p_rank << (p_layout.c + p_layout.l)) |
			(p_layout.l << GS_INSTANCE_SORT_LOCAL_SHIFT) | (p_single_chunk ? GS_INSTANCE_SORT_SINGLE_CHUNK : 0u);
}
static_assert((GS_INSTANCE_SORT_PREFIX_MASK & (GS_INSTANCE_FLAG_SHADOW_PASS | GS_INSTANCE_TEST_OVERRIDE_BY_ADDRESS | GS_INSTANCE_TEST_HIDE_ONLY_IN_RASTER)) == 0, "Sort prefix overlaps instance flags");
static_assert((31u << GS_INSTANCE_SORT_LOCAL_SHIFT) < (1u << MIN_LOCAL_BITS), "Local width overlaps sort prefix");
} // namespace gs_splat_identity

#endif
