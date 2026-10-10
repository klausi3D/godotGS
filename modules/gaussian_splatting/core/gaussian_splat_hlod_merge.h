#ifndef GAUSSIAN_SPLAT_HLOD_MERGE_H
#define GAUSSIAN_SPLAT_HLOD_MERGE_H

// HLOD interior-node merge (ADR docs/architecture/adr-hlod-streaming.md §6.3, candidate `merged`).
// Pure functions over splat arrays, independent of the tree, so they are host-testable on their
// own. The per-node choice between this merge and importance selection is slice S1b.
//
// Conventions (as the loaders store them, io/ply_loader.cpp): `opacity` is linear in [0, 1],
// `scale` is linear (the exp of the PLY log-scale), `rotation` is a unit quaternion, `sh_dc` holds
// SH_C0 x f_dc. Sigma_a >= sigma_b are the two largest scale axes.

#include "core/error/error_list.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

#include "gaussian_data.h"

#include <cstdint>

namespace gs_hlod {

// Fallible allocation probe for the bake's data-sized allocations (the OOM-probe row of
// docs/architecture/adr-import-input-hardening.md): LocalVector::reserve() CRASH_CONDs on a failed
// memrealloc, so each such allocation is first proven available with memalloc(), which returns
// null instead. False means "do not allocate; report ERR_OUT_OF_MEMORY". p_where names the site
// for the message and for the TESTS_ENABLED forced-failure seam in gs_vector_alloc.h (a string
// literal). Defined in gaussian_splat_hlod_bake.cpp.
bool bake_allocation_probe(uint64_t p_bytes, const char *p_where);

// Resizes r_vec to p_count, proving any growth with bake_allocation_probe() first. A grow makes
// LocalVector::reserve() allocate max(p_count, 2, 1.5 x capacity) elements; that is what is probed
// and then reserved explicitly, so the probed and the allocated size are the same. Returns false
// with r_vec unchanged when the probe fails.
template <typename T>
[[nodiscard]] bool fallible_resize(LocalVector<T> &r_vec, uint64_t p_count, const char *p_where) {
	if (p_count > uint64_t(UINT32_MAX)) {
		return false;
	}
	const uint64_t capacity = r_vec.get_capacity();
	if (p_count > capacity) {
		const uint64_t grown = MAX(p_count, MAX<uint64_t>(2u, capacity + ((1u + capacity) >> 1u)));
		if (grown > uint64_t(UINT32_MAX) || !bake_allocation_probe(grown * uint64_t(sizeof(T)), p_where)) {
			return false;
		}
		r_vec.reserve(uint32_t(grown));
	}
	r_vec.resize(uint32_t(p_count));
	return true;
}

// Interior-merge allocation bound (#1149 review). Per interior node the merge holds the children's
// payloads concatenated twice (bake_world's re-framed copy and the merge reference): at most
// kMaxChildren x max(leaf_max_splats, node_max_splats) = 8 x 16,384 = 131,072 splats at the
// defaults, i.e. 18.0 MiB of Gaussian (144 B) plus 131,072 x sh_high_order_count x 12 B of SH
// (22.5 MiB at 15 coefficients) EACH, before LocalVector's 1.5x growth; the bucketing scratch adds
// 24 + 16 + 1 B per splat (5.1 MiB), the merged output and the payload up to 16,384 splats each.
// That is ~100 MiB per node, scaling with the SH count and the interior parameters (neither is
// capped by bake_world), and every node's payload is retained until the end of the bake, which
// scales with the world. So these are not "small constant" allocations: each one goes through
// fallible_resize() and a failure surfaces as ERR_OUT_OF_MEMORY. Excluded: merge_cell_sat()'s
// SH accumulator, which allocates only above 16 SH coefficients (SH degree > 3) and then
// sh_high_order_count x 24 B per cell.

// A borrowed list of splats and their high-order SH (`count * sh_high_order_count` vectors,
// splat-major; may be null when sh_high_order_count is 0).
struct SplatSpan {
	const Gaussian *gaussians = nullptr;
	const Vector3 *sh_high_order = nullptr;
	uint32_t count = 0u;
	uint32_t sh_high_order_count = 0u;
};

// An owned list of splats, same layout as SplatSpan.
struct SplatList {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh_high_order;
	uint32_t sh_high_order_count = 0u;

	uint32_t size() const { return gaussians.size(); }
	void clear() {
		gaussians.clear();
		sh_high_order.clear();
	}
	SplatSpan span() const;
	// Appends p_count splats of p_src starting at p_first. SH counts must match
	// (ERR_INVALID_PARAMETER otherwise). Growth is fallible: ERR_OUT_OF_MEMORY leaves the list
	// unchanged. p_where_* label the two allocations (see bake_allocation_probe()).
	Error append(const SplatSpan &p_src, uint32_t p_first, uint32_t p_count,
			const char *p_where_gaussians = "gs_hlod::SplatList::append gaussians",
			const char *p_where_sh = "gs_hlod::SplatList::append sh_high_order");
};

// Importance used inside nodes: opacity x sigma_a x sigma_b (ADR §5.2 "A-area", §6.2).
float importance_area(const Gaussian &p_g);

// Writes the permutation that orders p_span by importance_area descending, ties by index
// ascending, so any prefix is the best subset by that key (ADR §6.2). Returns false (r_order
// empty) when the order or its key scratch cannot be allocated.
bool importance_order(const SplatSpan &p_span, LocalVector<uint32_t> &r_order);

// Packed symmetric covariance (xx, xy, xz, yy, yz, zz) of a splat, Sigma = R diag(s^2) R^T.
void splat_covariance(const Gaussian &p_g, double r_cov[6]);

// Moments of one merged cell, before and after the `sat` inflation (test hook).
struct CellMergeMoments {
	double weight_sum = 0.0; // sum of w_i = alpha_i sigma_a,i sigma_b,i (+1e-20 each)
	double mean[3] = { 0.0, 0.0, 0.0 };
	// Weighted mean of Sigma_i + (mu_i - mu)(mu_i - mu)^T, before inflation: xx, xy, xz, yy, yz, zz.
	double covariance[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
	double alpha_mass = 0.0; // M = sum alpha_i 2 pi sigma_a,i sigma_b,i
	double capacity = 0.0; // C = 2 pi sigma_a sigma_b of the merged covariance, after inflation
	double inflation = 1.0; // k in [1, kSatMaxInflation], applied to the two largest axes
	double alpha = 0.0;
};

static constexpr double kSatMaxInflation = 1.25;
static constexpr double kMaxMergedAlpha = 0.99;

// `sat` merge of p_members (indices into p_rep) into one splat (ADR §6.3):
// weights w = alpha x sigma_a x sigma_b; mean and covariance by weighted moment matching (H3DGS);
// SH (DC, first order, high order) as weighted means; M = sum alpha 2 pi sigma_a sigma_b;
// the two largest axes inflate by k = clamp(sqrt(M / C), 1, 1.25); alpha = min(1 - exp(-M / C), 0.99).
// A single member is copied unchanged (identity, ADR §6.3 / §11 decision 8d).
// Non-geometric fields (normal, painterly data, render_meta) come from the heaviest member (the
// first one on ties). p_out_sh receives p_rep.sh_high_order_count vectors.
void merge_cell_sat(const SplatSpan &p_rep, const uint32_t *p_members, uint32_t p_member_count,
		Gaussian &r_out, Vector3 *p_out_sh, CellMergeMoments *r_moments = nullptr);

// One bucketed splat: its cell (bit 63: DC encoding; then 21 bits per axis, lexicographic x, y, z)
// and its index.
struct MergeCellEntry {
	uint64_t key = 0u;
	uint32_t index = 0u;
	bool operator<(const MergeCellEntry &p_other) const {
		return key < p_other.key || (key == p_other.key && index < p_other.index);
	}
};

// Reusable buffers of the cell bucketing (avoids per-call allocation in the eps search).
struct MergeScratch {
	LocalVector<MergeCellEntry> entries;
	LocalVector<int64_t> cells;
	LocalVector<uint8_t> pass_through;
	LocalVector<uint32_t> members;
};

// Number of splats the merge produces at cell size p_eps: the pass-through splats
// (2 sigma_max >= eps) plus the number of occupied cells of the grid anchored at p_origin.
// A cell is per DC encoding: splats with different encodings never merge.
// Returns UINT32_MAX when the grid is too fine for the content (more than 2^21 cells per axis;
// r_code = ERR_PARAMETER_RANGE_ERROR) or the bucketing scratch cannot be allocated
// (r_code = ERR_OUT_OF_MEMORY). r_code, when given, is OK otherwise.
uint32_t merged_count_at_eps(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch,
		Error *r_code = nullptr);

// The merged candidate at cell size p_eps: pass-through splats unchanged in input order, then one
// `sat` merge per occupied cell in ascending cell-key order (the order of the ADR prototype).
// Returns ERR_INVALID_PARAMETER for a non-positive eps, ERR_PARAMETER_RANGE_ERROR when the grid is
// too fine, ERR_OUT_OF_MEMORY when an allocation fails; r_out is then empty.
Error merge_at_eps(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch, SplatList &r_out);

// ---------------------------------------------------------------------------------------------
// One interior node (ADR §6.3), independent of how the tree was split.
// ---------------------------------------------------------------------------------------------

struct InteriorBakeParams {
	uint32_t merge_grid = 64u; // first eps = cell edge / merge_grid
	double reduction = 4.0; // node target = ceil(children total / reduction)
	uint32_t node_max_splats = 16384u;
	double eps_growth = 1.25;
	uint32_t max_eps_steps = 1024u;
};

struct InteriorBakeScratch {
	SplatList reference;
	SplatList merged;
	MergeScratch merge;
	LocalVector<uint32_t> order;
};

struct InteriorBakeResult {
	SplatList payload; // importance-ordered
	double geometric_error = 0.0; // the eps the payload was built at
	uint32_t eps_steps = 0u;
	uint64_t merge_usec = 0u; // eps search + merge
	uint64_t order_usec = 0u;
};

// Builds one interior node from its children's payloads, all in one frame of reference:
// eps = max(p_cell_edge / merge_grid, 2 x p_max_child_error), raised by eps_growth until the `sat`
// merge holds at most min(node_max_splats, ceil(total / reduction)) splats; the payload is that
// merge, importance-ordered. p_grid_origin anchors the merge cells (the node's cell corner, so the
// merge cells are world-aligned too). Returns false (r_error set) when no eps reaches the target
// within max_eps_steps (only non-finite input can cause that; r_code = ERR_INVALID_DATA) or when an
// allocation fails (r_code = ERR_OUT_OF_MEMORY).
bool bake_interior_node(const SplatSpan *p_children, uint32_t p_child_count,
		const Vector3 &p_grid_origin, double p_cell_edge, double p_max_child_error,
		const InteriorBakeParams &p_params, InteriorBakeScratch &r_scratch, InteriorBakeResult &r_result,
		String *r_error, Error *r_code = nullptr);

} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_MERGE_H
