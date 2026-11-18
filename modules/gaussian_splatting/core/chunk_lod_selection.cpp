#include "chunk_lod_selection.h"

#include "../lod/lod_config.h"

namespace GaussianSplatting {

void select_chunk_lods(const LocalVector<ChunkLODCandidate> &p_candidates,
		const LODConfig &p_lod_config, int p_global_sh_band_level,
		uint64_t p_effective_drawn_splat_cap, ChunkLODPlan &r_plan) {
	// Phase 1 is a pure extraction: production currently has no drawn-cap admission.
	(void)p_effective_drawn_splat_cap;
	r_plan.selections.resize(p_candidates.size());
	r_plan.admitted_splats = 0;
	r_plan.refused_visible_chunks = 0;
	r_plan.lod_transitions = 0;
	for (uint32_t i = 0; i < p_candidates.size(); i++) {
		const ChunkLODCandidate &candidate = p_candidates[i];
		ChunkLODSelection selection;
		if (p_lod_config.enabled) {
			const int lod_level = p_lod_config.calculate_lod_level(candidate.distance);
			// Preserve both increments in the existing enabled-LOD path.
			if (candidate.previous_lod != uint32_t(lod_level)) {
				r_plan.lod_transitions++;
			}
			selection.lod = lod_level;
			selection.sh_band_level = p_lod_config.get_sh_band_for_lod(lod_level, p_global_sh_band_level);
			selection.splat_skip_factor = p_lod_config.get_splat_skip_factor(lod_level);
			selection.opacity_multiplier = p_lod_config.get_opacity_multiplier(candidate.distance);
			selection.effective_count = candidate.splat_count / selection.splat_skip_factor;
			if (selection.effective_count == 0 && candidate.splat_count > 0) {
				selection.effective_count = 1;
			}
		} else {
			selection.sh_band_level = p_global_sh_band_level;
			selection.effective_count = candidate.splat_count;
		}
		if (candidate.previous_lod != selection.lod || candidate.previous_target_lod != selection.lod) {
			r_plan.lod_transitions++;
		}
		selection.admission = candidate.visible ? ChunkLODAdmission::ADMITTED : ChunkLODAdmission::NOT_VISIBLE;
		if (selection.admission == ChunkLODAdmission::ADMITTED) {
			r_plan.admitted_splats += selection.effective_count;
		}
		r_plan.selections[i] = selection;
	}
}

} // namespace GaussianSplatting
