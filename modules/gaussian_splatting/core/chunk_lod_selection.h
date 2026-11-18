#ifndef CHUNK_LOD_SELECTION_H
#define CHUNK_LOD_SELECTION_H

#include "core/templates/local_vector.h"
#include <cstdint>

struct LODConfig;

namespace GaussianSplatting {

struct ChunkLODCandidate {
	uint32_t splat_count = 0;
	float distance = 0.0f;
	uint32_t previous_lod = 0;
	uint32_t previous_target_lod = 0;
	bool visible = false;
};

enum class ChunkLODAdmission {
	NOT_VISIBLE,
	ADMITTED,
	REFUSED_DRAW_CAP,
};

struct ChunkLODSelection {
	uint32_t lod = 0;
	int sh_band_level = 0;
	int splat_skip_factor = 1;
	float opacity_multiplier = 1.0f;
	uint32_t effective_count = 0;
	ChunkLODAdmission admission = ChunkLODAdmission::NOT_VISIBLE;
};

struct ChunkLODPlan {
	// Parallel to the candidates, including non-visible entries in a full LOD scan.
	LocalVector<ChunkLODSelection> selections;
	uint64_t admitted_splats = 0;
	uint32_t refused_visible_chunks = 0;
	uint32_t lod_transitions = 0;
};

// Host-only extraction of StreamingVisibilityController's chunk LOD policy.
// The current policy selects each chunk independently; it does not yet enforce
// p_effective_drawn_splat_cap. Keep that defect observable for #1486's RED proof.
void select_chunk_lods(const LocalVector<ChunkLODCandidate> &p_candidates,
		const LODConfig &p_lod_config, int p_global_sh_band_level,
		uint64_t p_effective_drawn_splat_cap, ChunkLODPlan &r_plan);

} // namespace GaussianSplatting

#endif // CHUNK_LOD_SELECTION_H
