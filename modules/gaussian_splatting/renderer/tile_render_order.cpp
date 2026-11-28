/**
 * tile_render_order.cpp — #1544: total order of the overlap (tile) sort.
 *
 * The overlap sort key is (tile, depth, tie) with a tie of at most 16 low bits of global_idx, and EMIT
 * takes each record's slot from atomicAdd, so records of one tile whose full keys are equal leave the
 * sort in emit order, which varies from frame to frame. Pass 4 of tile_prefix_scan.glsl is recorded
 * into the overlap sort's own compute list (IGPUSorter post-sort recorder) and orders every run of
 * equal keys by global_idx. See tile_order_contract.h.
 */

#include "tile_renderer.h"

#include "core/object/callable_method_pointer.h"
#include "servers/rendering/rendering_device.h"

#include "gpu_pass_timing.h"
#include "tile_order_contract.h"
#include "../logger/gs_logger.h"
#include "../interfaces/sync_policy.h"

#include <cstring>

bool TileRenderer::_record_tile_order_pass(RenderingDevice *p_device, RD::ComputeListID p_compute_list) {
	debug_stats.tile_order_pass_recorded = false;
	if (!p_device || p_compute_list == RD::INVALID_ID) {
		return false;
	}
	const RID pipeline = shader_resources.tile_prefix_pipeline_pass4;
	const RID shader = shader_resources.tile_prefix_shader_pass4;
	if (!pipeline.is_valid() || !shader.is_valid() || !debug_stats.tile_order_stats_buffer.is_valid() ||
			!global_sort_resources.keys_buffer.is_valid() || !global_sort_resources.values_buffer.is_valid() ||
			!global_sort_resources.indirect_dispatch_buffer.is_valid()) {
		return false;
	}
	// The previous frame's set has been submitted; release it before building this frame's.
	debug_stats.release_tile_order_uniform_set();

	Vector<RD::Uniform> uniforms;
	auto bind = [&](int p_binding, RID p_buffer) {
		RD::Uniform uniform;
		uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		uniform.binding = p_binding;
		uniform.append_id(p_buffer);
		uniforms.push_back(uniform);
	};
	bind(0, global_sort_resources.keys_buffer);
	bind(1, global_sort_resources.values_buffer);
	{
		// IndirectDispatchLayout; pinned to OrderCount (binding 2) by check_gaussian_layout_sync.py.
		RD::Uniform count_uniform;
		count_uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		count_uniform.binding = 2;
		count_uniform.append_id(global_sort_resources.indirect_dispatch_buffer);
		uniforms.push_back(count_uniform);
	}
	bind(3, debug_stats.tile_order_stats_buffer);
	const RID uniform_set = p_device->uniform_set_create(uniforms, shader, 0);
	if (!uniform_set.is_valid()) {
		ERR_PRINT_ONCE("[TileRenderer] #1544: failed to create the total-order pass uniform set");
		return false;
	}
	debug_stats.tile_order_uniform_set = uniform_set;
	debug_stats.tile_order_uniform_set_device = p_device;

	GaussianSplatting::TileOrderControlGPU control;
	// The live sorter's layout is the layout of the keys in the buffer.
	control.key_words = global_sort_resources.key_config.key_bits > 32u ? 2u : 1u;
	control.tie_bits = _get_sort_tie_bits();
	control.run_cap = GaussianSplatting::kTileOrderRunCap;
	control.test_mode = GaussianSplatting::TILE_ORDER_TEST_MODE_PRODUCTION;
#ifdef TESTS_ENABLED
	control.test_mode = GSTileOrderTestControl::order_test_mode.load();
	const uint32_t run_cap_override = GSTileOrderTestControl::run_cap_override.load();
	if (run_cap_override != 0u) {
		control.run_cap = run_cap_override;
	}
#endif

	p_device->compute_list_bind_compute_pipeline(p_compute_list, pipeline);
	p_device->compute_list_bind_uniform_set(p_compute_list, uniform_set, 0);
	p_device->compute_list_set_push_constant(p_compute_list, &control, sizeof(control));
	// A direct dispatch: the indirect buffer is bound as storage in this compute list (the sort's own
	// sets bind it too), and a buffer cannot be both in one list. The shader loops (grid stride) up
	// to element_count, so the group count only has to be bounded, not exact.
	const uint32_t capacity = MAX<uint32_t>(global_sort_resources.capacity, 1u);
	const uint32_t groups = MIN<uint32_t>((capacity + GaussianSplatting::kTilePrefixPassLocalSize - 1u) /
					GaussianSplatting::kTilePrefixPassLocalSize,
			65535u);
	p_device->compute_list_dispatch(p_compute_list, groups, 1, 1);
	debug_stats.tile_order_pass_recorded = true;
	return true;
}

void TileRenderer::TileRendererDebugStats::release_tile_order_uniform_set() {
	if (tile_order_uniform_set.is_valid() && tile_order_uniform_set_device &&
			tile_order_uniform_set_device->uniform_set_is_valid(tile_order_uniform_set)) {
		tile_order_uniform_set_device->free(tile_order_uniform_set);
	}
	tile_order_uniform_set = RID();
	tile_order_uniform_set_device = nullptr;
}

void TileRenderer::TileRendererDebugStats::poll_tile_order_stats(RenderingDevice *p_device, uint64_t p_frame_serial) {
	// A 32-byte async copy, issued only while no prior read is in flight; never a sync stall.
	if (!p_device || !tile_order_stats_buffer.is_valid()) {
		return;
	}
	if (p_frame_serial == 0 || tile_order_stats_readback.pending) {
		return;
	}
	tile_order_stats_readback.requested_frame_serial = p_frame_serial;
	Callable callback = callable_mp(&owner, &TileRenderer::_on_tile_order_stats_readback);
	if (p_device->buffer_get_data_async(tile_order_stats_buffer, callback, 0,
				sizeof(GaussianSplatting::TileOrderStatsGPU)) == OK) {
		tile_order_stats_readback.pending = true;
	}
}

void TileRenderer::TileRendererDebugStats::on_tile_order_stats_readback(const Vector<uint8_t> &p_data) {
	tile_order_stats_readback.pending = false;
	if ((size_t)p_data.size() < sizeof(GaussianSplatting::TileOrderStatsGPU)) {
		return;
	}
	GaussianSplatting::TileOrderStatsGPU sample;
	std::memcpy(&sample, p_data.ptr(), sizeof(sample));
	cached_tile_order_stats = sample;
	cached_tile_order_frame_serial = tile_order_stats_readback.requested_frame_serial;
	if (sample.key_overflow > 0u && !tile_order_overflow_warned) {
		tile_order_overflow_warned = true;
		GS_LOG_WARN_DEFAULT(vformat("[TileRenderer] #1544 tile_order_key_overflow: %d run(s) of more than %d overlap records with equal sort keys were left in sort order; their raster order is not deterministic",
				int(sample.key_overflow), int(GaussianSplatting::kTileOrderRunCap)));
	}
}

void TileRenderer::_on_tile_order_stats_readback(const Vector<uint8_t> &p_data) {
	debug_stats.on_tile_order_stats_readback(p_data);
}

#ifdef TESTS_ENABLED
bool TileRenderer::test_read_tile_order_stats(GaussianSplatting::TileOrderStatsGPU &r_stats) const {
	RenderingDevice *device = debug_stats.tile_order_stats_owner.device;
	if (!device || !debug_stats.tile_order_stats_buffer.is_valid()) {
		return false;
	}
	const Vector<uint8_t> bytes = gs_device_utils::safe_buffer_get_data(device, debug_stats.tile_order_stats_buffer, 0,
			sizeof(GaussianSplatting::TileOrderStatsGPU));
	if ((size_t)bytes.size() < sizeof(GaussianSplatting::TileOrderStatsGPU)) {
		return false;
	}
	std::memcpy(&r_stats, bytes.ptr(), sizeof(r_stats));
	return true;
}
#endif
