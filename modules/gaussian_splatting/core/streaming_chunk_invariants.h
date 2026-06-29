#ifndef STREAMING_CHUNK_INVARIANTS_H
#define STREAMING_CHUNK_INVARIANTS_H

#include "gaussian_streaming.h"

#include "core/string/ustring.h"

#include <cstdint>

// Chunk slot-ownership and upload-lifecycle invariant validators, extracted
// verbatim from gaussian_streaming.cpp's anonymous namespace (decomposition
// slice 2). These are free helpers over GaussianAtlasAllocator + the streaming
// chunk/diagnostics state; they call only each other and their parameters, so
// they live cleanly in their own translation unit. The upload-retirement and
// pending-byte helpers were intentionally NOT moved (they are function
// templates and/or depend on non-moved anon helpers).
namespace gs_chunk_invariants {

void _release_chunk_slot_if_matches(GaussianAtlasAllocator &p_allocator, uint64_t p_chunk_key, uint32_t p_expected_slot);
bool _chunk_slot_matches_allocator(const GaussianAtlasAllocator &p_allocator, uint64_t p_chunk_key, uint32_t p_expected_slot, uint32_t *r_mapped_slot = nullptr);
bool _record_streaming_invariant(bool p_invalid_state,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics,
        uint64_t &r_counter,
        const char *p_context,
        const String &p_message);
bool _validate_pending_upload_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics);
bool _validate_loaded_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics);
void _validate_idle_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        bool p_allow_deferred_allocator_release,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics);

} // namespace gs_chunk_invariants

#endif // STREAMING_CHUNK_INVARIANTS_H
