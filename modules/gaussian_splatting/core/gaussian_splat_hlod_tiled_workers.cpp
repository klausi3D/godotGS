#include "gaussian_splat_hlod_tiled_internal.h"

#include "core/os/thread.h"

#include <cmath>

namespace gs_hlod {
namespace tiled_bake {
namespace {

// The coordinator owns the process-wide OfflineJobScope. A worker only binds
// its privately owned admission/check state to the existing bake's TLS slot.
struct WorkerJobScope {
	OfflineJob *previous = active_offline_job;
	explicit WorkerJobScope(OfflineJob &p_job) { active_offline_job = &p_job; }
	~WorkerJobScope() { active_offline_job = previous; }
};

struct Worker {
	Context *context = nullptr;
	uint32_t node = 0;
	OfflineJob job;
	uint64_t reserved = 0;
	Thread *thread = nullptr;
	Error result = OK; // worker-owned until joined
};

uint32_t selection_lower_bound(const Context &p_context, const PlanNode &p_node, uint32_t p_global) {
	uint32_t first = p_node.selection_first;
	uint32_t end = first + p_node.input_count;
	while (first < end) {
		const uint32_t middle = first + (end - first) / 2u;
		if (p_context.selected_indices[middle] < p_global) {
			first = middle + 1u;
		} else {
			end = middle;
		}
	}
	return first;
}

void tile_region(const PlanNode &p_node, double r_corner[3], double &r_edge) {
	const GaussianSplatHlodCell &cell = p_node.record.cell;
	r_edge = std::ldexp(1.0, cell.e);
	r_corner[0] = double(cell.ix) * r_edge;
	r_corner[1] = double(cell.iy) * r_edge;
	r_corner[2] = double(cell.iz) * r_edge;
	if ((p_node.record.flags & kNodeFlagOriginCentredRoot) != 0u) {
		r_corner[0] = r_corner[1] = r_corner[2] = -r_edge;
		r_edge *= 2.0;
	}
}

bool chunk_may_select(const Context &p_context, const PlanNode &p_node, const TiledBakeChunkInfo &p_chunk) {
	if (p_node.indexed) {
		const uint32_t first = selection_lower_bound(p_context, p_node, p_chunk.first_splat);
		return first < p_node.selection_first + p_node.input_count &&
				uint64_t(p_context.selected_indices[first]) < uint64_t(p_chunk.first_splat) + p_chunk.splat_count;
	}
	double corner[3], edge;
	tile_region(p_node, corner, edge);
	// Descriptors have closed bounds; tiles exclude their upper faces. Use the
	// AABB's own real_t addition, matching how its extents were constructed.
	const Vector3 end = p_chunk.centre_bounds.get_end();
	for (uint32_t axis = 0; axis < 3u; ++axis) {
		if (double(end[axis]) < corner[axis] ||
				double(p_chunk.centre_bounds.position[axis]) >= corner[axis] + edge) {
			return false;
		}
	}
	return true;
}

bool selects(const Context &p_context, const PlanNode &p_node, uint32_t p_global, const Vector3 &p_position) {
	if (p_node.indexed) {
		const uint32_t first = selection_lower_bound(p_context, p_node, p_global);
		return first < p_node.selection_first + p_node.input_count && p_context.selected_indices[first] == p_global;
	}
	double corner[3], edge;
	tile_region(p_node, corner, edge);
	uint32_t octant = 0;
	for (uint32_t axis = 0; axis < 3u; ++axis) {
		const double value = double(p_position[axis]);
		if (value < corner[axis] || value >= corner[axis] + edge) {
			return false;
		}
		if (value >= corner[axis] + 0.5 * edge) {
			octant |= 1u << axis;
		}
	}
	return (p_node.octant_mask & (1u << octant)) != 0u;
}

Error bake_tile(Worker &p_worker) {
	Context &context = *p_worker.context;
	PlanNode &node = context.plan[p_worker.node];
	if (context.check() != OK) {
		return context.check();
	}
	if (node.indexed && uint64_t(node.selection_first) + node.input_count > context.selected_indices.size()) {
		return context.fail(ERR_INVALID_DATA);
	}
	TiledBakeChunk gathered;
	LocalVector<uint32_t> global_indices;
	if (!fallible_resize(gathered.gaussians, node.input_count, "gs_hlod::tiled input") ||
			!fallible_resize(gathered.sh_high_order, uint64_t(node.input_count) * context.info.sh_high_order_count,
					"gs_hlod::tiled input SH") ||
			!fallible_resize(global_indices, node.input_count, "gs_hlod::tiled input indices")) {
		return context.fail(ERR_OUT_OF_MEMORY);
	}
	bool ordered = true;
#ifdef TESTS_ENABLED
	if (context.params.test_hooks) {
		ordered = !context.params.test_hooks->bypass_input_ordering;
	}
#endif
	uint32_t written = 0;
	for (uint32_t i = 0; i < context.chunks.size(); ++i) {
		if (context.check() != OK) {
			return context.check();
		}
		const uint32_t chunk_id = ordered ? context.ordered_chunks[i] : i;
		const Chunk &descriptor = context.chunks[chunk_id];
		if (!chunk_may_select(context, node, descriptor.info)) {
			continue;
		}
		TiledBakeChunk decoded;
		const Error read_error = context.read(chunk_id, decoded);
		if (read_error != OK) {
			return context.fail(read_error);
		}
		for (uint32_t j = 0; j < descriptor.info.splat_count; ++j) {
			if ((j & 4095u) == 0u && context.check() != OK) {
				return context.check();
			}
			const uint32_t global = descriptor.info.first_splat + j;
			if (!selects(context, node, global, decoded.gaussians[j].position)) {
				continue;
			}
			if (written >= node.input_count) {
				return context.fail(ERR_INVALID_DATA);
			}
			gathered.gaussians[written] = decoded.gaussians[j];
			global_indices[written] = global;
			for (uint32_t sh = 0; sh < context.info.sh_high_order_count; ++sh) {
				gathered.sh_high_order[written * context.info.sh_high_order_count + sh] =
						decoded.sh_high_order[j * context.info.sh_high_order_count + sh];
			}
			++written;
		}
	}
	if (written != node.input_count) {
		return context.fail(ERR_INVALID_DATA);
	}
	BakeInput input;
	input.gaussians = gathered.gaussians.ptr();
	input.sh_high_order = gathered.sh_high_order.ptr();
	input.count = node.input_count;
	input.sh_high_order_count = context.info.sh_high_order_count;
	input.sh_first_order_count = context.info.sh_first_order_count;
	BakeParams params;
	const bool origin_centred = (node.record.flags & kNodeFlagOriginCentredRoot) != 0u;
	if (origin_centred && node.input_count > kMaxNodeSplats) {
		return context.fail(ERR_INVALID_DATA);
	}
	if (!origin_centred) {
		params.forced_root = &node.record.cell;
	}
	Error code = OK;
	{
		WorkerJobScope scope(p_worker.job);
		if (!bake_world(input, params, node.tile, nullptr, &code)) {
			return context.fail(code == OK ? ERR_INVALID_DATA : code);
		}
	}
	if (node.tile.tree.nodes.is_empty() || node.tile.leaf_source_index.size() != node.input_count) {
		return context.fail(ERR_INVALID_DATA);
	}
	for (uint32_t i = 0; i < node.tile.leaf_source_index.size(); ++i) {
		if ((i & 4095u) == 0u && context.check() != OK) {
			return context.check();
		}
		const uint32_t local = node.tile.leaf_source_index[i];
		if (local >= global_indices.size()) {
			return context.fail(ERR_INVALID_DATA);
		}
		node.tile.leaf_source_index[i] = global_indices[local];
	}
	if (origin_centred || node.indexed || (node.record.flags & kNodeFlagGroupedLeaf) != 0u) {
		GaussianSplatHlodNode &root = node.tile.tree.nodes[0];
		if (!root.is_leaf()) {
			return context.fail(ERR_INVALID_DATA);
		}
		root.cell = node.record.cell;
		root.flags = node.record.flags;
		const SplatSpan payload = { node.tile.leaf_gaussians.ptr(), node.tile.leaf_sh_high_order.ptr(),
			node.input_count, context.info.sh_high_order_count };
		finish_metrics(root, payload, nullptr, 0u);
	}
	// Stitching consumes each frontier record's completed root metrics and
	// payload counts; local child indices remain available for BFS expansion.
	node.record = node.tile.tree.nodes[0];
#ifdef TESTS_ENABLED
	if (context.params.test_hooks && context.params.test_hooks->tile_completed) {
		context.params.test_hooks->tile_completed(context.params.test_hooks->userdata);
	}
#endif
	return context.check();
}

void worker_entry(void *p_userdata) {
	Worker &worker = *static_cast<Worker *>(p_userdata);
	worker.result = bake_tile(worker);
}

} // namespace

Error bake_tiles(Context &p_context) {
	uint32_t work_count = 0;
	for (const PlanNode &node : p_context.plan) {
		if (node.work) {
			++work_count;
		}
	}
	if (work_count == 0u || p_context.params.worker_count == 0u) {
		return p_context.fail(ERR_INVALID_DATA);
	}
	uint32_t capacity = MIN(p_context.params.worker_count, work_count);
#ifndef THREADS_ENABLED
	capacity = 1u;
#endif
	const uint64_t worker_bytes = 2u * uint64_t(MAX(2u, capacity)) * (sizeof(Worker) + sizeof(Thread));
	if (!p_context.reserve(worker_bytes)) {
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	LocalVector<Worker> workers;
	if (!fallible_resize(workers, capacity, "gs_hlod::tiled workers")) {
		p_context.release(worker_bytes);
		return p_context.fail(ERR_OUT_OF_MEMORY);
	}
	uint32_t next = 0;
	while (next < p_context.plan.size() && p_context.check() == OK) {
		uint32_t count = 0;
		while (next < p_context.plan.size() && count < capacity) {
			if (!p_context.plan[next].work) {
				++next;
				continue;
			}
			const uint32_t input_count = p_context.plan[next].input_count;
			OfflineJob job = p_context.job(input_count);
			if (!job.reserve(input_count, p_context.info.sh_high_order_count, 0u)) {
				p_context.fail(ERR_OUT_OF_MEMORY);
				break;
			}
			// The job formula includes gathered input and bake owners. Charge a
			// decoded source chunk, global-index map and native stack separately.
			const uint64_t extra = p_context.decode_bytes + 3u * uint64_t(input_count) * sizeof(uint32_t) + (uint64_t(8) << 20u);
			if (extra > UINT64_MAX - job.reserved) {
				p_context.fail(ERR_OUT_OF_MEMORY);
				break;
			}
			const uint64_t reserved = job.reserved + extra;
			if (p_context.live_reserved > p_context.params.limits.max_ram_bytes ||
					reserved > p_context.params.limits.max_ram_bytes - p_context.live_reserved) {
				if (count == 0u) {
					p_context.fail(ERR_OUT_OF_MEMORY);
				}
				break; // A smaller admitted batch may still make progress.
			}
			if (!p_context.reserve(reserved)) {
				p_context.fail(ERR_OUT_OF_MEMORY);
				break;
			}
			Worker &worker = workers[count++];
			worker.context = &p_context;
			worker.node = next++;
			worker.job = job;
			worker.reserved = reserved;
			worker.result = OK;
		}
		// Every reservation is fixed before any thread starts; only the
		// coordinator updates the ledger, after draining the entire batch.
		for (uint32_t i = 0; i < count && p_context.check() == OK; ++i) {
#ifdef THREADS_ENABLED
			if (count > 1u) {
				if (!bake_allocation_probe(sizeof(Thread), "gs_hlod::tiled thread")) {
					p_context.fail(ERR_OUT_OF_MEMORY);
					break;
				}
				workers[i].thread = memnew(Thread);
				if (workers[i].thread->start(worker_entry, &workers[i]) == Thread::UNASSIGNED_ID) {
					memdelete(workers[i].thread);
					workers[i].thread = nullptr;
					p_context.fail(ERR_OUT_OF_MEMORY);
					break;
				}
			} else
#endif
			{
				worker_entry(&workers[i]);
			}
		}
		for (uint32_t i = 0; i < count; ++i) {
			Worker &worker = workers[i];
			if (worker.thread) {
				worker.thread->wait_to_finish();
				memdelete(worker.thread);
				worker.thread = nullptr;
			}
			if (worker.result == OK && !p_context.plan[worker.node].tile.tree.nodes.is_empty()) {
				++p_context.report.tiles_baked;
				p_context.report.largest_tile_splats = MAX(p_context.report.largest_tile_splats, p_context.plan[worker.node].input_count);
				p_context.report.stitch_levels = MAX(p_context.report.stitch_levels, p_context.plan[worker.node].depth);
			}
			p_context.release(worker.reserved);
			worker.reserved = 0;
		}
	}
	p_context.release(worker_bytes);
	return p_context.check();
}

} // namespace tiled_bake
} // namespace gs_hlod
