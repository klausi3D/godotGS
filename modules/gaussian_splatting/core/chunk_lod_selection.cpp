#include "chunk_lod_selection.h"

#include "../lod/lod_config.h"
#include <algorithm>
#include <limits>

namespace GaussianSplatting {

void select_chunk_lods(const LocalVector<ChunkLODCandidate> &p_candidates,
		const LODConfig &p_lod_config, int p_global_sh_band_level,
		uint64_t p_effective_drawn_splat_cap, ChunkLODPlan &r_plan) {
	r_plan.selections.resize(p_candidates.size());
	r_plan.admitted_splats = 0;
	r_plan.coarsened_visible_chunks = 0;
	r_plan.refused_visible_chunks = 0;
	r_plan.lod_transitions = 0;
	for (uint32_t i = 0; i < p_candidates.size(); i++) {
		const ChunkLODCandidate &candidate = p_candidates[i];
		ChunkLODSelection selection;
		if (candidate.has_nominal_selection) {
			selection = candidate.nominal_selection;
		} else if (p_lod_config.enabled) {
			const int lod_level = p_lod_config.calculate_lod_level(candidate.distance);
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
		selection.admission = candidate.visible ? ChunkLODAdmission::ADMITTED : ChunkLODAdmission::NOT_VISIBLE;
		if (selection.admission == ChunkLODAdmission::ADMITTED) {
			r_plan.admitted_splats += uint64_t(selection.effective_count) * candidate.draw_instance_count;
		}
		r_plan.selections[i] = selection;
	}

	if (r_plan.admitted_splats > p_effective_drawn_splat_cap) {
		LocalVector<uint32_t> priority;
		for (uint32_t i = 0; i < p_candidates.size(); i++) {
			if (p_candidates[i].visible && p_candidates[i].draw_instance_count > 0 && r_plan.selections[i].effective_count > 0) {
				priority.push_back(i);
			}
		}
		// Lowest priority first: farthest, then highest candidate index. Normalize
		// non-finite distances to retain a strict total order.
		std::sort(priority.ptr(), priority.ptr() + priority.size(), [&](uint32_t a, uint32_t b) {
			const float da = Math::is_finite(p_candidates[a].distance) ? p_candidates[a].distance : std::numeric_limits<float>::infinity();
			const float db = Math::is_finite(p_candidates[b].distance) ? p_candidates[b].distance : std::numeric_limits<float>::infinity();
			return da != db ? da > db : a > b;
		});
		for (uint32_t i : priority) {
			const ChunkLODCandidate &candidate = p_candidates[i];
			ChunkLODSelection &selection = r_plan.selections[i];
			bool coarsened = false;
			for (int lod = int(selection.lod) + 1; p_lod_config.enabled && lod < p_lod_config.num_levels &&
					r_plan.admitted_splats > p_effective_drawn_splat_cap; lod++) {
				const int skip = p_lod_config.get_splat_skip_factor(lod);
				const uint32_t count = MAX(candidate.splat_count / skip, candidate.splat_count > 0 ? 1u : 0u);
				if (count >= selection.effective_count) {
					continue;
				}
				r_plan.admitted_splats -= uint64_t(selection.effective_count - count) * candidate.draw_instance_count;
				selection.lod = lod;
				selection.splat_skip_factor = skip;
				selection.sh_band_level = p_lod_config.get_sh_band_for_lod(lod, p_global_sh_band_level);
				selection.effective_count = count;
				coarsened = true;
			}
			r_plan.coarsened_visible_chunks += coarsened ? 1u : 0u;
		}
		// No refusal while ANY available coarsening could retain full coverage.
		for (uint32_t i : priority) {
			if (r_plan.admitted_splats <= p_effective_drawn_splat_cap) {
				break;
			}
			ChunkLODSelection &selection = r_plan.selections[i];
			const uint32_t nominal_lod = p_candidates[i].has_nominal_selection
					? p_candidates[i].nominal_selection.lod : uint32_t(p_lod_config.calculate_lod_level(p_candidates[i].distance));
			if (p_lod_config.enabled && selection.lod > nominal_lod) {
				r_plan.coarsened_visible_chunks--; // Only final admitted coarsenings count.
			}
			r_plan.admitted_splats -= uint64_t(selection.effective_count) * p_candidates[i].draw_instance_count;
			selection.effective_count = 0;
			selection.admission = ChunkLODAdmission::REFUSED_DRAW_CAP;
			r_plan.refused_visible_chunks++;
		}
	}
	// Count final transitions, preserving both enabled-path increments. Rebuild
	// from the nominal plan each frame; previous cap-coarsened counts must not
	// cause a stationary camera to alternate between coarse and fine plans.
	for (uint32_t i = 0; i < p_candidates.size(); i++) {
		const ChunkLODCandidate &candidate = p_candidates[i];
		const uint32_t lod = r_plan.selections[i].lod;
		if (p_lod_config.enabled && candidate.previous_lod != lod) {
			r_plan.lod_transitions++;
		}
		if (candidate.previous_lod != lod || candidate.previous_target_lod != lod) {
			r_plan.lod_transitions++;
		}
	}
}

} // namespace GaussianSplatting
