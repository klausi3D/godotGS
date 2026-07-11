#ifndef GS_GAUSSIAN_IMPORTANCE_H
#define GS_GAUSSIAN_IMPORTANCE_H

// Shared, device-independent per-splat importance metric and top-k selection,
// extracted verbatim from renderer/resident_atlas_budget.h (#420) so both the
// renderer (resident atlas thinning) and the importer (io/, import-time pruning)
// can reuse the SAME ranking without io/ depending on renderer/. See
// docs/architecture/adr-import-importance-pruning.md (Decision 1). These are pure
// functions over core types (Gaussian / Math / LocalVector) only -- no
// RenderingDevice / RID / global-config dependencies -- so they stay unit-testable
// on the host without a GPU. resident_atlas_budget.h re-includes this header, so
// its existing consumers (and namespace) are unchanged.

#include "gaussian_data.h" // Gaussian

#include "core/math/math_funcs.h"
#include "core/templates/local_vector.h"

#include <algorithm>
#include <cstdint>

namespace ResidentAtlasBudget {

// The project's own per-splat importance metric (interfaces/gpu_culler.cpp:291-293):
// opacity (clamped) * (max scale axis + epsilon), floored to epsilon. Reused so the subset
// is selected by the same definition the culler uses, not a new heuristic -- with one
// deliberate refinement for this DESTRUCTIVE selection: use the scale MAGNITUDE
// (max(abs(scale))) rather than the signed axis. The covariance build squares scale and
// GaussianData::compute_radius() treats scale by magnitude, so a large negative-scale splat
// (e.g. (-5,-4,-3)) is visibly large and must not be forced to the minimum importance and
// preferentially dropped (Codex #420).
inline float gaussian_importance(const Gaussian &p_g) {
    const float opacity = CLAMP(p_g.opacity, 0.0f, 1.0f);
    const float size_factor = MAX(MAX(Math::abs(p_g.scale.x), Math::abs(p_g.scale.y)), Math::abs(p_g.scale.z));
    const float importance = opacity * (size_factor + 0.0001f);
    // Scan/training PLY data can carry NaN/Inf scale or opacity. A non-finite importance would
    // make the strict-weak-ordering comparator in select_top_k_indices non-transitive, which is
    // undefined behavior for std::nth_element / std::sort in this DESTRUCTIVE selection. Floor
    // any non-finite value so the metric is always finite and orderable (deterministic), and
    // such degenerate splats sort to the bottom rather than corrupting the kept set.
    if (!Math::is_finite(importance)) {
        return 0.0001f;
    }
    return MAX(0.0001f, importance);
}

// Select the `keep` highest-importance indices in [0, count), ties broken by
// ascending index (a strict total order => a unique, deterministic kept set), and
// return them SORTED ASCENDING so the caller can compact in place forward-only.
inline void select_top_k_indices(const float *p_importance, uint32_t p_count, uint32_t p_keep,
        LocalVector<uint32_t> &r_indices) {
    r_indices.clear();
    if (p_count == 0u || p_keep == 0u) {
        return;
    }
    if (p_keep >= p_count) {
        r_indices.resize(p_count);
        for (uint32_t i = 0; i < p_count; i++) {
            r_indices[i] = i;
        }
        return;
    }
    LocalVector<uint32_t> order;
    order.resize(p_count);
    for (uint32_t i = 0; i < p_count; i++) {
        order[i] = i;
    }
    const float *importance = p_importance;
    const auto better = [importance](uint32_t a, uint32_t b) {
        if (importance[a] != importance[b]) {
            return importance[a] > importance[b];
        }
        return a < b; // deterministic tie-break
    };
    std::nth_element(order.ptr(), order.ptr() + p_keep, order.ptr() + p_count, better);
    r_indices.resize(p_keep);
    for (uint32_t i = 0; i < p_keep; i++) {
        r_indices[i] = order[i];
    }
    std::sort(r_indices.ptr(), r_indices.ptr() + p_keep);
}

} // namespace ResidentAtlasBudget

#endif // GS_GAUSSIAN_IMPORTANCE_H
