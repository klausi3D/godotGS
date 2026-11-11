#include "gaussian_splat_hlod_tiled_bake.h"
#include "gaussian_splat_hlod_tiled_internal.h"

#include "core/os/os.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

namespace gs_hlod {
namespace tiled_bake {

Error Context::fail(Error p_code) {
	int expected = OK;
	failure.compare_exchange_strong(expected, int(p_code));
	return Error(failure.load());
}

Error Context::check() {
	const Error stopped = Error(failure.load());
	if (stopped != OK) {
		return stopped;
	}
	uint64_t now;
#ifdef TESTS_ENABLED
	if (params.test_hooks && params.test_hooks->clock_usec) {
		now = params.test_hooks->clock_usec(params.test_hooks->userdata);
	} else
#endif
	{
		now = OS::get_singleton()->get_ticks_usec();
	}
	if (now >= params.limits.deadline_usec) {
		return fail(ERR_TIMEOUT);
	}
	if (params.limits.cancel && params.limits.cancel->load()) {
		return fail(ERR_SKIP);
	}
	return OK;
}

bool Context::reserve(uint64_t p_bytes) {
	if (p_bytes > params.limits.max_ram_bytes - live_reserved) {
		return false; // scheduler may drain the current batch and retry
	}
	live_reserved += p_bytes;
	report.peak_reserved_bytes = MAX(report.peak_reserved_bytes, live_reserved);
	return true;
}

void Context::release(uint64_t p_bytes) {
	live_reserved -= p_bytes;
}

namespace {

bool finite_gaussian(const Gaussian &p_g) {
	const float fields[] = { p_g.position.x, p_g.position.y, p_g.position.z,
		p_g.scale.x, p_g.scale.y, p_g.scale.z, p_g.rotation.x, p_g.rotation.y, p_g.rotation.z, p_g.rotation.w,
		p_g.opacity, p_g.sh_dc.r, p_g.sh_dc.g, p_g.sh_dc.b, p_g.sh_dc.a,
		p_g.sh_1[0].x, p_g.sh_1[0].y, p_g.sh_1[0].z, p_g.sh_1[1].x, p_g.sh_1[1].y, p_g.sh_1[1].z,
		p_g.sh_1[2].x, p_g.sh_1[2].y, p_g.sh_1[2].z, p_g.area, p_g.normal.x, p_g.normal.y, p_g.normal.z,
		p_g.stroke_age, p_g.brush_axes.x, p_g.brush_axes.y };
	for (float f : fields) {
		if (!std::isfinite(f)) {
			return false;
		}
	}
	return true;
}

} // namespace

Error Context::read(uint32_t p_chunk, TiledBakeChunk &r_chunk) {
	if (check() != OK) {
		return check();
	}
	if (p_chunk >= chunks.size()) {
		return fail(ERR_INVALID_DATA);
	}
	String reason;
	const Chunk &chunk = chunks[p_chunk];
	const Error code = source.read_chunk(chunk.source_id, r_chunk, &reason);
	if (code != OK) {
		return fail(code == ERR_OUT_OF_MEMORY || code == ERR_TIMEOUT || code == ERR_SKIP ? code : ERR_INVALID_DATA);
	}
	if (r_chunk.gaussians.size() != chunk.info.splat_count ||
			r_chunk.sh_high_order.size() != uint64_t(chunk.info.splat_count) * info.sh_high_order_count) {
		return fail(ERR_INVALID_DATA);
	}
	// Validate every decoded snapshot, including its advertised closed bounds.
	const Vector3 chunk_end = chunk.info.centre_bounds.get_end();
	for (uint32_t i = 0; i < chunk.info.splat_count; ++i) {
		if ((i & 4095u) == 0u && check() != OK) {
			return check();
		}
		const Gaussian &g = r_chunk.gaussians[i];
		if (!finite_gaussian(g)) {
			return fail(ERR_INVALID_DATA);
		}
		for (int a = 0; a < 3; ++a) {
			const double lo = chunk.info.centre_bounds.position[a];
			const double hi = chunk_end[a];
			if (double(g.position[a]) < lo || double(g.position[a]) > hi) {
				return fail(ERR_INVALID_DATA);
			}
		}
		for (uint32_t c = 0; c < info.sh_high_order_count; ++c) {
			const Vector3 &sh = r_chunk.sh_high_order[uint64_t(i) * info.sh_high_order_count + c];
			if (!sh.is_finite()) {
				return fail(ERR_INVALID_DATA);
			}
		}
	}
	return check();
}

OfflineJob Context::job(uint32_t p_count) const {
	OfflineJob result;
	result.input = p_count;
	result.nodes = tiled_bake::node_bound(p_count);
	result.prepared = MIN(uint64_t(UINT32_MAX), uint64_t(p_count) + (uint64_t(p_count) + 2u) / 3u + result.nodes);
	result.scratch = params.limits.max_ram_bytes;
	result.output = params.limits.max_ram_bytes;
	result.deadline = params.limits.deadline_usec;
	// MERGE_ONLY never allocates the quality raster. Keep every other term of
	// D2-1's formula and its independent admission recheck unchanged.
	result.metric.scratch_peak_bytes = 0;
	result.check_callback = Context::check_job;
	result.check_userdata = const_cast<Context *>(this);
	return result;
}

namespace {

struct Reservation {
	Context &context;
	uint64_t bytes = 0;
	explicit Reservation(Context &p_context) : context(p_context) {}
	~Reservation() { context.release(bytes); }
	bool take(uint64_t p_bytes) {
		if (!context.reserve(p_bytes)) {
			context.fail(ERR_OUT_OF_MEMORY);
			return false;
		}
		bytes += p_bytes;
		return true;
	}
};

struct Counts {
	uint32_t count = 0;
	Vector3 first;
	Vector3 lo;
	Vector3 hi;
	bool identical = true;
	void add(const Counts &p_other) {
		if (p_other.count == 0u) {
			return;
		}
		if (count == 0u) {
			*this = p_other;
			return;
		}
		identical = identical && p_other.identical && std::memcmp(&first, &p_other.first, sizeof(Vector3)) == 0;
		for (int a = 0; a < 3; ++a) {
			lo[a] = MIN(lo[a], p_other.lo[a]);
			hi[a] = MAX(hi[a], p_other.hi[a]);
		}
		count += p_other.count; // disjoint subsets of the validated uint32 input
	}
	void add(const Vector3 &p_position) {
		Counts one;
		one.count = 1;
		one.first = one.lo = one.hi = p_position;
		add(one);
	}
};

struct Bin {
	GaussianSplatHlodCell cell;
	Counts counts;
};

// Fallible open-addressed histogram. 512 reserved bytes per bin cover bins,
// hash slots, vector growth and simultaneous old/new/allocation-probe storage.
// No per-splat partition or payload is retained in the spatial histogram.
class Histogram {
	Context &context;
	Reservation memory;
	LocalVector<uint32_t> slots; // bin id + 1; zero is empty
	static uint64_t hash(const GaussianSplatHlodCell &p_cell) {
		uint64_t h = uint64_t(p_cell.e) * 0x9e3779b97f4a7c15ull;
		for (int64_t v : { p_cell.ix, p_cell.iy, p_cell.iz }) {
			uint64_t x = uint64_t(v);
			x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
			x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
			h ^= x ^ (x >> 31u);
			h = (h << 21u) | (h >> 43u);
		}
		return h;
	}
	bool grow_slots(uint32_t p_size) {
		LocalVector<uint32_t> next;
		if (!fallible_resize(next, p_size, "gs_hlod::tiled histogram slots")) {
			context.fail(ERR_OUT_OF_MEMORY);
			return false;
		}
		for (uint32_t &s : next) {
			s = 0;
		}
		for (uint32_t i = 0; i < bins.size(); ++i) {
			uint32_t s = uint32_t(hash(bins[i].cell)) & (p_size - 1u);
			while (next[s] != 0u) {
				s = (s + 1u) & (p_size - 1u);
			}
			next[s] = i + 1u;
		}
		slots = std::move(next);
		return true;
	}

public:
	LocalVector<Bin> bins;
	explicit Histogram(Context &p_context) : context(p_context), memory(p_context) {}
	const Counts *find(const GaussianSplatHlodCell &p_cell) const {
		if (slots.is_empty()) {
			return nullptr;
		}
		uint32_t s = uint32_t(hash(p_cell)) & (slots.size() - 1u);
		while (slots[s] != 0u) {
			const Bin &bin = bins[slots[s] - 1u];
			if (bin.cell == p_cell) {
				return &bin.counts;
			}
			s = (s + 1u) & (slots.size() - 1u);
		}
		return nullptr;
	}
	bool add(const GaussianSplatHlodCell &p_cell, const Counts &p_counts) {
		if (p_counts.count == 0u) {
			return true;
		}
		if (const Counts *existing = find(p_cell)) {
			const_cast<Counts *>(existing)->add(p_counts);
			return true;
		}
		if (bins.size() >= (uint32_t(1) << 29u) || !memory.take(512u)) {
			context.fail(ERR_OUT_OF_MEMORY);
			return false;
		}
		if (slots.is_empty() || uint64_t(bins.size() + 1u) * 2u > slots.size()) {
			if (!grow_slots(slots.is_empty() ? 8u : slots.size() * 2u)) {
				return false;
			}
		}
		const uint32_t id = bins.size();
		if (!fallible_resize(bins, uint64_t(id) + 1u, "gs_hlod::tiled histogram bins")) {
			context.fail(ERR_OUT_OF_MEMORY);
			return false;
		}
		bins[id].cell = p_cell;
		bins[id].counts = p_counts;
		uint32_t s = uint32_t(hash(p_cell)) & (slots.size() - 1u);
		while (slots[s] != 0u) {
			s = (s + 1u) & (slots.size() - 1u);
		}
		slots[s] = id + 1u;
		return true;
	}
};

int64_t parent_index(int64_t p_index) {
	return p_index >= 0 ? p_index / 2 : (p_index + 1) / 2 - 1;
}

bool cell_at(const Vector3 &p_position, int32_t p_exponent, GaussianSplatHlodCell &r_cell) {
	r_cell.e = p_exponent;
	const double edge = std::ldexp(1.0, p_exponent);
	int64_t indices[3];
	for (int a = 0; a < 3; ++a) {
		const double v = std::floor(double(p_position[a]) / edge);
		if (!std::isfinite(v) || std::abs(v) >= double(kMaxCellIndexMagnitude)) {
			return false;
		}
		indices[a] = int64_t(v);
	}
	r_cell.ix = indices[0];
	r_cell.iy = indices[1];
	r_cell.iz = indices[2];
	return true;
}

void region(const GaussianSplatHlodNode &p_node, double r_lo[3], double &r_edge) {
	r_edge = std::ldexp(1.0, p_node.cell.e);
	if (p_node.flags & kNodeFlagOriginCentredRoot) {
		r_lo[0] = r_lo[1] = r_lo[2] = -r_edge;
		r_edge *= 2.0;
	} else {
		r_lo[0] = double(p_node.cell.ix) * r_edge;
		r_lo[1] = double(p_node.cell.iy) * r_edge;
		r_lo[2] = double(p_node.cell.iz) * r_edge;
	}
}

bool contains(const GaussianSplatHlodNode &p_node, const Vector3 &p_position) {
	double lo[3], edge;
	region(p_node, lo, edge);
	for (int a = 0; a < 3; ++a) {
		if (double(p_position[a]) < lo[a] || double(p_position[a]) >= lo[a] + edge) {
			return false;
		}
	}
	return true;
}

bool intersects(const GaussianSplatHlodNode &p_node, const AABB &p_bounds) {
	double lo[3], edge;
	region(p_node, lo, edge);
	const Vector3 end = p_bounds.get_end();
	for (int a = 0; a < 3; ++a) {
		const double lower = p_bounds.position[a];
		const double upper = end[a];
		if (upper < lo[a] || lower >= lo[a] + edge) {
			return false;
		}
	}
	return true;
}

GaussianSplatHlodCell child_cell(const GaussianSplatHlodNode &p_node, uint32_t p_octant) {
	GaussianSplatHlodCell cell;
	const int64_t x = int64_t(p_octant & 1u);
	const int64_t y = int64_t((p_octant >> 1u) & 1u);
	const int64_t z = int64_t((p_octant >> 2u) & 1u);
	if (p_node.flags & kNodeFlagOriginCentredRoot) {
		cell.e = p_node.cell.e;
		cell.ix = x - 1;
		cell.iy = y - 1;
		cell.iz = z - 1;
	} else {
		cell.e = p_node.cell.e - 1;
		cell.ix = p_node.cell.ix * 2 + x;
		cell.iy = p_node.cell.iy * 2 + y;
		cell.iz = p_node.cell.iz * 2 + z;
	}
	return cell;
}

uint64_t spread21(uint64_t p_value) {
	p_value &= 0x1fffffu;
	p_value = (p_value | (p_value << 32u)) & 0x1f00000000ffffull;
	p_value = (p_value | (p_value << 16u)) & 0x1f0000ff0000ffull;
	p_value = (p_value | (p_value << 8u)) & 0x100f00f00f00f00full;
	p_value = (p_value | (p_value << 4u)) & 0x10c30c30c30c30c3ull;
	p_value = (p_value | (p_value << 2u)) & 0x1249249249249249ull;
	return p_value;
}

struct IndexEntry {
	uint64_t key = 0;
	uint32_t global = 0;
};

class Planner {
	Context &context;
	Histogram &histogram;
	int32_t fine_exponent;

	uint32_t add_child(uint32_t p_parent, const GaussianSplatHlodCell &p_cell, uint32_t p_count, uint32_t p_flags) {
		if (context.plan.size() >= context.node_bound || context.plan[p_parent].depth + 1u > kMaxHeight ||
				context.plan[p_parent].record.child_count >= kMaxChildren) {
			context.fail(ERR_INVALID_DATA);
			return kNoParent;
		}
		const uint32_t id = context.plan.size();
		if (!fallible_resize(context.plan, uint64_t(id) + 1u, "gs_hlod::tiled plan")) {
			context.fail(ERR_OUT_OF_MEMORY);
			return kNoParent;
		}
		PlanNode &node = context.plan[id];
		node.record.cell = p_cell;
		node.record.flags = p_flags;
		node.record.parent = p_parent;
		node.input_count = p_count;
		node.depth = context.plan[p_parent].depth + 1u;
		PlanNode &parent = context.plan[p_parent];
		parent.children[parent.record.child_count++] = id;
		parent.record.flags = (parent.record.flags & ~kNodeFlagKindMask) | NODE_KIND_MERGED;
		return id;
	}

	Error child_counts(uint32_t p_node, Counts r_counts[kMaxChildren]) {
		const GaussianSplatHlodNode record = context.plan[p_node].record;
		if (child_cell(record, 0).e >= fine_exponent) {
			for (uint32_t c = 0; c < kMaxChildren; ++c) {
				if (const Counts *found = histogram.find(child_cell(record, c))) {
					r_counts[c] = *found;
				}
			}
		} else {
			// Only overfull fine bins need another histogram pass. Refine one
			// occupied cell at a time instead of retaining a splat-sized partition.
			Reservation decode(context);
			if (!decode.take(context.decode_bytes)) {
				return context.check();
			}
			TiledBakeChunk chunk;
			double lo[3], edge;
			region(record, lo, edge);
			for (uint32_t id : context.ordered_chunks) {
				if (!intersects(record, context.chunks[id].info.centre_bounds)) {
					continue;
				}
				if (context.read(id, chunk) != OK) {
					return context.check();
				}
				for (uint32_t i = 0; i < chunk.gaussians.size(); ++i) {
					if ((i & 4095u) == 0u && context.check() != OK) {
						return context.check();
					}
					const Vector3 &p = chunk.gaussians[i].position;
					if (contains(record, p)) {
						const uint32_t octant = (double(p.x) >= lo[0] + edge * 0.5 ? 1u : 0u) |
								(double(p.y) >= lo[1] + edge * 0.5 ? 2u : 0u) | (double(p.z) >= lo[2] + edge * 0.5 ? 4u : 0u);
						r_counts[octant].add(p);
					}
				}
			}
		}
		uint64_t sum = 0;
		for (uint32_t c = 0; c < kMaxChildren; ++c) {
			sum += r_counts[c].count;
		}
		return sum == context.plan[p_node].input_count ? context.check() : context.fail(ERR_INVALID_DATA);
	}

	Error index_group(uint32_t p_node, uint32_t p_first_chunk, uint32_t p_chunks,
			const LocalVector<IndexEntry> &p_entries, uint32_t p_origin_flags) {
		if (context.check() != OK) {
			return context.check();
		}
		const GaussianSplatHlodCell cell = context.plan[p_node].record.cell;
		if (p_chunks <= kMaxChildren) {
			for (uint32_t c = 0; c < p_chunks; ++c) {
				const uint32_t begin = (p_first_chunk + c) * kMaxNodeSplats;
				const uint32_t count = MIN(kMaxNodeSplats, p_entries.size() - begin);
				const uint32_t id = add_child(p_node, cell, count, kNodeFlagSplitByIndex | p_origin_flags);
				if (id == kNoParent) {
					return context.check();
				}
				const uint32_t offset = context.selected_indices.size();
				if (!fallible_resize(context.selected_indices, uint64_t(offset) + count, "gs_hlod::tiled index selections")) {
					return context.fail(ERR_OUT_OF_MEMORY);
				}
				for (uint32_t i = 0; i < count; ++i) {
					context.selected_indices[offset + i] = p_entries[begin + i].global;
				}
				std::sort(context.selected_indices.ptr() + offset, context.selected_indices.ptr() + offset + count);
				context.plan[id].work = true;
				context.plan[id].indexed = true;
				context.plan[id].selection_first = offset;
			}
		} else {
			for (uint32_t g = 0; g < kMaxChildren; ++g) {
				const uint32_t first = p_first_chunk + uint32_t(uint64_t(p_chunks) * g / kMaxChildren);
				const uint32_t last = p_first_chunk + uint32_t(uint64_t(p_chunks) * (g + 1u) / kMaxChildren);
				const uint32_t begin = first * kMaxNodeSplats;
				const uint32_t end = uint32_t(MIN(uint64_t(p_entries.size()), uint64_t(last) * kMaxNodeSplats));
				const uint32_t id = add_child(p_node, cell, end - begin, kNodeFlagSplitByIndex | p_origin_flags);
				if (id == kNoParent || index_group(id, first, last - first, p_entries, p_origin_flags) != OK) {
					return context.check();
				}
			}
		}
		return context.check();
	}

	Error split_by_index(uint32_t p_node) {
		const GaussianSplatHlodNode record = context.plan[p_node].record;
		const uint32_t count = context.plan[p_node].input_count;
		Reservation memory(context);
		if (!memory.take(uint64_t(count) * sizeof(IndexEntry) * 3u + context.decode_bytes)) {
			return context.check();
		}
		LocalVector<IndexEntry> entries;
		if (!fallible_resize(entries, count, "gs_hlod::tiled index entries")) {
			return context.fail(ERR_OUT_OF_MEMORY);
		}
		TiledBakeChunk chunk;
		double lo[3], edge;
		region(record, lo, edge);
		uint32_t cursor = 0;
		for (uint32_t id : context.ordered_chunks) {
			if (!intersects(record, context.chunks[id].info.centre_bounds)) {
				continue;
			}
			if (context.read(id, chunk) != OK) {
				return context.check();
			}
			for (uint32_t i = 0; i < chunk.gaussians.size(); ++i) {
				if ((i & 4095u) == 0u && context.check() != OK) {
					return context.check();
				}
				const Vector3 &p = chunk.gaussians[i].position;
				if (!contains(record, p)) {
					continue;
				}
				if (cursor >= count) {
					return context.fail(ERR_INVALID_DATA);
				}
				uint64_t key = 0;
				for (int a = 0; a < 3; ++a) {
					const double t = (double(p[a]) - lo[a]) / edge * 2097152.0;
					key |= spread21(uint64_t(CLAMP(t, 0.0, 2097151.0))) << uint32_t(a);
				}
				entries[cursor++] = { key, context.chunks[id].info.first_splat + i };
			}
		}
		if (cursor != count) {
			return context.fail(ERR_INVALID_DATA);
		}
		std::sort(entries.ptr(), entries.ptr() + entries.size(), [](const IndexEntry &p_a, const IndexEntry &p_b) {
			return p_a.key < p_b.key || (p_a.key == p_b.key && p_a.global < p_b.global);
		});
		return index_group(p_node, 0u, uint32_t((uint64_t(count) + kMaxNodeSplats - 1u) / kMaxNodeSplats),
				entries, record.flags & kNodeFlagOriginCentredRoot);
	}

public:
	Planner(Context &p_context, Histogram &p_histogram, int32_t p_fine_exponent) :
			context(p_context), histogram(p_histogram), fine_exponent(p_fine_exponent) {}

	Error split(uint32_t p_node, const Counts &p_counts) {
		if (context.check() != OK) {
			return context.check();
		}
		const GaussianSplatHlodNode record = context.plan[p_node].record;
		if (p_counts.count <= kMaxNodeSplats ||
				(p_counts.count <= context.params.max_tile_splats && !(record.flags & kNodeFlagOriginCentredRoot))) {
			context.plan[p_node].work = true;
			return OK;
		}
		if (p_counts.identical || (!(record.flags & kNodeFlagOriginCentredRoot) && record.cell.e <= kMinCellExponent)) {
			return split_by_index(p_node);
		}
		Counts counts[kMaxChildren];
		if (child_counts(p_node, counts) != OK) {
			return context.check();
		}
		uint32_t group_count = 0, group_octants = 0, single_octant = 0;
		uint8_t mask = 0;
		auto flush = [&]() -> Error {
			if (group_count == 0u) {
				return OK;
			}
			const GaussianSplatHlodCell cell = group_octants == 1u ? child_cell(record, single_octant) : record.cell;
			const uint32_t flags = group_octants == 1u ? 0u : kNodeFlagGroupedLeaf | (record.flags & kNodeFlagOriginCentredRoot);
			const uint32_t id = add_child(p_node, cell, group_count, flags);
			if (id == kNoParent) {
				return context.check();
			}
			context.plan[id].work = true;
			context.plan[id].octant_mask = group_octants == 1u ? 0xff : mask;
			group_count = group_octants = 0;
			mask = 0;
			return OK;
		};
		for (uint32_t c = 0; c < kMaxChildren; ++c) {
			const uint32_t count = counts[c].count;
			if (count == 0u) {
				continue;
			}
			if (count > kMaxNodeSplats) {
				if (flush() != OK) {
					return context.check();
				}
				const uint32_t id = add_child(p_node, child_cell(record, c), count, 0u);
				if (id == kNoParent || split(id, counts[c]) != OK) {
					return context.check();
				}
			} else {
				if (group_count + count > kMaxNodeSplats && flush() != OK) {
					return context.check();
				}
				group_count += count;
				++group_octants;
				single_octant = c;
				mask |= uint8_t(1u << c);
			}
		}
		return flush();
	}
};

Error descriptors(Context &p_context, int32_t &r_fine, int32_t &r_coarse) {
	Context &ctx = p_context;
	if (!fallible_resize(ctx.chunks, ctx.info.chunk_count, "gs_hlod::tiled descriptors") ||
			!fallible_resize(ctx.ordered_chunks, ctx.info.chunk_count, "gs_hlod::tiled chunk order")) {
		return ctx.fail(ERR_OUT_OF_MEMORY);
	}
	double lo[3] = { DBL_MAX, DBL_MAX, DBL_MAX };
	double hi[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
	uint32_t max_chunk = 0;
	for (uint32_t i = 0; i < ctx.info.chunk_count; ++i) {
		if (ctx.check() != OK) {
			return ctx.check();
		}
		String error;
		TiledBakeChunkInfo &info = ctx.chunks[i].info;
		const Error code = ctx.source.get_chunk_info(i, info, &error);
		if (code != OK) {
			return ctx.fail(code == ERR_OUT_OF_MEMORY || code == ERR_TIMEOUT || code == ERR_SKIP ? code : ERR_INVALID_DATA);
		}
		if (info.splat_count == 0u || uint64_t(info.first_splat) + info.splat_count > ctx.info.splat_count ||
				uint64_t(info.splat_count) * ctx.info.sh_high_order_count > UINT32_MAX) {
			return ctx.fail(ERR_INVALID_DATA);
		}
		const Vector3 end = info.centre_bounds.get_end();
		for (int a = 0; a < 3; ++a) {
			const double lower = info.centre_bounds.position[a];
			const double size = info.centre_bounds.size[a];
			if (!std::isfinite(lower) || !std::isfinite(size) || !std::isfinite(end[a]) || size < 0.0) {
				return ctx.fail(ERR_INVALID_DATA);
			}
			lo[a] = MIN(lo[a], lower);
			hi[a] = MAX(hi[a], double(end[a]));
		}
		max_chunk = MAX(max_chunk, info.splat_count);
		ctx.chunks[i].source_id = i;
		ctx.ordered_chunks[i] = i;
	}
	std::sort(ctx.ordered_chunks.ptr(), ctx.ordered_chunks.ptr() + ctx.ordered_chunks.size(), [&ctx](uint32_t p_a, uint32_t p_b) {
		return ctx.chunks[p_a].info.first_splat < ctx.chunks[p_b].info.first_splat;
	});
	uint64_t cursor = 0;
	for (uint32_t id : ctx.ordered_chunks) {
		if (ctx.chunks[id].info.first_splat != cursor) {
			return ctx.fail(ERR_INVALID_DATA);
		}
		cursor += ctx.chunks[id].info.splat_count;
	}
	if (cursor != ctx.info.splat_count) {
		return ctx.fail(ERR_INVALID_DATA);
	}
	ctx.decode_bytes = 3u * uint64_t(max_chunk) * ctx.payload_bytes;
	GaussianSplatHlodCell coarse;
	bool origin_centred = false;
	const double origin[3] = {};
	r_coarse = choose_root_cell(origin, lo, hi, coarse, origin_centred) ? coarse.e : kMaxCellExponent;
	r_fine = MIN<int32_t>(3, r_coarse);
	// Avoid narrowing an out-of-range double to int64 even for far-away source
	// AABBs. Actual centres/root are independently checked in the histogram pass.
	while (r_fine < kMaxCellExponent) {
		const double edge = std::ldexp(1.0, r_fine);
		bool fits = true;
		for (int a = 0; a < 3; ++a) {
			fits = fits && std::abs(std::floor(lo[a] / edge)) < double(kMaxCellIndexMagnitude) &&
					std::abs(std::floor(hi[a] / edge)) < double(kMaxCellIndexMagnitude);
		}
		if (fits) {
			break;
		}
		++r_fine;
	}
	r_coarse = MAX(r_coarse, r_fine);
	return ctx.check();
}

Error histogram_and_plan(Context &p_context, int32_t p_fine, int32_t p_coarse) {
	Context &ctx = p_context;
	Histogram histogram(ctx);
	Counts total;
	{
		Reservation decode(ctx);
		if (!decode.take(ctx.decode_bytes)) {
			return ctx.check();
		}
		TiledBakeChunk chunk;
		for (uint32_t id : ctx.ordered_chunks) {
			if (ctx.read(id, chunk) != OK) {
				return ctx.check();
			}
			for (uint32_t i = 0; i < chunk.gaussians.size(); ++i) {
				if ((i & 4095u) == 0u && ctx.check() != OK) {
					return ctx.check();
				}
				Counts one;
				one.add(chunk.gaussians[i].position);
				total.add(one);
				GaussianSplatHlodCell cell;
				if (!cell_at(one.first, p_fine, cell)) {
					return ctx.fail(ERR_INVALID_DATA);
				}
				if (!histogram.add(cell, one)) {
					return ctx.check();
				}
			}
		}
	}
	// Aggregate occupied fine cells, not individual splats, into ancestor counts.
	const uint32_t fine_count = histogram.bins.size();
	for (uint32_t i = 0; i < fine_count; ++i) {
		if (ctx.check() != OK) {
			return ctx.check();
		}
		const Bin bin = histogram.bins[i]; // insertion can relocate the histogram
		GaussianSplatHlodCell cell = bin.cell;
		while (cell.e < p_coarse) {
			++cell.e;
			cell.ix = parent_index(cell.ix);
			cell.iy = parent_index(cell.iy);
			cell.iz = parent_index(cell.iz);
			if (!histogram.add(cell, bin.counts)) {
				return ctx.check();
			}
		}
	}
	if (total.count != ctx.info.splat_count || !fallible_resize(ctx.plan, 1u, "gs_hlod::tiled root")) {
		return ctx.fail(total.count != ctx.info.splat_count ? ERR_INVALID_DATA : ERR_OUT_OF_MEMORY);
	}
	const double origin[3] = {};
	const double lo[3] = { total.lo.x, total.lo.y, total.lo.z };
	const double hi[3] = { total.hi.x, total.hi.y, total.hi.z };
	bool origin_centred = false;
	if (!choose_root_cell(origin, lo, hi, ctx.plan[0].record.cell, origin_centred)) {
		return ctx.fail(ERR_INVALID_DATA);
	}
	ctx.plan[0].record.flags = origin_centred ? kNodeFlagOriginCentredRoot : 0u;
	ctx.plan[0].input_count = total.count;
	Planner planner(ctx, histogram, p_fine);
	return planner.split(0u, total);
}

} // namespace
} // namespace tiled_bake

Error TiledAutoTree::build(const TiledBakeSource &p_source, const TiledBakeParams &p_params,
		BakeResult &r_result, TiledBakeReport &r_report, String *r_error) {
	r_result = BakeResult();
	r_report = TiledBakeReport();
	if (r_error) {
		*r_error = String();
	}
	tiled_bake::Context context(p_source, p_params, r_report);
	auto fail = [&](Error p_code) {
		context.fail(p_code);
		if (r_error) {
			*r_error = vformat("Tiled HLOD build failed closed (error %d).", int(context.check()));
		}
		return context.check();
	};
	if (context.check() != OK) {
		return fail(context.check());
	}
	context.info = p_source.get_info();
	if (p_params.worker_count == 0u || p_params.max_tile_splats <= kMaxNodeSplats ||
			context.info.splat_count == 0u || context.info.chunk_count == 0u ||
			context.info.chunk_count > context.info.splat_count || context.info.sh_first_order_count > 3u ||
			context.info.sh_high_order_count > 21u ||
			uint64_t(context.info.splat_count) * context.info.sh_high_order_count > UINT32_MAX) {
		return fail(ERR_INVALID_DATA);
	}
	context.payload_bytes = sizeof(Gaussian) + uint64_t(context.info.sh_high_order_count) * sizeof(Vector3);
	context.node_bound = tiled_bake::node_bound(context.info.splat_count);
	if (context.node_bound > UINT32_MAX) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	// Reserve retained tiles + stitched payload + final assembly, including
	// vector growth/probes, mappings and node metadata BEFORE the first allocation.
	// The reduction-4 bound is the same inequality used by OfflineJob::reserve.
	const uint64_t interiors = (uint64_t(context.info.splat_count) + 2u) / 3u + context.node_bound;
	const uint64_t resident = 4u * (uint64_t(context.info.splat_count) + interiors) * context.payload_bytes;
	const uint64_t metadata = context.node_bound * 4096u + uint64_t(context.info.splat_count) * 64u +
			uint64_t(context.info.chunk_count) * 256u;
	tiled_bake::Reservation permanent(context);
	if (!permanent.take(resident + metadata)) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	OfflineJob coordinator = context.job(context.info.splat_count);
	OfflineJobScope lease(coordinator);
	if (!lease.acquired) {
		return fail(ERR_BUSY);
	}
	int32_t fine = 3, coarse = 3;
	Error code = tiled_bake::descriptors(context, fine, coarse);
	if (code == OK) {
		code = tiled_bake::histogram_and_plan(context, fine, coarse);
	}
	if (code == OK) {
		code = tiled_bake::bake_tiles(context);
	}
	BakeResult assembled;
	if (code == OK) {
		bool has_stitch = false;
		for (const tiled_bake::PlanNode &node : context.plan) {
			has_stitch = has_stitch || !node.work;
		}
		// One serial merge workspace; reserve the existing job formula's bounded
		// eight-child maximum. No tile worker remains alive during stitching.
		OfflineJob stitch = context.job(kMaxChildren * kMaxNodeSplats);
		tiled_bake::Reservation scratch(context);
		if (has_stitch && (!stitch.reserve(kMaxChildren * kMaxNodeSplats, context.info.sh_high_order_count, 0u) ||
				!scratch.take(stitch.reserved))) {
			code = context.fail(ERR_OUT_OF_MEMORY);
		} else {
			code = tiled_bake::stitch_and_assemble(context, assembled);
		}
	}
	if (code != OK || context.check() != OK) {
		return fail(code != OK ? code : context.check());
	}
	r_result = std::move(assembled);
	return OK;
}

} // namespace gs_hlod
