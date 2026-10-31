#include "render_frame_context_manager.h"

#include "core/object/object.h"
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
	clear_hlod_views();
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

bool RenderFrameContextManager::hlod_identity_from_token(const GaussianHlodViewLifetime *p_token, ObjectID p_buffers, RID p_target, HlodViewIdentity &r_identity) {
	r_identity = HlodViewIdentity();
	if (!p_token || !p_buffers.is_valid() || !p_target.is_valid() || p_token->get_epoch() == 0) { return false; }
	r_identity.buffers = p_buffers;
	r_identity.target = p_target;
	r_identity.epoch = p_token->get_epoch();
	return true;
}

bool RenderFrameContextManager::acquire_hlod_identity(RenderSceneBuffersRD *p_buffers, HlodViewIdentity &r_identity) {
	r_identity = HlodViewIdentity();
	if (!p_buffers || !p_buffers->get_render_target().is_valid()) {
		return false;
	}
	const StringName token_name = SNAME("gaussian_splat_hlod_view_lifetime");
	Ref<RenderBufferCustomDataRD> existing = p_buffers->has_custom_data(token_name) ? p_buffers->get_custom_data(token_name) : Ref<RenderBufferCustomDataRD>();
	Ref<GaussianHlodViewLifetime> token = existing;
	if (existing.is_valid() && token.is_null()) {
		return false; // Never replace another custom-data owner.
	}
	if (token.is_null()) {
		token.instantiate();
		token->configure(p_buffers); // set_custom_data does not call configure.
		p_buffers->set_custom_data(token_name, token);
	}
	return hlod_identity_from_token(token.ptr(), p_buffers->get_instance_id(), p_buffers->get_render_target(), r_identity);
}

bool RenderFrameContextManager::default_hlod_liveness(const HlodViewIdentity &p_identity) {
	RenderSceneBuffersRD *buffers = Object::cast_to<RenderSceneBuffersRD>(ObjectDB::get_instance(p_identity.buffers));
	if (!buffers || buffers->get_render_target() != p_identity.target) { return false; }
	const StringName token_name = SNAME("gaussian_splat_hlod_view_lifetime");
	if (!buffers->has_custom_data(token_name)) { return false; }
	Ref<GaussianHlodViewLifetime> token = buffers->get_custom_data(token_name);
	return token.is_valid() && token->get_epoch() == p_identity.epoch;
}

uint32_t RenderFrameContextManager::evict_stale_hlod_bindings() {
	uint32_t evicted = 0;
	for (HlodBinding &binding : hlod_bindings) {
		if (binding.used && !hlod_liveness(binding.identity)) {
			drop_hlod_binding(binding);
			++evicted;
		}
	}
	return evicted;
}

void RenderFrameContextManager::release_hlod_snapshot_data() {
	for (int i = 0; i < hlod_snapshot.assets.size(); ++i) { hlod_snapshot.assets.write[i].data = Ref<GaussianSplatHlodRuntimeData>(); }
}

uint64_t RenderFrameContextManager::hlod_view_key(const HlodViewIdentity &p_identity) {
	uint64_t h = uint64_t(p_identity.buffers);
	h ^= p_identity.epoch + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

HlodFallback RenderFrameContextManager::hlod_fallback_for_admission(gs_hlod_runtime::ViewSet::AdmitStatus p_status) {
	using ViewSet = gs_hlod_runtime::ViewSet;
	HlodFallback out;
	switch (p_status) {
		case ViewSet::ADMITTED:
			return out;
		case ViewSet::REFUSED_BUDGET:
			out.reason = "hlod_view_budget_refused";
			out.budget_bound_frames = 1;
			break;
		case ViewSet::REFUSED_VIEW_LIMIT:
			out.reason = "hlod_view_limit_refused";
			break;
		case ViewSet::REFUSED_CAPS:
			out.reason = "hlod_view_caps_refused";
			out.budget_bound_frames = 1;
			break;
		case ViewSet::REFUSED_RESOURCES:
			out.reason = "hlod_view_resources_refused";
			break;
		case ViewSet::REFUSED_DUPLICATE:
			out.reason = "hlod_view_key_conflict";
			break;
		case ViewSet::REFUSED_INVALID:
		default:
			out.reason = "hlod_view_admission_invalid";
			break;
	}
	return out;
}

HlodFallback RenderFrameContextManager::hlod_fallback_for_snapshot(gs_hlod_runtime::ViewSet::SnapshotStatus p_status) {
	using ViewSet = gs_hlod_runtime::ViewSet;
	HlodFallback out;
	switch (p_status) {
		case ViewSet::SNAPSHOT_OK:
			return out;
		case ViewSet::SNAPSHOT_OVER_BUDGET:
			out.reason = "hlod_snapshot_over_budget";
			out.budget_bound_frames = 1;
			break;
		case ViewSet::SNAPSHOT_INVALID:
		default:
			out.reason = "hlod_snapshot_invalid";
			break;
	}
	return out;
}

// Every readiness other than READY maps to the canonical non-HLOD route; the previous per-status reason
// strings and counters are kept where the underlying cut status is still known. READY with zero records
// is the selector's EMPTY status (everything culled): a complete, legitimately empty cut.
HlodFallback RenderFrameContextManager::hlod_fallback_for_view(const gs_hlod_runtime::ViewSet::ViewResult &p_result) {
	using ViewSet = gs_hlod_runtime::ViewSet;
	using Cut = gs_hlod_runtime::CutHistory;
	HlodFallback out;
	switch (p_result.readiness) {
		case ViewSet::READY:
			return out;
		case ViewSet::LOADING:
			if (p_result.worst_status == Cut::BOOTSTRAP_MISSING_RESIDENCY) {
				out.reason = "hlod_bootstrap_missing_residency";
				out.budget_bound_frames = 1;
			} else {
				out.reason = "cut_unavailable";
			}
			break;
		case ViewSet::NOT_ADMITTED:
			if (p_result.reservation_exceeded) {
				out.reason = "hlod_view_reservation_exceeded";
				out.budget_bound_frames = 1;
			} else if (p_result.worst_status == Cut::BOOTSTRAP_INFEASIBLE) {
				out.reason = "hlod_bootstrap_infeasible";
				out.budget_bound_frames = 1;
				out.root_dropped_frames = 1; // Legacy counter name: no complete frontier fits the caps.
			} else if (p_result.worst_status == Cut::BOOTSTRAP_EXHAUSTED) {
				out.reason = "hlod_bootstrap_search_exhausted";
				out.budget_bound_frames = 1;
			} else {
				out.reason = "hlod_view_not_admitted";
				out.budget_bound_frames = 1;
			}
			break;
		case ViewSet::FAILED:
		default:
			out.reason = p_result.worst_status == Cut::BOOTSTRAP_ARITHMETIC ? "hlod_bootstrap_arithmetic_invalid" : "hlod_view_failed";
			break;
	}
	return out;
}

void RenderFrameContextManager::drop_hlod_binding(HlodBinding &r_binding) {
	if (r_binding.used) {
		hlod_view_set.remove_view(hlod_view_key(r_binding.identity));
	}
	r_binding = HlodBinding();
}

uint32_t RenderFrameContextManager::hlod_bound_view_count() const {
	uint32_t count = 0;
	for (const HlodBinding &binding : hlod_bindings) { count += binding.used ? 1u : 0u; }
	return count;
}

bool RenderFrameContextManager::is_hlod_view_live(uint64_t p_view_key) const {
	for (const HlodBinding &binding : hlod_bindings) {
		if (binding.used && hlod_view_key(binding.identity) == p_view_key) { return hlod_liveness(binding.identity); }
	}
	return false;
}

HlodViewOutcome RenderFrameContextManager::select_hlod_view(const HlodViewIdentity &p_identity, uint64_t p_source,
		gs_hlod_runtime::SelectionSnapshot &p_snapshot, gs_hlod_runtime::ViewSpec p_spec) {
	using ViewSet = gs_hlod_runtime::ViewSet;
	HlodViewOutcome out;
	if (!p_identity.is_valid()) {
		out.fallback.reason = "complete_view_history_unavailable";
		return out;
	}
	p_spec.view_key = hlod_view_key(p_identity);
	p_snapshot.views.resize(1);
	p_snapshot.views.write[0] = p_spec;
	p_snapshot.budget.max_views = HLOD_MAX_VIEWS;

	// A buffers object that kept its id but changed epoch (free_data/configure), render target or snapshot
	// owns a different view: drop the old one, so the view is re-admitted exactly where the old slot reset.
	HlodBinding *binding = nullptr;
	for (HlodBinding &candidate : hlod_bindings) {
		if (!candidate.used || candidate.identity.buffers != p_identity.buffers) { continue; }
		if (candidate.identity.epoch == p_identity.epoch && candidate.identity.target == p_identity.target &&
				candidate.source == p_source) {
			binding = &candidate;
		} else {
			drop_hlod_binding(candidate);
		}
	}
	// Admits the view into a free binding, evicting the least recently used view when all are taken.
	auto admit = [&]() -> bool {
		// Views of freed buffers or of a stale epoch must not hold a slot or budget against a live one.
		evict_stale_hlod_bindings();
		HlodBinding *slot = &hlod_bindings[0];
		for (HlodBinding &candidate : hlod_bindings) {
			if (!candidate.used) { slot = &candidate; break; }
			if (candidate.last_used < slot->last_used) { slot = &candidate; }
		}
		drop_hlod_binding(*slot);
		const ViewSet::AdmitResult admitted = hlod_view_set.admit_view(p_snapshot, p_spec);
		out.admit = admitted.status;
		if (admitted.status != ViewSet::ADMITTED) {
			out.fallback = hlod_fallback_for_admission(admitted.status);
			return false;
		}
		slot->used = true;
		slot->identity = p_identity;
		slot->source = p_source;
		binding = slot;
		out.admitted_now = true;
		return true;
	};
	if (!binding) {
		if (!admit()) { return out; }
	} else {
		out.admit = ViewSet::ADMITTED;
	}
	for (int attempt = 0; attempt < 2; ++attempt) {
		binding->last_used = ++hlod_view_clock;
		const ViewSet::SnapshotStatus snapshot_status = hlod_view_set.select(p_snapshot);
		uint32_t count = 0;
		const ViewSet::ViewResult *results = hlod_view_set.results(count);
		bool found = false;
		for (uint32_t i = 0; i < count; ++i) {
			if (results[i].view_key == p_spec.view_key) {
				out.view = results[i];
				found = true;
			}
		}
		if (snapshot_status == ViewSet::SNAPSHOT_INVALID || !found) {
			out.fallback = hlod_fallback_for_snapshot(ViewSet::SNAPSHOT_INVALID);
			return out;
		}
		if (out.view.reservation_exceeded && attempt == 0) {
			// The snapshot or caps outgrew the reservation: re-admit against the current snapshot, once.
			drop_hlod_binding(*binding);
			binding = nullptr;
			if (!admit()) { return out; }
			continue;
		}
		out.fallback = hlod_fallback_for_view(out.view);
		if (out.fallback.reason == nullptr && snapshot_status != ViewSet::SNAPSHOT_OK) {
			out.fallback = hlod_fallback_for_snapshot(snapshot_status);
		}
		if (out.active()) {
			out.records = hlod_view_set.drawn(p_spec.view_key, out.record_count);
		}
		return out;
	}
	return out;
}

void RenderFrameContextManager::clear_hlod_views() {
	for (HlodBinding &binding : hlod_bindings) { drop_hlod_binding(binding); }
	hlod_snapshot = gs_hlod_runtime::SelectionSnapshot();
	hlod_view_clock = 0;
}
