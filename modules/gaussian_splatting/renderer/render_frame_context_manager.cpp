#include "render_frame_context_manager.h"

#include "core/os/os.h"

#include <atomic>

RenderFrameContextManager::RenderFrameContextManager() {
	reset();
}

void RenderFrameContextManager::reset_frame_state() {
	frame_state.visible_splat_count.store(0, std::memory_order_release);
	frame_state.frame_counter = 0;
	frame_state.sort_time_ms = 0.0f;
	frame_state.render_time_ms = 0.0f;
	frame_state.animation_time_seconds = 0.0;
}

double RenderFrameContextManager::sample_render_animation_time_seconds() {
	// Lazy epoch: first call latches "now" as zero, later calls report
	// elapsed wall-clock seconds. std::atomic gives us thread-safe
	// initialization without a mutex; callers from the render thread and
	// any helper threads agree on the same epoch.
	static std::atomic<uint64_t> epoch_usec{0};
	uint64_t now_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	uint64_t expected = 0;
	if (epoch_usec.compare_exchange_strong(expected, now_usec, std::memory_order_acq_rel)) {
		return 0.0;
	}
	const uint64_t epoch = epoch_usec.load(std::memory_order_acquire);
	if (now_usec < epoch) {
		// Clock went backwards (can happen under manipulated or virtualized
		// clocks). Treat as "no time elapsed" rather than negative time so
		// shader phases don't regress.
		return 0.0;
	}
	return double(now_usec - epoch) * 1e-6;
}

void RenderFrameContextManager::reset_view_state_defaults() {
	view_state.last_camera_to_world_transform = Transform3D();
	view_state.last_camera_projection.set_perspective(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
	view_state.last_camera_position = Vector3(0, 0, 5);
	view_state.manual_viewport_override = Size2i();
	view_state.manual_viewport_format_override = RD::DATA_FORMAT_MAX;
	view_state.active_viewport_color_format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	view_state.using_scene_data = false;
}

void RenderFrameContextManager::reset() {
    clear_hlod_histories();
	reset_frame_state();
	reset_view_state_defaults();
}

namespace {
std::atomic<uint64_t> hlod_lifecycle_epoch{ 1 };
}

void GaussianHlodViewLifetime::configure(RenderSceneBuffersRD *p_buffers) {
    epoch = p_buffers ? hlod_lifecycle_epoch.fetch_add(1, std::memory_order_relaxed) : 0;
}

void GaussianHlodViewLifetime::free_data() {
    epoch = hlod_lifecycle_epoch.fetch_add(1, std::memory_order_relaxed);
}

gs_hlod_runtime::CutHistory *RenderFrameContextManager::acquire_hlod_history(RenderSceneBuffersRD *p_buffers,
        uint64_t p_source, uint64_t p_policy, uint32_t p_node_count, uint64_t &r_epoch) {
    r_epoch = 0;
    if (!p_buffers || !p_buffers->get_render_target().is_valid()) {
        return nullptr;
    }
    const StringName token_name = SNAME("gaussian_splat_hlod_view_lifetime");
    Ref<RenderBufferCustomDataRD> existing = p_buffers->has_custom_data(token_name) ? p_buffers->get_custom_data(token_name) : Ref<RenderBufferCustomDataRD>();
    Ref<GaussianHlodViewLifetime> token = existing;
    if (existing.is_valid() && token.is_null()) {
        return nullptr; // Never replace another custom-data owner.
    }
    if (token.is_null()) {
        token.instantiate();
        token->configure(p_buffers); // set_custom_data does not call configure.
        p_buffers->set_custom_data(token_name, token);
    }
    r_epoch = token->get_epoch();
    if (r_epoch == 0) { return nullptr; }
    HlodViewSlot *slot = nullptr;
    for (HlodViewSlot &candidate : hlod_views) {
        if (candidate.buffers == p_buffers->get_instance_id() && candidate.epoch == r_epoch &&
                candidate.target == p_buffers->get_render_target()) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        slot = &hlod_views[0];
        for (HlodViewSlot &candidate : hlod_views) {
            if (candidate.last_used < slot->last_used) { slot = &candidate; }
        }
        if (!slot->history.reserve(p_node_count)) { return nullptr; }
        slot->buffers = p_buffers->get_instance_id();
        slot->target = p_buffers->get_render_target();
        slot->epoch = r_epoch;
        slot->source = p_source;
        slot->policy = p_policy;
    } else if (slot->source != p_source || slot->policy != p_policy) {
        if (!slot->history.reserve(p_node_count)) { return nullptr; }
        slot->source = p_source;
        slot->policy = p_policy;
    }
    slot->last_used = ++hlod_view_clock;
    return &slot->history;
}

uint64_t RenderFrameContextManager::hlod_history_bytes() const {
    uint64_t bytes = 0;
    for (const HlodViewSlot &slot : hlod_views) { bytes += slot.history.allocated_bytes(); }
    return bytes;
}

void RenderFrameContextManager::clear_hlod_histories() {
    for (HlodViewSlot &slot : hlod_views) { slot = HlodViewSlot(); }
    hlod_view_clock = 0;
}
