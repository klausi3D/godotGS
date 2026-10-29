#ifndef GS_RESIDENT_ATLAS_BUDGET_H
#define GS_RESIDENT_ATLAS_BUDGET_H

// Pure, device-independent helpers for clamping the resident atlas to the
// device VRAM budget (#321) and packing an importance-ordered subset (a coarse
// LOD) when the full atlas would not fit. Deliberately free of RenderingDevice /
// RID / global-config dependencies so the budget math and the subset selection
// are unit-testable on the host without a GPU. See
// resident_instance_contract_publisher.cpp for the single production caller.

// gaussian_importance() + select_top_k_indices() were extracted to a neutral core
// header (#456 / ADR Decision 1) so the importer can reuse them without io/ ->
// renderer/ layering. Re-included here so this file's consumers are unchanged.
#include "../core/gaussian_importance.h" // ResidentAtlasBudget::gaussian_importance(), select_top_k_indices()

#include "../core/gaussian_data.h" // Gaussian

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

#include <algorithm>
#include <cstdint>

namespace ResidentAtlasBudget {

// Static device-local VRAM capacity below which RenderingDevice::get_device_memory_budget()
// is treated as unknown. Mirrors streaming_vram_regulator.cpp's
// MIN_TRUSTED_DEVICE_CAPACITY_BYTES (#321): a degenerate small-positive value would
// otherwise clamp the atlas to nonsense.
static constexpr uint64_t kMinTrustedDeviceBudgetBytes = uint64_t(256) * 1024 * 1024;

// Absolute single-upload staging ceiling, shared with the resident publisher's
// kMaxResidentUploadBytes. Datasets above this still hard-reject (streaming is the
// right answer); the budget cap below only ever tightens beneath it.
static constexpr uint64_t kResidentStagingCeilingBytes = uint64_t(2) * 1024 * 1024 * 1024;

// Fraction of device-local VRAM the resident atlas may occupy. The remainder is
// reserved for the same frame's sort / splat-ref / visible-chunk buffers, engine
// framebuffers, and driver reserve (get_device_memory_budget() is the device heap,
// not net of this app's non-atlas allocations). The 0.60 * budget term only binds
// below ~3.3 GB device VRAM (0.60 * budget < 2 GB); at/above that, the staging
// ceiling dominates and capable GPUs are byte-identical to today.
static constexpr double kResidentAtlasHeadroomFraction = 0.60;

// Effective byte cap for the packed resident atlas.
//  - device_budget_bytes == 0 or below the sane floor => capacity unknown =>
//    cap stays at the staging ceiling (do-no-harm on UMA/iGPU/software/non-Vulkan).
//  - override_mb > 0 pins an explicit cap (low-end clamp + on-capable-GPU validation).
inline uint64_t compute_effective_atlas_cap_bytes(uint64_t device_budget_bytes, uint32_t override_mb,
        uint64_t staging_ceiling_bytes = kResidentStagingCeilingBytes,
        double headroom_fraction = kResidentAtlasHeadroomFraction,
        uint64_t min_trusted_budget_bytes = kMinTrustedDeviceBudgetBytes) {
    uint64_t cap = staging_ceiling_bytes;
    if (device_budget_bytes >= min_trusted_budget_bytes) {
        const uint64_t budget_cap = uint64_t(double(device_budget_bytes) * headroom_fraction);
        cap = MIN(cap, budget_cap);
    }
    if (override_mb > 0) {
        const uint64_t override_bytes = uint64_t(override_mb) * 1024u * 1024u;
        cap = MIN(cap, override_bytes);
    }
    return cap;
}

// Byte limit for the HLOD route's device-wide ledger (every RenderingDevice buffer plus the tile and
// compositor textures, get_hlod_owned_byte_upper_bound()). The ledger includes the canonical
// route's own sort / tile / atlas buffers (819 MB on the 252k-leaf holzbank scan), so it cannot be
// bounded by the HLOD-owned byte cap; that cap bounds the HLOD bundle on its own. The ledger is
// bounded by the same documented device-heap fraction as the atlas (kResidentAtlasHeadroomFraction;
// ADR 6.5 / 14.1: budget checks use allocated bytes against one physical budget). An unknown or
// untrusted heap size keeps the fixed hlod_owned_cap (fail closed, as before).
inline uint64_t compute_hlod_device_ledger_limit_bytes(uint64_t device_budget_bytes, uint64_t hlod_owned_cap_bytes,
        double headroom_fraction = kResidentAtlasHeadroomFraction,
        uint64_t min_trusted_budget_bytes = kMinTrustedDeviceBudgetBytes) {
    if (device_budget_bytes >= min_trusted_budget_bytes) {
        return uint64_t(double(device_budget_bytes) * headroom_fraction);
    }
    return device_budget_bytes ? MIN(device_budget_bytes, hlod_owned_cap_bytes) : hlod_owned_cap_bytes;
}

// Admission of a candidate HLOD bundle: its own bytes must fit the HLOD-owned cap, and the
// device-wide ledger plus the candidate must fit the ledger limit. Pure so the boundaries are testable.
inline bool hlod_candidate_fits(uint64_t candidate_bytes, uint64_t device_bytes, uint64_t ledger_limit_bytes,
        uint64_t hlod_owned_cap_bytes) {
    return candidate_bytes <= hlod_owned_cap_bytes && candidate_bytes <= ledger_limit_bytes &&
            device_bytes <= ledger_limit_bytes - candidate_bytes;
}

// Same for an already prepared bundle: device_bytes already includes the bundle.
inline bool hlod_prepared_fits(uint64_t owned_bytes, uint64_t device_bytes, uint64_t ledger_limit_bytes,
        uint64_t hlod_owned_cap_bytes) {
    return owned_bytes <= hlod_owned_cap_bytes && device_bytes <= ledger_limit_bytes;
}

// Streaming decision for an HLOD tree (package D): stream whenever the whole-tree bundles cannot be admitted -
// atlas budget, HLOD-owned cap or device ledger - never by raising a limit.
inline bool hlod_should_stream(uint64_t whole_atlas_bytes, uint64_t atlas_limit_bytes, bool whole_candidate_fits) {
    return whole_atlas_bytes > atlas_limit_bytes || !whole_candidate_fits;
}

// Pages a streamed HLOD atlas may hold. `fixed_bundle_bytes` is the relative bundle without its atlas records.
// The result is clamped by the atlas cap, the tree itself, the HLOD-owned cap and the device ledger headroom
// (the same limits hlod_candidate_fits applies); 0 means not even one page is admissible.
inline uint64_t hlod_stream_page_budget(uint64_t atlas_limit_bytes, uint64_t record_stride, uint32_t page_splats,
        uint64_t total_node_pages, uint64_t fixed_bundle_bytes, uint64_t hlod_owned_cap_bytes,
        uint64_t ledger_limit_bytes, uint64_t device_bytes) {
    if (record_stride == 0 || page_splats == 0) { return 0; }
    const uint64_t page_bytes = record_stride * page_splats;
    if (fixed_bundle_bytes >= hlod_owned_cap_bytes || device_bytes > ledger_limit_bytes ||
            ledger_limit_bytes - device_bytes <= fixed_bundle_bytes) { return 0; }
    uint64_t pages = MIN(atlas_limit_bytes / page_bytes, total_node_pages);
    pages = MIN(pages, (hlod_owned_cap_bytes - fixed_bundle_bytes) / page_bytes);
    pages = MIN(pages, (ledger_limit_bytes - device_bytes - fixed_bundle_bytes) / page_bytes);
    return pages;
}

struct SubsetPlan {
    bool reduced = false;       // true when the atlas must be thinned to fit
    double keep_ratio = 1.0;    // global fraction of splats to keep (1.0 == full)
    uint64_t target_keep = 0;   // soft splat target (cap_bytes / packed_size)
    uint64_t source_count = 0;  // full atlas splat count
    uint64_t cap_bytes = 0;     // effective byte cap that produced this plan
};

// Decide whether the full atlas fits the cap and, if not, the global keep ratio.
inline SubsetPlan compute_subset_plan(uint64_t total_gaussians, uint64_t effective_cap_bytes,
        uint32_t packed_gaussian_size = 144u) {
    SubsetPlan plan;
    plan.source_count = total_gaussians;
    plan.cap_bytes = effective_cap_bytes;
    plan.target_keep = total_gaussians;
    if (packed_gaussian_size == 0u || total_gaussians == 0u) {
        return plan;
    }
    const uint64_t cap_splats = effective_cap_bytes / uint64_t(packed_gaussian_size);
    if (cap_splats >= total_gaussians) {
        return plan; // fits: keep_ratio == 1.0, reduced == false
    }
    plan.reduced = true;
    plan.target_keep = cap_splats;
    plan.keep_ratio = double(cap_splats) / double(total_gaussians);
    return plan;
}

// Per-asset slice of the global keep budget when the resident atlas spans multiple assets.
// Each asset gets a share proportional to its splat count (ceil, so a non-empty asset keeps
// >=1), capped by the budget still globally available. This keeps any one asset -- including
// the currently visible/casting ones -- from being starved to zero by an earlier large
// (possibly hidden) asset processed first in the stable superset. The split is a pure function
// of per-device counts, NOT visibility, so the atlas stays stable across visibility flips
// (Codex #420 round 3).
inline uint32_t resident_asset_budget(uint64_t p_target_keep, uint64_t p_asset_count,
        uint64_t p_total_gaussians, uint32_t p_global_remaining) {
    if (p_asset_count == 0u || p_total_gaussians == 0u) {
        return 0u;
    }
    const uint64_t proportional = (p_target_keep * p_asset_count + p_total_gaussians - 1u) / p_total_gaussians;
    const uint64_t share = MAX<uint64_t>(uint64_t(1), proportional);
    return uint32_t(MIN<uint64_t>(uint64_t(p_global_remaining), share));
}

// Per-chunk keep count under a global keep_ratio. Floors at 1 for any non-empty
// chunk so thinning never deletes a whole spatial region (preserves coverage on
// real-scan content); the per-chunk floor remainder is the documented soft
// overshoot bounded by the hard staging ceiling.
inline uint32_t compute_chunk_keep_count(uint32_t count, double keep_ratio) {
    if (count == 0u) {
        return 0u;
    }
    if (keep_ratio >= 1.0) {
        return count;
    }
    if (keep_ratio <= 0.0) {
        return 1u;
    }
    uint32_t keep = uint32_t(Math::round(double(count) * keep_ratio));
    keep = MAX(1u, keep);
    return MIN(keep, count);
}

// gaussian_importance() and select_top_k_indices() now live in
// core/gaussian_importance.h (re-included above); they remain in namespace
// ResidentAtlasBudget so this file's callers and consumers are unchanged.

// Compact a captured chunk snapshot down to its top-importance subset in place,
// moving both the Gaussian payload and its parallel high-order SH block (stride =
// p_sh_stride coefficients per gaussian, gaussian-major, per capture_*_snapshot).
// `r_remaining_budget` is the global cap on cumulative kept splats across all chunks: the
// per-chunk keep is clamped to it and it is decremented by the kept count, so the resident
// atlas honors the device VRAM target even when many per-chunk floors would otherwise
// overshoot it (e.g. a dataset of many one-splat static chunks) -- Codex #420. Returns the
// number of kept gaussians (0 when the budget is exhausted -> the caller drops the chunk).
inline uint32_t compact_chunk_by_importance(LocalVector<Gaussian> &r_gaussians,
        LocalVector<Vector3> &r_sh_high_order, uint32_t p_sh_stride, double p_keep_ratio,
        uint32_t &r_remaining_budget) {
    const uint32_t count = r_gaussians.size();
    uint32_t keep = compute_chunk_keep_count(count, p_keep_ratio);
    keep = MIN(keep, r_remaining_budget); // hard global cap
    if (keep >= count) {
        r_remaining_budget -= count;
        return count;
    }
    if (keep == 0) {
        r_gaussians.clear();
        r_sh_high_order.clear();
        return 0;
    }

    LocalVector<float> importance;
    importance.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        importance[i] = gaussian_importance(r_gaussians[i]);
    }

    LocalVector<uint32_t> keep_indices;
    select_top_k_indices(importance.ptr(), count, keep, keep_indices);

    // Forward in-place compaction: keep_indices is ascending and keep <= count, so
    // keep_indices[j] >= j for every j -- writing slot j never clobbers an as-yet
    // unread source slot.
    for (uint32_t j = 0; j < keep_indices.size(); j++) {
        const uint32_t src = keep_indices[j];
        if (src != j) {
            r_gaussians[j] = r_gaussians[src];
        }
    }
    r_gaussians.resize(keep);

    if (p_sh_stride > 0u && uint64_t(r_sh_high_order.size()) >= uint64_t(count) * uint64_t(p_sh_stride)) {
        Vector3 *sh = r_sh_high_order.ptr();
        for (uint32_t j = 0; j < keep_indices.size(); j++) {
            const uint32_t src = keep_indices[j];
            if (src != j) {
                const uint64_t dst_base = uint64_t(j) * uint64_t(p_sh_stride);
                const uint64_t src_base = uint64_t(src) * uint64_t(p_sh_stride);
                for (uint32_t c = 0; c < p_sh_stride; c++) {
                    sh[dst_base + c] = sh[src_base + c];
                }
            }
        }
        r_sh_high_order.resize(keep * p_sh_stride);
    }
    r_remaining_budget -= keep;
    return keep;
}

// Texture bytes a tile grid of `new_px` pixels charges to the HLOD ledger when `live_bytes` are charged for `cur_px`
// pixels now (the output, resolved, depth and normal targets scale with the pixel count). A smaller or equal grid never
// costs more than it does now; an unknown current size (0) keeps the live bytes. Rounded up, saturating.
inline uint64_t hlod_scale_texture_bytes_for_pixels(uint64_t live_bytes, uint64_t cur_px, uint64_t new_px) {
    if (cur_px == 0 || new_px == 0 || new_px <= cur_px || live_bytes == 0) { return live_bytes; }
    if (live_bytes > UINT64_MAX / new_px) { return UINT64_MAX; }
    return (live_bytes * new_px + cur_px - 1u) / cur_px;
}

// What a streamed HLOD view must do with the tile grid before its scratch is admitted. A streamed world never runs a
// canonical frame, so nothing else resizes the grid: a view whose size differs from the grid, or a grid without overlap
// storage, must be provisioned for THIS view, per view and per frame. The new targets must fit the ledger headroom
// (`wanted_texture_bytes` is what the grid would charge afterwards); otherwise the view is refused, honestly.
enum class HlodViewGridAction { KEEP, RESIZE, REFUSE_MEMORY };
inline HlodViewGridAction hlod_view_grid_action(int p_grid_x, int p_grid_y, int p_view_x, int p_view_y, uint32_t p_overlap_capacity,
        uint64_t p_live_texture_bytes, uint64_t p_wanted_texture_bytes, uint64_t p_headroom_bytes) {
    if (p_grid_x == p_view_x && p_grid_y == p_view_y && p_overlap_capacity != 0) { return HlodViewGridAction::KEEP; }
    if (p_wanted_texture_bytes > p_live_texture_bytes && p_wanted_texture_bytes - p_live_texture_bytes > p_headroom_bytes) {
        return HlodViewGridAction::REFUSE_MEMORY;
    }
    return HlodViewGridAction::RESIZE;
}

} // namespace ResidentAtlasBudget

#endif // GS_RESIDENT_ATLAS_BUDGET_H
