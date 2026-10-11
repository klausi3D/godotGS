#include "gaussian_splat_hlod_bake.h"

#include "gs_vector_alloc.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace gs_hlod {

namespace {

struct BuildNode {
	GaussianSplatHlodCell cell;
	uint32_t begin = 0u; // range in the permutation
	uint32_t end = 0u;
	uint32_t flags = 0u;
	uint32_t parent = kNoParent; // build index
	LocalVector<uint32_t> children; // build indices, in octant order
};

inline bool finite_gaussian(const Gaussian &p_g) {
	const float v[] = { p_g.position.x, p_g.position.y, p_g.position.z, p_g.scale.x, p_g.scale.y, p_g.scale.z,
		p_g.rotation.x, p_g.rotation.y, p_g.rotation.z, p_g.rotation.w, p_g.opacity, p_g.sh_dc.r, p_g.sh_dc.g,
		p_g.sh_dc.b, p_g.sh_dc.a, p_g.sh_1[0].x, p_g.sh_1[0].y, p_g.sh_1[0].z, p_g.sh_1[1].x, p_g.sh_1[1].y,
		p_g.sh_1[1].z, p_g.sh_1[2].x, p_g.sh_1[2].y, p_g.sh_1[2].z, p_g.area, p_g.normal.x, p_g.normal.y,
		p_g.normal.z, p_g.stroke_age, p_g.brush_axes.x, p_g.brush_axes.y };
	for (float f : v) {
		if (!Math::is_finite(f)) {
			return false;
		}
	}
	return true;
}

inline uint64_t spread21(uint64_t p_v) {
	p_v &= 0x1fffffu;
	p_v = (p_v | (p_v << 32u)) & 0x1f00000000ffffull;
	p_v = (p_v | (p_v << 16u)) & 0x1f0000ff0000ffull;
	p_v = (p_v | (p_v << 8u)) & 0x100f00f00f00f00full;
	p_v = (p_v | (p_v << 4u)) & 0x10c30c30c30c30c3ull;
	p_v = (p_v | (p_v << 2u)) & 0x1249249249249249ull;
	return p_v;
}

// Bounds accumulated in double, in some frame; rounded outward into a node record.
struct BoundsAccumulator {
	double lo[3] = { DBL_MAX, DBL_MAX, DBL_MAX };
	double hi[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
	void add(const double p_lo[3], const double p_hi[3]) {
		for (int a = 0; a < 3; a++) {
			lo[a] = MIN(lo[a], p_lo[a]);
			hi[a] = MAX(hi[a], p_hi[a]);
		}
	}
	bool valid() const { return lo[0] <= hi[0]; }
};

// Largest float f with p_center + f <= p_target (or smallest with >=), evaluated in double
// exactly as the validator evaluates it, so stored bounds contain what they must.
inline float round_down_offset(double p_center, double p_target) {
	float f = float(p_target - p_center);
	while (p_center + double(f) > p_target) {
		f = std::nextafter(f, -FLT_MAX);
	}
	return f;
}

inline float round_up_offset(double p_center, double p_target) {
	float f = float(p_target - p_center);
	while (p_center + double(f) < p_target) {
		f = std::nextafter(f, FLT_MAX);
	}
	return f;
}

inline float round_up(double p_v) {
	float f = float(p_v);
	if (double(f) < p_v) {
		f = std::nextafter(f, FLT_MAX);
	}
	return f;
}

// Adds the 3-sigma axis-aligned extent of p_span (positions in a frame offset by p_shift from
// the accumulator's frame) and returns, through r_sphere_*, data for the radius pass.
void add_payload_bounds(const SplatSpan &p_span, const double p_shift[3], BoundsAccumulator &r_bounds) {
	for (uint32_t i = 0; i < p_span.count; i++) {
		const Gaussian &g = p_span.gaussians[i];
		double cov[6];
		splat_covariance(g, cov);
		const double half[3] = { 3.0 * std::sqrt(MAX(cov[0], 0.0)), 3.0 * std::sqrt(MAX(cov[3], 0.0)), 3.0 * std::sqrt(MAX(cov[5], 0.0)) };
		const double p[3] = { double(g.position.x) + p_shift[0], double(g.position.y) + p_shift[1], double(g.position.z) + p_shift[2] };
		const double lo[3] = { p[0] - half[0], p[1] - half[1], p[2] - half[2] };
		const double hi[3] = { p[0] + half[0], p[1] + half[1], p[2] + half[2] };
		r_bounds.add(lo, hi);
	}
}

double payload_radius(const SplatSpan &p_span, const double p_shift[3], const double p_mid[3]) {
	double radius = 0.0;
	for (uint32_t i = 0; i < p_span.count; i++) {
		const Gaussian &g = p_span.gaussians[i];
		const double d[3] = { double(g.position.x) + p_shift[0] - p_mid[0], double(g.position.y) + p_shift[1] - p_mid[1],
			double(g.position.z) + p_shift[2] - p_mid[2] };
		const double s_max = MAX(Math::abs(g.scale.x), MAX(Math::abs(g.scale.y), Math::abs(g.scale.z)));
		radius = MAX(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 3.0 * s_max);
	}
	return radius;
}

} // namespace

bool bake_allocation_probe(uint64_t p_bytes, const char *p_where) {
	if (p_bytes == 0u) {
		return true; // nothing is allocated, so nothing can fail (and the test seam stays armed)
	}
#if defined(TESTS_ENABLED)
	if (gs_vector_alloc_consume_forced_failure(p_where)) {
		return false;
	}
#endif
	if (p_bytes > uint64_t(SIZE_MAX)) {
		return false;
	}
	void *probe = memalloc(size_t(p_bytes));
	if (probe == nullptr) {
		return false;
	}
	memfree(probe);
	return true;
}

bool choose_root_cell(const double p_origin[3], const double p_min[3], const double p_max[3],
		GaussianSplatHlodCell &r_cell, bool &r_origin_centred) {
	double extent = 0.0;
	bool straddles = false;
	for (int a = 0; a < 3; a++) {
		if (!std::isfinite(p_min[a]) || !std::isfinite(p_max[a]) || p_min[a] > p_max[a]) {
			return false;
		}
		extent = MAX(extent, p_max[a] - p_min[a]);
		// Cells are half-open [i 2^e, (i + 1) 2^e): a centre exactly on the plane is in cell 0.
		straddles = straddles || (p_min[a] - p_origin[a] < 0.0 && p_max[a] - p_origin[a] >= 0.0);
	}
	if (straddles) {
		// The smallest e with every centre in [O - 2^e, O + 2^e).
		for (int32_t e = kMinCellExponent; e <= kMaxCellExponent; e++) {
			const double half = std::ldexp(1.0, e);
			bool covers = true;
			for (int a = 0; a < 3 && covers; a++) {
				covers = (p_min[a] - p_origin[a]) >= -half && (p_max[a] - p_origin[a]) < half;
			}
			if (covers) {
				r_cell = GaussianSplatHlodCell();
				r_cell.e = e;
				r_origin_centred = true;
				return true;
			}
		}
		return false;
	}
	int32_t e = kMinCellExponent;
	if (extent > 0.0) {
		int exp2 = 0;
		std::frexp(extent, &exp2); // extent in [2^(exp2-1), 2^exp2)
		e = MAX<int32_t>(kMinCellExponent, exp2 - 1);
	}
	for (; e <= kMaxCellExponent; e++) {
		const double edge = std::ldexp(1.0, e);
		int64_t idx[3] = { 0, 0, 0 };
		bool covers = true;
		for (int a = 0; a < 3 && covers; a++) {
			const double lo = std::floor((p_min[a] - p_origin[a]) / edge);
			const double hi = std::floor((p_max[a] - p_origin[a]) / edge);
			covers = lo == hi && std::fabs(lo) < double(kMaxCellIndexMagnitude);
			idx[a] = covers ? int64_t(lo) : 0;
		}
		if (covers) {
			r_cell.e = e;
			r_cell.ix = idx[0];
			r_cell.iy = idx[1];
			r_cell.iz = idx[2];
			r_origin_centred = false;
			return true;
		}
	}
	return false;
}

bool bake_world(const BakeInput &p_input, const BakeParams &p_params, BakeResult &r_result, String *r_error,
		Error *r_code) {
	const uint64_t t_start = OS::get_singleton()->get_ticks_usec();
	auto fail = [&](const String &p_message, Error p_code = ERR_INVALID_DATA) {
		if (r_error) {
			*r_error = p_message;
		}
		if (r_code) {
			*r_code = p_code;
		}
		return false;
	};
	// Every splat-count-sized resize goes through here (bake_allocation_probe()). The vectors are
	// empty, so LocalVector::reserve() allocates exactly p_count elements, the size probed.
	auto resize_or_fail = [&](auto &r_vec, uint64_t p_count, const char *p_where) {
		const uint64_t bytes = p_count > uint64_t(UINT32_MAX) ? UINT64_MAX : p_count * uint64_t(sizeof(*r_vec.ptr()));
		if (p_count > uint64_t(UINT32_MAX) || !bake_allocation_probe(bytes, p_where)) {
			return fail(vformat("HLOD bake cannot allocate %s (%d elements).", String(p_where), p_count), ERR_OUT_OF_MEMORY);
		}
		r_vec.resize(uint32_t(p_count));
		return true;
	};
	r_result = BakeResult();
	const uint32_t n = p_input.count;
	const uint32_t sh_count = p_input.sh_high_order_count;
	if (n == 0u || p_input.gaussians == nullptr) {
		return fail("HLOD bake needs at least one splat.");
	}
	if (sh_count > 0u && p_input.sh_high_order == nullptr) {
		return fail("HLOD bake input declares SH coefficients but provides none.");
	}
	const uint32_t leaf_max = p_params.leaf_max_splats;
	if (leaf_max == 0u || leaf_max > kMaxNodeSplats) {
		return fail(vformat("HLOD leaf size %d is outside [1, %d].", leaf_max, kMaxNodeSplats));
	}

	// Validate and bound the centres.
	double lo[3] = { DBL_MAX, DBL_MAX, DBL_MAX };
	double hi[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
	for (uint32_t i = 0; i < n; i++) {
		const Gaussian &g = p_input.gaussians[i];
		if (!finite_gaussian(g)) {
			return fail(vformat("HLOD bake refuses splat %d: a field is not finite.", i));
		}
		if (sh_count > 0u) {
			const Vector3 *sh = p_input.sh_high_order + uint64_t(i) * sh_count;
			for (uint32_t c = 0; c < sh_count; c++) {
				if (!Math::is_finite(sh[c].x) || !Math::is_finite(sh[c].y) || !Math::is_finite(sh[c].z)) {
					return fail(vformat("HLOD bake refuses splat %d: an SH coefficient is not finite.", i));
				}
			}
		}
		const double p[3] = { g.position.x, g.position.y, g.position.z };
		for (int a = 0; a < 3; a++) {
			lo[a] = MIN(lo[a], p[a]);
			hi[a] = MAX(hi[a], p[a]);
		}
	}
	GaussianSplatHlodTree &tree = r_result.tree;
	tree.origin[0] = tree.origin[1] = tree.origin[2] = 0.0; // the world frame's origin
	GaussianSplatHlodCell root_cell;
	bool origin_centred = false;
	if (!choose_root_cell(tree.origin, lo, hi, root_cell, origin_centred)) {
		return fail("HLOD bake cannot place the content on the world lattice.");
	}
	tree.bake_rule_version = kBakeRuleVersion;
	tree.sh_high_order_count = sh_count;

	// ---------------------------------------------------------------- tree build (§6.1)
	LocalVector<uint32_t> perm;
	if (!resize_or_fail(perm, n, "gs_hlod::bake_world perm")) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		perm[i] = i;
	}
	LocalVector<uint32_t> scratch_perm;
	if (!resize_or_fail(scratch_perm, n, "gs_hlod::bake_world scratch_perm")) {
		return false;
	}
	LocalVector<BuildNode> build;
	build.push_back(BuildNode());
	build[0].begin = 0u;
	build[0].end = n;
	build[0].cell = root_cell;
	build[0].flags = origin_centred ? kNodeFlagOriginCentredRoot : 0u;
	LocalVector<uint32_t> stack;
	stack.push_back(0u);
	uint32_t index_split_groups = 0u;

	// Corner and edge of a build node's region (the origin-centred root is [O - 2^e, O + 2^e)).
	auto region = [&](const BuildNode &p_node, double r_corner[3], double &r_edge) {
		if ((p_node.flags & kNodeFlagOriginCentredRoot) != 0u) {
			const double half = std::ldexp(1.0, p_node.cell.e);
			r_edge = 2.0 * half;
			for (int a = 0; a < 3; a++) {
				r_corner[a] = tree.origin[a] - half;
			}
			return;
		}
		r_edge = std::ldexp(1.0, p_node.cell.e);
		r_corner[0] = tree.origin[0] + double(p_node.cell.ix) * r_edge;
		r_corner[1] = tree.origin[1] + double(p_node.cell.iy) * r_edge;
		r_corner[2] = tree.origin[2] + double(p_node.cell.iz) * r_edge;
	};
	// The lattice cell of octant p_octant of a build node.
	auto octant_cell = [&](const BuildNode &p_node, uint32_t p_octant) {
		GaussianSplatHlodCell c;
		const int64_t b[3] = { int64_t(p_octant & 1u), int64_t((p_octant >> 1u) & 1u), int64_t((p_octant >> 2u) & 1u) };
		if ((p_node.flags & kNodeFlagOriginCentredRoot) != 0u) {
			c.e = p_node.cell.e;
			c.ix = b[0] - 1;
			c.iy = b[1] - 1;
			c.iz = b[2] - 1;
			return c;
		}
		c.e = p_node.cell.e - 1;
		c.ix = p_node.cell.ix * 2 + b[0];
		c.iy = p_node.cell.iy * 2 + b[1];
		c.iz = p_node.cell.iz * 2 + b[2];
		return c;
	};
	auto add_child = [&](uint32_t p_parent, const GaussianSplatHlodCell &p_cell, uint32_t p_begin, uint32_t p_end, uint32_t p_flags) {
		BuildNode child;
		child.cell = p_cell;
		child.begin = p_begin;
		child.end = p_end;
		child.flags = p_flags;
		child.parent = p_parent;
		const uint32_t id = build.size();
		build.push_back(child);
		build[p_parent].children.push_back(id);
		return id;
	};

	while (!stack.is_empty()) {
		const uint32_t id = stack[stack.size() - 1u];
		stack.resize(stack.size() - 1u);
		const uint32_t begin = build[id].begin;
		const uint32_t end = build[id].end;
		const uint32_t count = end - begin;
		if (count <= leaf_max) {
			continue; // leaf
		}
		const GaussianSplatHlodCell cell = build[id].cell;
		// Shared-cell children of an origin-centred root keep its flag so they name its cube.
		const uint32_t shared_cell_flags = build[id].flags & kNodeFlagOriginCentredRoot;
		double corner[3];
		double edge;
		region(build[id], corner, edge);
		const bool finest = (build[id].flags & kNodeFlagOriginCentredRoot) == 0u && cell.e <= kMinCellExponent;

		bool identical = true;
		const Vector3 first = p_input.gaussians[perm[begin]].position;
		for (uint32_t i = begin + 1u; i < end && identical; i++) {
			identical = memcmp(&p_input.gaussians[perm[i]].position, &first, sizeof(Vector3)) == 0;
		}

		if (identical || finest) {
			// Index-split group (§6.1): order by (full-precision Morton code in the cell, source
			// index), cut into ceil(N / leaf_max) leaves sharing the cell; more than 8 leaves get
			// intermediate nodes on the same cell so no node has more than 8 children.
			index_split_groups++;
			LocalVector<uint64_t> keys;
			if (!resize_or_fail(keys, count, "gs_hlod::bake_world index_split_keys")) {
				return false;
			}
			for (uint32_t i = 0; i < count; i++) {
				const Vector3 &p = p_input.gaussians[perm[begin + i]].position;
				uint64_t code = 0u;
				const double pv[3] = { p.x, p.y, p.z };
				for (int a = 0; a < 3; a++) {
					const double t = (pv[a] - corner[a]) / edge * 2097152.0;
					const uint64_t q = uint64_t(CLAMP(t, 0.0, 2097151.0));
					code |= spread21(q) << uint32_t(a);
				}
				keys[i] = code;
			}
			LocalVector<uint32_t> order;
			if (!resize_or_fail(order, count, "gs_hlod::bake_world index_split_order")) {
				return false;
			}
			for (uint32_t i = 0; i < count; i++) {
				order[i] = i;
			}
			const uint64_t *k = keys.ptr();
			const uint32_t *src = perm.ptr() + begin;
			std::sort(order.ptr(), order.ptr() + count, [k, src](uint32_t p_a, uint32_t p_b) {
				return k[p_a] < k[p_b] || (k[p_a] == k[p_b] && src[p_a] < src[p_b]);
			});
			for (uint32_t i = 0; i < count; i++) {
				scratch_perm[i] = src[order[i]];
			}
			memcpy(perm.ptr() + begin, scratch_perm.ptr(), sizeof(uint32_t) * count);

			// Groups of chunks: a frontier of (node, first chunk, chunk count).
			const uint32_t chunks = (count + leaf_max - 1u) / leaf_max;
			struct Group {
				uint32_t node;
				uint32_t first;
				uint32_t count;
			};
			LocalVector<Group> groups;
			groups.push_back({ id, 0u, chunks });
			while (!groups.is_empty()) {
				const Group grp = groups[groups.size() - 1u];
				groups.resize(groups.size() - 1u);
				if (grp.count <= kMaxChildren) {
					for (uint32_t c = 0; c < grp.count; c++) {
						const uint32_t chunk = grp.first + c;
						const uint32_t c_begin = begin + chunk * leaf_max;
						const uint32_t c_end = MIN(end, c_begin + leaf_max);
						add_child(grp.node, cell, c_begin, c_end, kNodeFlagSplitByIndex | shared_cell_flags);
					}
					continue;
				}
				// Split into 8 consecutive, nearly equal groups of chunks.
				for (uint32_t g = 0; g < kMaxChildren; g++) {
					const uint32_t g_first = grp.first + uint32_t(uint64_t(grp.count) * g / kMaxChildren);
					const uint32_t g_last = grp.first + uint32_t(uint64_t(grp.count) * (g + 1u) / kMaxChildren);
					if (g_last == g_first) {
						continue;
					}
					const uint32_t c_begin = begin + g_first * leaf_max;
					const uint32_t c_end = MIN(end, begin + g_last * leaf_max);
					const uint32_t child = add_child(grp.node, cell, c_begin, c_end, kNodeFlagSplitByIndex | shared_cell_flags);
					groups.push_back({ child, g_first, g_last - g_first });
				}
			}
			continue;
		}

		// Octant partition at the cell midpoint (stable counting sort, octant bit a = axis a).
		uint32_t bucket_count[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		const double mid[3] = { corner[0] + 0.5 * edge, corner[1] + 0.5 * edge, corner[2] + 0.5 * edge };
		LocalVector<uint8_t> octant;
		if (!resize_or_fail(octant, count, "gs_hlod::bake_world octant")) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			const Vector3 &p = p_input.gaussians[perm[begin + i]].position;
			const uint8_t o = uint8_t((double(p.x) >= mid[0] ? 1u : 0u) | (double(p.y) >= mid[1] ? 2u : 0u) | (double(p.z) >= mid[2] ? 4u : 0u));
			octant[i] = o;
			bucket_count[o]++;
		}
		uint32_t bucket_start[9];
		bucket_start[0] = 0u;
		for (int o = 0; o < 8; o++) {
			bucket_start[o + 1] = bucket_start[o] + bucket_count[o];
		}
		uint32_t cursor[8];
		for (int o = 0; o < 8; o++) {
			cursor[o] = bucket_start[o];
		}
		for (uint32_t i = 0; i < count; i++) {
			scratch_perm[cursor[octant[i]]++] = perm[begin + i];
		}
		memcpy(perm.ptr() + begin, scratch_perm.ptr(), sizeof(uint32_t) * count);

		// Children in octant order. Octants over leaf_max are subdivided; consecutive small
		// octants are grouped into one leaf while they fit (§6.1). The grouping depends only on
		// this cell's content.
		uint32_t group_begin = 0u, group_end = 0u, group_octants = 0u, group_octant = 0u;
		auto flush_group = [&]() {
			if (group_octants == 0u) {
				return;
			}
			GaussianSplatHlodCell child_cell = cell;
			uint32_t flags = kNodeFlagGroupedLeaf | shared_cell_flags;
			if (group_octants == 1u) {
				child_cell = octant_cell(build[id], group_octant);
				flags = 0u;
			}
			add_child(id, child_cell, begin + group_begin, begin + group_end, flags);
			group_octants = 0u;
		};
		for (uint32_t o = 0; o < 8u; o++) {
			const uint32_t c = bucket_count[o];
			if (c == 0u) {
				continue;
			}
			if (c > leaf_max) {
				flush_group();
				const GaussianSplatHlodCell child_cell = octant_cell(build[id], o);
				const uint32_t child = add_child(id, child_cell, begin + bucket_start[o], begin + bucket_start[o + 1u], 0u);
				stack.push_back(child);
				continue;
			}
			if (group_octants > 0u && (group_end - group_begin) + c > leaf_max) {
				flush_group();
			}
			if (group_octants == 0u) {
				group_begin = bucket_start[o];
				group_octant = o;
			}
			group_end = bucket_start[o + 1u];
			group_octants++;
		}
		flush_group();
	}
	const uint64_t t_tree = OS::get_singleton()->get_ticks_usec();

	// ---------------------------------------------------------------- node table (BFS)
	LocalVector<uint32_t> bfs; // node index -> build index
	LocalVector<uint32_t> node_of_build;
	if (!resize_or_fail(node_of_build, build.size(), "gs_hlod::bake_world node_of_build")) {
		return false;
	}
	bfs.push_back(0u);
	node_of_build[0] = 0u;
	for (uint32_t i = 0; i < bfs.size(); i++) {
		const BuildNode &b = build[bfs[i]];
		for (uint32_t c = 0; c < b.children.size(); c++) {
			node_of_build[b.children[c]] = bfs.size();
			bfs.push_back(b.children[c]);
		}
	}
	const uint32_t node_count = bfs.size();
	if (!resize_or_fail(tree.nodes, node_count, "gs_hlod::bake_world nodes")) {
		return false;
	}
	for (uint32_t i = 0; i < node_count; i++) {
		const BuildNode &b = build[bfs[i]];
		GaussianSplatHlodNode &node = tree.nodes[i];
		node = GaussianSplatHlodNode();
		node.cell = b.cell;
		node.parent = b.parent == kNoParent ? kNoParent : node_of_build[b.parent];
		node.child_count = b.children.size();
		node.first_child = b.children.is_empty() ? 0u : node_of_build[b.children[0]];
		node.flags = (b.children.is_empty() ? NODE_KIND_LEAF : NODE_KIND_MERGED) | b.flags;
	}

	// ---------------------------------------------------------------- leaves (§6.2)
	// Depth-first leaf order, so every subtree's leaves are contiguous; importance order inside.
	LocalVector<float> importance;
	if (!resize_or_fail(importance, n, "gs_hlod::bake_world importance")) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		importance[i] = importance_area(p_input.gaussians[i]);
	}
	if (!resize_or_fail(r_result.leaf_gaussians, n, "gs_hlod::bake_world leaf_gaussians")) {
		return false;
	}
	if (sh_count > 0u && !resize_or_fail(r_result.leaf_sh_high_order, uint64_t(n) * sh_count, "gs_hlod::bake_world leaf_sh_high_order")) {
		return false;
	}
	if (!resize_or_fail(r_result.leaf_source_index, n, "gs_hlod::bake_world leaf_source_index")) {
		return false;
	}
	uint32_t leaf_cursor = 0u;
	LocalVector<uint32_t> dfs;
	dfs.push_back(0u);
	while (!dfs.is_empty()) {
		const uint32_t node_index = dfs[dfs.size() - 1u];
		dfs.resize(dfs.size() - 1u);
		GaussianSplatHlodNode &node = tree.nodes[node_index];
		if (node.child_count > 0u) {
			for (uint32_t c = node.child_count; c > 0u; c--) {
				dfs.push_back(node.first_child + c - 1u);
			}
			continue;
		}
		const BuildNode &b = build[bfs[node_index]];
		uint32_t *range = perm.ptr() + b.begin;
		const uint32_t count = b.end - b.begin;
		const float *imp = importance.ptr();
		std::sort(range, range + count, [imp](uint32_t p_a, uint32_t p_b) {
			return imp[p_a] > imp[p_b] || (imp[p_a] == imp[p_b] && p_a < p_b);
		});
		node.payload_first = leaf_cursor;
		node.payload_count = count;
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t src = range[i];
			r_result.leaf_gaussians[leaf_cursor + i] = p_input.gaussians[src];
			r_result.leaf_source_index[leaf_cursor + i] = src;
			if (sh_count > 0u) {
				memcpy(r_result.leaf_sh_high_order.ptr() + uint64_t(leaf_cursor + i) * sh_count,
						p_input.sh_high_order + uint64_t(src) * sh_count, sizeof(Vector3) * sh_count);
			}
		}
		leaf_cursor += count;
	}
	tree.leaf_splat_count = n;

	// ---------------------------------------------------------------- interior merge (§6.3)
	// Bottom-up (reverse BFS). Every payload is node-relative: positions relative to the centre
	// of the node's cell; children are re-expressed relative to the parent's centre first.
	LocalVector<SplatList> interior_payloads;
	if (!resize_or_fail(interior_payloads, node_count, "gs_hlod::bake_world interior_payloads")) {
		return false;
	}
	LocalVector<double> node_lo, node_hi; // world-frame bounds (double), 3 per node
	if (!resize_or_fail(node_lo, uint64_t(node_count) * 3u, "gs_hlod::bake_world node_lo") ||
			!resize_or_fail(node_hi, uint64_t(node_count) * 3u, "gs_hlod::bake_world node_hi")) {
		return false;
	}
	InteriorBakeScratch interior_scratch;
	InteriorBakeResult interior_result;
	SplatList children_frame;
	children_frame.sh_high_order_count = sh_count;
	LocalVector<SplatSpan> child_spans;
	uint64_t merge_usec = 0u, order_usec = 0u;

	for (uint32_t node_index = node_count; node_index > 0u; node_index--) {
		const uint32_t i = node_index - 1u;
		GaussianSplatHlodNode &node = tree.nodes[i];
		double center[3];
		tree.node_cell_center(node, center);
		BoundsAccumulator bounds;
		SplatSpan own;
		const double world_shift_leaf[3] = { 0.0, 0.0, 0.0 };
		double rel_shift[3]; // shift from the payload's frame to the world frame
		if (node.child_count == 0u) {
			own.gaussians = r_result.leaf_gaussians.ptr() + node.payload_first;
			own.sh_high_order = sh_count > 0u ? r_result.leaf_sh_high_order.ptr() + node.payload_first * sh_count : nullptr;
			own.count = node.payload_count;
			own.sh_high_order_count = sh_count;
			add_payload_bounds(own, world_shift_leaf, bounds); // absolute positions
			for (int a = 0; a < 3; a++) {
				rel_shift[a] = 0.0;
			}
		} else {
			// Children in this node's frame.
			children_frame.clear();
			child_spans.resize(node.child_count);
			double max_child_error = 0.0;
			for (uint32_t c = 0; c < node.child_count; c++) {
				const uint32_t ci = node.first_child + c;
				const GaussianSplatHlodNode &child = tree.nodes[ci];
				max_child_error = MAX(max_child_error, double(child.geometric_error));
				SplatSpan src;
				double shift[3];
				if (child.child_count == 0u) {
					src.gaussians = r_result.leaf_gaussians.ptr() + child.payload_first;
					src.sh_high_order = sh_count > 0u ? r_result.leaf_sh_high_order.ptr() + child.payload_first * sh_count : nullptr;
					src.count = child.payload_count;
					for (int a = 0; a < 3; a++) {
						shift[a] = -center[a];
					}
				} else {
					src = interior_payloads[ci].span();
					double child_center[3];
					tree.node_cell_center(child, child_center);
					for (int a = 0; a < 3; a++) {
						shift[a] = child_center[a] - center[a];
					}
				}
				src.sh_high_order_count = sh_count;
				const uint32_t base = children_frame.size();
				const Error appended = children_frame.append(src, 0u, src.count,
						"gs_hlod::bake_world children_frame_gaussians", "gs_hlod::bake_world children_frame_sh_high_order");
				if (appended != OK) {
					return fail(vformat("HLOD node %d: cannot gather the children's payloads.", i),
							appended == ERR_OUT_OF_MEMORY ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA);
				}
				for (uint32_t k = base; k < children_frame.size(); k++) {
					Vector3 &p = children_frame.gaussians[k].position;
					p = Vector3(real_t(double(p.x) + shift[0]), real_t(double(p.y) + shift[1]), real_t(double(p.z) + shift[2]));
				}
				child_spans[c] = SplatSpan();
			}
			// Spans into the converted children (after all appends, so pointers are stable).
			uint32_t offset = 0u;
			for (uint32_t c = 0; c < node.child_count; c++) {
				const uint32_t ci = node.first_child + c;
				const uint32_t cnt = tree.nodes[ci].child_count == 0u ? tree.nodes[ci].payload_count : interior_payloads[ci].size();
				child_spans[c].gaussians = children_frame.gaussians.ptr() + offset;
				child_spans[c].sh_high_order = sh_count > 0u ? children_frame.sh_high_order.ptr() + uint64_t(offset) * sh_count : nullptr;
				child_spans[c].count = cnt;
				child_spans[c].sh_high_order_count = sh_count;
				offset += cnt;
			}
			const double edge = tree.node_cell_edge(node);
			const Vector3 grid_origin(real_t(-0.5 * edge), real_t(-0.5 * edge), real_t(-0.5 * edge));
			String error;
			Error interior_code = ERR_INVALID_DATA;
			if (!bake_interior_node(child_spans.ptr(), node.child_count, grid_origin, edge, max_child_error,
						p_params.interior, interior_scratch, interior_result, &error, &interior_code)) {
				return fail(vformat("HLOD node %d: %s", i, error), interior_code == ERR_OUT_OF_MEMORY ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA);
			}
			merge_usec += interior_result.merge_usec;
			order_usec += interior_result.order_usec;
			// Moved, not copied: the payload was allocated fallibly in bake_interior_node(), and a
			// copy would be a second, unprobed allocation per node (all retained until the end).
			interior_payloads[i] = std::move(interior_result.payload);
			node.geometric_error = round_up(interior_result.geometric_error);
			// Monotone after float rounding: a parent's error is at least twice its children's
			// in double; keep it >= every child after rounding too.
			for (uint32_t c = 0; c < node.child_count; c++) {
				node.geometric_error = MAX(node.geometric_error, tree.nodes[node.first_child + c].geometric_error);
			}
			own = interior_payloads[i].span();
			for (int a = 0; a < 3; a++) {
				rel_shift[a] = center[a];
			}
			add_payload_bounds(own, rel_shift, bounds);
			uint32_t tallest = 0u;
			for (uint32_t c = 0; c < node.child_count; c++) {
				const uint32_t ci = node.first_child + c;
				const double clo[3] = { node_lo[ci * 3u], node_lo[ci * 3u + 1u], node_lo[ci * 3u + 2u] };
				const double chi[3] = { node_hi[ci * 3u], node_hi[ci * 3u + 1u], node_hi[ci * 3u + 2u] };
				bounds.add(clo, chi);
				tallest = MAX(tallest, tree.nodes[ci].height);
			}
			node.height = tallest + 1u;
		}

		// Store bounds relative to the cell centre, rounded outward; record the world bounds the
		// validator will reconstruct, so parents contain exactly what children store.
		for (int a = 0; a < 3; a++) {
			node.aabb_min[a] = round_down_offset(center[a], bounds.lo[a]);
			node.aabb_max[a] = round_up_offset(center[a], bounds.hi[a]);
			node_lo[i * 3u + a] = center[a] + double(node.aabb_min[a]);
			node_hi[i * 3u + a] = center[a] + double(node.aabb_max[a]);
		}
		const double mid[3] = { 0.5 * (node_lo[i * 3u] + node_hi[i * 3u]), 0.5 * (node_lo[i * 3u + 1u] + node_hi[i * 3u + 1u]),
			0.5 * (node_lo[i * 3u + 2u] + node_hi[i * 3u + 2u]) };
		double radius = payload_radius(own, rel_shift, mid);
		for (int a = 0; a < 3; a++) {
			radius = MAX(radius, 0.5 * (double(node.aabb_max[a]) - double(node.aabb_min[a])));
		}
		for (uint32_t c = 0; c < node.child_count; c++) {
			const uint32_t ci = node.first_child + c;
			const double cmid[3] = { 0.5 * (node_lo[ci * 3u] + node_hi[ci * 3u]), 0.5 * (node_lo[ci * 3u + 1u] + node_hi[ci * 3u + 1u]),
				0.5 * (node_lo[ci * 3u + 2u] + node_hi[ci * 3u + 2u]) };
			const double d[3] = { cmid[0] - mid[0], cmid[1] - mid[1], cmid[2] - mid[2] };
			radius = MAX(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + double(tree.nodes[ci].radius));
		}
		node.radius = round_up(radius);
	}

	// ---------------------------------------------------------------- interior section (BFS)
	uint64_t interior_total = 0u;
	for (uint32_t i = 0; i < node_count; i++) {
		interior_total += interior_payloads[i].size();
	}
	if (uint64_t(n) + interior_total > uint64_t(UINT32_MAX)) {
		return fail("HLOD payload exceeds the 32-bit splat index space.");
	}
	// The loader refuses an SH section over UINT32_MAX vectors; refuse to bake one it cannot load.
	if ((uint64_t(n) + interior_total) * uint64_t(sh_count) > uint64_t(UINT32_MAX)) {
		return fail("HLOD payload's SH section exceeds the 32-bit element count the loader accepts.");
	}
	tree.interior_splat_count = uint32_t(interior_total);
	if (!resize_or_fail(tree.interior_gaussians, tree.interior_splat_count, "gs_hlod::bake_world interior_gaussians")) {
		return false;
	}
	if (sh_count > 0u && !resize_or_fail(tree.interior_sh_high_order, uint64_t(tree.interior_splat_count) * sh_count,
								 "gs_hlod::bake_world interior_sh_high_order")) {
		return false;
	}
	uint32_t interior_cursor = 0u;
	for (uint32_t i = 0; i < node_count; i++) {
		GaussianSplatHlodNode &node = tree.nodes[i];
		if (node.child_count == 0u) {
			continue;
		}
		const SplatList &payload = interior_payloads[i];
		node.payload_first = uint64_t(n) + interior_cursor;
		node.payload_count = payload.size();
		memcpy(tree.interior_gaussians.ptr() + interior_cursor, payload.gaussians.ptr(), sizeof(Gaussian) * payload.size());
		if (sh_count > 0u) {
			memcpy(tree.interior_sh_high_order.ptr() + uint64_t(interior_cursor) * sh_count, payload.sh_high_order.ptr(),
					sizeof(Vector3) * payload.size() * sh_count);
		}
		interior_cursor += payload.size();
	}
	tree.interior_resident = true;

	String reason;
	if (!gs_hlod_validate_tree(tree, &reason)) {
		return fail(vformat("HLOD bake produced an invalid tree (bug): %s", reason));
	}
	const uint64_t t_end = OS::get_singleton()->get_ticks_usec();
	tree.bake_stats.tree_build_ms = double(t_tree - t_start) / 1000.0;
	tree.bake_stats.merge_ms = double(merge_usec) / 1000.0;
	tree.bake_stats.order_ms = double(order_usec) / 1000.0;
	tree.bake_stats.total_ms = double(t_end - t_start) / 1000.0;
	tree.bake_stats.index_split_groups = index_split_groups;
	tree.has_bake_stats = true;
	return true;
}

} // namespace gs_hlod
