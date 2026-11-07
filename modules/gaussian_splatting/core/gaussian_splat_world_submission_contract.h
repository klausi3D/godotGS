#ifndef GAUSSIAN_SPLAT_WORLD_SUBMISSION_CONTRACT_H
#define GAUSSIAN_SPLAT_WORLD_SUBMISSION_CONTRACT_H

// What the scene director hands the renderer for a world submission. GaussianSplatRenderer exposes these as
// WorldSubmissionContract and WorldSubmissionRuntimeStateSnapshot.

#include "core/math/aabb.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include "gaussian_data.h"
#include "gaussian_splat_hlod_runtime_data.h"
#include "gaussian_streaming.h"
#include "streaming_chunk_payload_source.h"
#include "../interfaces/gpu_culler.h"

struct GaussianSplatWorldSubmissionContract {
    Ref<GaussianSplatHlodRuntimeData> hlod_snapshot;
    String hlod_reason;
    float hlod_tau_px = 0.0f;
    uint64_t hlod_owner_key = 0;
    uint64_t hlod_world_key = 0; // The submitting world's identity, also when the cut is disabled and no snapshot exists.
    Ref<GaussianData> gaussian_data;
    Ref<ChunkPayloadSource> payload_source;
    Vector<StaticChunk> static_chunks;
    String debug_label;
    bool has_desired_residency_hint = false;
    int32_t desired_residency_hint = 0;
    bool lod_enabled = true;
    float lod_bias = 1.0f;
    float lod_max_distance = 0.0f;
    bool frustum_culling = true;
    bool async_upload_enabled = true;
    float opacity_multiplier = 1.0f;
    int max_splats = 1000000;
    // The view's splat cap BEFORE max_splats is clamped to the world's data count (0: not known). max_splats bounds what
    // the world can draw; this is the pool the HLOD route's node instances reserve from (PR1b, ADR 4.5).
    int view_max_splats = 0;
    GaussianStreamingSystem::ConfigOverrides streaming_overrides;
};

struct GaussianSplatWorldSubmissionRuntimeStateSnapshot {
    Ref<GaussianSplatHlodRuntimeData> hlod_snapshot;
    String hlod_reason;
    float hlod_tau_px = 0.0f;
    uint64_t hlod_owner_key = 0;
    uint64_t hlod_world_key = 0;
    bool valid = false;
    Ref<GaussianData> gaussian_data;
    Ref<ChunkPayloadSource> payload_source;
    uint32_t payload_source_splat_count = 0;
    uint32_t payload_source_sh_degree = 0;
    AABB payload_source_bounds;
    Vector<StaticChunk> static_chunks;
    bool lod_enabled = true;
    float lod_bias = 1.0f;
    float lod_max_distance = 0.0f;
    bool frustum_culling = true;
    bool async_upload_enabled = true;
    float opacity_multiplier = 1.0f;
    int max_splats = 1000000;
    int view_max_splats = 0; // See WorldSubmissionContract::view_max_splats.
    GaussianStreamingSystem::ConfigOverrides streaming_overrides;
    bool has_active_world_submission = false;
    bool has_desired_residency_hint = false;
    int32_t desired_residency_hint = 0;
};

#endif // GAUSSIAN_SPLAT_WORLD_SUBMISSION_CONTRACT_H
