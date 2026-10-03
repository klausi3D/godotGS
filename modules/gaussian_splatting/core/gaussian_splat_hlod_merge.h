#ifndef GAUSSIAN_SPLAT_HLOD_MERGE_H
#define GAUSSIAN_SPLAT_HLOD_MERGE_H

// HLOD interior-node merge (ADR docs/architecture/adr-hlod-streaming.md §6.3, candidate `merged`).
// Pure functions over splat arrays, independent of the tree, so they are host-testable on their
// own. The per-node choice between this merge and importance selection is slice S1b.
//
// Conventions (as the loaders store them, io/ply_loader.cpp): `opacity` is linear in [0, 1],
// `scale` is linear (the exp of the PLY log-scale), `rotation` is a unit quaternion, `sh_dc` holds
// SH_C0 x f_dc. Sigma_a >= sigma_b are the two largest scale axes.

#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

#include "gaussian_data.h"

#include <cstdint>

namespace gs_hlod {

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
	// Appends p_count splats of p_src starting at p_first. SH counts must match.
	void append(const SplatSpan &p_src, uint32_t p_first, uint32_t p_count);
};

// Importance used inside nodes: opacity x sigma_a x sigma_b (ADR §5.2 "A-area", §6.2).
float importance_area(const Gaussian &p_g);

// Writes the permutation that orders p_span by importance_area descending, ties by index
// ascending, so any prefix is the best subset by that key (ADR §6.2).
void importance_order(const SplatSpan &p_span, LocalVector<uint32_t> &r_order);

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

// One bucketed splat: its cell (21 bits per axis, lexicographic x, y, z) and its index.
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
// Returns UINT32_MAX when the grid is too fine for the content (more than 2^21 cells per axis).
uint32_t merged_count_at_eps(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch);

// The merged candidate at cell size p_eps: pass-through splats unchanged in input order, then one
// `sat` merge per occupied cell in ascending cell-key order (the order of the ADR prototype).
void merge_at_eps(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch, SplatList &r_out);

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
// within max_eps_steps (only non-finite input can cause that).
bool bake_interior_node(const SplatSpan *p_children, uint32_t p_child_count,
		const Vector3 &p_grid_origin, double p_cell_edge, double p_max_child_error,
		const InteriorBakeParams &p_params, InteriorBakeScratch &r_scratch, InteriorBakeResult &r_result,
		String *r_error);

} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_MERGE_H
