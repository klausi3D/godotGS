// Phase 13: Property bindings extracted from gaussian_splat_renderer.cpp
// This file contains all ClassDB bindings, ADD_PROPERTY, and BIND_ENUM_CONSTANT macros.
// Keeping bindings separate from logic improves code organization.

#include "gaussian_splat_renderer.h"
#ifdef TESTS_ENABLED
#include "../io/gs_payload_page_store.h"
#include "hlod_node_streamer.h"
#include "splat_identity_test_control.h"
#include "gpu_pass_timing.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering_server.h"
#include <cstring>

Error GaussianSplatRenderer::_test_set_payload_telemetry_delays(const Dictionary &p_delays) {
	auto &state = get_resource_state();
	if (!state.hlod_page_store) { return ERR_UNAVAILABLE; }
	if (state.hlod_streamer.is_valid() && state.hlod_streamer->_test_snapshot().jobs_in_flight != 0) { return ERR_BUSY; }
	const Variant read = p_delays.get("read_usec", Variant());
	const Variant decode = p_delays.get("decode_usec", Variant());
	if (read.get_type() != Variant::INT || decode.get_type() != Variant::INT ||
			int64_t(read) < 0 || int64_t(read) > UINT32_MAX || int64_t(decode) < 0 || int64_t(decode) > UINT32_MAX) {
		return ERR_INVALID_PARAMETER;
	}
	return state.hlod_page_store->_test_set_telemetry_delays(uint32_t(int64_t(read)), uint32_t(int64_t(decode)));
}

Error GaussianSplatRenderer::_test_set_shared_layers_saturation(bool p_hold_uploads) {
	auto &state = get_resource_state();
	if (!state.hlod_streaming || state.hlod_streamer.is_null()) { return ERR_UNAVAILABLE; }
	return state.hlod_streamer->_test_set_upload_hold(p_hold_uploads);
}
// GS-GAME-1 owns no production API. Each call is confined to the render thread.
// Buffer lifetime is the renderer's; descriptors borrow it and teardown clears
// the global borrowed RID before freeing it. Async readbacks own their staging.
Dictionary GaussianSplatRenderer::_test_gs_game_1_control(const String &p_control) {
    Dictionary result;
    auto *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { result["error"] = "render_thread_required"; return result; }
    static const char *names[] = { "none", "P1", "H1", "W1", "W2", "W3", "W4", "C1", "R1", "R2", "R3", "S1", "O2", "SVW", "SVU" };
    bool known = false;
    for (const char *name : names) { known |= p_control == name; }
    if (!known) { result["error"] = "unknown_control"; return result; }
    using namespace GSSplatIdentityTestControl;
    set_force_address_keying(p_control == "W1");
    set_force_address_wind_phase(p_control == "W1");
    force_coarse_fade_zero.store(p_control == "W2");
    force_caster_without_deformation.store(p_control == "W3");
    force_legacy_wind_weight.store(p_control == "W4");
    force_nonresident_draw.store(p_control == "H1");
    disable_cache_invalidation.store(p_control == "C1");
    override_by_address.store(p_control == "R1");
    hide_only_in_raster.store(p_control == "R3");
    asset_set_reset.store(p_control == "S1");
    rows_after_shadow.store(p_control == "O2");
    // Shadow split PR0 (renderer/hlod_shadow_face_cull.h): SVW draws every light view from the whole caster list (the
    // reference for the per-view lists), SVU culls by record centres only (the unsafe negative control).
    shadow_view_lists_disabled.store(p_control == "SVW" || shadow_view_lists_env_whole()); // The env keeps a whole-list lane.
    shadow_view_lists_unsafe.store(p_control == "SVU");
    expected_region_key.store(false);
    // Fault bits are packed into rows; force an honest repack on a switch change.
    get_resource_state().hlod_instance_generation = UINT64_MAX;
    test_defer_region_override_upload(p_control == "R2");
    // Tightens only: DeviceVramLedger::configure preserves any lower limit.
    test_set_hlod_route_limit_override(8000000000LL);
    result["control"] = p_control;
    result["tests_enabled"] = true;
    result["shader_variant"] = shader_variant();
    result["address_phase"] = get_force_address_wind_phase();
    result["address_tie_break"] = get_force_address_keying();
    result["region_upload_deferred"] = test_region_override_upload_is_deferred();
    Dictionary switches;
    switches["H1"] = force_nonresident_draw.load();
    switches["W1"] = get_force_address_keying() && get_force_address_wind_phase();
    switches["W2"] = force_coarse_fade_zero.load();
    switches["W3"] = force_caster_without_deformation.load();
    switches["W4"] = force_legacy_wind_weight.load();
    switches["C1"] = disable_cache_invalidation.load();
    switches["R1"] = override_by_address.load();
    switches["R2"] = test_region_override_upload_is_deferred();
    switches["R3"] = hide_only_in_raster.load();
    switches["S1"] = asset_set_reset.load();
    switches["O2"] = rows_after_shadow.load();
    switches["SVW"] = shadow_view_lists_disabled.load();
    switches["SVU"] = shadow_view_lists_unsafe.load();
    result["switches"] = switches;
    // P1 executes its workload through RD only after observing this test-only bridge.
    result["switch_observed"] = p_control == "P1" || p_control == "none" || bool(switches.get(p_control, false));
    return result;
}

Dictionary GaussianSplatRenderer::_test_gs_game_1_capture(const Array &p_probes, int p_target_region, int p_level) {
    Dictionary result;
    auto *server = RenderingServer::get_singleton();
    RenderingDevice *rd = get_device_state().rd;
    if (!server || !server->is_on_render_thread() || !rd || rd != server->get_rendering_device() ||
            p_probes.is_empty() || p_probes.size() > 64 || p_target_region < 0 || p_target_region > 65535 || p_level < -1 || p_level > 65534) {
        result["error"] = "render_thread_live_device_and_1_to_64_keys_required"; return result;
    }
    using namespace GSSplatIdentityTestControl;
    if (capture_buffer.is_valid() && capture_buffer != test_game_capture_buffer) {
        result["error"] = "another_capture_owner"; return result;
    }
    // std430: 16-byte header; each sample is 14 vec4/uvec4 words (224 B).
    constexpr uint32_t stride = sizeof(GSGame1Sample);
    Vector<uint8_t> bytes;
    if (bytes.resize(capture_header_bytes + capture_max_samples * stride) != OK) { result["error"] = "capture_allocation_failed"; return result; }
    bytes.fill(0);
    ++test_game_capture_epoch;
    // Unarmed while scripts author inputs. The arm call establishes the first subsequent draw.
    uint32_t header[4] = { uint32_t(p_probes.size()), 0u, 0u, uint32_t(p_target_region) | (uint32_t(p_level + 1) << 16) };
    memcpy(bytes.ptrw(), header, sizeof(header));
    for (int i = 0; i < p_probes.size(); ++i) {
        if (p_probes[i].get_type() != Variant::DICTIONARY) { result["error"] = "probe_type"; return result; }
        Dictionary key = p_probes[i];
        uint32_t words[4] = {};
        const char *fields[] = { "node", "local", "row" };
        for (int k = 0; k < 3; ++k) {
            const Variant value = key.get(fields[k], Variant());
            if (value.get_type() != Variant::INT || int64_t(value) < 0 || int64_t(value) > UINT32_MAX) {
                result["error"] = "probe_key_range"; return result;
            }
            words[k] = uint32_t(int64_t(value));
        }
        memcpy(bytes.ptrw() + capture_header_bytes + i * stride, words, sizeof(words));
    }
    if (test_game_capture_buffer.is_valid() && !test_game_capture_owner.matches(rd)) { _test_gs_game_1_clear(); }
    if (!test_game_capture_buffer.is_valid()) {
        test_game_capture_buffer = rd->storage_buffer_create(bytes.size(), bytes);
        if (!test_game_capture_buffer.is_valid()) { result["error"] = "capture_buffer_failed"; return result; }
        test_game_capture_owner.set(rd);
    } else if (rd->buffer_update(test_game_capture_buffer, 0, bytes.size(), bytes.ptr()) != OK) {
        _test_gs_game_1_clear(); result["error"] = "capture_upload_failed"; return result;
    }
    capture_buffer = test_game_capture_buffer;
    capture_device_id = rd->get_device_instance_id();
    capture_enabled.store(true);
    result["epoch"] = test_game_capture_epoch;
    result["buffer"] = test_game_capture_buffer;
    result["bytes"] = bytes.size();
    result["stride"] = stride;
    return result;
}

int64_t GaussianSplatRenderer::_test_gs_game_1_arm_capture() {
    auto *server = RenderingServer::get_singleton();
    RenderingDevice *rd = get_device_state().rd;
    if (!server || !server->is_on_render_thread() || !rd || !test_game_capture_owner.matches(rd) ||
            !rd->buffer_is_valid(test_game_capture_buffer)) { return -1; }
    if (rd->buffer_update(test_game_capture_buffer, 4, 4, &test_game_capture_epoch) != OK) { return -1; }
    invalidate_cached_render();
    return RSG::rasterizer ? int64_t(RSG::rasterizer->get_frame_number() + 1) : -1;
}

void GaussianSplatRenderer::GameImageObservation::readback(const Vector<uint8_t> &p_bytes, const Ref<GameImageObservation> &p_box) {
    if (p_box.is_null()) { return; }
    MutexLock lock(p_box->mutex);
    if (p_box->cancelled) { return; }
    p_box->result["bytes"] = p_bytes;
    p_box->result["complete"] = !p_bytes.is_empty();
}

Dictionary GaussianSplatRenderer::_test_gs_game_1_image_request(const Array &p_absent_nodes, const Transform3D &p_pose) {
    Dictionary result;
    auto *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { result["error"] = "render_thread_required"; return result; }
    if (test_game_image.is_valid()) { MutexLock lock(test_game_image->mutex); test_game_image->cancelled = true; }
    test_game_image.instantiate();
    test_game_image->pose = p_pose;
    const auto &state = get_resource_state();
    if (state.hlod_streamer.is_null()) { test_game_image.unref(); result["error"] = "streamer_absent"; return result; }
    for (int i = 0; i < p_absent_nodes.size(); ++i) {
        const uint32_t id = uint32_t(int64_t(p_absent_nodes[i]));
        if (state.hlod_prepared_snapshot.is_null() || id >= uint32_t(state.hlod_prepared_snapshot->nodes.size()) ||
                state.hlod_streamer->get_residency().state(id) == gs_hlod_runtime::NodeResidency::RESIDENT) {
            test_game_image.unref(); result["error"] = "readmission_target_not_absent"; return result;
        }
    }
    test_game_image->absent_nodes = p_absent_nodes.duplicate();
    invalidate_cached_render();
    result["armed"] = true;
    return result;
}

void GaussianSplatRenderer::_test_gs_game_1_publish_image(const RasterStageOutput &p_output) {
    if (test_game_image.is_null()) { return; }
    Ref<GameImageObservation> box = test_game_image;
    RenderingDevice *rd = get_device_state().rd;
    {
        MutexLock lock(box->mutex);
        if (box->cancelled || box->submitted || !rd || p_output.reused_cached_render || p_output.rastered_splat_count == 0 ||
                !p_output.color.is_valid() || !get_camera_transform().is_equal_approx(box->pose)) { return; }
        const Dictionary stats = get_render_stats();
        const Array drawn = stats.get("hlod_drawn_node_ids", Array());
        Array admitted;
        for (int i = 0; i < box->absent_nodes.size(); ++i) {
            if (drawn.has(box->absent_nodes[i])) { admitted.push_back(box->absent_nodes[i]); }
        }
        if (!box->absent_nodes.is_empty() && admitted.is_empty()) { return; }
        box->submitted = true; // One shot, before scheduling the copy. Later frames cannot replace it.
        box->result["frame"] = int64_t(GaussianSplatting::GpuPassTiming::current_frame_serial());
        box->result["drawn_digest"] = stats.get("hlod_drawn_node_digest", Variant());
        box->result["drawn_nodes"] = drawn.duplicate();
        box->result["absent_at_arm"] = box->absent_nodes.duplicate();
        box->result["first_readmitted_nodes"] = admitted;
        box->result["width"] = p_output.internal_size.x;
        box->result["height"] = p_output.internal_size.y;
        box->result["format"] = int(rd->texture_get_format(p_output.color).format);
    }
    const Error error = rd->texture_get_data_async(p_output.color, 0,
            callable_mp_static(&GameImageObservation::readback).bind(box));
    if (error != OK) { MutexLock lock(box->mutex); box->result["error"] = "image_readback_submission_failed"; }
}

Dictionary GaussianSplatRenderer::_test_gs_game_1_image_poll() {
    auto *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread() || test_game_image.is_null()) { return Dictionary(); }
    MutexLock lock(test_game_image->mutex);
    return test_game_image->result.duplicate(true);
}

Dictionary GaussianSplatRenderer::_test_gs_game_1_observe() {
    Dictionary result;
    auto *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { result["error"] = "render_thread_required"; return result; }
    const auto &state = get_resource_state();
    result["stats"] = get_render_stats().duplicate(true);
    result["frame"] = int64_t(get_frame_state().frame_counter);
    result["capture_buffer"] = test_game_capture_buffer;
    result["capture_epoch"] = test_game_capture_epoch;
    result["shader_variant"] = GSSplatIdentityTestControl::shader_variant();
    result["atlas_buffer"] = state.hlod_relative.buffers.atlas_gaussian_buffer;
    result["raw_stride"] = int(sizeof(PackedGaussian));
    result["raw_payload"] = !state.hlod_relative.buffers.quantization_required;
    Array row_keys;
    for (uint64_t key : state.hlod_instance_row_keys) { row_keys.push_back(int64_t(key)); }
    result["row_keys"] = row_keys;
    Array instance_probes;
    for (const auto &draw : state.hlod_instance_draws) {
        Dictionary key;
        key["node"] = draw.chunk_id; key["local"] = 0; key["row"] = draw.instance_id;
        instance_probes.push_back(key);
    }
    result["instance_probes"] = instance_probes;
    Array nodes;
    if (state.hlod_prepared_snapshot.is_valid() && state.hlod_streamer.is_valid()) {
        const auto &source = *state.hlod_prepared_snapshot.ptr();
        const auto &residency = state.hlod_streamer->get_residency();
        for (uint32_t node = 0; node < uint32_t(source.nodes.size()); ++node) {
            Dictionary entry;
            entry["node"] = node;
            entry["level"] = source.nodes[node].topology.height;
            entry["parent"] = source.nodes[node].topology.parent;
            entry["first_child"] = source.nodes[node].topology.first_child;
            entry["child_count"] = source.nodes[node].topology.child_count;
            entry["splats"] = source.nodes[node].topology.payload_count;
            entry["eps"] = source.nodes[node].topology.geometric_error;
            entry["edge"] = Math::pow(2.0, source.nodes[node].topology.cell.e) *
                    ((source.nodes[node].topology.flags & gs_hlod::kNodeFlagOriginCentredRoot) ? 2.0 : 1.0);
            const Vector3 center = source.nodes[node].payload_center;
            Array origin;
            origin.push_back(center.x); origin.push_back(center.y); origin.push_back(center.z);
            entry["center"] = origin;
            entry["resident"] = residency.state(node) == gs_hlod_runtime::NodeResidency::RESIDENT;
            Vector<uint32_t> pages;
            uint32_t base = 0;
            Array page_values;
            if (residency.page_allocation(node, pages, base)) { for (uint32_t page : pages) { page_values.push_back(page); } }
            entry["pages"] = page_values;
            // Churn diagnosis: what holds a resident node (is_protected() inputs, LRU key).
            gs_hlod_runtime::NodeResidency::TestHold hold;
            if (residency.test_hold(node, hold)) {
                Dictionary held;
                held["leases"] = hold.leases;
                held["sibling_leases"] = hold.sibling_leases;
                held["last_drawn_frame"] = int64_t(hold.last_drawn_frame);
                held["drawn_this_frame"] = hold.drawn_this_frame;
                held["edit_hold"] = hold.edit_hold;
                held["pinned_root"] = hold.pinned_root;
                entry["hold"] = held;
            }
            nodes.push_back(entry);
        }
        result["residency_frame"] = int64_t(residency.frame());
    }
    result["nodes"] = nodes;
    return result;
}

// ADR §7 W1 step 3: evicts the given nodes through the residency's own evict(), the call the LRU makes under
// page pressure. It refuses protected nodes (drawn this frame or their ancestors, leased, edit-held, the root)
// and non-resident ones; the run retires and the streamer clears the node metadata as for any eviction.
Dictionary GaussianSplatRenderer::_test_gs_game_1_evict(const Array &p_nodes) {
    Dictionary result;
    auto *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { result["error"] = "render_thread_required"; return result; }
    auto &state = get_resource_state();
    if (state.hlod_streamer.is_null()) { result["error"] = "streamer_absent"; return result; }
    Array evicted, refused;
    for (int i = 0; i < p_nodes.size(); ++i) {
        const Variant value = p_nodes[i];
        if (value.get_type() != Variant::INT || int64_t(value) < 0 || int64_t(value) > UINT32_MAX) { result["error"] = "node_range"; return result; }
        const uint32_t node = uint32_t(int64_t(value));
        if (state.hlod_streamer->_test_evict_node(node) == OK) { evicted.push_back(node); } else { refused.push_back(node); }
    }
    invalidate_cached_render();
    result["evicted"] = evicted;
    result["refused"] = refused;
    return result;
}

bool GaussianSplatRenderer::_test_gs_game_1_expected_regions(bool p_enabled) {
    auto *server = RenderingServer::get_singleton();
    if (!server || !server->is_on_render_thread()) { return false; }
    GSSplatIdentityTestControl::expected_region_key.store(p_enabled);
    return true;
}

void GaussianSplatRenderer::_test_gs_game_1_clear() {
    auto *server = RenderingServer::get_singleton();
    ERR_FAIL_COND(!server || !server->is_on_render_thread());
    _test_gs_game_1_clear_resources();
}

void GaussianSplatRenderer::_test_gs_game_1_clear_resources() {
    using namespace GSSplatIdentityTestControl;
    if (test_game_image.is_valid()) { MutexLock lock(test_game_image->mutex); test_game_image->cancelled = true; }
    test_game_image.unref();
    expected_region_key.store(false);
    if (capture_buffer == test_game_capture_buffer) {
        capture_enabled.store(false);
        capture_buffer = RID();
        capture_device_id = 0;
    }
    if (test_game_capture_buffer.is_valid() && test_game_capture_owner.device) {
        test_game_capture_owner.device->free(test_game_capture_buffer);
    }
    test_game_capture_buffer = RID();
    test_game_capture_owner.clear();
}

#endif
#ifdef TESTS_ENABLED
#include "../resources/color_grading_resource.h"
#endif

void GaussianSplatRenderer::_bind_methods() {
#ifdef TESTS_ENABLED
    ClassDB::bind_method(D_METHOD("_test_set_payload_telemetry_delays", "delays"), &GaussianSplatRenderer::_test_set_payload_telemetry_delays);
    ClassDB::bind_method(D_METHOD("_test_set_shared_layers_saturation", "hold_uploads"), &GaussianSplatRenderer::_test_set_shared_layers_saturation);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_control", "control"), &GaussianSplatRenderer::_test_gs_game_1_control);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_capture", "probes", "target_region", "level"), &GaussianSplatRenderer::_test_gs_game_1_capture);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_arm_capture"), &GaussianSplatRenderer::_test_gs_game_1_arm_capture);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_image_request", "absent_nodes", "pose"), &GaussianSplatRenderer::_test_gs_game_1_image_request);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_image_poll"), &GaussianSplatRenderer::_test_gs_game_1_image_poll);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_observe"), &GaussianSplatRenderer::_test_gs_game_1_observe);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_evict", "nodes"), &GaussianSplatRenderer::_test_gs_game_1_evict);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_expected_regions", "enabled"), &GaussianSplatRenderer::_test_gs_game_1_expected_regions);
    ClassDB::bind_method(D_METHOD("_test_gs_game_1_clear"), &GaussianSplatRenderer::_test_gs_game_1_clear);
#endif
    // Initialization
    ClassDB::bind_method(D_METHOD("initialize"), &GaussianSplatRenderer::initialize);

    // Data management
    ClassDB::bind_method(D_METHOD("set_gaussian_data", "data"), &GaussianSplatRenderer::set_gaussian_data);
    ClassDB::bind_method(D_METHOD("get_gaussian_data"), &GaussianSplatRenderer::get_gaussian_data);
    ClassDB::bind_method(D_METHOD("set_painterly_material", "material"), &GaussianSplatRenderer::set_painterly_material);
    ClassDB::bind_method(D_METHOD("get_painterly_material"), &GaussianSplatRenderer::get_painterly_material);
    ClassDB::bind_method(D_METHOD("force_sort_for_view", "camera_transform"), &GaussianSplatRenderer::force_sort_for_view);
    ClassDB::bind_method(D_METHOD("notify_camera_cut"), &GaussianSplatRenderer::notify_camera_cut);

    // Rendering configuration
    ClassDB::bind_method(D_METHOD("set_render_mode", "mode"), &GaussianSplatRenderer::set_render_mode);
    ClassDB::bind_method(D_METHOD("get_render_mode"), &GaussianSplatRenderer::get_render_mode);
    ClassDB::bind_method(D_METHOD("set_opacity_multiplier", "opacity"), &GaussianSplatRenderer::set_opacity_multiplier);
    ClassDB::bind_method(D_METHOD("get_opacity_multiplier"), &GaussianSplatRenderer::get_opacity_multiplier);
    ClassDB::bind_method(D_METHOD("set_static_sort_cache_enabled", "enabled"), &GaussianSplatRenderer::set_static_sort_cache_enabled);
    ClassDB::bind_method(D_METHOD("is_static_sort_cache_enabled"), &GaussianSplatRenderer::is_static_sort_cache_enabled);
    ClassDB::bind_method(D_METHOD("set_cached_render_reuse_enabled", "enabled"), &GaussianSplatRenderer::set_cached_render_reuse_enabled);
    ClassDB::bind_method(D_METHOD("is_cached_render_reuse_enabled"), &GaussianSplatRenderer::is_cached_render_reuse_enabled);
    ClassDB::bind_method(D_METHOD("reload_gpu_sorting_config_from_project_settings"), &GaussianSplatRenderer::reload_gpu_sorting_config_from_project_settings);

    // Performance controls
    ClassDB::bind_method(D_METHOD("set_lod_enabled", "enabled"), &GaussianSplatRenderer::set_lod_enabled);
    ClassDB::bind_method(D_METHOD("get_lod_enabled"), &GaussianSplatRenderer::get_lod_enabled);
    ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &GaussianSplatRenderer::set_lod_bias);
    ClassDB::bind_method(D_METHOD("get_lod_bias"), &GaussianSplatRenderer::get_lod_bias);
    ClassDB::bind_method(D_METHOD("set_lod_min_screen_size", "pixels"), &GaussianSplatRenderer::set_lod_min_screen_size);
    ClassDB::bind_method(D_METHOD("get_lod_min_screen_size"), &GaussianSplatRenderer::get_lod_min_screen_size);
    ClassDB::bind_method(D_METHOD("set_lod_max_distance", "distance"), &GaussianSplatRenderer::set_lod_max_distance);
    ClassDB::bind_method(D_METHOD("get_lod_max_distance"), &GaussianSplatRenderer::get_lod_max_distance);
    ClassDB::bind_method(D_METHOD("set_importance_cull_threshold", "threshold"), &GaussianSplatRenderer::set_importance_cull_threshold);
    ClassDB::bind_method(D_METHOD("get_importance_cull_threshold"), &GaussianSplatRenderer::get_importance_cull_threshold);
    ClassDB::bind_method(D_METHOD("set_cull_radius_multiplier", "multiplier"), &GaussianSplatRenderer::set_cull_radius_multiplier);
    ClassDB::bind_method(D_METHOD("get_cull_radius_multiplier"), &GaussianSplatRenderer::get_cull_radius_multiplier);
    ClassDB::bind_method(D_METHOD("set_cull_frustum_plane_slack", "slack"), &GaussianSplatRenderer::set_cull_frustum_plane_slack);
    ClassDB::bind_method(D_METHOD("get_cull_frustum_plane_slack"), &GaussianSplatRenderer::get_cull_frustum_plane_slack);
    ClassDB::bind_method(D_METHOD("set_cull_near_tolerance", "tolerance"), &GaussianSplatRenderer::set_cull_near_tolerance);
    ClassDB::bind_method(D_METHOD("get_cull_near_tolerance"), &GaussianSplatRenderer::get_cull_near_tolerance);
    ClassDB::bind_method(D_METHOD("set_cull_far_tolerance", "tolerance"), &GaussianSplatRenderer::set_cull_far_tolerance);
    ClassDB::bind_method(D_METHOD("get_cull_far_tolerance"), &GaussianSplatRenderer::get_cull_far_tolerance);
    ClassDB::bind_method(D_METHOD("set_tiny_splat_screen_radius", "pixels"), &GaussianSplatRenderer::set_tiny_splat_screen_radius);
    ClassDB::bind_method(D_METHOD("get_tiny_splat_screen_radius"), &GaussianSplatRenderer::get_tiny_splat_screen_radius);
    ClassDB::bind_method(D_METHOD("set_opacity_aware_culling", "enabled"), &GaussianSplatRenderer::set_opacity_aware_culling);
    ClassDB::bind_method(D_METHOD("is_opacity_aware_culling"), &GaussianSplatRenderer::is_opacity_aware_culling);
    ClassDB::bind_method(D_METHOD("set_visibility_threshold", "threshold"), &GaussianSplatRenderer::set_visibility_threshold);
    ClassDB::bind_method(D_METHOD("get_visibility_threshold"), &GaussianSplatRenderer::get_visibility_threshold);
    ClassDB::bind_method(D_METHOD("set_distance_cull_enabled", "enabled"), &GaussianSplatRenderer::set_distance_cull_enabled);
    ClassDB::bind_method(D_METHOD("is_distance_cull_enabled"), &GaussianSplatRenderer::is_distance_cull_enabled);
    ClassDB::bind_method(D_METHOD("set_distance_cull_start", "distance"), &GaussianSplatRenderer::set_distance_cull_start);
    ClassDB::bind_method(D_METHOD("get_distance_cull_start"), &GaussianSplatRenderer::get_distance_cull_start);
    ClassDB::bind_method(D_METHOD("set_distance_cull_max_rate", "rate"), &GaussianSplatRenderer::set_distance_cull_max_rate);
    ClassDB::bind_method(D_METHOD("get_distance_cull_max_rate"), &GaussianSplatRenderer::get_distance_cull_max_rate);
    ClassDB::bind_method(D_METHOD("set_overflow_autotune_enabled", "enabled"), &GaussianSplatRenderer::set_overflow_autotune_enabled);
    ClassDB::bind_method(D_METHOD("is_overflow_autotune_enabled"), &GaussianSplatRenderer::is_overflow_autotune_enabled);
    ClassDB::bind_method(D_METHOD("set_max_splats", "count"), &GaussianSplatRenderer::set_max_splats);
    ClassDB::bind_method(D_METHOD("get_max_splats"), &GaussianSplatRenderer::get_max_splats);
    ClassDB::bind_method(D_METHOD("set_frustum_culling", "enabled"), &GaussianSplatRenderer::set_frustum_culling);
    ClassDB::bind_method(D_METHOD("get_frustum_culling"), &GaussianSplatRenderer::get_frustum_culling);

    // Painterly configuration
    ClassDB::bind_method(D_METHOD("set_painterly_enabled", "enabled"), &GaussianSplatRenderer::set_painterly_enabled);
    ClassDB::bind_method(D_METHOD("get_painterly_enabled"), &GaussianSplatRenderer::get_painterly_enabled);
    ClassDB::bind_method(D_METHOD("set_painterly_low_end_mode", "enabled"), &GaussianSplatRenderer::set_painterly_low_end_mode);
    ClassDB::bind_method(D_METHOD("get_painterly_low_end_mode"), &GaussianSplatRenderer::get_painterly_low_end_mode);
    ClassDB::bind_method(D_METHOD("set_painterly_enable_strokes", "enabled"), &GaussianSplatRenderer::set_painterly_enable_strokes);
    ClassDB::bind_method(D_METHOD("get_painterly_enable_strokes"), &GaussianSplatRenderer::get_painterly_enable_strokes);
    ClassDB::bind_method(D_METHOD("set_painterly_internal_scale", "scale"), &GaussianSplatRenderer::set_painterly_internal_scale);
    ClassDB::bind_method(D_METHOD("get_painterly_internal_scale"), &GaussianSplatRenderer::get_painterly_internal_scale);
    ClassDB::bind_method(D_METHOD("set_painterly_edge_threshold", "threshold"), &GaussianSplatRenderer::set_painterly_edge_threshold);
    ClassDB::bind_method(D_METHOD("get_painterly_edge_threshold"), &GaussianSplatRenderer::get_painterly_edge_threshold);
    ClassDB::bind_method(D_METHOD("set_painterly_edge_intensity", "intensity"), &GaussianSplatRenderer::set_painterly_edge_intensity);
    ClassDB::bind_method(D_METHOD("get_painterly_edge_intensity"), &GaussianSplatRenderer::get_painterly_edge_intensity);
    ClassDB::bind_method(D_METHOD("set_painterly_stroke_length", "pixels"), &GaussianSplatRenderer::set_painterly_stroke_length);
    ClassDB::bind_method(D_METHOD("get_painterly_stroke_length"), &GaussianSplatRenderer::get_painterly_stroke_length);
    ClassDB::bind_method(D_METHOD("set_painterly_stroke_opacity", "opacity"), &GaussianSplatRenderer::set_painterly_stroke_opacity);
    ClassDB::bind_method(D_METHOD("get_painterly_stroke_opacity"), &GaussianSplatRenderer::get_painterly_stroke_opacity);
    ClassDB::bind_method(D_METHOD("set_painterly_gamma", "gamma"), &GaussianSplatRenderer::set_painterly_gamma);
    ClassDB::bind_method(D_METHOD("get_painterly_gamma"), &GaussianSplatRenderer::get_painterly_gamma);

    // Quality presets
    ClassDB::bind_method(D_METHOD("set_quality_preset", "preset"), &GaussianSplatRenderer::set_quality_preset);
    ClassDB::bind_method(D_METHOD("get_quality_preset"), &GaussianSplatRenderer::get_quality_preset);

    // Interactive State System
    ClassDB::bind_method(D_METHOD("set_interactive_state", "state"), &GaussianSplatRenderer::set_interactive_state);
    ClassDB::bind_method(D_METHOD("get_interactive_state"), &GaussianSplatRenderer::get_interactive_state);
    ClassDB::bind_method(D_METHOD("enable_highlight_effect", "color"), &GaussianSplatRenderer::enable_highlight_effect);
    ClassDB::bind_method(D_METHOD("enable_outline_effect", "color", "width"), &GaussianSplatRenderer::enable_outline_effect);
    ClassDB::bind_method(D_METHOD("remove_visual_effects"), &GaussianSplatRenderer::remove_visual_effects);

    // Performance monitoring
    ClassDB::bind_method(D_METHOD("get_render_stats"), &GaussianSplatRenderer::get_render_stats);
    ClassDB::bind_method(D_METHOD("get_binning_debug_counters"), &GaussianSplatRenderer::get_binning_debug_counters);
    ClassDB::bind_method(D_METHOD("benchmark_sorting_performance"), &GaussianSplatRenderer::benchmark_sorting_performance);
    ClassDB::bind_method(D_METHOD("run_sort_benchmark", "sizes"), &GaussianSplatRenderer::run_sort_benchmark);
    ClassDB::bind_method(D_METHOD("get_last_sort_metrics"), &GaussianSplatRenderer::get_last_sort_metrics);
	ClassDB::bind_method(D_METHOD("get_sort_metrics_history"), &GaussianSplatRenderer::get_sort_metrics_history);
	ClassDB::bind_method(D_METHOD("get_sort_time_ms"), &GaussianSplatRenderer::get_sort_time_ms);
	ClassDB::bind_method(D_METHOD("get_render_time_ms"), &GaussianSplatRenderer::get_render_time_ms);
	ClassDB::bind_method(D_METHOD("get_overflow_tile_count"), &GaussianSplatRenderer::get_overflow_tile_count);
	ClassDB::bind_method(D_METHOD("get_clamped_records"), &GaussianSplatRenderer::get_clamped_records);
	ClassDB::bind_method(D_METHOD("get_aggregated_count"), &GaussianSplatRenderer::get_aggregated_count);
	ClassDB::bind_method(D_METHOD("get_overflow_stats"), &GaussianSplatRenderer::get_overflow_stats);
	ClassDB::bind_method(D_METHOD("get_visible_splat_count"), &GaussianSplatRenderer::get_visible_splat_count);
	ClassDB::bind_method(D_METHOD("has_rendered_content"), &GaussianSplatRenderer::has_rendered_content);
	ClassDB::bind_method(D_METHOD("was_last_viewport_copy_successful"), &GaussianSplatRenderer::was_last_viewport_copy_successful);
	ClassDB::bind_method(D_METHOD("get_last_viewport_copy_source_size"), &GaussianSplatRenderer::get_last_viewport_copy_source_size);
	ClassDB::bind_method(D_METHOD("get_last_viewport_copy_dest_size"), &GaussianSplatRenderer::get_last_viewport_copy_dest_size);
	ClassDB::bind_method(D_METHOD("get_pipeline_trace_snapshot"), &GaussianSplatRenderer::get_pipeline_trace_snapshot);
	ClassDB::bind_method(D_METHOD("get_pipeline_trace_json"), &GaussianSplatRenderer::get_pipeline_trace_json);
	ClassDB::bind_method(D_METHOD("dump_pipeline_trace_to_file", "path"), &GaussianSplatRenderer::dump_pipeline_trace_to_file);

    ClassDB::bind_method(D_METHOD("set_debug_show_tile_grid", "enabled"), &GaussianSplatRenderer::set_debug_show_tile_grid);
    ClassDB::bind_method(D_METHOD("is_debug_show_tile_grid"), &GaussianSplatRenderer::is_debug_show_tile_grid);
    ClassDB::bind_method(D_METHOD("set_debug_show_density_heatmap", "enabled"), &GaussianSplatRenderer::set_debug_show_density_heatmap);
    ClassDB::bind_method(D_METHOD("is_debug_show_density_heatmap"), &GaussianSplatRenderer::is_debug_show_density_heatmap);
    ClassDB::bind_method(D_METHOD("set_debug_show_performance_hud", "enabled"), &GaussianSplatRenderer::set_debug_show_performance_hud);
    ClassDB::bind_method(D_METHOD("is_debug_show_performance_hud"), &GaussianSplatRenderer::is_debug_show_performance_hud);
    ClassDB::bind_method(D_METHOD("set_debug_show_residency_hud", "enabled"), &GaussianSplatRenderer::set_debug_show_residency_hud);
    ClassDB::bind_method(D_METHOD("is_debug_show_residency_hud"), &GaussianSplatRenderer::is_debug_show_residency_hud);
    ClassDB::bind_method(D_METHOD("set_debug_show_tile_bounds", "enabled"), &GaussianSplatRenderer::set_debug_show_tile_bounds);
    ClassDB::bind_method(D_METHOD("get_debug_show_tile_bounds"), &GaussianSplatRenderer::get_debug_show_tile_bounds);
    ClassDB::bind_method(D_METHOD("set_debug_show_splat_coverage", "enabled"), &GaussianSplatRenderer::set_debug_show_splat_coverage);
    ClassDB::bind_method(D_METHOD("get_debug_show_splat_coverage"), &GaussianSplatRenderer::get_debug_show_splat_coverage);
    ClassDB::bind_method(D_METHOD("set_debug_show_overflow_tiles", "enabled"), &GaussianSplatRenderer::set_debug_show_overflow_tiles);
    ClassDB::bind_method(D_METHOD("get_debug_show_overflow_tiles"), &GaussianSplatRenderer::get_debug_show_overflow_tiles);
    ClassDB::bind_method(D_METHOD("set_debug_show_projection_issues", "enabled"), &GaussianSplatRenderer::set_debug_show_projection_issues);
    ClassDB::bind_method(D_METHOD("get_debug_show_projection_issues"), &GaussianSplatRenderer::get_debug_show_projection_issues);
    ClassDB::bind_method(D_METHOD("set_debug_show_white_albedo", "enabled"), &GaussianSplatRenderer::set_debug_show_white_albedo);
    ClassDB::bind_method(D_METHOD("get_debug_show_white_albedo"), &GaussianSplatRenderer::get_debug_show_white_albedo);
    ClassDB::bind_method(D_METHOD("set_debug_show_hlod_level_tint", "enabled"), &GaussianSplatRenderer::set_debug_show_hlod_level_tint);
    ClassDB::bind_method(D_METHOD("get_debug_show_hlod_level_tint"), &GaussianSplatRenderer::get_debug_show_hlod_level_tint);
    ClassDB::bind_method(D_METHOD("set_debug_show_shadow_opacity", "enabled"), &GaussianSplatRenderer::set_debug_show_shadow_opacity);
    ClassDB::bind_method(D_METHOD("get_debug_show_shadow_opacity"), &GaussianSplatRenderer::get_debug_show_shadow_opacity);
    ClassDB::bind_method(D_METHOD("set_debug_show_device_boundaries", "enabled"), &GaussianSplatRenderer::set_debug_show_device_boundaries);
    ClassDB::bind_method(D_METHOD("is_debug_show_device_boundaries"), &GaussianSplatRenderer::is_debug_show_device_boundaries);
	ClassDB::bind_method(D_METHOD("set_debug_show_texture_states", "enabled"), &GaussianSplatRenderer::set_debug_show_texture_states);
	ClassDB::bind_method(D_METHOD("is_debug_show_texture_states"), &GaussianSplatRenderer::is_debug_show_texture_states);
	ClassDB::bind_method(D_METHOD("set_debug_compute_raster_policy", "policy"), &GaussianSplatRenderer::set_debug_compute_raster_policy);
	ClassDB::bind_method(D_METHOD("get_debug_compute_raster_policy"), &GaussianSplatRenderer::get_debug_compute_raster_policy);
	ClassDB::bind_method(D_METHOD("set_debug_dump_gpu_counters", "enabled"), &GaussianSplatRenderer::set_debug_dump_gpu_counters);
	ClassDB::bind_method(D_METHOD("get_debug_dump_gpu_counters"), &GaussianSplatRenderer::get_debug_dump_gpu_counters);
	ClassDB::bind_method(D_METHOD("set_debug_binning_counters_enabled", "enabled"), &GaussianSplatRenderer::set_debug_binning_counters_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_binning_counters_enabled"), &GaussianSplatRenderer::get_debug_binning_counters_enabled);
	ClassDB::bind_method(D_METHOD("set_debug_pipeline_trace_enabled", "enabled"), &GaussianSplatRenderer::set_debug_pipeline_trace_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_pipeline_trace_enabled"), &GaussianSplatRenderer::get_debug_pipeline_trace_enabled);
	ClassDB::bind_method(D_METHOD("set_perf_capture_force_runtime_statistics", "enabled"), &GaussianSplatRenderer::set_perf_capture_force_runtime_statistics);
	ClassDB::bind_method(D_METHOD("get_perf_capture_force_runtime_statistics"), &GaussianSplatRenderer::get_perf_capture_force_runtime_statistics);
	ClassDB::bind_method(D_METHOD("set_perf_capture_raster_shader_counters", "enabled"), &GaussianSplatRenderer::set_perf_capture_raster_shader_counters);
	ClassDB::bind_method(D_METHOD("get_perf_capture_raster_shader_counters"), &GaussianSplatRenderer::get_perf_capture_raster_shader_counters);
	ClassDB::bind_method(D_METHOD("set_debug_cull_guardrails_enabled", "enabled"), &GaussianSplatRenderer::set_debug_cull_guardrails_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_cull_guardrails_enabled"), &GaussianSplatRenderer::get_debug_cull_guardrails_enabled);
	ClassDB::bind_method(D_METHOD("set_debug_splat_audit_enabled", "enabled"), &GaussianSplatRenderer::set_debug_splat_audit_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_splat_audit_enabled"), &GaussianSplatRenderer::get_debug_splat_audit_enabled);
	ClassDB::bind_method(D_METHOD("set_debug_splat_audit_sample_count", "count"), &GaussianSplatRenderer::set_debug_splat_audit_sample_count);
	ClassDB::bind_method(D_METHOD("get_debug_splat_audit_sample_count"), &GaussianSplatRenderer::get_debug_splat_audit_sample_count);
	ClassDB::bind_method(D_METHOD("set_debug_overlay_opacity", "opacity"), &GaussianSplatRenderer::set_debug_overlay_opacity);
	ClassDB::bind_method(D_METHOD("get_debug_overlay_opacity"), &GaussianSplatRenderer::get_debug_overlay_opacity);
    ClassDB::bind_method(D_METHOD("set_solid_coverage_enabled", "enabled"), &GaussianSplatRenderer::set_solid_coverage_enabled);
    ClassDB::bind_method(D_METHOD("is_solid_coverage_enabled"), &GaussianSplatRenderer::is_solid_coverage_enabled);
    ClassDB::bind_method(D_METHOD("set_solid_coverage_alpha_floor", "alpha_floor"), &GaussianSplatRenderer::set_solid_coverage_alpha_floor);
    ClassDB::bind_method(D_METHOD("get_solid_coverage_alpha_floor"), &GaussianSplatRenderer::get_solid_coverage_alpha_floor);
	ClassDB::bind_method(D_METHOD("set_debug_show_resolve_input", "enabled"), &GaussianSplatRenderer::set_debug_show_resolve_input);
	ClassDB::bind_method(D_METHOD("get_debug_show_resolve_input"), &GaussianSplatRenderer::get_debug_show_resolve_input);
	ClassDB::bind_method(D_METHOD("set_debug_show_resolve_output", "enabled"), &GaussianSplatRenderer::set_debug_show_resolve_output);
	ClassDB::bind_method(D_METHOD("get_debug_show_resolve_output"), &GaussianSplatRenderer::get_debug_show_resolve_output);
	ClassDB::bind_method(D_METHOD("reload_pipeline_feature_set"), &GaussianSplatRenderer::reload_pipeline_feature_set);

    // Jacobian diagnostic toggles for radial stretching investigation
    ClassDB::bind_method(D_METHOD("set_jacobian_bypass_radius_depth_floor", "enabled"), &GaussianSplatRenderer::set_jacobian_bypass_radius_depth_floor);
    ClassDB::bind_method(D_METHOD("get_jacobian_bypass_radius_depth_floor"), &GaussianSplatRenderer::get_jacobian_bypass_radius_depth_floor);
    ClassDB::bind_method(D_METHOD("set_jacobian_bypass_j_col2_clamp", "enabled"), &GaussianSplatRenderer::set_jacobian_bypass_j_col2_clamp);
    ClassDB::bind_method(D_METHOD("get_jacobian_bypass_j_col2_clamp"), &GaussianSplatRenderer::get_jacobian_bypass_j_col2_clamp);
    ClassDB::bind_method(D_METHOD("set_jacobian_invert_j_col2_sign", "enabled"), &GaussianSplatRenderer::set_jacobian_invert_j_col2_sign);
    ClassDB::bind_method(D_METHOD("get_jacobian_invert_j_col2_sign"), &GaussianSplatRenderer::get_jacobian_invert_j_col2_sign);
    ClassDB::bind_method(D_METHOD("set_max_conic_aspect", "aspect"), &GaussianSplatRenderer::set_max_conic_aspect);
    ClassDB::bind_method(D_METHOD("get_max_conic_aspect"), &GaussianSplatRenderer::get_max_conic_aspect);

    ClassDB::bind_method(D_METHOD("set_debug_preview_mode", "mode"), &GaussianSplatRenderer::set_debug_preview_mode);
    ClassDB::bind_method(D_METHOD("get_debug_preview_mode"), &GaussianSplatRenderer::get_debug_preview_mode);
    ClassDB::bind_method(D_METHOD("get_runtime_diagnostic_snapshot"), &GaussianSplatRenderer::get_runtime_diagnostic_snapshot);
    ClassDB::bind_method(D_METHOD("get_tile_renderer"), &GaussianSplatRenderer::get_tile_renderer);

    // Properties
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "gaussian_data", PROPERTY_HINT_RESOURCE_TYPE, "GaussianData"),
                 "set_gaussian_data", "get_gaussian_data");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "painterly_material", PROPERTY_HINT_RESOURCE_TYPE, "PainterlyMaterial"),
                 "set_painterly_material", "get_painterly_material");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "render_mode", PROPERTY_HINT_ENUM, "3D,2D,Hybrid"),
            "set_render_mode", "get_render_mode");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "render/opacity_multiplier", PROPERTY_HINT_RANGE, "0,8,0.01"),
            "set_opacity_multiplier", "get_opacity_multiplier");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sort/static_cache_enabled"),
                 "set_static_sort_cache_enabled", "is_static_sort_cache_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "render/cached_render_reuse_enabled"),
            "set_cached_render_reuse_enabled", "is_cached_render_reuse_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "interactive_state", PROPERTY_HINT_ENUM, "Normal,Hovered,Selected,Disabled"),
                 "set_interactive_state", "get_interactive_state");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lod_enabled"), "set_lod_enabled", "get_lod_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_bias", PROPERTY_HINT_RANGE, "0.01,8.0,0.01"), "set_lod_bias", "get_lod_bias");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_min_screen_size", PROPERTY_HINT_RANGE, "0.0,64.0,0.1"), "set_lod_min_screen_size", "get_lod_min_screen_size");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_max_distance", PROPERTY_HINT_RANGE, "0.0,1000.0,1.0"), "set_lod_max_distance", "get_lod_max_distance");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/importance_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_importance_cull_threshold", "get_importance_cull_threshold");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/radius_multiplier", PROPERTY_HINT_RANGE, "0.5,16.0,0.1"), "set_cull_radius_multiplier", "get_cull_radius_multiplier");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/frustum_plane_slack", PROPERTY_HINT_RANGE, "1.0,8.0,0.1"), "set_cull_frustum_plane_slack", "get_cull_frustum_plane_slack");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/near_tolerance", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_cull_near_tolerance", "get_cull_near_tolerance");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/far_tolerance", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_cull_far_tolerance", "get_cull_far_tolerance");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/tiny_splat_screen_radius", PROPERTY_HINT_RANGE, "0.0,10.0,0.05"), "set_tiny_splat_screen_radius", "get_tiny_splat_screen_radius");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cull/opacity_aware_culling"), "set_opacity_aware_culling", "is_opacity_aware_culling");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/visibility_threshold", PROPERTY_HINT_RANGE, "0.0001,0.1,0.0001"), "set_visibility_threshold", "get_visibility_threshold");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cull/distance_cull_enabled"), "set_distance_cull_enabled", "is_distance_cull_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/distance_cull_start", PROPERTY_HINT_RANGE, "0.0,1000.0,1.0"), "set_distance_cull_start", "get_distance_cull_start");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull/distance_cull_max_rate", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_distance_cull_max_rate", "get_distance_cull_max_rate");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cull/overflow_autotune_enabled"), "set_overflow_autotune_enabled", "is_overflow_autotune_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "max_splats", PROPERTY_HINT_RANGE, "1000,10000000,1000"),
                 "set_max_splats", "get_max_splats");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "frustum_culling"), "set_frustum_culling", "get_frustum_culling");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "painterly/enabled"), "set_painterly_enabled", "get_painterly_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "painterly/low_end_mode"), "set_painterly_low_end_mode", "get_painterly_low_end_mode");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "painterly/enable_strokes"), "set_painterly_enable_strokes", "get_painterly_enable_strokes");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "painterly/internal_scale", PROPERTY_HINT_RANGE, "0.25,1.0,0.05"),
                 "set_painterly_internal_scale", "get_painterly_internal_scale");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "painterly/edge_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"),
                 "set_painterly_edge_threshold", "get_painterly_edge_threshold");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "painterly/edge_intensity", PROPERTY_HINT_RANGE, "0,8,0.1"),
                 "set_painterly_edge_intensity", "get_painterly_edge_intensity");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "painterly/stroke_length", PROPERTY_HINT_RANGE, "1,128,1"),
                 "set_painterly_stroke_length", "get_painterly_stroke_length");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "painterly/stroke_opacity", PROPERTY_HINT_RANGE, "0,1,0.01"),
                 "set_painterly_stroke_opacity", "get_painterly_stroke_opacity");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "painterly/gamma", PROPERTY_HINT_RANGE, "0.5,4.0,0.05"),
                 "set_painterly_gamma", "get_painterly_gamma");

    // Debug overlays from PR #145
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_tile_grid"), "set_debug_show_tile_grid", "is_debug_show_tile_grid");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_density_heatmap"), "set_debug_show_density_heatmap", "is_debug_show_density_heatmap");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_performance_hud"), "set_debug_show_performance_hud", "is_debug_show_performance_hud");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_residency_hud"), "set_debug_show_residency_hud", "is_debug_show_residency_hud");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_tile_bounds"), "set_debug_show_tile_bounds", "get_debug_show_tile_bounds");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_splat_coverage"), "set_debug_show_splat_coverage", "get_debug_show_splat_coverage");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_overflow_tiles"), "set_debug_show_overflow_tiles", "get_debug_show_overflow_tiles");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_projection_issues"), "set_debug_show_projection_issues", "get_debug_show_projection_issues");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_white_albedo"), "set_debug_show_white_albedo", "get_debug_show_white_albedo");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_hlod_level_tint"), "set_debug_show_hlod_level_tint", "get_debug_show_hlod_level_tint");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_shadow_opacity"), "set_debug_show_shadow_opacity", "get_debug_show_shadow_opacity");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_resolve_input"), "set_debug_show_resolve_input", "get_debug_show_resolve_input");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_resolve_output"), "set_debug_show_resolve_output", "get_debug_show_resolve_output");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_device_boundaries"), "set_debug_show_device_boundaries", "is_debug_show_device_boundaries");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/show_texture_states"), "set_debug_show_texture_states", "is_debug_show_texture_states");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug/compute_raster_policy", PROPERTY_HINT_ENUM, "Default,ForceOn,ForceOff"),
			"set_debug_compute_raster_policy", "get_debug_compute_raster_policy");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/dump_gpu_counters"), "set_debug_dump_gpu_counters", "get_debug_dump_gpu_counters");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/enable_pipeline_trace"), "set_debug_pipeline_trace_enabled", "get_debug_pipeline_trace_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug/enable_splat_audit"), "set_debug_splat_audit_enabled", "get_debug_splat_audit_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug/splat_audit_sample_count", PROPERTY_HINT_RANGE, "1,64,1"),
			"set_debug_splat_audit_sample_count", "get_debug_splat_audit_sample_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug/overlay_opacity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_debug_overlay_opacity", "get_debug_overlay_opacity");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "render/solid_coverage_enabled"), "set_solid_coverage_enabled", "is_solid_coverage_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "render/solid_coverage_alpha_floor", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_solid_coverage_alpha_floor", "get_solid_coverage_alpha_floor");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "debug/preview_mode", PROPERTY_HINT_ENUM, "Off,Wireframe,Points,Depth,Heatmap,Runtime Modifications"), "set_debug_preview_mode", "get_debug_preview_mode");

    // Enums
    BIND_ENUM_CONSTANT(MODE_3D);
    BIND_ENUM_CONSTANT(MODE_2D);
    BIND_ENUM_CONSTANT(MODE_HYBRID);

    BIND_ENUM_CONSTANT(STATE_NORMAL);
    BIND_ENUM_CONSTANT(STATE_HOVERED);
    BIND_ENUM_CONSTANT(STATE_SELECTED);
    BIND_ENUM_CONSTANT(STATE_DISABLED);

    BIND_ENUM_CONSTANT(DEBUG_PREVIEW_OFF);
    BIND_ENUM_CONSTANT(DEBUG_PREVIEW_WIREFRAME);
    BIND_ENUM_CONSTANT(DEBUG_PREVIEW_POINTS);
    BIND_ENUM_CONSTANT(DEBUG_PREVIEW_DEPTH);
    BIND_ENUM_CONSTANT(DEBUG_PREVIEW_HEATMAP);
    BIND_ENUM_CONSTANT(DEBUG_PREVIEW_RUNTIME_MODIFICATIONS);

#ifdef TESTS_ENABLED
    ClassDB::bind_method(D_METHOD("test_set_hlod_publication_failure", "failure"), &GaussianSplatRenderer::test_set_hlod_publication_failure);
    ClassDB::bind_method(D_METHOD("test_set_hlod_route_limit_override", "bytes"), &GaussianSplatRenderer::test_set_hlod_route_limit_override);
    ClassDB::bind_method(D_METHOD("test_set_hlod_hold_world_idle", "hold"), &GaussianSplatRenderer::test_set_hlod_hold_world_idle);
    ClassDB::bind_method(D_METHOD("test_observe_hlod_publication", "request_id"), &GaussianSplatRenderer::test_observe_hlod_publication, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("test_hlod_foreign_device_publication"), &GaussianSplatRenderer::test_hlod_foreign_device_publication);
    ClassDB::bind_method(D_METHOD("test_set_hlod_grading", "grading"), &GaussianSplatRenderer::set_color_grading);
    ClassDB::bind_method(D_METHOD("test_bake_hlod_small_leaf_fixture", "data"), &GaussianSplatRenderer::test_bake_hlod_small_leaf_fixture);
    // #104: expose the render-thread dispatch characterization hooks to GDScript so the
    // executable harness (tests/ci/test_render_thread_dispatch.gd) can drive them under a
    // LIVE RenderingServer + render loop, which the C++ [RequiresGPU] doctests cannot reach
    // (doctest's --test boot never creates a RenderingServer, so those cases only skip). These
    // bindings exist only in TESTS_ENABLED builds and never ship in a release template.
    ClassDB::bind_method(D_METHOD("test_set_render_thread_dispatch_timeout_usec", "timeout_usec"), &GaussianSplatRenderer::test_set_render_thread_dispatch_timeout_usec);
    ClassDB::bind_method(D_METHOD("test_get_render_thread_dispatch_timeout_usec"), &GaussianSplatRenderer::test_get_render_thread_dispatch_timeout_usec);
    ClassDB::bind_method(D_METHOD("test_dispatch_call_on_render_thread_blocking_without_completion"), &GaussianSplatRenderer::test_dispatch_call_on_render_thread_blocking_without_completion);
    ClassDB::bind_method(D_METHOD("test_dispatch_call_on_render_thread_blocking_with_completion"), &GaussianSplatRenderer::test_dispatch_call_on_render_thread_blocking_with_completion);
    ClassDB::bind_method(D_METHOD("test_is_render_thread_dispatch_path_active"), &GaussianSplatRenderer::test_is_render_thread_dispatch_path_active);
    ClassDB::bind_method(D_METHOD("test_get_render_thread_dispatch_completed_request_id"), &GaussianSplatRenderer::test_get_render_thread_dispatch_completed_request_id);
    ClassDB::bind_method(D_METHOD("test_notify_render_thread_dispatch_completed", "request_id"), &GaussianSplatRenderer::test_notify_render_thread_dispatch_completed);
#endif // TESTS_ENABLED
}
