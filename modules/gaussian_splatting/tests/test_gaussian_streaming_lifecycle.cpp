#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_streaming.h"

#include "test_macros.h"

#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "servers/rendering_server.h"

extern "C" int test_gaussian_streaming_lifecycle_cpp_force_link() {
    return 0;
}

namespace {

Ref<GaussianData> _create_streaming_phase_order_test_data(uint32_t p_count = 1024) {
    Ref<GaussianData> data;
    data.instantiate();

    LocalVector<Gaussian> gaussians;
    gaussians.resize(p_count);

    const uint32_t grid_width = 32;
    for (uint32_t i = 0; i < p_count; i++) {
        Gaussian &g = gaussians[i];
        const float x = float(i % grid_width) * 0.05f;
        const float y = float((i / grid_width) % grid_width) * 0.05f;
        const float z = -2.0f - float(i / (grid_width * grid_width)) * 0.05f;
        g.position = Vector3(x, y, z);
        g.scale = Vector3(0.05f, 0.05f, 0.05f);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 0.85f, 0.7f, 1.0f);
        g.normal = Vector3(0.0f, 1.0f, 0.0f);
        g.area = 0.01f;
    }

    data->set_gaussians(gaussians);
    return data;
}

struct TestRenderingDeviceHandle {
    RenderingDevice *rd = nullptr;
    bool owns_rd = false;

    ~TestRenderingDeviceHandle() {
        if (owns_rd && rd) {
            memdelete(rd);
        }
    }
};

TestRenderingDeviceHandle _get_test_rendering_device() {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (!rs) {
        return {};
    }

    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) {
        rd = rs->create_local_rendering_device();
        return { rd, rd != nullptr };
    }
    return { rd, false };
}

StreamingUploadPipeline::PendingChunkUpload *_wait_for_prepared_upload(StreamingUploadPipeline &p_uploads) {
    StreamingUploadPipeline::PendingChunkUpload *prepared_job = nullptr;
    for (int i = 0; i < 500; i++) {
        {
            MutexLock lock(p_uploads.pack_mutex);
            if (p_uploads.upload_queue_read_idx < p_uploads.upload_queue.size()) {
                prepared_job = p_uploads.upload_queue[p_uploads.upload_queue_read_idx];
                if (prepared_job && !prepared_job->packed_data.is_empty()) {
                    break;
                }
                prepared_job = nullptr;
            }
        }
        OS::get_singleton()->delay_usec(1000);
    }
    return prepared_job;
}

void _tamper_first_payload_byte(StreamingUploadPipeline &p_uploads) {
    MutexLock lock(p_uploads.pack_mutex);
    StreamingUploadPipeline::PendingChunkUpload *prepared_job =
            p_uploads.upload_queue[p_uploads.upload_queue_read_idx];
    REQUIRE(prepared_job != nullptr);
    REQUIRE(!prepared_job->packed_data.is_empty());
    PackedGaussian *packed_data = prepared_job->packed_data.ptrw();
    REQUIRE(packed_data != nullptr);
    uint8_t *payload_bytes = reinterpret_cast<uint8_t *>(packed_data);
    payload_bytes[0] ^= 0x01;
}

void _advance_frames_until_upload_retired(Ref<GaussianStreamingSystem> p_system, uint32_t p_max_frames = 8) {
    for (uint32_t i = 0; i < p_max_frames; i++) {
        p_system->begin_frame();
        p_system->end_frame();
        if (p_system->get_pending_upload_retirement_slots() == 0) {
            return;
        }
    }
}

} // namespace

TEST_CASE("[Streaming Pipeline] stop_pack_threads clears partial lifecycle state") {
    GaussianStreamingSystem system;
    auto &uploads = system._internal_get_upload_pipeline();

    uploads.pack_thread_running.store(false, std::memory_order_release);
    uploads.pack_thread_exit.store(true, std::memory_order_release);
    uploads.pack_threads.resize(2);
    uploads.pack_thread_contexts.resize(2);
    uploads.pack_threads[0] = nullptr;
    uploads.pack_threads[1] = nullptr;

    uploads.stop_pack_threads(system);

    CHECK(uploads.pack_threads.is_empty());
    CHECK(uploads.pack_thread_contexts.is_empty());
    CHECK_FALSE(uploads.pack_thread_running.load(std::memory_order_acquire));
    CHECK_FALSE(uploads.pack_thread_exit.load(std::memory_order_acquire));
}

TEST_CASE("[Streaming Pipeline] sync pack rescue does not steal worker-owned pack jobs") {
    GaussianStreamingSystem system;
    auto &uploads = system._internal_get_upload_pipeline();

    uploads._test_set_async_pack_queue_owner(true);
    uploads._test_enqueue_dummy_pack_job();

    CHECK(uploads._test_promote_pack_jobs_sync(1) == 0);
    CHECK(uploads.get_pack_queue_depth_cached() == 1);
    CHECK(uploads.get_upload_queue_depth_cached() == 0);
}

TEST_CASE("[Streaming Pipeline] upload retirement gates chunk residency until frame barrier") {
    GaussianStreamingSystem system;
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    chunks.resize(1);
    GaussianStreamingTypes::StreamingChunk &chunk = chunks[0];
    chunk.start_idx = 0;
    chunk.count = 128;
    chunk.is_visible = true;
    chunk.effective_count = chunk.count;
    system._test_register_primary_asset_for_chunks();
    system._test_reset_atlas_allocator(1);

    const uint64_t chunk_key = system._test_make_chunk_key(0, 0);
    uint32_t buffer_slot = UINT32_MAX;
    const uint64_t upload_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
    REQUIRE(system._test_atlas_allocator().allocate_slot(chunk_key, buffer_slot));
    REQUIRE(system._test_begin_chunk_upload(0, 0, chunk, buffer_slot));
    REQUIRE(system._test_stage_chunk_upload_retirement(0, 0, chunk, buffer_slot,
            upload_bytes,
            2,
            GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER));

    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK_FALSE(chunk.gpu_resident);
    CHECK(system.get_loaded_chunks() == 0);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == upload_bytes);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);

    system._test_process_upload_retirements();
    CHECK_FALSE(chunk.is_loaded);
    system.begin_frame();
    CHECK_FALSE(chunk.is_loaded);
    CHECK(chunk.upload_pending);
    system.begin_frame();

    CHECK(chunk.is_loaded);
    CHECK(chunk.gpu_resident);
    CHECK_FALSE(chunk.upload_pending);
    CHECK(system.get_loaded_chunks() == 1);
    CHECK(system.get_chunks_loaded_this_frame() == 1);
    CHECK(system._test_get_retired_upload_slots_this_frame() == 1);
    CHECK(system._test_get_retired_upload_bytes_this_frame() == upload_bytes);
    CHECK(system.get_pending_upload_retirement_slots() == 0);
    CHECK(system.get_pending_upload_retirement_bytes() == 0);
}

TEST_CASE("[Streaming Pipeline] cancel_chunk_jobs preserves pending retirement slot until frame barrier") {
    GaussianStreamingSystem system;
    auto &uploads = system._internal_get_upload_pipeline();
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    chunks.resize(1);
    GaussianStreamingTypes::StreamingChunk &chunk = chunks[0];
    chunk.start_idx = 0;
    chunk.count = 128;
    chunk.is_visible = true;
    chunk.effective_count = chunk.count;
    system._test_register_primary_asset_for_chunks();
    system._test_reset_atlas_allocator(1);

    const uint64_t chunk_key = system._test_make_chunk_key(0, 0);
    uint32_t buffer_slot = UINT32_MAX;
    const uint64_t upload_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
    REQUIRE(system._test_atlas_allocator().allocate_slot(chunk_key, buffer_slot));
    REQUIRE(system._test_begin_chunk_upload(0, 0, chunk, buffer_slot));
    REQUIRE(system._test_stage_chunk_upload_retirement(0, 0, chunk, buffer_slot,
            upload_bytes,
            2,
            GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER));

    uploads.cancel_chunk_jobs(system, 0, 0, buffer_slot);

    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(chunk.buffer_slot == buffer_slot);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == upload_bytes);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);

    system._test_process_upload_retirements();
    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);

    system.begin_frame();
    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);
    system.begin_frame();

    CHECK(chunk.is_loaded);
    CHECK(chunk.gpu_resident);
    CHECK_FALSE(chunk.upload_pending);
    CHECK(system.get_chunks_loaded_this_frame() == 1);
    CHECK(system._test_get_retired_upload_slots_this_frame() == 1);
    CHECK(system._test_get_retired_upload_bytes_this_frame() == upload_bytes);
    CHECK(system.get_pending_upload_retirement_slots() == 0);
    CHECK(system.get_pending_upload_retirement_bytes() == 0);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);
}

TEST_CASE("[Streaming Pipeline] sync fallback drain counts immediate retirement once") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 3531;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());
    REQUIRE(system->request_chunk_residency(asset_id, 0, 0) == OK);
    REQUIRE(system->_test_enqueue_sync_fallback_chunk_load(asset_id, 0, true));

    uint32_t evictions_left = 0;
    bool eviction_blocked = false;
    const uint32_t drained = system->_test_drain_sync_fallback_chunk_loads(1, evictions_left, eviction_blocked);

    CHECK(drained == 1);
    CHECK(system->get_loaded_chunks() == 1);
    CHECK(system->get_chunks_loaded_this_frame() == 1);
    CHECK(system->_test_get_retired_upload_slots_this_frame() == 1);
}

TEST_CASE("[Streaming Pipeline] rollback of stale slot assignment preserves other pending reservations") {
    GaussianStreamingSystem system;
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    chunks.resize(2);
    for (uint32_t i = 0; i < 2; i++) {
        chunks[i].start_idx = i * 128;
        chunks[i].count = 128;
        chunks[i].is_visible = true;
        chunks[i].effective_count = chunks[i].count;
    }
    system._test_register_primary_asset_for_chunks();
    system._test_reset_atlas_allocator(2);

    uint32_t pending_slot = UINT32_MAX;
    const uint64_t pending_key = system._test_make_chunk_key(0, 0);
    REQUIRE(system._test_atlas_allocator().allocate_slot(pending_key, pending_slot));
    REQUIRE(system._test_begin_chunk_upload(0, 0, chunks[0], pending_slot));
    const uint64_t pending_bytes = chunks[0].pending_upload_bytes;
    REQUIRE(pending_bytes > 0);

    uint32_t stale_slot = UINT32_MAX;
    const uint64_t stale_key = system._test_make_chunk_key(0, 1);
    REQUIRE(system._test_atlas_allocator().allocate_slot(stale_key, stale_slot));
    chunks[1].buffer_slot = stale_slot;
    chunks[1].upload_pending = false;
    chunks[1].pending_upload_bytes = 0;

    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == pending_bytes);

    system._test_rollback_pending_chunk(0, 1, chunks[1], true);

    CHECK_FALSE(chunks[1].upload_pending);
    CHECK(chunks[1].buffer_slot == UINT32_MAX);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == pending_bytes);
    CHECK(chunks[0].upload_pending);
}

TEST_CASE("[Streaming Pipeline] generation-stale retirement tickets keep upload slots reserved") {
    GaussianStreamingSystem system;
    const uint32_t asset_id = 353;
    system.register_asset(asset_id, _create_streaming_phase_order_test_data());
    system._test_reset_atlas_allocator(1);

    GaussianStreamingTypes::AtlasAssetState *asset = system._test_get_asset_state(asset_id);
    REQUIRE(asset != nullptr);
    LocalVector<GaussianStreamingTypes::StreamingChunk> &asset_chunks = system._test_get_asset_chunks(*asset);
    REQUIRE(!asset_chunks.is_empty());
    GaussianStreamingTypes::StreamingChunk &chunk = asset_chunks[0];

    const uint64_t upload_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
    uint32_t buffer_slot = UINT32_MAX;
    REQUIRE(system._test_atlas_allocator().allocate_slot(system._test_make_chunk_key(asset_id, 0), buffer_slot));
    REQUIRE(system._test_begin_chunk_upload(asset_id, 0, chunk, buffer_slot));
    REQUIRE(system._test_stage_chunk_upload_retirement(asset_id, 0, chunk, buffer_slot,
            upload_bytes, 2,
            GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER));

    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == upload_bytes);
    CHECK(system._test_get_reserved_chunk_count() == 1);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);

    system.register_asset(asset_id, _create_streaming_phase_order_test_data());

    GaussianStreamingTypes::AtlasAssetState *refreshed_asset = system._test_get_asset_state(asset_id);
    REQUIRE(refreshed_asset != nullptr);
    LocalVector<GaussianStreamingTypes::StreamingChunk> &refreshed_chunks = system._test_get_asset_chunks(*refreshed_asset);
    REQUIRE(!refreshed_chunks.is_empty());
    CHECK_FALSE(refreshed_chunks[0].upload_pending);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == upload_bytes);
    CHECK(system._test_get_reserved_chunk_count() == 1);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 0);

    system.begin_frame();
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    system.begin_frame();

    CHECK(system.get_pending_upload_retirement_slots() == 0);
    CHECK(system.get_pending_upload_retirement_bytes() == 0);
    CHECK(system._test_get_reserved_chunk_count() == 0);
    CHECK(system._test_atlas_allocator().get_free_slot_count() == 1);
    CHECK(system._test_get_failed_upload_retirements() == 0);
}

TEST_CASE("[Streaming Pipeline] reserved chunk count combines loaded and pending upload counters") {
    GaussianStreamingSystem system;
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    chunks.resize(2);
    for (uint32_t i = 0; i < chunks.size(); i++) {
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk.start_idx = i * 128;
        chunk.count = 128;
        chunk.effective_count = chunk.count;
    }
    system._test_register_primary_asset_for_chunks();
    system._test_reset_atlas_allocator(2);
    system._test_mark_chunk_loaded_for_eviction(0, 0, false, 1, 1, 1.0f);

    uint32_t buffer_slot = UINT32_MAX;
    REQUIRE(system._test_atlas_allocator().allocate_slot(system._test_make_chunk_key(0, 1), buffer_slot));
    REQUIRE(system._test_begin_chunk_upload(0, 1, chunks[1], buffer_slot));

    CHECK(system.get_loaded_chunks() == 1);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system._test_get_reserved_chunk_count() == 2);
}

TEST_CASE("[Streaming Pipeline] upload payload checksum validation is off by default") {
    StreamingUploadPipeline uploads;

    CHECK_FALSE(uploads._test_is_upload_payload_checksum_validation_enabled());
}

TEST_CASE("[Streaming Pipeline] production upload path skips payload checksum hashing") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        MESSAGE("Skipping - Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4241;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    StreamingUploadPipeline::_test_reset_payload_checksum_hash_calls();
    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    REQUIRE(queued_upload);

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    REQUIRE(prepared_job != nullptr);
    _tamper_first_payload_byte(uploads);

    uploads.process_upload_queue(system_ref);
    _advance_frames_until_upload_retired(system);

    CHECK(StreamingUploadPipeline::_test_get_payload_checksum_hash_calls() == 0);
    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 1);

    Dictionary analytics = system->get_streaming_analytics();
    Dictionary diagnostics = analytics.get("diagnostics", Dictionary());
    CHECK(String(analytics.get("diagnostics_category", String())) == "ok");
    CHECK(int64_t(diagnostics.get("integrity_mismatch_count", int64_t(-1))) == 0);
}

TEST_CASE("[Streaming Pipeline] async chunk upload rejects tampered payload checksums when validation is enabled") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads._test_set_upload_payload_checksum_validation_enabled(true);
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        MESSAGE("Skipping - Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4242;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    StreamingUploadPipeline::_test_reset_payload_checksum_hash_calls();
    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    REQUIRE(queued_upload);

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    REQUIRE(prepared_job != nullptr);
    _tamper_first_payload_byte(uploads);

    uploads.process_upload_queue(system_ref);
    _advance_frames_until_upload_retired(system);

    CHECK(StreamingUploadPipeline::_test_get_payload_checksum_hash_calls() >= 2);
    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 0);

    Dictionary analytics = system->get_streaming_analytics();
    Dictionary diagnostics = analytics.get("diagnostics", Dictionary());
    CHECK(String(analytics.get("diagnostics_category", String())) == "integrity_mismatch");
    CHECK(String(analytics.get("diagnostics_reason", String())).contains("checksum mismatch"));
    CHECK(bool(analytics.get("diagnostics_has_failure", false)));
    CHECK(String(diagnostics.get("category", String())) == "integrity_mismatch");
    CHECK(String(diagnostics.get("reason", String())).contains("checksum mismatch"));
    CHECK(int64_t(diagnostics.get("invariant_upload_lifecycle_violations", int64_t(0))) == 1);
    CHECK(String(diagnostics.get("last_invariant_context", String())) == "process_upload_queue.payload_checksum");
    CHECK(String(diagnostics.get("last_invariant_message", String())).contains("checksum mismatch"));
    CHECK(int64_t(diagnostics.get("integrity_mismatch_count", int64_t(0))) == 1);
    CHECK(String(diagnostics.get("last_integrity_mismatch_message", String())).contains("checksum mismatch"));

    system->initialize_empty(rd);
    system->begin_frame();
    system->end_frame();

    Dictionary reset_analytics = system->get_streaming_analytics();
    Dictionary reset_diagnostics = reset_analytics.get("diagnostics", Dictionary());
    CHECK(String(reset_analytics.get("diagnostics_category", String("ok"))) == "ok");
    CHECK(String(reset_analytics.get("diagnostics_reason", String("healthy"))) == "healthy");
    CHECK_FALSE(bool(reset_analytics.get("diagnostics_has_failure", true)));
    CHECK(int64_t(reset_diagnostics.get("integrity_mismatch_count", int64_t(-1))) == 0);
    CHECK(String(reset_diagnostics.get("last_integrity_mismatch_message", String())).is_empty());
}

TEST_CASE("[Streaming Pipeline] enabling checksum validation rejects pending jobs without baselines") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        MESSAGE("Skipping - Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4243;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    StreamingUploadPipeline::_test_reset_payload_checksum_hash_calls();
    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    REQUIRE(queued_upload);

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    REQUIRE(prepared_job != nullptr);
    CHECK_FALSE(prepared_job->payload_checksum_valid);

    uploads._test_set_upload_payload_checksum_validation_enabled(true);
    uploads.process_upload_queue(system_ref);
    _advance_frames_until_upload_retired(system);

    CHECK(StreamingUploadPipeline::_test_get_payload_checksum_hash_calls() == 0);
    CHECK(system->get_pending_pack_jobs() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
    CHECK(system->get_loaded_chunks() == 0);

    Dictionary analytics = system->get_streaming_analytics();
    Dictionary diagnostics = analytics.get("diagnostics", Dictionary());
    CHECK(String(analytics.get("diagnostics_category", String())) == "integrity_mismatch");
    CHECK(String(analytics.get("diagnostics_reason", String())).contains("without a checksum baseline"));
    CHECK(int64_t(diagnostics.get("invariant_upload_lifecycle_violations", int64_t(0))) == 1);
    CHECK(int64_t(diagnostics.get("integrity_mismatch_count", int64_t(0))) == 1);
    CHECK(String(diagnostics.get("last_invariant_context", String())) == "process_upload_queue.payload_checksum_missing");
    CHECK(String(diagnostics.get("last_integrity_mismatch_message", String())).contains("without a checksum baseline"));
}

TEST_CASE("[Streaming Pipeline] update_streaming publishes phase timings before atlas sync and keeps atlas generation stable when idle") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    {
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        system->initialize_empty(rd);
        if (!system->is_runtime_ready()) {
            MESSAGE("Skipping - Streaming runtime not ready");
            return;
        }

        const uint32_t asset_id = 31415;
        system->register_asset(asset_id, _create_streaming_phase_order_test_data());

        Transform3D camera_transform;
        camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
        Projection projection;
        projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

        const RID asset_meta_before = system->get_asset_meta_buffer();
        const RID chunk_meta_before = system->get_chunk_meta_buffer();
        const RID asset_chunk_index_before = system->get_asset_chunk_index_buffer();
        const uint64_t generation_before = system->get_atlas_generation();

        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        const uint64_t generation_after_update = system->get_atlas_generation();
        CHECK(generation_after_update > generation_before);
        CHECK(generation_after_update > 0);
        system->end_frame();

        Dictionary analytics = system->get_streaming_analytics();
        CHECK(analytics.has("scheduler_visibility_cpu_ms"));
        CHECK(analytics.has("scheduler_load_cpu_ms"));
        CHECK(analytics.has("scheduler_build_visible_cpu_ms"));
        CHECK(analytics.has("scheduler_prefetch_cpu_ms"));
        CHECK(analytics.has("scheduler_update_cpu_ms"));
        CHECK(analytics.has("scheduler_cpu_total_attributed_ms"));
        CHECK(analytics.has("scheduler_cpu_unattributed_ms"));
        CHECK(analytics.has("atlas_generation"));

        const double visibility_cpu_ms = double(analytics.get("scheduler_visibility_cpu_ms", 0.0));
        const double load_cpu_ms = double(analytics.get("scheduler_load_cpu_ms", 0.0));
        const double build_visible_cpu_ms = double(analytics.get("scheduler_build_visible_cpu_ms", 0.0));
        const double prefetch_cpu_ms = double(analytics.get("scheduler_prefetch_cpu_ms", 0.0));
        const double update_cpu_ms = double(analytics.get("scheduler_update_cpu_ms", 0.0));
        const double attributed_cpu_ms = double(analytics.get("scheduler_cpu_total_attributed_ms", 0.0));
        const double unattributed_cpu_ms = double(analytics.get("scheduler_cpu_unattributed_ms", 0.0));

        CHECK(visibility_cpu_ms >= 0.0);
        CHECK(load_cpu_ms >= 0.0);
        CHECK(build_visible_cpu_ms >= 0.0);
        CHECK(prefetch_cpu_ms >= 0.0);
        CHECK(update_cpu_ms >= 0.0);
        CHECK(attributed_cpu_ms >= 0.0);
        CHECK(unattributed_cpu_ms >= 0.0);
        CHECK(update_cpu_ms + 0.0001 >= attributed_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= visibility_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= load_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= build_visible_cpu_ms);
        CHECK(attributed_cpu_ms + 0.0001 >= prefetch_cpu_ms);

        CHECK(int64_t(analytics.get("atlas_generation", int64_t(-1))) == int64_t(generation_after_update));
        CHECK(system->get_atlas_generation() == generation_after_update);
        CHECK(system->get_asset_meta_buffer().is_valid());
        CHECK(system->get_chunk_meta_buffer().is_valid());
        CHECK(system->get_asset_chunk_index_buffer().is_valid());

        const uint64_t generation_after_first_update = generation_after_update;
        const RID asset_meta_after_first_update = system->get_asset_meta_buffer();
        const RID chunk_meta_after_first_update = system->get_chunk_meta_buffer();
        const RID asset_chunk_index_after_first_update = system->get_asset_chunk_index_buffer();

        system->begin_frame();
        system->update_streaming(camera_transform, projection);
        system->end_frame();

        CHECK(system->get_atlas_generation() == generation_after_first_update);
        CHECK(system->get_asset_meta_buffer().get_id() == asset_meta_after_first_update.get_id());
        CHECK(system->get_chunk_meta_buffer().get_id() == chunk_meta_after_first_update.get_id());
        CHECK(system->get_asset_chunk_index_buffer().get_id() == asset_chunk_index_after_first_update.get_id());
        const bool asset_meta_stable = (system->get_asset_meta_buffer().get_id() == asset_meta_before.get_id()) || (asset_meta_before.get_id() == 0);
        CHECK(asset_meta_stable);
        const bool chunk_meta_stable = (system->get_chunk_meta_buffer().get_id() == chunk_meta_before.get_id()) || (chunk_meta_before.get_id() == 0);
        CHECK(chunk_meta_stable);
        const bool chunk_index_stable = (system->get_asset_chunk_index_buffer().get_id() == asset_chunk_index_before.get_id()) || (asset_chunk_index_before.get_id() == 0);
        CHECK(chunk_index_stable);
    }
}

TEST_CASE("[Streaming Pipeline] initialize_empty republishes atlas state after registry cleanup") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 27182;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    Transform3D camera_transform;
    camera_transform.origin = Vector3(0.0f, 0.0f, 5.0f);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 2000.0f);

    system->begin_frame();
    system->update_streaming(camera_transform, projection);
    system->end_frame();

    const uint64_t generation_before_reinit = system->get_atlas_generation();
    CHECK(generation_before_reinit > 0);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());

    system->initialize_empty(rd);
    CHECK(system->is_runtime_ready());
    CHECK(system->get_atlas_generation() > generation_before_reinit);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
    CHECK_FALSE(system->has_asset(asset_id));
}

TEST_CASE("[Streaming Pipeline] initialize_empty keeps atlas metadata buffers valid with zero chunks") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    const uint64_t first_generation = system->get_atlas_generation();
    CHECK(first_generation > 0);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());

    system->initialize_empty(rd);

    CHECK(system->is_runtime_ready());
    CHECK(system->get_atlas_generation() > first_generation);
    CHECK(system->get_asset_meta_buffer().is_valid());
    CHECK(system->get_chunk_meta_buffer().is_valid());
    CHECK(system->get_asset_chunk_index_buffer().is_valid());
}

namespace {

// Captures every ERR_PRINT / WARN routed through Godot's error handlers so
// the failed-init regression test can count how many ERR diagnostics escape
// from a streaming system that never acquired a RenderingDevice. Pattern
// borrowed from test_integration.cpp's ScopedErrorCapture.
struct ScopedStreamingErrorCapture : public ErrorHandlerList {
    Vector<String> messages;
    int error_count = 0;

    static void _error_handler(void *p_userdata, const char *, const char *, int,
            const char *p_error, const char *p_message,
            bool, ErrorHandlerType p_type) {
        ScopedStreamingErrorCapture *self = static_cast<ScopedStreamingErrorCapture *>(p_userdata);
        String message;
        if (p_message && p_message[0]) {
            message = String::utf8(p_message);
        } else if (p_error) {
            message = String::utf8(p_error);
        }
        if (!message.is_empty()) {
            self->messages.push_back(message);
        }
        if (p_type == ERR_HANDLER_ERROR) {
            self->error_count++;
        }
    }

    ScopedStreamingErrorCapture() {
        errfunc = _error_handler;
        userdata = this;
        add_error_handler(this);
    }

    ~ScopedStreamingErrorCapture() {
        remove_error_handler(this);
    }

    int streaming_init_failed_count() const {
        int n = 0;
        for (int i = 0; i < messages.size(); i++) {
            if (messages[i].find("[Streaming]") != -1 &&
                    messages[i].find("Initialization failed") != -1) {
                n++;
            }
        }
        return n;
    }

    int streaming_update_aborted_count() const {
        int n = 0;
        for (int i = 0; i < messages.size(); i++) {
            if (messages[i].find("[Streaming]") != -1 &&
                    messages[i].find("update_streaming aborted") != -1) {
                n++;
            }
        }
        return n;
    }
};

} // namespace

// PR #352 regression: a GaussianStreamingSystem whose initialize() fails
// because no RenderingDevice is available must not flood the log when
// update_streaming() is then driven once per frame, and must not crash.
//
// Before the warned-once latch and entry guards, every frame re-entered
// _run_streaming_frame_pipeline -> _load_visible_chunks, each call re-emitted
// the "runtime not loadable" ERR, and run_module_tests.py's [untagged] lane
// produced 602 SEH CrashHandlerException dumps with empty backtraces. See the
// work-package brief in #352 for the full reproduction.
//
// This test deliberately bypasses any test infrastructure that would acquire
// a real device — it constructs the system directly, calls initialize() with
// no RenderDeviceManager, then ticks update_streaming five times and asserts
// the diagnostic noise is bounded.
TEST_CASE("[GaussianSplatting][Streaming] Initialize without device emits at most one ERR_PRINT") {
    // If a RenderingDevice happens to be available in this lane, the failed-
    // init path is not exercised and the test is irrelevant — skip rather
    // than fail. The intent is to validate the *absence* of a cascade when
    // the device is unavailable. GaussianStreamingSystem::initialize() can
    // also obtain a device via GaussianSplatManager::get_primary_rendering_device()
    // (including its local-device fallback), so probe that path too —
    // otherwise initialize() succeeds in lanes where the manager supplies a
    // device and the CHECK_FALSE assertions below fail spuriously. Matches
    // the 3-probe pattern in 1c447b99e3 and the REQUIRE_STREAMING_CAPABLE
    // macro fix in f42e803ce9.
    bool has_device = RenderingDevice::get_singleton() != nullptr;
    if (!has_device) {
        if (RenderingServer *rs_probe = RenderingServer::get_singleton()) {
            has_device = rs_probe->get_rendering_device() != nullptr;
        }
    }
    if (!has_device) {
        if (GaussianSplatManager *mgr_probe = GaussianSplatManager::get_singleton()) {
            has_device = mgr_probe->get_primary_rendering_device() != nullptr;
        }
    }
    if (has_device) {
        MESSAGE("Skipping test - this regression targets the no-device path; "
                "this lane has a RenderingDevice");
        return;
    }

    LocalVector<Gaussian> gaussians;
    gaussians.resize(256);
    for (uint32_t i = 0; i < gaussians.size(); i++) {
        Gaussian &g = gaussians[i];
        g.position = Vector3(float(i) * 0.01f, 0.0f, -2.0f);
        g.scale = Vector3(0.05f, 0.05f, 0.05f);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 1.0f, 1.0f, 1.0f);
        g.normal = Vector3(0.0f, 1.0f, 0.0f);
        g.area = 0.01f;
    }

    Ref<::GaussianData> data;
    data.instantiate();
    data->set_gaussians(gaussians);

    ScopedStreamingErrorCapture capture;

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize(data);

    // initialize() must have failed (no device) and left the system in a
    // safe, non-ready state.
    CHECK_FALSE(system->is_runtime_ready());
    CHECK_FALSE(system->is_streaming_capable());

    // Exactly one [Streaming] initialization failure diagnostic. The wording
    // can be one of several flavors (runtime not loadable, buffer overflow,
    // addressable limit) depending on the platform's reported defaults, so
    // we don't pin the exact substring beyond the [Streaming] +
    // "Initialization failed" pair.
    const int init_errs_after_initialize = capture.streaming_init_failed_count();
    CHECK(init_errs_after_initialize <= 1);

    // Now drive update_streaming five times. With the PR #352 guards in
    // place this must not crash and must not emit additional [Streaming]
    // ERRs — the warned-once latch is owned by initialize() so subsequent
    // re-entries are silent.
    const Transform3D camera = Transform3D();
    const Projection projection = Projection();
    for (int i = 0; i < 5; i++) {
        system->update_streaming(camera, projection);
    }

    // Strong assertion: fewer than 5 ERR_PRINTs across the 5 update calls.
    // The brief asks for "<5"; with the warned-once latch in place this
    // should be 0 (the initialize call already armed the latch).
    const int init_errs_after_updates = capture.streaming_init_failed_count();
    CHECK(init_errs_after_updates - init_errs_after_initialize == 0);
    const int update_errs_after_updates = capture.streaming_update_aborted_count();
    CHECK(update_errs_after_updates == 0);

    // Belt-and-braces total cap from the brief: combined [Streaming] ERRs
    // across 5 update calls must stay below 5 (5 = naive per-frame
    // re-emission baseline).
    const int total_streaming_errs = init_errs_after_updates + update_errs_after_updates;
    CHECK(total_streaming_errs < 5);

    // Surviving the 5-call loop without an SEH crash IS the load-bearing
    // assertion — doctest fails the test if the process aborts. Reaching
    // this line proves the cascade is closed.
}

TEST_CASE("[Streaming Pipeline] async upload dropped fail-closed on a pack-time/effective stride flip before writing (#766)") {
    // #766 follow-up: the async pack path always emits the 144 B PackedGaussian layout, and
    // process_upload_queue() sizes/offsets the write by sizeof(PackedGaussian). If the effective
    // atlas stride flips 144->80 (a mixed-DC (un)registration toggling per-chunk quantization
    // DC-compatibility) while a 144 B job is in flight, writing it would splice the 144 B payload
    // across the now-80 B slot grid and clobber neighboring resident 80 B slots. The #757
    // retirement guard only drops the ticket AFTER the write; the #766 pre-write guard in
    // process_upload_queue() must drop it BEFORE any buffer_update.
    //
    // Mutation/discrimination: remove the pre-write stride guard in process_upload_queue() and the
    // 144 B job is written (the #757 guard then drops it at retirement instead) ->
    // stride_flip_dropped_prewrite_uploads stays 0 while stride_flip_dropped_upload_retirements
    // becomes 1. The prewrite-counter assertion below is what flips RED.
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        MESSAGE("Skipping - Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        MESSAGE("Skipping - Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        MESSAGE("Skipping - Async pack threads unavailable");
        return;
    }

    // Effective 144 B stride at pack time (quantization enabled but mixed-DC), so the async pack
    // path is permitted and stamps packed_stride_bytes = sizeof(PackedGaussian).
    system->_test_set_quantization_state(true, false);
    REQUIRE(system->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));

    const uint32_t asset_id = 4243;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    REQUIRE(queued_upload);
    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    REQUIRE(prepared_job != nullptr);

    // Flip the effective stride to 80 B BEFORE processing the queue. The in-flight 144 B job now
    // mismatches the atlas grid; writing it would corrupt neighboring 80 B slots.
    system->_test_set_quantization_state(true, true);
    REQUIRE(system->_test_atlas_gaussian_stride_bytes() != uint64_t(sizeof(PackedGaussian)));

    const uint64_t prewrite_before = system->_test_get_stride_flip_dropped_prewrite_uploads();
    const uint64_t retire_before = system->_test_get_stride_flip_dropped_upload_retirements();

    uploads.process_upload_queue(system_ref);

    // Load-bearing discriminator: the job was dropped BEFORE any buffer write by the pre-write
    // stride guard, so the pre-write counter advances by exactly one...
    CHECK(system->_test_get_stride_flip_dropped_prewrite_uploads() == prewrite_before + 1);
    // ...and NOT via the retirement path (no write happened, so no retirement ticket was staged).
    CHECK(system->_test_get_stride_flip_dropped_upload_retirements() == retire_before);
    CHECK(system->get_loaded_chunks() == 0);
    CHECK(system->get_pending_upload_jobs() == 0);
}
