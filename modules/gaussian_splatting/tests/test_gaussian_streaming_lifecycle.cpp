#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_streaming.h"
#include "../renderer/gaussian_gpu_layout.h"

#include "test_macros.h"
#include "gs_test_pump.h"

#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
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
    if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
        return { rd, false };
    }
    if (RenderingServer *rs = RenderingServer::get_singleton()) {
        RenderingDevice *rd = rs->create_local_rendering_device();
        return { rd, rd != nullptr };
    }
    return {};
}

StreamingUploadPipeline::PendingChunkUpload *_wait_for_prepared_upload(StreamingUploadPipeline &p_uploads) {
    StreamingUploadPipeline::PendingChunkUpload *prepared_job = nullptr;
    const auto prepared = TestGaussianSplatting::gs_pump_until([&]() {
        Thread::yield();
        MutexLock lock(p_uploads.pack_mutex);
        if (p_uploads.upload_queue_read_idx < p_uploads.upload_queue.size()) {
            prepared_job = p_uploads.upload_queue[p_uploads.upload_queue_read_idx];
            if (prepared_job && !prepared_job->packed_data.is_empty()) {
                return true;
            }
            prepared_job = nullptr;
        }
        return false;
    });
    if (!prepared.ready()) {
        FAIL("The pack worker did not prepare an upload ", prepared.describe());
        return nullptr;
    }
    return prepared_job;
}

bool _tamper_first_payload_byte(StreamingUploadPipeline &p_uploads) {
    MutexLock lock(p_uploads.pack_mutex);
    if (p_uploads.upload_queue_read_idx >= p_uploads.upload_queue.size()) {
        FAIL("Prepared upload queue is empty before payload mutation");
        return false;
    }
    StreamingUploadPipeline::PendingChunkUpload *prepared_job =
            p_uploads.upload_queue[p_uploads.upload_queue_read_idx];
    if (!prepared_job || prepared_job->packed_data.is_empty()) {
        FAIL("Prepared upload payload is unavailable before mutation");
        return false;
    }
    PackedGaussian *packed_data = prepared_job->packed_data.ptrw();
    if (!packed_data) {
        FAIL("Prepared upload payload cannot be written");
        return false;
    }
    uint8_t *payload_bytes = reinterpret_cast<uint8_t *>(packed_data);
    payload_bytes[0] ^= 0x01;
    return true;
}

void _advance_frames_until_upload_retired(Ref<GaussianStreamingSystem> p_system) {
    const auto retired = TestGaussianSplatting::gs_pump_until([&]() {
        p_system->begin_frame();
        p_system->end_frame();
        return p_system->get_pending_upload_retirement_slots() == 0;
    });
    if (!retired.ready()) {
        FAIL("Upload retirement did not reach its frame barrier ", retired.describe());
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
    REQUIRE(system._test_atlas_allocator().allocate_slot(chunk_key, GaussianStreamingSystem::atlas_pages_for_splats(chunk.count), buffer_slot));
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
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);

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
    REQUIRE(system._test_atlas_allocator().allocate_slot(chunk_key, GaussianStreamingSystem::atlas_pages_for_splats(chunk.count), buffer_slot));
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
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);

    system._test_process_upload_retirements();
    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);

    system.begin_frame();
    CHECK(chunk.upload_pending);
    CHECK_FALSE(chunk.is_loaded);
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);
    system.begin_frame();

    CHECK(chunk.is_loaded);
    CHECK(chunk.gpu_resident);
    CHECK_FALSE(chunk.upload_pending);
    CHECK(system.get_chunks_loaded_this_frame() == 1);
    CHECK(system._test_get_retired_upload_slots_this_frame() == 1);
    CHECK(system._test_get_retired_upload_bytes_this_frame() == upload_bytes);
    CHECK(system.get_pending_upload_retirement_slots() == 0);
    CHECK(system.get_pending_upload_retirement_bytes() == 0);
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] sync fallback drain counts immediate retirement once") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    const uint32_t asset_id = 3531;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());
    const Error request_error = system->request_chunk_residency(asset_id, 0, 0);
    CHECK(request_error == OK);
    if (request_error != OK) {
        FAIL("The sync-fallback fixture could not request chunk residency");
        return;
    }
    const bool load_queued = system->_test_enqueue_sync_fallback_chunk_load(asset_id, 0, true);
    CHECK(load_queued);
    if (!load_queued) {
        FAIL("The sync-fallback fixture could not queue its chunk load");
        return;
    }

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
    REQUIRE(system._test_atlas_allocator().allocate_slot(pending_key, GaussianStreamingSystem::atlas_pages_for_splats(chunks[0].count), pending_slot));
    REQUIRE(system._test_begin_chunk_upload(0, 0, chunks[0], pending_slot));
    const uint64_t pending_bytes = chunks[0].pending_upload_bytes;
    REQUIRE(pending_bytes > 0);

    uint32_t stale_slot = UINT32_MAX;
    const uint64_t stale_key = system._test_make_chunk_key(0, 1);
    REQUIRE(system._test_atlas_allocator().allocate_slot(stale_key, GaussianStreamingSystem::atlas_pages_for_splats(chunks[1].count), stale_slot));
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
    REQUIRE(system._test_atlas_allocator().allocate_slot(system._test_make_chunk_key(asset_id, 0),
            GaussianStreamingSystem::atlas_pages_for_splats(chunk.count), buffer_slot));
    REQUIRE(system._test_begin_chunk_upload(asset_id, 0, chunk, buffer_slot));
    REQUIRE(system._test_stage_chunk_upload_retirement(asset_id, 0, chunk, buffer_slot,
            upload_bytes, 2,
            GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER));

    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == upload_bytes);
    CHECK(system._test_get_reserved_chunk_count() == 1);
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);

    system.register_asset(asset_id, _create_streaming_phase_order_test_data());

    GaussianStreamingTypes::AtlasAssetState *refreshed_asset = system._test_get_asset_state(asset_id);
    REQUIRE(refreshed_asset != nullptr);
    LocalVector<GaussianStreamingTypes::StreamingChunk> &refreshed_chunks = system._test_get_asset_chunks(*refreshed_asset);
    REQUIRE(!refreshed_chunks.is_empty());
    CHECK_FALSE(refreshed_chunks[0].upload_pending);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system.get_pending_upload_retirement_bytes() == upload_bytes);
    CHECK(system._test_get_reserved_chunk_count() == 1);
    CHECK(system._test_atlas_allocator().get_free_page_count() == 0);

    system.begin_frame();
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    system.begin_frame();

    CHECK(system.get_pending_upload_retirement_slots() == 0);
    CHECK(system.get_pending_upload_retirement_bytes() == 0);
    CHECK(system._test_get_reserved_chunk_count() == 0);
    CHECK(system._test_atlas_allocator().get_free_page_count() == 1);
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
    REQUIRE(system._test_atlas_allocator().allocate_slot(system._test_make_chunk_key(0, 1), GaussianStreamingSystem::atlas_pages_for_splats(chunks[1].count), buffer_slot));
    REQUIRE(system._test_begin_chunk_upload(0, 1, chunks[1], buffer_slot));

    CHECK(system.get_loaded_chunks() == 1);
    CHECK(system.get_pending_upload_retirement_slots() == 1);
    CHECK(system._test_get_reserved_chunk_count() == 2);
}

TEST_CASE("[Streaming Pipeline] upload payload checksum validation is off by default") {
    StreamingUploadPipeline uploads;

    CHECK_FALSE(uploads._test_is_upload_payload_checksum_validation_enabled());
}

TEST_CASE("[Streaming Pipeline][RequiresGPU] production upload path skips payload checksum hashing") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4241;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    StreamingUploadPipeline::_test_reset_payload_checksum_hash_calls();
    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    CHECK(queued_upload);
    if (!queued_upload) {
        FAIL("The streaming upload request could not be queued");
        return;
    }

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    CHECK(prepared_job != nullptr);
    if (!prepared_job) {
        FAIL("The streaming pack worker produced no prepared upload");
        return;
    }
    if (!_tamper_first_payload_byte(uploads)) {
        return;
    }

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

TEST_CASE("[Streaming Pipeline][RequiresGPU] async chunk upload rejects tampered payload checksums when validation is enabled") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads._test_set_upload_payload_checksum_validation_enabled(true);
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4242;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    StreamingUploadPipeline::_test_reset_payload_checksum_hash_calls();
    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    CHECK(queued_upload);
    if (!queued_upload) {
        FAIL("The streaming upload request could not be queued");
        return;
    }

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    CHECK(prepared_job != nullptr);
    if (!prepared_job) {
        FAIL("The streaming pack worker produced no prepared upload");
        return;
    }
    if (!_tamper_first_payload_byte(uploads)) {
        return;
    }

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

TEST_CASE("[Streaming Pipeline][RequiresGPU] enabling checksum validation rejects pending jobs without baselines") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("Async pack threads unavailable");
        return;
    }

    const uint32_t asset_id = 4243;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    StreamingUploadPipeline::_test_reset_payload_checksum_hash_calls();
    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    CHECK(queued_upload);
    if (!queued_upload) {
        FAIL("The streaming upload request could not be queued");
        return;
    }

    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    CHECK(prepared_job != nullptr);
    if (!prepared_job) {
        FAIL("The streaming pack worker produced no prepared upload");
        return;
    }
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

TEST_CASE("[Streaming Pipeline][RequiresGPU] update_streaming publishes phase timings before atlas sync and keeps atlas generation stable when idle") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    {
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        system->initialize_empty(rd);
        if (!system->is_runtime_ready()) {
            FAIL("Streaming runtime not ready");
            return;
        }

        const uint32_t asset_id = 31415;
        system->register_asset(asset_id, _create_streaming_phase_order_test_data());
        // Publishing the registered asset replaces initialize_empty's 4-byte
        // placeholders. Idle RID stability starts after that topology change.
        system->_test_sync_global_atlas_state(rd);

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

TEST_CASE("[Streaming Pipeline][RequiresGPU] initialize_empty republishes atlas state after registry cleanup") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
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

TEST_CASE("[Streaming Pipeline][RequiresGPU] initialize_empty keeps atlas metadata buffers valid with zero chunks") {
    const TestRenderingDeviceHandle rd_handle = _get_test_rendering_device();
    RenderingDevice *rd = rd_handle.rd;
    if (!rd) {
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
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

TEST_CASE("[Streaming Pipeline][RequiresGPU] async upload dropped fail-closed on a pack-time/effective stride flip before writing (#766)") {
    // #766 follow-up: the async pack path always emits the 128 B PackedGaussian layout, and
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
        FAIL("Rendering device unavailable");
        return;
    }

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    system->initialize_empty(rd);
    if (!system->is_runtime_ready()) {
        FAIL("Streaming runtime not ready");
        return;
    }

    GaussianStreamingSystem &system_ref = *system.ptr();
    auto &uploads = system->_internal_get_upload_pipeline();
    if (!uploads.async_pack_enabled || !uploads.pack_thread_running.load(std::memory_order_acquire)) {
        FAIL("Async pack threads unavailable");
        return;
    }

    // Effective 144 B stride at pack time (quantization enabled but mixed-DC), so the async pack
    // path is permitted and stamps packed_stride_bytes = sizeof(PackedGaussian).
    system->_test_set_quantization_state(true, false);
    CHECK(system->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian)));
    if (system->_test_atlas_gaussian_stride_bytes() != uint64_t(sizeof(PackedGaussian))) {
        FAIL("The stride-flip fixture did not begin with the packed Gaussian layout");
        return;
    }

    const uint32_t asset_id = 4243;
    system->register_asset(asset_id, _create_streaming_phase_order_test_data());

    const bool queued_upload = uploads.queue_chunk_load(system_ref, asset_id, 0);
    CHECK(queued_upload);
    if (!queued_upload) {
        FAIL("The streaming upload request could not be queued");
        return;
    }
    StreamingUploadPipeline::PendingChunkUpload *prepared_job = _wait_for_prepared_upload(uploads);
    CHECK(prepared_job != nullptr);
    if (!prepared_job) {
        FAIL("The streaming pack worker produced no prepared upload");
        return;
    }

    // Flip the effective stride to 80 B BEFORE processing the queue. The in-flight 144 B job now
    // mismatches the atlas grid; writing it would corrupt neighboring 80 B slots.
    system->_test_set_quantization_state(true, true);
    CHECK(system->_test_atlas_gaussian_stride_bytes() != uint64_t(sizeof(PackedGaussian)));
    if (system->_test_atlas_gaussian_stride_bytes() == uint64_t(sizeof(PackedGaussian))) {
        FAIL("The stride-flip fixture did not change its effective Gaussian layout");
        return;
    }

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

// ---------------------------------------------------------------------------
// #1087: chunk demand must stop at the distance the renderer can draw. Before the
// fix the only bounds were the camera far plane (discovery) and a 100 km load
// threshold, so every frustum-visible chunk out to z_far was demanded and loaded
// although the depth pass drops every splat beyond lod_max_distance / lod_bias.
// ---------------------------------------------------------------------------

namespace {

constexpr float LOAD_DISTANCE_TEST_HALF_EXTENT = 2.0f;

// Chunks on the camera's view axis (camera at the origin looking down -Z), centers
// at 10, 20, 30 ... m. Each chunk's nearest bounds point is HALF_EXTENT closer.
void _setup_load_distance_chunks(GaussianStreamingSystem &r_system, uint32_t p_count, float p_first_distance) {
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = r_system._test_get_primary_chunks();
    chunks.resize(p_count);
    const Vector3 half(LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT);
    for (uint32_t i = 0; i < p_count; i++) {
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk = GaussianStreamingTypes::StreamingChunk();
        chunk.start_idx = i;
        chunk.count = 1;
        chunk.effective_count = 1;
        chunk.center = Vector3(0.0f, 0.0f, -(p_first_distance + 10.0f * float(i)));
        chunk.bounds = AABB(chunk.center - half, half * 2.0f);
        chunk.max_radius = LOAD_DISTANCE_TEST_HALF_EXTENT;
        chunk.is_visible = false;
        chunk.buffer_slot = UINT32_MAX;
    }
}

// Runs one visibility + needed-set pass and returns the published needed_chunks.
int64_t _run_load_distance_frame(GaussianStreamingSystem &r_system) {
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 4000.0f); // the Camera3D default far plane
    const Transform3D camera_transform; // origin, looking down -Z
    r_system.begin_frame();
    r_system._test_get_visibility_controller().update_chunk_visibility(r_system, camera_transform, projection);
    r_system._test_set_visible_scan_result(false, 0);
    r_system._test_build_visible_chunk_list();
    r_system.end_frame();
    return int64_t(r_system.get_streaming_analytics().get("needed_chunks", int64_t(-1)));
}

} // namespace

TEST_CASE("[Streaming Pipeline] Chunk demand stops at the load distance limit (#1087)") {
    // 16 chunks take the linear visibility path, 80 the spatial-grid path.
    struct Shape {
        uint32_t chunk_count;
        float limit;
    };
    const Shape shapes[] = { { 16, 59.0f }, { 80, 309.0f } };
    for (const Shape &shape : shapes) {
        CAPTURE(shape.chunk_count);
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        _setup_load_distance_chunks(*system.ptr(), shape.chunk_count, 10.0f);
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();

        // Precondition, and the unbounded behaviour is kept: with no limit every
        // chunk is inside the 4000 m frustum and demanded.
        system->set_load_distance_limit(0.0f);
        if (_run_load_distance_frame(*system.ptr()) != int64_t(shape.chunk_count)) {
            FAIL("fixture precondition: all ", shape.chunk_count, " chunks must be demanded without a limit");
            continue;
        }

        system->set_load_distance_limit(shape.limit);
        const int64_t needed = _run_load_distance_frame(*system.ptr());
        int64_t expected_needed = 0;
        for (uint32_t i = 0; i < shape.chunk_count; i++) {
            CAPTURE(i);
            // Independent of the engine helper: on the axis the nearest bounds point
            // is the center distance minus the half extent.
            const float near_distance = (10.0f + 10.0f * float(i)) - LOAD_DISTANCE_TEST_HALF_EXTENT;
            const bool within = near_distance <= shape.limit;
            expected_needed += within ? 1 : 0;
            // Nothing beyond the bound is discovered; nothing within it is dropped.
            CHECK(chunks[i].is_visible == within);
        }
        CHECK(needed == expected_needed);
        CHECK(expected_needed < int64_t(shape.chunk_count));
        // A chunk whose center is past the limit but whose bounds reach inside it
        // still holds drawable splats, so it stays demanded.
        const uint32_t straddling = uint32_t((shape.limit + 1.0f - 10.0f) / 10.0f);
        CHECK(chunks[straddling].distance > shape.limit);
        CHECK(chunks[straddling].is_visible);
    }
}

TEST_CASE("[Streaming Pipeline] A view whose chunks all lie past the load distance limit is not a zero-visible stall (#1087)") {
    // A camera that is simply far from the content sees nothing because of the bound,
    // not because of a frustum false negative. The zero-visible machinery (stall
    // warning, forced recovery that can mark every chunk visible) must stay quiet.
    // 16 chunks take the linear path (every chunk is distance-culled); 80 take the
    // grid path (the clamped query box finds no candidate at all).
    const uint32_t shapes[] = { 16, 80 };
    for (uint32_t chunk_count : shapes) {
        CAPTURE(chunk_count);
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        _setup_load_distance_chunks(*system.ptr(), chunk_count, 100.0f);
        system->set_load_distance_limit(50.0f);

        Projection projection;
        projection.set_perspective(60.0f, 1.0f, 0.1f, 4000.0f);
        const Transform3D camera_transform;
        StreamingVisibilityController &visibility = system->_test_get_visibility_controller();
        // Long enough for the startup guard (frame 1) and for the persistent trigger
        // (16 frames) plus cooldown (30 frames) to fire if the state were a stall.
        for (int frame = 0; frame < 64; frame++) {
            system->begin_frame();
            visibility.update_chunk_visibility(*system.ptr(), camera_transform, projection);
            visibility.handle_zero_visible_chunk_recovery(*system.ptr());
            system->_test_set_visible_scan_result(false, 0);
            system->_test_build_visible_chunk_list();
            system->end_frame();
        }
        const Dictionary analytics = system->get_streaming_analytics();
        CHECK(int(analytics.get("zero_visible_recoveries_triggered", -1)) == 0);
        CHECK(int(analytics.get("zero_visible_stall_detections", -1)) == 0);
        CHECK(int(system->get_chunk_culling_stats().get("visible_chunks", -1)) == 0);
        CHECK(int64_t(analytics.get("needed_chunks", int64_t(-1))) == 0);
    }
}

TEST_CASE("[Streaming Pipeline] Zero-visible recovery stays armed when an in-range chunk was frustum-rejected (#1087)") {
    // Mixed view: the chunks ahead are past the limit, but one chunk inside the limit
    // (behind the camera) was rejected by the frustum. That rejection may be a frustum
    // false negative, which is what recovery exists for, so the bound must not
    // suppress it.
    const uint32_t chunk_count = 16;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    _setup_load_distance_chunks(*system.ptr(), chunk_count, 100.0f);
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    const Vector3 half(LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT);
    chunks[0].center = Vector3(0.0f, 0.0f, 20.0f); // behind the camera, 18 m away
    chunks[0].bounds = AABB(chunks[0].center - half, half * 2.0f);
    system->set_load_distance_limit(50.0f);

    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 4000.0f);
    const Transform3D camera_transform;
    StreamingVisibilityController &visibility = system->_test_get_visibility_controller();
    system->begin_frame();
    visibility.update_chunk_visibility(*system.ptr(), camera_transform, projection);
    const Dictionary culling = system->get_chunk_culling_stats();
    if (int(culling.get("visible_chunks", -1)) != 0 || int(culling.get("frustum_culled_chunks", -1)) != 1 ||
            int(culling.get("distance_culled_chunks", -1)) != int(chunk_count - 1)) {
        FAIL("fixture precondition: expected 0 visible, 1 frustum-culled, ", chunk_count - 1, " distance-culled");
        return;
    }
    visibility.handle_zero_visible_chunk_recovery(*system.ptr());
    system->end_frame();
    CHECK(int(system->get_streaming_analytics().get("zero_visible_recoveries_triggered", -1)) >= 1);
}

TEST_CASE("[Streaming Pipeline] Grid zero-visible recovery searches past out-of-range rings for an in-range chunk (#1087)") {
    // Spatial-grid recovery (64+ chunks) used to stop at the first non-empty ring of
    // cells. Here the camera's own cell holds only chunk A, which is past the limit,
    // while chunk B (in range, behind the camera, so frustum-rejected and recovery is
    // armed) sits just across the cell boundary. Recovery must skip A and reach B.
    // Geometry: fillers along -Z out to -6,400 m make the grid cell ~100 m; one filler
    // at x = -93 puts the x cell boundary at about x = +5, between the camera (x = 0)
    // and B (x = 6..10). A at z = -30 is in the camera's cell, 28 m away.
    const float limit = 10.0f;
    const uint32_t filler_count = 78;
    const uint32_t chunk_count = filler_count + 2;
    const uint32_t chunk_a = filler_count;
    const uint32_t chunk_b = filler_count + 1;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(chunk_count);
    const Vector3 half(LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT);
    for (uint32_t i = 0; i < chunk_count; i++) {
        Vector3 center;
        if (i == chunk_a) {
            center = Vector3(0.0f, 0.0f, -30.0f);
        } else if (i == chunk_b) {
            center = Vector3(8.0f, 0.0f, 2.0f);
        } else {
            center = Vector3(i == 0 ? -93.0f : 0.0f, 0.0f, -(100.0f + (6298.0f / float(filler_count - 1)) * float(i)));
        }
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk = GaussianStreamingTypes::StreamingChunk();
        chunk.start_idx = i;
        chunk.count = 1;
        chunk.effective_count = 1;
        chunk.center = center;
        chunk.bounds = AABB(center - half, half * 2.0f);
        chunk.max_radius = LOAD_DISTANCE_TEST_HALF_EXTENT;
        chunk.is_visible = false;
        chunk.buffer_slot = UINT32_MAX;
    }
    system->set_load_distance_limit(limit);

    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 8000.0f);
    const Transform3D camera_transform;
    StreamingVisibilityController &visibility = system->_test_get_visibility_controller();
    system->begin_frame();
    visibility.update_chunk_visibility(*system.ptr(), camera_transform, projection);
    const Dictionary culling = system->get_chunk_culling_stats();
    if (int(culling.get("visible_chunks", -1)) != 0 || int(culling.get("frustum_culled_chunks", -1)) < 1) {
        system->end_frame();
        FAIL("fixture precondition: expected an empty view with B frustum-rejected, got visible=",
                int(culling.get("visible_chunks", -1)), " frustum_culled=", int(culling.get("frustum_culled_chunks", -1)));
        return;
    }
    visibility.handle_zero_visible_chunk_recovery(*system.ptr());
    system->_test_set_visible_scan_result(false, 0);
    system->_test_build_visible_chunk_list();
    system->end_frame();
    CHECK(int(system->get_streaming_analytics().get("zero_visible_recoveries_triggered", -1)) >= 1);
    CHECK(chunks[chunk_b].is_visible);
    CHECK_FALSE(chunks[chunk_a].is_visible);
    CHECK(int64_t(system->get_streaming_analytics().get("needed_chunks", int64_t(-1))) >= 1);
}

TEST_CASE("[Streaming Pipeline] Predictive prefetch never queues a chunk past the load distance limit (#1087)") {
    // Prefetch reaches 1.5 x prefetch_lookahead_distance (10 m by default) around the
    // predicted position. With a 5 m limit, a chunk 8 m from that position could not be
    // drawn there either, so it must not be prefetched; one 3 m away must be.
    const uint32_t chunk_count = 16;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    _setup_load_distance_chunks(*system.ptr(), chunk_count, 100.0f); // all far from the origin
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    const Vector3 predicted(0.0f, 0.0f, -2000.0f);
    const Vector3 half(LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT, LOAD_DISTANCE_TEST_HALF_EXTENT);
    const uint32_t near_chunk = 0; // nearest bounds point 1 m from the predicted position
    const uint32_t far_chunk = 1; // nearest bounds point 6 m from it, inside the prefetch ball
    chunks[near_chunk].center = predicted + Vector3(3.0f, 0.0f, 0.0f);
    chunks[near_chunk].bounds = AABB(chunks[near_chunk].center - half, half * 2.0f);
    chunks[far_chunk].center = predicted + Vector3(-8.0f, 0.0f, 0.0f);
    chunks[far_chunk].bounds = AABB(chunks[far_chunk].center - half, half * 2.0f);
    LocalVector<Gaussian> gaussians;
    gaussians.resize(chunk_count);
    for (uint32_t i = 0; i < chunk_count; i++) {
        gaussians[i].position = chunks[i].center;
        gaussians[i].scale = Vector3(0.05f, 0.05f, 0.05f);
        gaussians[i].rotation = Quaternion();
        gaussians[i].opacity = 1.0f;
    }
    Ref<GaussianData> data;
    data.instantiate();
    data->set_gaussians(gaussians);
    system->_test_begin_device_free_load_scan(data, 32);
    system->set_load_distance_limit(5.0f);

    StreamingVisibilityController &visibility = system->_test_get_visibility_controller();
    // Prefetch needs a moving camera.
    visibility.update_camera_tracking(Vector3(0.0f, 0.0f, 0.0f), 0.1f);
    visibility.update_camera_tracking(Vector3(0.0f, 0.0f, -5.0f), 0.1f);
    const uint32_t queued = visibility.prefetch_chunks_at_predicted_position(*system.ptr(), predicted, 8, 8, UINT32_MAX);
    const bool near_queued = system->_test_sync_fallback_queued(0u, near_chunk);
    const bool far_queued = system->_test_sync_fallback_queued(0u, far_chunk);
    system->_test_end_device_free_load_scan();
    if (queued == 0) {
        FAIL("fixture precondition: prefetch queued nothing, so the bound is not exercised");
        return;
    }
    CHECK(near_queued);
    CHECK_FALSE(far_queued);
}

TEST_CASE("[Streaming Pipeline] Load scan and sync drain never queue a chunk past the load distance limit (#1087)") {
    // The visibility pass is not the only gate: the load scan and the sync-fallback
    // drain apply the same bound themselves. Here visibility ran unbounded, so the far
    // chunks are visible, and then the limit shrank (as when the camera's draw
    // distance drops between frames). Neither path may queue or load a far chunk.
    const uint32_t chunk_count = 16;
    const float limit = 59.0f; // chunks 0..5 are within it (nearest bounds point 8..58 m)
    LocalVector<Gaussian> gaussians;
    gaussians.resize(chunk_count);
    for (uint32_t i = 0; i < chunk_count; i++) {
        gaussians[i].position = Vector3(0.0f, 0.0f, -(10.0f + 10.0f * float(i)));
        gaussians[i].scale = Vector3(0.05f, 0.05f, 0.05f);
        gaussians[i].rotation = Quaternion();
        gaussians[i].opacity = 1.0f;
    }
    Ref<GaussianData> data;
    data.instantiate();
    data->set_gaussians(gaussians);
    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 4000.0f);
    const Transform3D camera_transform;

    // Load scan (_load_visible_chunks).
    {
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        _setup_load_distance_chunks(*system.ptr(), chunk_count, 10.0f);
        system->_test_begin_device_free_load_scan(data, 32);
        system->set_load_distance_limit(0.0f);
        system->_test_get_visibility_controller().update_chunk_visibility(*system.ptr(), camera_transform, projection);
        if (int(system->get_chunk_culling_stats().get("visible_chunks", -1)) != int(chunk_count)) {
            system->_test_end_device_free_load_scan();
            FAIL("fixture precondition: all chunks must be visible before the limit shrinks");
            return;
        }
        system->set_load_distance_limit(limit);
        const uint32_t candidates = system->_test_load_visible_chunks(32);
        uint32_t queued_within = 0;
        for (uint32_t i = 0; i < chunk_count; i++) {
            CAPTURE(i);
            const bool within = (10.0f + 10.0f * float(i)) - LOAD_DISTANCE_TEST_HALF_EXTENT <= limit;
            const bool queued = system->_test_sync_fallback_queued(0u, i); // 0 = primary asset id
            if (!within) {
                CHECK_FALSE(queued);
            }
            queued_within += (within && queued) ? 1 : 0;
        }
        CHECK(candidates == 6);
        // The legal route still works: in-range chunks are queued.
        CHECK(queued_within > 0);
        system->_test_end_device_free_load_scan();
    }

    // Sync-fallback drain (_drain_sync_fallback_chunk_loads).
    {
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        _setup_load_distance_chunks(*system.ptr(), chunk_count, 10.0f);
        system->_test_begin_device_free_load_scan(data, 32);
        system->set_load_distance_limit(0.0f);
        system->_test_get_visibility_controller().update_chunk_visibility(*system.ptr(), camera_transform, projection);
        system->set_load_distance_limit(limit);
        const uint32_t far_chunk = 10; // nearest bounds point 108 m
        if (!system->_test_enqueue_sync_fallback_chunk_load(0u, far_chunk, false)) {
            system->_test_end_device_free_load_scan();
            FAIL("fixture precondition: the far chunk must enter the sync-fallback queue");
            return;
        }
        uint32_t evictions_left = 0;
        bool eviction_blocked = false;
        const uint32_t drained = system->_test_drain_sync_fallback_chunk_loads(32, evictions_left, eviction_blocked);
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
        CHECK(drained == 0);
        CHECK_FALSE(chunks[far_chunk].is_loaded);
        CHECK_FALSE(chunks[far_chunk].upload_pending);
        // Dropped as irrelevant, not attempted: with no device an attempted load would
        // also fail and leave the chunk unloaded, so count the attempts directly.
        CHECK(system->_test_get_sync_fallback_attempted_count() == 0);
        CHECK(system->_test_get_sync_fallback_stalled_count() >= 1);
        CHECK_FALSE(system->_test_sync_fallback_queued(0u, far_chunk));
        system->_test_end_device_free_load_scan();
    }
}

// ---------------------------------------------------------------------------
// #1088: the streaming atlas allocates pages, not fixed 65,536-splat slots, and the
// VRAM budget binds in bytes.
//
// The first two cases use only API that existed before #1088 (initialize(),
// get_buffer_capacity_splats(), the atlas byte stride), so they run unchanged on the
// base and fail there: the base sized the atlas as (chunks + 25%) x 8 MiB slots,
// capped only by max_chunks_in_vram, whatever the budget.
// ---------------------------------------------------------------------------

namespace {

struct AtlasSizingFixture {
    Ref<GaussianStreamingSystem> system;
    uint64_t total_splats = 0;
    uint32_t chunk_count = 0;
};

// One primary chunk per layout hint, each p_splats_per_chunk splats, built through the
// real initialize() path. Without a RenderingDevice the buffer is not created, but the
// atlas capacity is sized exactly as with one.
AtlasSizingFixture _initialize_atlas_sizing_fixture(uint32_t p_chunk_count, uint32_t p_splats_per_chunk,
        uint32_t p_budget_mb, uint32_t p_max_chunks) {
    AtlasSizingFixture fixture;
    fixture.chunk_count = p_chunk_count;
    fixture.total_splats = uint64_t(p_chunk_count) * p_splats_per_chunk;
    Ref<GaussianData> data = _create_streaming_phase_order_test_data(uint32_t(fixture.total_splats));

    Vector<GaussianStreamingSystem::ChunkLayoutHint> hints;
    hints.resize(p_chunk_count);
    for (uint32_t i = 0; i < p_chunk_count; i++) {
        GaussianStreamingSystem::ChunkLayoutHint &hint = hints.write[i];
        // Same shape the render orchestrator publishes for world chunks: remapped through
        // an (identity) source-index table.
        hint.start_idx = 0;
        hint.count = p_splats_per_chunk;
        hint.source_index_offset = i * p_splats_per_chunk;
        hint.source_indices_remapped = true;
        hint.center = Vector3(float(i), 0.0f, -5.0f);
        hint.bounds = AABB(hint.center - Vector3(0.5f, 0.5f, 0.5f), Vector3(1.0f, 1.0f, 1.0f));
        hint.radius = 0.5f;
    }

    fixture.system.instantiate();
    GaussianStreamingSystem::ConfigOverrides overrides;
    overrides.override_vram_budget = true;
    overrides.vram_budget_config.auto_regulate_enabled = false;
    overrides.vram_budget_config.budget_mb = p_budget_mb;
    overrides.vram_budget_config.min_chunks = 1;
    overrides.vram_budget_config.max_chunks = p_max_chunks;
    fixture.system->set_config_overrides(overrides);
    Vector<uint32_t> source_indices;
    source_indices.resize(int(fixture.total_splats));
    for (uint32_t i = 0; i < uint32_t(fixture.total_splats); i++) {
        source_indices.write[i] = i;
    }
    fixture.system->set_primary_chunk_layout(hints, source_indices);
    fixture.system->initialize(data);
    return fixture;
}

} // namespace

TEST_CASE("[Streaming Pipeline] Startup atlas allocation fits the VRAM budget in bytes (#1088)") {
    // 3,000 chunks would need far more than 64 MiB; the budget must cap the allocation.
    const uint64_t budget_bytes = 64ull * 1024ull * 1024ull;
    AtlasSizingFixture fixture = _initialize_atlas_sizing_fixture(3000, 1, 64, 4096);
    if (fixture.system->_test_get_primary_chunks().size() != fixture.chunk_count) {
        FAIL("fixture precondition: the layout hints must produce one chunk per hint");
        return;
    }
    const uint64_t atlas_bytes =
            uint64_t(fixture.system->get_buffer_capacity_splats()) * fixture.system->_test_atlas_gaussian_stride_bytes();
    CHECK(atlas_bytes > 0);
    // At least one full-size chunk must still fit, or the atlas would be unloadable.
    CHECK(fixture.system->get_buffer_capacity_splats() >= GaussianStreamingSystem::CHUNK_SIZE);
    CHECK(atlas_bytes <= budget_bytes);
    // The chunk metadata and index buffers are created after the atlas is sized, but they count
    // against the same budget: atlas + those buffers must fit too.
    const uint64_t meta_bytes = uint64_t(fixture.chunk_count) * (sizeof(ChunkMetaGPU) + sizeof(AssetChunkIndexGPU)) +
            sizeof(AssetMetaGPU);
    CHECK(atlas_bytes + meta_bytes <= budget_bytes);
}

TEST_CASE("[Streaming Pipeline] Startup atlas allocation follows the asset's splats, not its chunk count (#1088)") {
    // 200 chunks of 1,000 splats: 200k splats in total. A generous budget, so only the
    // sizing rule decides. The atlas may hold growth headroom, but it must stay within a
    // small multiple of the asset itself plus a fixed floor of four full-size chunks.
    AtlasSizingFixture fixture = _initialize_atlas_sizing_fixture(200, 1000, 4096, 4096);
    if (fixture.system->_test_get_primary_chunks().size() != fixture.chunk_count) {
        FAIL("fixture precondition: the layout hints must produce one chunk per hint");
        return;
    }
    const uint64_t capacity_splats = fixture.system->get_buffer_capacity_splats();
    CHECK(capacity_splats >= fixture.total_splats);
    CHECK(capacity_splats <= 2u * fixture.total_splats + 4ull * GaussianStreamingSystem::CHUNK_SIZE);
}

TEST_CASE("[Streaming Pipeline] Atlas page runs never overlap and free space stays exact under churn (#1088)") {
    // Randomized allocate/release churn checked against a brute-force page map after every
    // operation: runs are disjoint and inside capacity, free + used == capacity, and
    // can_allocate(k) is true exactly when some free run of k pages exists.
    const uint32_t capacity = 512;
    GaussianAtlasAllocator allocator;
    allocator.reset(capacity);
    LocalVector<uint64_t> live_keys;
    uint64_t next_key = 1;
    uint32_t rng = 0x1088u;
    auto next_rand = [&rng]() -> uint32_t {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };

    bool consistent = true;
    for (uint32_t op = 0; op < 4000 && consistent; op++) {
        const bool do_release = !live_keys.is_empty() && (next_rand() % 100u) < 45u;
        if (do_release) {
            const uint32_t idx = next_rand() % live_keys.size();
            allocator.release_slot(live_keys[idx]);
            live_keys.remove_at_unordered(idx);
        } else {
            const uint32_t pages = 1u + next_rand() % 23u; // 1..23 pages, the corridor's range
            uint32_t slot = UINT32_MAX;
            if (allocator.allocate_slot(next_key, pages, slot)) {
                live_keys.push_back(next_key);
            }
            next_key++;
        }

        LocalVector<uint8_t> owner;
        owner.resize(capacity);
        for (uint32_t p = 0; p < capacity; p++) {
            owner[p] = 0;
        }
        uint32_t used = 0;
        for (uint64_t key : live_keys) {
            GaussianAtlasAllocator::PageRun run;
            if (!allocator.get_run(key, run) || run.page_count == 0 ||
                    uint64_t(run.first_page) + run.page_count > capacity) {
                consistent = false;
                break;
            }
            for (uint32_t p = run.first_page; p < run.first_page + run.page_count; p++) {
                if (owner[p] != 0) {
                    consistent = false; // two runs share a page
                }
                owner[p] = 1;
            }
            used += run.page_count;
        }
        uint32_t longest_free = 0;
        uint32_t current_free = 0;
        for (uint32_t p = 0; p < capacity; p++) {
            current_free = owner[p] == 0 ? current_free + 1 : 0;
            longest_free = MAX(longest_free, current_free);
        }
        if (allocator.get_used_page_count() != used || allocator.get_free_page_count() != capacity - used ||
                allocator.get_largest_free_run() != longest_free ||
                allocator.can_allocate(longest_free + 1) || (longest_free > 0 && !allocator.can_allocate(longest_free))) {
            consistent = false;
        }
    }
    CHECK(consistent);
    CHECK(live_keys.size() > 0);

    for (uint64_t key : live_keys) {
        allocator.release_slot(key);
    }
    // Everything released: one coalesced run spanning the whole atlas.
    CHECK(allocator.get_free_run_count() == 1);
    CHECK(allocator.get_largest_free_run() == capacity);
}

TEST_CASE("[Streaming Pipeline] Resident chunks fill their atlas pages (#1088)") {
    // Corridor-like chunk sizes (4k..23k splats) made resident through the engine's own
    // load bookkeeping. The atlas bytes they hold (evictable usage) must be within one
    // page per chunk of their payload: fill >= 0.9 here. The fixed 65,536-splat slot
    // held ~20% of this.
    const uint32_t sizes[] = { 12713, 4120, 21441, 8004, 18942, 22618, 12820, 9001, 15555, 6400 };
    const uint32_t chunk_count = sizeof(sizes) / sizeof(sizes[0]);
    GaussianStreamingSystem system;
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    chunks.resize(chunk_count);
    uint64_t start = 0;
    uint64_t payload_splats = 0;
    for (uint32_t i = 0; i < chunk_count; i++) {
        chunks[i].start_idx = uint32_t(start);
        chunks[i].count = sizes[i];
        chunks[i].effective_count = sizes[i];
        start += sizes[i];
        payload_splats += sizes[i];
    }
    system._test_register_primary_asset_for_chunks();
    system._test_reset_atlas_allocator(4096);
    for (uint32_t i = 0; i < chunk_count; i++) {
        system._test_mark_chunk_loaded_for_eviction(0, i, false, 0, i + 1, 1.0f);
    }
    if (system.get_loaded_chunks() != chunk_count) {
        FAIL("fixture precondition: every chunk must become resident");
        return;
    }
    const uint64_t stride = system._test_atlas_gaussian_stride_bytes();
    const uint64_t held_bytes = system._test_get_evictable_vram_usage_bytes();
    const uint64_t payload_bytes = payload_splats * stride;
    CHECK(held_bytes >= payload_bytes);
    CHECK(held_bytes < payload_bytes + uint64_t(chunk_count) * GaussianStreamingSystem::ATLAS_PAGE_SPLATS * stride);
    CHECK(double(payload_bytes) / double(held_bytes) >= 0.9);
    // The allocator agrees: exactly the pages those runs hold are in use.
    CHECK(uint64_t(system._test_atlas_allocator().get_used_page_count()) *
                    GaussianStreamingSystem::ATLAS_PAGE_SPLATS * stride ==
            held_bytes);
}

namespace {

// Four 4-page chunks fill a 16-page atlas: A=[0,4) B=[4,8) C=[8,12) D=[12,16).
// LRU order is A, C, B, D, so the two oldest victims are not adjacent.
void _setup_fragmented_atlas(GaussianStreamingSystem &r_system, const bool (&p_visible)[4] = { false, false, false, false }) {
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = r_system._test_get_primary_chunks();
    chunks.resize(5);
    const uint32_t page = GaussianStreamingSystem::ATLAS_PAGE_SPLATS;
    for (uint32_t i = 0; i < 4; i++) {
        chunks[i].start_idx = i * 4 * page;
        chunks[i].count = 4 * page;
        chunks[i].effective_count = chunks[i].count;
    }
    chunks[4].start_idx = 16 * page;
    chunks[4].count = 8 * page; // the incoming chunk needs 8 contiguous pages
    chunks[4].effective_count = chunks[4].count;
    r_system._test_register_primary_asset_for_chunks();
    r_system._test_reset_atlas_allocator(16);
    for (int i = 0; i < 10; i++) {
        r_system.begin_frame(); // clear the eviction hysteresis window
    }
    const uint64_t last_used[4] = { 1, 3, 2, 4 }; // A oldest, then C, B, D
    for (uint32_t i = 0; i < 4; i++) {
        r_system._test_mark_chunk_loaded_for_eviction(0, i, p_visible[i], 0, last_used[i], 1.0f);
    }
}

} // namespace

TEST_CASE("[Streaming Pipeline] Admission evicts until the incoming chunk's page run fits (#1088)") {
    GaussianStreamingSystem system;
    _setup_fragmented_atlas(system);
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    if (system.get_loaded_chunks() != 4 || system._test_atlas_allocator().get_free_page_count() != 0) {
        FAIL("fixture precondition: the atlas must start full with four resident chunks");
        return;
    }
    const uint32_t required_pages = GaussianStreamingSystem::atlas_pages_for_splats(chunks[4].count);
    const uint64_t page_bytes = uint64_t(GaussianStreamingSystem::ATLAS_PAGE_SPLATS) * system._test_atlas_gaussian_stride_bytes();
    const uint64_t held_before = system._test_get_evictable_vram_usage_bytes();

    SUBCASE("enough eviction budget: evicts until a contiguous run exists, and no further") {
        uint32_t evictions = 0;
        CHECK(system._test_evict_until_atlas_fit(required_pages, 4, evictions));
        CHECK(system._test_atlas_allocator().can_allocate(required_pages));
        // A and C are freed first (8 pages, but split); B joins them into one 12-page run.
        CHECK(evictions == 3);
        CHECK_FALSE(chunks[0].is_loaded);
        CHECK_FALSE(chunks[1].is_loaded);
        CHECK_FALSE(chunks[2].is_loaded);
        CHECK(chunks[3].is_loaded);
        // Eviction freed exactly the evicted runs' bytes, which is enough for the new chunk.
        CHECK(held_before - system._test_get_evictable_vram_usage_bytes() == 12u * page_bytes);
        CHECK(system._test_atlas_allocator().get_free_page_count() >= required_pages);
    }

    SUBCASE("exhausted eviction budget: stops, and the freed pages stay usable") {
        uint32_t evictions = 0;
        CHECK_FALSE(system._test_evict_until_atlas_fit(required_pages, 2, evictions));
        CHECK(evictions == 2);
        CHECK(system.get_loaded_chunks() == 2);
        CHECK_FALSE(system._test_atlas_allocator().can_allocate(required_pages));
        CHECK(system._test_atlas_allocator().can_allocate(4));
        CHECK(held_before - system._test_get_evictable_vram_usage_bytes() == 8u * page_bytes);
    }
}

TEST_CASE("[Streaming Pipeline] Upload coalescing merges back-to-back full page runs of any size (#1088)") {
    const uint32_t page = GaussianStreamingSystem::ATLAS_PAGE_SPLATS;
    LocalVector<StreamingUploadPipeline::UploadCoalescingCandidate> candidates;
    StreamingUploadPipeline::UploadCoalescingCandidate a;
    a.buffer_slot = 7;
    a.page_count = 3;
    a.packed_count = 3 * page;
    candidates.push_back(a);
    StreamingUploadPipeline::UploadCoalescingCandidate b;
    b.buffer_slot = 10; // starts on the page right after a's run
    b.page_count = 5;
    b.packed_count = 5 * page;
    candidates.push_back(b);
    StreamingUploadPipeline::UploadCoalescingCandidate tail;
    tail.buffer_slot = 15;
    tail.page_count = 2;
    tail.packed_count = page + 17; // does not fill its run: a gap would follow it
    candidates.push_back(tail);

    const uint64_t page_bytes = uint64_t(page) * sizeof(PackedGaussian);
    const StreamingUploadPipeline::UploadCoalescingPlan plan =
            StreamingUploadPipeline::_test_plan_coalesced_upload_batch(candidates, 64u * page_bytes);
    CHECK(plan.coalesced_job_count == 2);
    CHECK(plan.total_bytes == 8u * page_bytes);

    // A run that does not start where the previous one ends is never merged.
    candidates[1].buffer_slot = 11;
    const StreamingUploadPipeline::UploadCoalescingPlan split =
            StreamingUploadPipeline::_test_plan_coalesced_upload_batch(candidates, 64u * page_bytes);
    CHECK(split.coalesced_job_count == 1);
    CHECK(split.total_bytes == 3u * page_bytes);
}

TEST_CASE("[Streaming Pipeline] Page-run eviction does not thrash a sliding corridor window (#1088)") {
    // 600 chunks with corridor-like sizes (1k..22.6k splats) stream through an atlas sized
    // to the window plus 25% slack. The window slides forward one chunk per step and loads
    // nearest-first; each admission may evict up to 4 least-recently-used chunks (the
    // default max_evictions_per_frame), one at a time.
    //
    // Some evictions are inherent: a 23-page chunk replacing 2-page chunks needs several
    // victims even with perfect packing. The fragmentation cost is the rest: an eviction
    // made while enough pages were already free (just not contiguous), which compaction
    // would have avoided, and an admission refused although enough pages were free.
    const uint32_t chunk_count = 600;
    const uint32_t max_evictions = 4;
    GaussianStreamingSystem system;
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    chunks.resize(chunk_count);
    uint32_t rng = 0x1088u;
    uint64_t start = 0;
    uint64_t total_pages = 0;
    for (uint32_t i = 0; i < chunk_count; i++) {
        rng = rng * 1664525u + 1013904223u;
        const uint32_t count = 1000u + (rng >> 8) % 21619u;
        chunks[i].start_idx = uint32_t(start);
        chunks[i].count = count;
        chunks[i].effective_count = count;
        start += count;
        total_pages += GaussianStreamingSystem::atlas_pages_for_splats(count);
    }
    const uint32_t avg_pages = uint32_t(total_pages / chunk_count);
    const uint32_t window = 40;
    const uint32_t capacity = uint32_t(uint64_t(window) * avg_pages * 5u / 4u);
    system._test_register_primary_asset_for_chunks();
    system._test_reset_atlas_allocator(capacity);
    for (int i = 0; i < 10; i++) {
        system.begin_frame();
    }
    const GaussianAtlasAllocator &allocator = system._test_atlas_allocator();

    uint64_t use_stamp = 1;
    uint32_t admissions = 0;
    uint32_t evictions = 0;
    uint32_t fragmentation_evictions = 0; // made while enough pages were free, just not contiguous
    uint32_t refused = 0;
    uint32_t refused_with_free_pages = 0;
    uint64_t window_slots = 0;
    uint64_t window_resident = 0;
    for (uint32_t cam = 0; cam + window <= chunk_count; cam++) {
        system.begin_frame();
        for (uint32_t i = 0; i < chunk_count; i++) {
            chunks[i].is_visible = i >= cam && i < cam + window;
            if (chunks[i].is_visible && chunks[i].is_loaded) {
                chunks[i].last_used_frame = use_stamp++;
            }
        }
        for (uint32_t i = cam; i < cam + window; i++) {
            if (chunks[i].is_loaded) {
                continue;
            }
            const uint32_t pages = GaussianStreamingSystem::atlas_pages_for_splats(chunks[i].count);
            for (uint32_t k = 0; k < max_evictions && !allocator.can_allocate(pages); k++) {
                const bool fragmented = allocator.get_free_page_count() >= pages;
                uint32_t used = 0;
                system._test_evict_until_atlas_fit(pages, 1, used);
                if (used == 0) {
                    break;
                }
                evictions++;
                fragmentation_evictions += fragmented ? 1 : 0;
            }
            if (!allocator.can_allocate(pages)) {
                refused++;
                refused_with_free_pages += allocator.get_free_page_count() >= pages ? 1 : 0;
                continue;
            }
            system._test_mark_chunk_loaded_for_eviction(0, i, true, 0, use_stamp++, 1.0f);
            admissions++;
        }
        for (uint32_t i = cam; i < cam + window; i++) {
            window_slots++;
            window_resident += chunks[i].is_loaded ? 1 : 0;
        }
    }
    const double window_residency = double(window_resident) / double(window_slots);
    MESSAGE(vformat("admissions=%d evictions=%d fragmentation_evictions=%d refused=%d refused_with_free_pages=%d window_residency=%.4f capacity_pages=%d",
            admissions, evictions, fragmentation_evictions, refused, refused_with_free_pages, window_residency, capacity));
    // Measured with best-fit + plain LRU (1,024-splat pages): 0.92 evictions and 0.21
    // fragmentation evictions per admission, 1 of 600 admissions refused with enough free
    // pages (it fits on a later frame), window residency 0.9999. Choosing the LRU victim that
    // completes a run instead (the obvious "smarter" policy) measured worse here: 0.53
    // fragmentation evictions and 13 refusals, because it leaves old chunks scattered.
    // The bounds below leave headroom over the measurement but fail on real thrash.
    CHECK(admissions >= chunk_count);
    CHECK(evictions > 0);
    CHECK(double(evictions) / double(admissions) <= 1.0);
    CHECK(double(fragmentation_evictions) / double(admissions) <= 0.30);
    CHECK(refused_with_free_pages * 100u <= admissions);
    CHECK(window_residency >= 0.99);
}

TEST_CASE("[Streaming Pipeline] A visible chunk is evicted only when that eviction lets the incoming chunk fit (#1088)") {
    SUBCASE("no single visible victim completes the run: nothing on screen is evicted") {
        GaussianStreamingSystem system;
        const bool visible[4] = { true, true, true, true };
        _setup_fragmented_atlas(system, visible);
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
        if (system.get_loaded_chunks() != 4) {
            FAIL("fixture precondition: four resident chunks");
            return;
        }
        uint32_t evictions = 0;
        CHECK_FALSE(system._test_evict_until_atlas_fit(8, 4, evictions));
        CHECK(evictions == 0);
        CHECK(system.get_loaded_chunks() == 4);
        for (uint32_t i = 0; i < 4; i++) {
            CHECK(chunks[i].is_loaded);
        }
    }

    SUBCASE("a visible victim that completes the run is taken, and only that one") {
        GaussianStreamingSystem system;
        // A (oldest) is off screen; B, C, D are visible. Evicting A frees pages 0-3; of the
        // visible chunks only B (pages 4-7) joins that hole into the 8 pages the chunk needs.
        const bool visible[4] = { false, true, true, true };
        _setup_fragmented_atlas(system, visible);
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
        uint32_t evictions = 0;
        CHECK(system._test_evict_until_atlas_fit(8, 4, evictions));
        CHECK(evictions == 2);
        CHECK_FALSE(chunks[0].is_loaded);
        CHECK_FALSE(chunks[1].is_loaded);
        CHECK(chunks[2].is_loaded); // C is visible and older than D, but evicting it would not fit
        CHECK(chunks[3].is_loaded);
        CHECK(system._test_atlas_allocator().can_allocate(8));
    }
}

TEST_CASE("[Streaming Pipeline] A zero-page request never evicts (#1088)") {
    GaussianStreamingSystem system;
    _setup_fragmented_atlas(system);
    uint32_t evictions = 0;
    CHECK_FALSE(system._test_evict_until_atlas_fit(0, 4, evictions));
    CHECK(evictions == 0);
    CHECK(system.get_loaded_chunks() == 4);
}

TEST_CASE("[Streaming Pipeline] Atlas accounting follows the allocated run, not the splat count (#1088)") {
    // Load a 4-page chunk, then change its splat count while it is resident (what a layout
    // change racing a resident chunk would look like). Unloading must give back exactly the
    // four pages the run holds, so the accounting returns to zero.
    GaussianStreamingSystem system;
    _setup_fragmented_atlas(system);
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
    const uint64_t page_bytes = uint64_t(GaussianStreamingSystem::ATLAS_PAGE_SPLATS) * system._test_atlas_gaussian_stride_bytes();
    CHECK(system._test_get_evictable_vram_usage_bytes() == 16u * page_bytes);
    for (uint32_t i = 0; i < 4; i++) {
        chunks[i].count = 100; // would be one page if recomputed from the count
        chunks[i].effective_count = 100;
    }
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(system._test_evict_least_recently_used(false) ==
                StreamingEvictionController::EvictionResult::EvictedNonVisible);
    }
    CHECK(system.get_loaded_chunks() == 0);
    CHECK(system._test_get_evictable_vram_usage_bytes() == 0);
    CHECK(system._test_atlas_allocator().get_used_page_count() == 0);
}

namespace {

// A 512-page atlas (64 MiB at 128 B) under a 64 MiB budget with the default 85% warning threshold,
// 10-page chunks, and a 64-chunk cap. Before #1088's regulator alignment the regulator compared
// payload to the budget: it stopped admitting at 85% and evicted ahead of demand at 76.5%.
void _setup_regulated_atlas(GaussianStreamingSystem &r_system, uint32_t p_resident_chunks) {
    GaussianStreamingSystem::ConfigOverrides overrides;
    overrides.override_vram_budget = true;
    overrides.vram_budget_config.auto_regulate_enabled = false;
    overrides.vram_budget_config.budget_mb = 64;
    overrides.vram_budget_config.min_chunks = 1;
    overrides.vram_budget_config.max_chunks = 64;
    r_system.set_config_overrides(overrides);
    r_system.initialize_empty(nullptr); // no device: creates the regulator, buffer stays absent
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = r_system._test_get_primary_chunks();
    chunks.resize(60);
    const uint32_t count = 10u * GaussianStreamingSystem::ATLAS_PAGE_SPLATS;
    for (uint32_t i = 0; i < chunks.size(); i++) {
        chunks[i].start_idx = i * count;
        chunks[i].count = count;
        chunks[i].effective_count = count;
    }
    r_system._test_register_primary_asset_for_chunks();
    r_system._test_reset_atlas_allocator(512);
    r_system._test_set_atlas_page_ceiling(512);
    for (int i = 0; i < 10; i++) {
        r_system.begin_frame();
    }
    for (uint32_t i = 0; i < p_resident_chunks; i++) {
        r_system._test_mark_chunk_loaded_for_eviction(0, i, false, 0, i + 1, 1.0f);
    }
}

} // namespace

TEST_CASE("[Streaming Pipeline] The VRAM regulator lets the budget-sized atlas fill to its occupancy target (#1088)") {
    SUBCASE("86% full: admission stays open and nothing is evicted ahead of demand") {
        GaussianStreamingSystem system;
        _setup_regulated_atlas(system, 44); // 440 of 512 pages
        if (system._test_atlas_allocator().get_used_page_count() != 440 ||
                system._test_atlas_occupancy_target_pages() != 448) {
            FAIL("fixture precondition: 440 pages used, occupancy target 448");
            return;
        }
        system._test_update_vram_regulator();
        CHECK(system._test_regulator_allows_load());
        bool blocked = false;
        CHECK(system._test_evict_for_vram_budget(blocked) == 0);
        CHECK(system.get_loaded_chunks() == 44);
    }

    SUBCASE("past the target: evicts back to it, no further") {
        GaussianStreamingSystem system;
        _setup_regulated_atlas(system, 47); // 470 pages > 448
        bool blocked = false;
        CHECK(system._test_evict_for_vram_budget(blocked) == 3);
        CHECK(system._test_atlas_allocator().get_used_page_count() == 440);
        CHECK(system._test_atlas_allocator().get_used_page_count() <= system._test_atlas_occupancy_target_pages());
    }
}

// ---------------------------------------------------------------------------
// #1176: a predictive prefetch is a guess about where the camera will be, not demand.
// It used to share the admission path of needed loads, so when the atlas held only
// visible chunks it took the visible-eviction fallback and opened a hole on screen for
// an off-screen chunk. The admission intent now forbids that fallback for prefetch.
// ---------------------------------------------------------------------------

namespace {

constexpr float PREFETCH_ADMISSION_TEST_HALF_EXTENT = 1.0f;

// Four visible 4-page chunks fill a 16-page atlas: A=[0,4) B=[4,8) C=[8,12) D=[12,16).
// Chunk 4 also needs 4 pages, is not visible, and sits at p_predicted_pos, so prefetch
// selects it. Any single visible victim would complete its run: nothing but the
// admission intent keeps a visible chunk in place.
void _setup_full_visible_atlas_with_prefetch_candidate(GaussianStreamingSystem &r_system,
        const Vector3 &p_predicted_pos) {
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = r_system._test_get_primary_chunks();
    chunks.resize(5);
    const uint32_t page = GaussianStreamingSystem::ATLAS_PAGE_SPLATS;
    const Vector3 half(PREFETCH_ADMISSION_TEST_HALF_EXTENT, PREFETCH_ADMISSION_TEST_HALF_EXTENT,
            PREFETCH_ADMISSION_TEST_HALF_EXTENT);
    for (uint32_t i = 0; i < 5; i++) {
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk = GaussianStreamingTypes::StreamingChunk();
        chunk.start_idx = i * 4 * page;
        chunk.count = 4 * page;
        chunk.effective_count = chunk.count;
        // The resident chunks are on screen next to the camera; the candidate is ahead.
        chunk.center = i < 4 ? Vector3(float(i) * 3.0f, 0.0f, -3.0f) : p_predicted_pos;
        chunk.bounds = AABB(chunk.center - half, half * 2.0f);
        chunk.max_radius = PREFETCH_ADMISSION_TEST_HALF_EXTENT;
        chunk.is_visible = i < 4;
        chunk.buffer_slot = UINT32_MAX;
    }
    for (int i = 0; i < 10; i++) {
        r_system.begin_frame(); // clear the eviction hysteresis window
    }
    // Primary asset with source data, a placeholder buffer and a fresh prefetch scan budget.
    r_system._test_begin_device_free_load_scan(_create_streaming_phase_order_test_data(64), 1);
    r_system._test_reset_atlas_allocator(16);
    for (uint32_t i = 0; i < 4; i++) {
        r_system._test_mark_chunk_loaded_for_eviction(0, i, true, 0, i + 1, 3.0f);
    }
    // Prefetch needs a moving camera: here at (0, 0, -5) heading down -Z.
    StreamingVisibilityController &visibility = r_system._test_get_visibility_controller();
    visibility.update_camera_tracking(Vector3(0.0f, 0.0f, 0.0f), 0.1f);
    visibility.update_camera_tracking(Vector3(0.0f, 0.0f, -5.0f), 0.1f);
}

int64_t _read_prefetch_visible_eviction_refusals(GaussianStreamingSystem &r_system) {
    r_system.end_frame();
    return int64_t(r_system.get_streaming_analytics().get("prefetch_visible_eviction_refusals", int64_t(-1)));
}

} // namespace

TEST_CASE("[Streaming Pipeline] Predictive prefetch never evicts a visible chunk to make room (#1176)") {
    // 1.0 x prefetch_lookahead_distance (10 m) ahead of the camera, so the sync drain's
    // own prediction check also places the candidate there.
    const Vector3 predicted(0.0f, 0.0f, -15.0f);

    SUBCASE("async route: the prefetch is skipped and counted, and a needed load still evicts") {
        GaussianStreamingSystem system;
        _setup_full_visible_atlas_with_prefetch_candidate(system, predicted);
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
        auto &uploads = system._internal_get_upload_pipeline();
        uploads._test_set_async_pack_queue_owner(true); // prefetch takes queue_chunk_load()
        if (system.get_loaded_chunks() != 4 || system._test_atlas_allocator().get_free_page_count() != 0) {
            system._test_end_device_free_load_scan();
            FAIL("fixture precondition: four visible resident chunks must fill the atlas");
            return;
        }

        const uint32_t queued = system._test_get_visibility_controller().prefetch_chunks_at_predicted_position(
                system, predicted, 8, 8, UINT32_MAX);
        CHECK(queued == 0);
        CHECK(system.get_loaded_chunks() == 4);
        for (uint32_t i = 0; i < 4; i++) {
            CAPTURE(i);
            CHECK(chunks[i].is_loaded);
        }
        CHECK_FALSE(chunks[4].upload_pending);
        CHECK(system.get_visible_chunks_evicted_this_frame() == 0);

        // The legal route still works: the same chunk requested as needed demand may take
        // the visible-eviction fallback (one victim) and is queued.
        CHECK(uploads.queue_chunk_load(system, 0, 4));
        CHECK(chunks[4].upload_pending);
        CHECK(system.get_loaded_chunks() == 3);
        CHECK(system.get_visible_chunks_evicted_this_frame() == 1);

        system._test_end_device_free_load_scan();
        // Wired into the published analytics: exactly the one refused prefetch admission.
        CHECK(_read_prefetch_visible_eviction_refusals(system) == 1);
    }

    SUBCASE("sync route: the drain admits a predicted-only chunk without visible eviction") {
        GaussianStreamingSystem system;
        _setup_full_visible_atlas_with_prefetch_candidate(system, predicted);
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
        // No async pack owner: prefetch only queues into the sync-fallback queue and the
        // drain makes the admission decision.
        const uint32_t queued = system._test_get_visibility_controller().prefetch_chunks_at_predicted_position(
                system, predicted, 8, 8, UINT32_MAX);
        if (queued != 1 || !system._test_sync_fallback_queued(0u, 4)) {
            system._test_end_device_free_load_scan();
            FAIL("fixture precondition: prefetch must queue the candidate for the sync drain");
            return;
        }

        uint32_t evictions_left = 4;
        bool eviction_blocked = false;
        system._test_drain_sync_fallback_chunk_loads(32, evictions_left, eviction_blocked);
        CHECK(system._test_get_sync_fallback_attempted_count() == 1);
        CHECK(system.get_loaded_chunks() == 4);
        for (uint32_t i = 0; i < 4; i++) {
            CAPTURE(i);
            CHECK(chunks[i].is_loaded);
        }
        CHECK_FALSE(chunks[4].is_loaded);
        CHECK_FALSE(chunks[4].upload_pending);
        CHECK(evictions_left == 4);
        // A refused prediction leaves the frame's eviction budget to the needed loads.
        CHECK_FALSE(eviction_blocked);

        system._test_end_device_free_load_scan();
        CHECK(_read_prefetch_visible_eviction_refusals(system) == 1);
    }
}

// ---------------------------------------------------------------------------
// #1177: cancel_chunk_jobs() filters only the pack and upload queues. A pack job a
// worker has already dequeued survives the cancel; the chunk is rolled back and its run
// released, and when the chunk is queued again best fit hands back the same run. The
// stale upload then matched the chunk's slot, was accepted, and staged a second
// retirement ticket whose retirement rolled back the valid upload. Every upload now
// carries the chunk's upload_sequence, checked at pack completion and at retirement.
// ---------------------------------------------------------------------------

namespace {

int64_t _read_streaming_analytics_counter(GaussianStreamingSystem &r_system, const char *p_key) {
    r_system.end_frame();
    return int64_t(r_system.get_streaming_analytics().get(p_key, int64_t(-1)));
}

} // namespace

TEST_CASE("[Streaming Pipeline] A cancelled pack job a worker already holds cannot retire over the re-queued upload (#1177)") {
    SUBCASE("pack completion drops the stale upload; only the re-queued upload stages a ticket and loads") {
        Ref<GaussianStreamingSystem> system;
        system.instantiate();
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
        chunks.resize(1);
        chunks[0] = GaussianStreamingTypes::StreamingChunk();
        chunks[0].start_idx = 0;
        chunks[0].count = 128;
        chunks[0].effective_count = chunks[0].count;
        chunks[0].is_visible = true;
        chunks[0].buffer_slot = UINT32_MAX;
        system->_test_begin_device_free_load_scan(_create_streaming_phase_order_test_data(256), 1);
        auto &uploads = system->_internal_get_upload_pipeline();
        uploads._test_set_async_pack_queue_owner(true); // queue_chunk_load() needs a pack-queue owner

        if (!uploads.queue_chunk_load(*system.ptr(), 0, 0)) {
            system->_test_end_device_free_load_scan();
            FAIL("fixture precondition: the chunk must be queued for packing");
            return;
        }
        const uint32_t first_run = chunks[0].buffer_slot;
        // A pack worker dequeues the job (pack_thread_func moves it out of pack_queue)...
        StreamingUploadPipeline::PackJob stale_job;
        if (!uploads._test_pop_pack_job(stale_job)) {
            system->_test_end_device_free_load_scan();
            FAIL("fixture precondition: the queued pack job must be dequeued");
            return;
        }
        // ...and before it finishes the chunk is cancelled. The cancel cannot see the job.
        uploads.cancel_chunk_jobs(*system.ptr(), 0, 0, UINT32_MAX);
        // The chunk is requested again and best fit hands back the run just freed.
        if (chunks[0].upload_pending || !uploads.queue_chunk_load(*system.ptr(), 0, 0) ||
                chunks[0].buffer_slot != first_run) {
            system->_test_end_device_free_load_scan();
            FAIL("fixture precondition: cancel, then re-queue into the same page run");
            return;
        }

        // The worker finishes the cancelled job, then the re-queued one.
        uploads._test_complete_pack_job(stale_job);
        CHECK(uploads._test_complete_pack_jobs() == 1);
        // process_upload_queue()'s acceptance and finalize steps, without the buffer write.
        const uint32_t staged = uploads._test_finalize_completed_uploads_without_gpu_write(*system.ptr());
        CHECK(staged == 1);
        CHECK(chunks[0].upload_pending);
        CHECK(chunks[0].buffer_slot == first_run);

        _advance_frames_until_upload_retired(system);
        CHECK(chunks[0].is_loaded);
        CHECK_FALSE(chunks[0].upload_pending);
        CHECK(chunks[0].buffer_slot == first_run);
        CHECK(system->get_loaded_chunks() == 1);
        CHECK(system->_test_get_failed_upload_retirements() == 0);
        CHECK(system->get_pending_upload_retirement_slots() == 0);

        system->_test_end_device_free_load_scan();
        CHECK(_read_streaming_analytics_counter(*system.ptr(), "stale_sequence_dropped_uploads") == 1);
        CHECK(int64_t(system->get_streaming_analytics().get("pending_upload_retirement_tickets", int64_t(-1))) == 0);
    }

    SUBCASE("retirement drops a ticket of a superseded upload instead of rolling back the current one") {
        // Defense in depth for the pack-completion check: a ticket staged for an upload that
        // was rolled back and begun again in the same run must not retire over the new one.
        GaussianStreamingSystem system;
        LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system._test_get_primary_chunks();
        chunks.resize(1);
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[0];
        chunk.start_idx = 0;
        chunk.count = 128;
        chunk.is_visible = true;
        chunk.effective_count = chunk.count;
        system._test_register_primary_asset_for_chunks();
        system._test_reset_atlas_allocator(4);

        const uint64_t chunk_key = system._test_make_chunk_key(0, 0);
        const uint32_t pages = GaussianStreamingSystem::atlas_pages_for_splats(chunk.count);
        const uint64_t upload_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
        uint32_t first_run = UINT32_MAX;
        REQUIRE(system._test_atlas_allocator().allocate_slot(chunk_key, pages, first_run));
        REQUIRE(system._test_begin_chunk_upload(0, 0, chunk, first_run));
        REQUIRE(system._test_stage_chunk_upload_retirement(0, 0, chunk, first_run, upload_bytes, 2,
                GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER));
        // The upload is cancelled while its ticket is in flight...
        system._test_rollback_pending_chunk(0, 0, chunk, true);
        // ...and the chunk begins a new upload in the same run, with its own ticket.
        uint32_t second_run = UINT32_MAX;
        REQUIRE(system._test_atlas_allocator().allocate_slot(chunk_key, pages, second_run));
        if (second_run != first_run) {
            FAIL("fixture precondition: best fit must hand back the same run");
            return;
        }
        REQUIRE(system._test_begin_chunk_upload(0, 0, chunk, second_run));
        REQUIRE(system._test_stage_chunk_upload_retirement(0, 0, chunk, second_run, upload_bytes, 2,
                GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER));

        for (int frame = 0; frame < 8 && !chunk.is_loaded; frame++) {
            system.begin_frame();
        }
        CHECK(chunk.is_loaded);
        CHECK(chunk.buffer_slot == second_run);
        CHECK(system.get_loaded_chunks() == 1);
        CHECK(system._test_get_failed_upload_retirements() == 0);
        CHECK(system._test_atlas_allocator().get_used_page_count() == pages);
        CHECK(_read_streaming_analytics_counter(system, "stale_sequence_dropped_upload_retirements") == 1);
        CHECK(int64_t(system.get_streaming_analytics().get("pending_upload_retirement_tickets", int64_t(-1))) == 0);
    }
}

// ---------------------------------------------------------------------------
// #1178: a chunk whose payload read fails permanently used to be rolled back silently
// (no counter, no log, no memo) and re-queued by the load scan on the very next frame,
// forever. It is now counted, logged once, and retried with exponential backoff.
// ---------------------------------------------------------------------------

namespace {

// In-memory payload source whose reads of one chunk (by start index) always fail, as a
// truncated .gsplatworld or a short network read would. Counts the reads of that chunk.
class FailingChunkPayloadSource : public InMemoryChunkPayloadSource {
    uint32_t failing_start = UINT32_MAX;

public:
    mutable uint32_t failing_reads = 0;

    FailingChunkPayloadSource(const Ref<GaussianData> &p_data, uint32_t p_failing_start) :
            InMemoryChunkPayloadSource(p_data), failing_start(p_failing_start) {}

    bool capture_chunk_snapshot(uint32_t p_start, uint32_t p_count,
            LocalVector<Gaussian> &r_gaussians,
            LocalVector<Vector3> &r_sh_high_order,
            uint32_t &r_sh_first_order_count,
            uint32_t &r_sh_high_order_count) const override {
        if (p_start == failing_start) {
            failing_reads++;
            return false;
        }
        return InMemoryChunkPayloadSource::capture_chunk_snapshot(p_start, p_count, r_gaussians,
                r_sh_high_order, r_sh_first_order_count, r_sh_high_order_count);
    }
};

int _count_payload_read_failure_logs(const ScopedStreamingErrorCapture &p_capture) {
    int n = 0;
    for (int i = 0; i < p_capture.messages.size(); i++) {
        if (p_capture.messages[i].contains("[Streaming]") && p_capture.messages[i].contains("payload read failed")) {
            n++;
        }
    }
    return n;
}

} // namespace

TEST_CASE("[Streaming Pipeline] A chunk whose payload read keeps failing backs off, is counted and logged once (#1178)") {
    const uint32_t chunk_count = 4;
    const uint32_t splats_per_chunk = 128;
    const uint32_t failing_chunk = 1;
    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(chunk_count);
    const Vector3 half(2.0f, 2.0f, 2.0f);
    for (uint32_t i = 0; i < chunk_count; i++) {
        GaussianStreamingTypes::StreamingChunk &chunk = chunks[i];
        chunk = GaussianStreamingTypes::StreamingChunk();
        chunk.start_idx = i * splats_per_chunk;
        chunk.count = splats_per_chunk;
        chunk.effective_count = splats_per_chunk;
        chunk.center = Vector3(0.0f, 0.0f, -(10.0f + 10.0f * float(i))); // all in view, in range
        chunk.bounds = AABB(chunk.center - half, half * 2.0f);
        chunk.max_radius = 2.0f;
        chunk.is_visible = false;
        chunk.buffer_slot = UINT32_MAX;
    }
    const Ref<GaussianData> data = _create_streaming_phase_order_test_data(chunk_count * splats_per_chunk);
    system->_test_begin_device_free_load_scan(data, 1);
    FailingChunkPayloadSource *source = memnew(FailingChunkPayloadSource(data, chunks[failing_chunk].start_idx));
    system->set_chunk_payload_source(0, Ref<ChunkPayloadSource>(source));
    auto &uploads = system->_internal_get_upload_pipeline();
    uploads._test_set_async_pack_queue_owner(true); // the load scan takes queue_chunk_load()

    Projection projection;
    projection.set_perspective(60.0f, 1.0f, 0.1f, 4000.0f);
    const Transform3D camera_transform;
    ScopedStreamingErrorCapture capture;
    // One streaming frame as update_streaming() orders it, minus the GPU write: visibility,
    // load scan, pack (worker stand-in), pack completion and ticket staging.
    auto run_frames = [&](uint32_t p_frames) {
        for (uint32_t frame = 0; frame < p_frames; frame++) {
            system->begin_frame();
            system->_test_get_visibility_controller().update_chunk_visibility(*system.ptr(), camera_transform, projection);
            system->_test_load_visible_chunks(32);
            uploads._test_complete_pack_jobs();
            uploads._test_finalize_completed_uploads_without_gpu_write(*system.ptr());
            system->end_frame();
        }
    };

    // 20 frames: inside the first backoff window (30 frames).
    run_frames(20);
    if (source->failing_reads == 0) {
        system->_test_end_device_free_load_scan();
        FAIL("fixture precondition: the load scan must reach the failing chunk's payload read");
        return;
    }
    // One read, not one per frame.
    CHECK(source->failing_reads == 1);
    CHECK(int64_t(system->get_streaming_analytics().get("chunk_payload_read_failures", int64_t(-1))) == 1);
    CHECK(_count_payload_read_failure_logs(capture) == 1);
    CHECK_FALSE(chunks[failing_chunk].is_loaded);
    CHECK_FALSE(chunks[failing_chunk].upload_pending);
    // The healthy chunks were not starved by the failing one.
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (i != failing_chunk) {
            CAPTURE(i);
            CHECK(chunks[i].is_loaded);
        }
    }
    CHECK(system->get_loaded_chunks() == chunk_count - 1);

    // 40 more frames: the retry happens once the backoff expires (bounded retry, not
    // "never retried", #56), fails again and doubles the wait. Still one log line.
    run_frames(40);
    CHECK(source->failing_reads == 2);
    CHECK(int64_t(system->get_streaming_analytics().get("chunk_payload_read_failures", int64_t(-1))) == 2);
    CHECK(_count_payload_read_failure_logs(capture) == 1);
    CHECK_FALSE(chunks[failing_chunk].is_loaded);
    CHECK(system->get_loaded_chunks() == chunk_count - 1);

    system->_test_end_device_free_load_scan();
}

// ---------------------------------------------------------------------------
// #1179: StagedFileChunkPayloadSource cached one FileAccess per Thread::ID and never
// evicted ids of exited threads. The pack workers are stopped and started again on every
// re-init and whenever pack_worker_threads changes, so every restart left open handles on
// the world file behind. Handles now come from a pool bounded by concurrent captures.
// ---------------------------------------------------------------------------

TEST_CASE("[Streaming Pipeline] Restarting the pack threads does not leak payload file handles (#1179)") {
    const uint32_t splat_count = 128;
    LocalVector<Gaussian> gaussians;
    gaussians.resize(splat_count);
    for (uint32_t i = 0; i < splat_count; i++) {
        Gaussian &g = gaussians[i];
        g.position = Vector3(float(i) * 0.05f, 0.0f, -2.0f);
        g.scale = Vector3(0.05f, 0.05f, 0.05f);
        g.rotation = Quaternion();
        g.opacity = 1.0f;
        g.sh_dc = Color(1.0f, 0.85f, 0.7f, 1.0f);
    }
    const String path = OS::get_singleton()->get_temp_path().path_join(
            vformat("godotgs_streaming_1179_%d.gsplatpayload", int64_t(OS::get_singleton()->get_ticks_usec())));
    {
        Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
        if (f.is_null()) {
            FAIL("could not open the temporary payload file for writing");
            return;
        }
        f->store_buffer(reinterpret_cast<const uint8_t *>(gaussians.ptr()), uint64_t(splat_count) * sizeof(Gaussian));
    }
    Ref<StagedFileChunkPayloadSource> source;
    source.instantiate();
    source->configure(path, 0, 0, splat_count, 0, 0, 0, AABB());

    Ref<GaussianStreamingSystem> system;
    system.instantiate();
    LocalVector<GaussianStreamingTypes::StreamingChunk> &chunks = system->_test_get_primary_chunks();
    chunks.resize(1);
    chunks[0] = GaussianStreamingTypes::StreamingChunk();
    chunks[0].start_idx = 0;
    chunks[0].count = splat_count;
    chunks[0].effective_count = splat_count;
    chunks[0].is_visible = true;
    chunks[0].buffer_slot = UINT32_MAX;
    system->_test_begin_device_free_load_scan(_create_streaming_phase_order_test_data(splat_count), 1);
    system->set_chunk_payload_source(0, source); // workers now read the chunk from the file
    auto &uploads = system->_internal_get_upload_pipeline();
    const uint32_t worker_threads = MAX(1u, uploads.pack_worker_threads);

    // More restarts than the bound, so a handle kept per exited thread would exceed it.
    const uint32_t restarts = worker_threads + 3;
    uint32_t rounds_packed = 0;
    for (uint32_t round = 0; round < restarts; round++) {
        CAPTURE(round);
        uploads.start_pack_threads(*system.ptr());
        if (!uploads.pack_thread_running.load(std::memory_order_acquire)) {
            break;
        }
        system->begin_frame(); // fresh per-frame queue budget
        // A real pack worker of this generation reads the chunk through the source.
        if (uploads.queue_chunk_load(*system.ptr(), 0, 0) && _wait_for_prepared_upload(uploads) != nullptr) {
            rounds_packed++;
        }
        // Joins the workers and drops the pending upload, as a tuning change or re-init does.
        uploads.stop_pack_threads(*system.ptr());
    }
    const uint64_t file_opens = source->get_file_open_count();
    const uint64_t bytes_read = source->get_bytes_read();
    system->_test_end_device_free_load_scan();
    system.unref();
    source.unref();
    DirAccess::remove_absolute(path);

    if (rounds_packed != restarts) {
        FAIL("fixture precondition: every restart must pack the chunk on a worker thread (packed ", rounds_packed,
                " of ", restarts, ")");
        return;
    }
    // Every round really read through the file...
    CHECK(bytes_read == uint64_t(restarts) * splat_count * sizeof(Gaussian));
    CHECK(file_opens >= 1);
    // ...and the open handles stay bounded by what can read at the same time (the workers
    // plus the main thread's sync pack), not by how many threads ever existed.
    CHECK(file_opens <= uint64_t(worker_threads) + 1);
}
