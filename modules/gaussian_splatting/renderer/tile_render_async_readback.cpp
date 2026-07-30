#include "tile_renderer.h"

#include "../core/gs_vector_alloc.h" // #798: gs_resize_or_fail() for resize-then-ptrw() outputs
#include "pipeline_io_contracts.h"

#include <cstring>

void TileRenderer::TileAsyncReadback::on_overflow_flag_readback(const Vector<uint8_t> &p_data, uint64_t p_request_frame_serial) {
    if (!overflow_state.pending_readback || overflow_state.requested_frame_serial != p_request_frame_serial) {
        return;
    }

    // Callback for async readback from indirect buffer (element_count onwards).
    // Layout matches IndirectDispatchLayout (element_count, overflow_flag, unclamped_total).
    overflow_state.pending_readback = false;
    overflow_state.requested_frame_serial = 0;
    if (p_data.size() < int(GaussianSplatting::kIndirectDispatchElementCountReadbackSize)) {
        WARN_PRINT_ONCE("[TileRenderer] Async overlap readback returned insufficient data; retaining previous overlap estimate");
        return;
    }
    const uint32_t *data = reinterpret_cast<const uint32_t *>(p_data.ptr());
    overflow_state.overflow_detected = (data[1] != 0);
    overflow_state.last_unclamped_total = data[2];
    overflow_state.first_frame_complete = true;
}

void TileRenderer::TileAsyncReadback::on_tile_counts_readback(const Vector<uint8_t> &p_data, uint64_t p_request_frame_serial) {
    if (!tile_counts_state.pending_readback || tile_counts_state.requested_frame_serial != p_request_frame_serial) {
        return;
    }

    tile_counts_state.pending_readback = false;
    tile_counts_state.requested_frame_serial = 0;
    const uint32_t expected_tiles = tile_counts_state.cached_total_tiles;
    const size_t expected_bytes = size_t(expected_tiles) * sizeof(uint32_t);
    if (p_data.size() < int(expected_bytes) || expected_tiles == 0) {
        return;
    }
    // Copy the tile counts data into our cached vector.
    // #798: cached_counts is a PERSISTENT cache that grows when the tile grid grows (a
    // viewport resize). A failed grow leaves it non-empty at its old, too-short size with a
    // still-valid ptrw(), and the memcpy length is expected_bytes (derived from the REQUESTED
    // cached_total_tiles), so it would silently overflow a live heap block. Fail closed with
    // this callback's own contract: return without setting first_frame_complete, exactly like
    // the insufficient-data branch above -- the sibling overflow callback documents this as
    // "retaining previous overlap estimate". pending_readback was already cleared above, so
    // the next frame issues a fresh readback. gs_resize_or_fail() clear()s on failure, which
    // is what keeps the consumer safe: _collect_render_statistics() gates
    // _build_render_stats_from_cached_counts() on cached_counts.is_empty(), and that consumer
    // reads counts[i] up to cached_total_tiles -- so a SHORT cached_counts would be an
    // out-of-bounds READ there. Clearing it makes that gate fire instead.
    if (!gs_resize_or_fail(tile_counts_state.cached_counts, (int64_t)expected_tiles,
                "TileRenderer::TileAsyncReadback::on_tile_counts_readback")) {
        return;
    }
    memcpy(tile_counts_state.cached_counts.ptrw(), p_data.ptr(), expected_bytes);
    tile_counts_state.first_frame_complete = true;
}
