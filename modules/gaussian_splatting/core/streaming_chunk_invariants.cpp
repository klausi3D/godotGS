#include "streaming_chunk_invariants.h"

#include "core/error/error_macros.h"
#include "core/string/ustring.h"

namespace gs_chunk_invariants {

void _release_chunk_slot_if_matches(GaussianAtlasAllocator &p_allocator, uint64_t p_chunk_key, uint32_t p_expected_slot) {
    uint32_t mapped_slot = UINT32_MAX;
    if (!p_allocator.get_slot(p_chunk_key, mapped_slot)) {
        return;
    }
    if (p_expected_slot != UINT32_MAX && mapped_slot != p_expected_slot) {
        return;
    }
    p_allocator.release_slot(p_chunk_key);
}

bool _chunk_slot_matches_allocator(const GaussianAtlasAllocator &p_allocator, uint64_t p_chunk_key, uint32_t p_expected_slot, uint32_t *r_mapped_slot) {
    uint32_t mapped_slot = UINT32_MAX;
    const bool has_slot = p_allocator.get_slot(p_chunk_key, mapped_slot);
    if (r_mapped_slot) {
        *r_mapped_slot = mapped_slot;
    }
    return has_slot && mapped_slot == p_expected_slot;
}

bool _record_streaming_invariant(bool p_invalid_state,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics,
        uint64_t &r_counter,
        const char *p_context,
        const String &p_message) {
    if (!p_invalid_state) {
        return false;
    }
    r_counter++;
    r_diagnostics.last_invariant_context = p_context;
    r_diagnostics.last_invariant_message = p_message;
    WARN_PRINT(p_message);
    return true;
}

bool _validate_pending_upload_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics) {
    if (_record_streaming_invariant(p_chunk.is_loaded, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d cannot be both loaded and upload_pending.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }
    if (_record_streaming_invariant(p_chunk.gpu_resident, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d pending upload cannot be GPU-resident.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }
    if (_record_streaming_invariant(p_chunk.buffer_slot == UINT32_MAX, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d upload_pending requires a valid buffer_slot.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }

    uint32_t mapped_slot = UINT32_MAX;
    const bool slot_match = _chunk_slot_matches_allocator(p_allocator, p_chunk_key, p_chunk.buffer_slot, &mapped_slot);
    return _record_streaming_invariant(!slot_match, r_diagnostics,
            r_diagnostics.invariant_slot_ownership_violations,
            p_context,
            vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d upload_pending slot=%d not tracked by allocator (mapped=%d).",
                    p_context, p_asset_id, p_chunk_idx, p_chunk.buffer_slot,
                    mapped_slot == UINT32_MAX ? -1 : int(mapped_slot)));
}

bool _validate_loaded_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics) {
    if (_record_streaming_invariant(!p_chunk.gpu_resident, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d loaded chunk is not GPU-resident.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }
    if (_record_streaming_invariant(p_chunk.buffer_slot == UINT32_MAX, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d loaded chunk is missing buffer_slot.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }

    uint32_t mapped_slot = UINT32_MAX;
    const bool slot_match = _chunk_slot_matches_allocator(p_allocator, p_chunk_key, p_chunk.buffer_slot, &mapped_slot);
    return _record_streaming_invariant(!slot_match, r_diagnostics,
            r_diagnostics.invariant_slot_ownership_violations,
            p_context,
            vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d loaded slot=%d not tracked by allocator (mapped=%d).",
                    p_context, p_asset_id, p_chunk_idx, p_chunk.buffer_slot,
                    mapped_slot == UINT32_MAX ? -1 : int(mapped_slot)));
}

void _validate_idle_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        bool p_allow_deferred_allocator_release,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics) {
    uint32_t mapped_slot = UINT32_MAX;
    const bool slot_tracked = p_allocator.get_slot(p_chunk_key, mapped_slot);
    if (_record_streaming_invariant(p_chunk.buffer_slot != UINT32_MAX, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d has buffer_slot=%d while not loaded/pending.",
                        p_context, p_asset_id, p_chunk_idx, p_chunk.buffer_slot))) {
        return;
    }
    if (p_allow_deferred_allocator_release) {
        return;
    }
    _record_streaming_invariant(slot_tracked, r_diagnostics,
            r_diagnostics.invariant_slot_ownership_violations,
            p_context,
            vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d allocator tracks slot=%d while chunk is not loaded/pending.",
                    p_context, p_asset_id, p_chunk_idx, mapped_slot));
}

} // namespace gs_chunk_invariants
