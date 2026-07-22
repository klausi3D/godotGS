#include "gaussian_splat_scene_director.h"

#include "gs_project_settings.h"
#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "../logger/gs_logger.h"
#include "../logger/gs_debug_trace.h"
#include "gaussian_splat_manager.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "../resources/color_grading_resource.h"
#include "scene/3d/node_3d.h"
#include "scene/main/node.h"

#include <cstring>
#include <utility>

static bool _is_scene_director_log_enabled() {
	// Canonical gate: all_debug || frame || data, honoring GS_SILENCE_LOGS.
	return gs::settings::is_any_debug_log_enabled();
}

static void _bump_instance_generation(uint64_t &r_generation) {
	r_generation++;
	if (r_generation == 0) {
		r_generation = 1;
	}
}

static void _bump_instance_asset_generation(uint64_t &r_generation) {
	r_generation++;
	if (r_generation == 0) {
		r_generation = 1;
	}
}

static bool _dict_get_bool(const Dictionary &p_dict, const StringName &p_key, bool p_default) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	const Variant value = p_dict[p_key];
	if (value.get_type() == Variant::BOOL) {
		return (bool)value;
	}
	if (value.get_type() == Variant::INT) {
		return int64_t(value) != 0;
	}
	return p_default;
}

static int _dict_get_int(const Dictionary &p_dict, const StringName &p_key, int p_default) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	const Variant value = p_dict[p_key];
	if (value.get_type() == Variant::FLOAT) {
		return int((double)value);
	}
	return int(value);
}

static float _dict_get_float(const Dictionary &p_dict, const StringName &p_key, float p_default) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	const Variant value = p_dict[p_key];
	if (value.get_type() == Variant::INT) {
		return (float)int64_t(value);
	}
	return (float)(double)value;
}

static String _dict_get_string(const Dictionary &p_dict, const StringName &p_key, const String &p_default = String()) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	return String(p_dict[p_key]);
}

static Dictionary _dict_get_dictionary(const Dictionary &p_dict, const StringName &p_key) {
	if (!p_dict.has(p_key)) {
		return Dictionary();
	}
	const Variant value = p_dict[p_key];
	return value.get_type() == Variant::DICTIONARY ? Dictionary(value) : Dictionary();
}

static const StringName &WORLD_OVERRIDE_LOD_ENABLED() { static const StringName s("lod_enabled"); return s; }
static const StringName &WORLD_OVERRIDE_LOD_BIAS() { static const StringName s("lod_bias"); return s; }
static const StringName &WORLD_OVERRIDE_LOD_MAX_DISTANCE() { static const StringName s("lod_max_distance"); return s; }
static const StringName &WORLD_OVERRIDE_MAX_SPLATS() { static const StringName s("max_splats"); return s; }
static const StringName &WORLD_OVERRIDE_FRUSTUM_CULLING() { static const StringName s("frustum_culling"); return s; }
static const StringName &WORLD_OVERRIDE_ASYNC_UPLOAD_ENABLED() { static const StringName s("async_upload_enabled"); return s; }
static const StringName &WORLD_OVERRIDE_OPACITY_MULTIPLIER() { static const StringName s("opacity_multiplier"); return s; }
static const StringName &WORLD_OVERRIDE_STREAMING() { static const StringName s("streaming"); return s; }
static const StringName &WORLD_STREAMING_OVERRIDE_PREFETCH() { static const StringName s("override_prefetch"); return s; }
static const StringName &WORLD_STREAMING_PREDICTIVE_PREFETCH_ENABLED() { static const StringName s("predictive_prefetch_enabled"); return s; }
static const StringName &WORLD_STREAMING_PREFETCH_LOOKAHEAD_DISTANCE() { static const StringName s("prefetch_lookahead_distance"); return s; }
static const StringName &WORLD_STREAMING_OVERRIDE_VRAM_BUDGET() { static const StringName s("override_vram_budget"); return s; }
static const StringName &WORLD_STREAMING_VRAM_BUDGET_MB() { static const StringName s("vram_budget_mb"); return s; }
static const StringName &WORLD_STREAMING_VRAM_MIN_CHUNKS() { static const StringName s("vram_min_chunks"); return s; }
static const StringName &WORLD_STREAMING_VRAM_MAX_CHUNKS() { static const StringName s("vram_max_chunks"); return s; }
static const StringName &WORLD_STREAMING_OVERRIDE_IO_SOURCE() { static const StringName s("override_io_source"); return s; }
static const StringName &EFFECTOR_ENABLED_PROPERTY() { static const StringName s("enabled"); return s; }
static const StringName &EFFECTOR_RADIUS_PROPERTY() { static const StringName s("radius"); return s; }
static const StringName &EFFECTOR_STRENGTH_PROPERTY() { static const StringName s("strength"); return s; }
static const StringName &EFFECTOR_FALLOFF_PROPERTY() { static const StringName s("falloff"); return s; }
static const StringName &EFFECTOR_FREQUENCY_PROPERTY() { static const StringName s("frequency"); return s; }
static const StringName &EFFECTOR_AFFECT_POSITION_PROPERTY() { static const StringName s("affect_position"); return s; }
static const StringName &EFFECTOR_AFFECT_OPACITY_PROPERTY() { static const StringName s("affect_opacity"); return s; }
static const StringName &EFFECTOR_OPACITY_STRENGTH_PROPERTY() { static const StringName s("opacity_strength"); return s; }
static const StringName &EFFECTOR_LAYER_MASK_PROPERTY() { static const StringName s("layer_mask"); return s; }
static const StringName &EFFECTOR_SCOPE_MODE_PROPERTY() { static const StringName s("scope_mode"); return s; }
static const StringName &EFFECTOR_SCOPE_ROOT_PROPERTY() { static const StringName s("scope_root"); return s; }
static const StringName &EFFECTOR_PRIORITY_PROPERTY() { static const StringName s("priority"); return s; }

static float _sanitize_finite_float(float p_value, float p_default, const String &p_context, const char *p_field) {
	if (Math::is_finite(p_value)) {
		return p_value;
	}
	WARN_PRINT(vformat("[GaussianSplatSceneDirector] Non-finite %s for %s; using %.3f.", p_field, p_context, p_default));
	return p_default;
}

static float _sanitize_non_negative_float(float p_value, float p_default, const String &p_context, const char *p_field) {
	const float value = _sanitize_finite_float(p_value, p_default, p_context, p_field);
	if (value < 0.0f) {
		WARN_PRINT(vformat("[GaussianSplatSceneDirector] Negative %s for %s; clamping to 0.", p_field, p_context));
		return 0.0f;
	}
	return value;
}

static float _sanitize_min_float(float p_value, float p_default, float p_min, const String &p_context, const char *p_field) {
	const float value = _sanitize_finite_float(p_value, p_default, p_context, p_field);
	if (value < p_min) {
		WARN_PRINT(vformat("[GaussianSplatSceneDirector] %s for %s below %.3f; clamping.", p_field, p_context, p_min));
		return p_min;
	}
	return value;
}

static float _encode_u32_as_float_bits(uint32_t p_value) {
	float encoded = 0.0f;
	static_assert(sizeof(encoded) == sizeof(p_value), "Expected float/u32 bit widths to match");
	memcpy(&encoded, &p_value, sizeof(encoded));
	return encoded;
}

GaussianSplatRenderer::WorldSubmissionContract GaussianSplatSceneDirector::SubmissionStore::build_contract(
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state,
		const WorldSubmissionRecord &p_record) {
	GaussianSplatRenderer::WorldSubmissionContract contract;
	contract.gaussian_data = p_record.gaussian_data;
	contract.payload_source = p_record.payload_source;
	contract.static_chunks = p_record.static_chunks;
	contract.debug_label = _dict_get_string(p_record.metadata, StringName("world_path"));
	contract.has_desired_residency_hint = p_record.has_desired_residency_hint;
	contract.desired_residency_hint = p_record.desired_residency_hint;

	const Dictionary &overrides = p_record.desired_renderer_overrides;
	contract.lod_enabled = _dict_get_bool(overrides, WORLD_OVERRIDE_LOD_ENABLED(), p_renderer_state.lod_enabled);
	contract.lod_bias = _dict_get_float(overrides, WORLD_OVERRIDE_LOD_BIAS(), p_renderer_state.lod_bias);
	contract.lod_max_distance = _dict_get_float(overrides, WORLD_OVERRIDE_LOD_MAX_DISTANCE(), p_renderer_state.lod_max_distance);
	contract.frustum_culling = _dict_get_bool(overrides, WORLD_OVERRIDE_FRUSTUM_CULLING(), p_renderer_state.frustum_culling);
	contract.async_upload_enabled = _dict_get_bool(overrides, WORLD_OVERRIDE_ASYNC_UPLOAD_ENABLED(), p_renderer_state.async_upload_enabled);
	contract.opacity_multiplier = _dict_get_float(overrides, WORLD_OVERRIDE_OPACITY_MULTIPLIER(), p_renderer_state.opacity_multiplier);
	contract.streaming_overrides = p_renderer_state.streaming_overrides;

	if (overrides.has(WORLD_OVERRIDE_STREAMING())) {
		const Dictionary streaming_dict = _dict_get_dictionary(overrides, WORLD_OVERRIDE_STREAMING());
		contract.streaming_overrides.override_prefetch =
				_dict_get_bool(streaming_dict, WORLD_STREAMING_OVERRIDE_PREFETCH(), contract.streaming_overrides.override_prefetch);
		contract.streaming_overrides.predictive_prefetch_enabled =
				_dict_get_bool(streaming_dict, WORLD_STREAMING_PREDICTIVE_PREFETCH_ENABLED(),
						contract.streaming_overrides.predictive_prefetch_enabled);
		contract.streaming_overrides.prefetch_lookahead_distance =
				_dict_get_float(streaming_dict, WORLD_STREAMING_PREFETCH_LOOKAHEAD_DISTANCE(),
						contract.streaming_overrides.prefetch_lookahead_distance);
		contract.streaming_overrides.override_vram_budget =
				_dict_get_bool(streaming_dict, WORLD_STREAMING_OVERRIDE_VRAM_BUDGET(),
						contract.streaming_overrides.override_vram_budget);
		contract.streaming_overrides.vram_budget_config.budget_mb =
				MAX(0, _dict_get_int(streaming_dict, WORLD_STREAMING_VRAM_BUDGET_MB(),
						int(contract.streaming_overrides.vram_budget_config.budget_mb)));
		contract.streaming_overrides.vram_budget_config.min_chunks =
				MAX(0, _dict_get_int(streaming_dict, WORLD_STREAMING_VRAM_MIN_CHUNKS(),
						int(contract.streaming_overrides.vram_budget_config.min_chunks)));
		contract.streaming_overrides.vram_budget_config.max_chunks =
				MAX(0, _dict_get_int(streaming_dict, WORLD_STREAMING_VRAM_MAX_CHUNKS(),
						int(contract.streaming_overrides.vram_budget_config.max_chunks)));
		contract.streaming_overrides.override_io_source =
				_dict_get_bool(streaming_dict, WORLD_STREAMING_OVERRIDE_IO_SOURCE(),
						contract.streaming_overrides.override_io_source);
		if (contract.streaming_overrides.override_vram_budget) {
			contract.streaming_overrides.vram_budget_config.min_chunks =
					MIN(contract.streaming_overrides.vram_budget_config.min_chunks,
							contract.streaming_overrides.vram_budget_config.max_chunks);
		}
	}

	const uint32_t data_count = p_record.gaussian_data.is_valid() ? p_record.gaussian_data->get_count() : 0;
	const int baseline_max_splats = MAX(1, p_renderer_state.max_splats);
	const int requested_max_splats = _dict_get_int(overrides, WORLD_OVERRIDE_MAX_SPLATS(), baseline_max_splats);
	int effective_max_splats = requested_max_splats;
	if (effective_max_splats <= 0) {
		effective_max_splats = data_count > 0 ? int(data_count) : baseline_max_splats;
	}
	if (data_count > 0) {
		effective_max_splats = MIN(effective_max_splats, int(data_count));
	}
	contract.max_splats = MAX(1, effective_max_splats);
	return contract;
}

GaussianSplatSceneDirector *GaussianSplatSceneDirector::singleton = nullptr;

// #611: see the declaration in the header for what a non-zero value means.
SafeNumeric<uint64_t> GaussianSplatSceneDirector::renderer_contract_lock_violations{ 0 };

uint64_t GaussianSplatSceneDirector::get_renderer_contract_lock_violation_count() {
	return renderer_contract_lock_violations.get();
}

void GaussianSplatSceneDirector::reset_renderer_contract_lock_violation_count() {
	renderer_contract_lock_violations.set(0);
}

void GaussianSplatSceneDirector::_report_renderer_contract_lock_violation(const char *p_site) const {
	if (!GaussianSplatting::report_lock_held_at_boundary(world_mutex, renderer_contract_lock_violations)) {
		return;
	}
	// Reported, not aborted: the one remaining known violation is
	// submit_world_submission's apply, whose result gates a commit/reject
	// decision that a fail-fast return would silently turn into a rejection on
	// every live-render-thread submission. See #611 (PR B).
	ERR_PRINT_ONCE(vformat(
			"[GaussianSplatSceneDirector] #611: %s entered while this thread holds world_mutex. "
			"The blocking render-thread dispatch it performs can stall for the full dispatch "
			"timeout because the render thread may be waiting for world_mutex. Queue the work "
			"on DeferredRendererWork instead.",
			String(p_site)));
}

void GaussianSplatSceneDirector::DeferredRendererWork::queue_apply(const Ref<GaussianSplatRenderer> &p_renderer,
		const GaussianSplatRenderer::WorldSubmissionContract &p_contract) {
	if (p_renderer.is_null()) {
		return;
	}
	Entry entry;
	entry.renderer = p_renderer;
	entry.contract = p_contract;
	entry.kind = Kind::APPLY;
	entries.push_back(entry);
}

void GaussianSplatSceneDirector::DeferredRendererWork::queue_restore(const Ref<GaussianSplatRenderer> &p_renderer,
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot) {
	if (p_renderer.is_null()) {
		return;
	}
	Entry entry;
	entry.renderer = p_renderer;
	entry.restore_state = p_snapshot;
	entry.kind = Kind::RESTORE;
	entries.push_back(entry);
}

void GaussianSplatSceneDirector::DeferredRendererWork::queue_restore_first(const Ref<GaussianSplatRenderer> &p_renderer,
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot) {
	if (p_renderer.is_null()) {
		return;
	}
	Entry entry;
	entry.renderer = p_renderer;
	entry.restore_state = p_snapshot;
	entry.kind = Kind::RESTORE;
	// Front-insert: see the contract in the header. The rollback has to precede a
	// stale apply queued by _get_or_create_world_for_scenario, or that apply would
	// be undone and the renderer left cleared while the director still holds the
	// previous record. LocalVector has no insert-at-front and the queue is at most
	// a couple of entries deep, so rebuild it -- same shape as queue_initialize.
	LocalVector<Entry> reordered;
	reordered.push_back(entry);
	for (Entry &existing : entries) {
		reordered.push_back(existing);
	}
	entries = std::move(reordered);
}

void GaussianSplatSceneDirector::DeferredRendererWork::queue_initialize(const Ref<GaussianSplatRenderer> &p_renderer) {
	if (p_renderer.is_null()) {
		return;
	}
	Entry entry;
	entry.renderer = p_renderer;
	entry.kind = Kind::INITIALIZE;
	// Front-insert, not push_back: see queue_initialize's contract in the header.
	// The inline call this replaces ran under the lock, i.e. ahead of any apply
	// _get_or_create_world_for_scenario had already queued for the same renderer.
	// LocalVector has no insert-at-front, and the queue is at most a couple of
	// entries deep, so rebuild it.
	LocalVector<Entry> reordered;
	reordered.push_back(entry);
	for (Entry &existing : entries) {
		reordered.push_back(existing);
	}
	entries = std::move(reordered);
}

GaussianSplatSceneDirector::DeferredRendererWork::Kind
GaussianSplatSceneDirector::DeferredRendererWork::get_entry_kind(uint32_t p_index) const {
	ERR_FAIL_COND_V(p_index >= entries.size(), Kind::APPLY);
	return entries[p_index].kind;
}

void GaussianSplatSceneDirector::DeferredRendererWork::cancel() {
	entries.clear();
}

void GaussianSplatSceneDirector::_initialize_world_renderer(SharedWorld &p_world, RendererContractWorkQueue *r_deferred_work) {
	if (p_world.renderer.is_null()) {
		return;
	}
	// Guard first, boundary check second: when the renderer is already up (or an
	// initialization is already queued on the render thread) there is no dispatch
	// to invert, so reporting a violation here would be noise. This mirrors where
	// PR A placed the check in the other two boundary functions.
	const auto &resource_state = p_world.renderer->get_resource_state();
	if (resource_state.gpu_resources_initialized || resource_state.gpu_initialization_pending) {
		return;
	}
	if (r_deferred_work) {
		r_deferred_work->queue_initialize(p_world.renderer);
		return;
	}
	// No queue supplied: keep the historical inline behaviour rather than
	// silently skipping the initialization, and report the inversion so the
	// counter sees this route instead of under-reporting it.
	_report_renderer_contract_lock_violation("GaussianSplatSceneDirector::_initialize_world_renderer");
	p_world.renderer->initialize();
}

void GaussianSplatSceneDirector::DeferredRendererWork::flush() {
	if (entries.is_empty()) {
		return;
	}
	// Move first: an entry's own destructor (dropping the last renderer Ref) must
	// not run against a queue that is still being iterated.
	LocalVector<Entry> pending = std::move(entries);
	entries.clear();
	for (Entry &entry : pending) {
		GaussianSplatRenderer *renderer = entry.renderer.ptr();
		if (!renderer) {
			continue;
		}
		if (entry.kind == Kind::INITIALIZE) {
			// Re-evaluate the guard here rather than trusting the decision made
			// under the lock: the check and the call were atomic inline, and
			// deferring split them apart. Skipping is the correct outcome when
			// something initialized this renderer in the gap -- that is what the
			// guard is for -- so a skip is deliberately NOT counted as dispatched.
			const auto &resource_state = renderer->get_resource_state();
			if (resource_state.gpu_resources_initialized || resource_state.gpu_initialization_pending) {
				continue;
			}
			dispatched_entry_count++;
			renderer->initialize();
			continue;
		}
		dispatched_entry_count++;
		if (entry.kind == Kind::RESTORE) {
			// Mirrors _restore_world_submission_renderer exactly.
			if (!entry.restore_state.valid) {
				renderer->clear_world_submission_contract();
				continue;
			}
			const Error err = renderer->restore_world_submission_runtime_state(entry.restore_state);
			if (err != OK) {
				GS_LOG_RENDERER_ERROR(vformat("[GaussianSplatSceneDirector] Failed to restore world submission renderer state (err=%d).", err));
			}
			continue;
		}
		// Mirrors _apply_world_submission_to_renderer's error path exactly; the
		// callers this queue replaces all discarded the boolean result.
		const Error err = renderer->apply_world_submission_contract(entry.contract);
		if (err != OK) {
			GS_LOG_RENDERER_ERROR(vformat("[GaussianSplatSceneDirector] Failed to apply world submission contract (err=%d).", err));
		}
	}
}

GaussianSplatSceneDirector *GaussianSplatSceneDirector::get_singleton() {
    return singleton;
}

GaussianSplatSceneDirector::GaussianSplatSceneDirector() {
    if (!singleton) {
        singleton = this;
    }
}

GaussianSplatSceneDirector::~GaussianSplatSceneDirector() {
    // Release all SharedWorld entries so their Ref<GaussianSplatRenderer>
    // instances are unreferenced, allowing GPU resources (compute/shader
    // RIDs, buffers) to be freed.  Without this, each F6 runtime cycle
    // leaks an entire renderer's worth of GPU allocations.
    worlds.clear();
    if (singleton == this) {
        singleton = nullptr;
    }
}

void GaussianSplatSceneDirector::_bind_methods() {
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_or_create_world_for_scenario(const RID &p_scenario, bool p_require_renderer,
		RendererContractWorkQueue *r_deferred_work) {
	if (!p_scenario.is_valid()) {
		return nullptr;
	}

	SharedWorld *entry = worlds.getptr(p_scenario);
	if (!entry) {
		SharedWorld world;
		world.scenario = p_scenario;
		worlds.insert(p_scenario, world);
		entry = worlds.getptr(p_scenario);
	}

	if (entry && p_require_renderer && !entry->renderer.is_valid()) {
		GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
		RenderingDevice *device = manager ? manager->get_primary_rendering_device() : nullptr;
		if (!device) {
			static bool warned_missing_device = false;
			if (!warned_missing_device) {
				warned_missing_device = true;
				GS_LOG_RENDERER_ERROR(
						"[GaussianSplatSceneDirector] Unable to acquire primary RenderingDevice for shared renderer (scenario=" +
						String::num_uint64((uint64_t)p_scenario.get_id()) +
						"). Gaussian splat instances in this world will be collected but skipped because no renderer can be attached.");
			}
			return entry;
		}

		entry->renderer = Ref<GaussianSplatRenderer>(memnew(GaussianSplatRenderer(device)));
		if (_is_scene_director_log_enabled()) {
			GS_LOG_RENDERER_DEBUG("[SceneDirector] Created shared renderer (deferred initialization)");
		}
		if (entry->submission_store.is_active()) {
			const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot renderer_state =
					entry->renderer->snapshot_world_submission_runtime_state();
			if (!entry->submission_store.get_record().renderer_restore_state.valid) {
				entry->submission_store.mutable_record().renderer_restore_state = renderer_state;
			}
			// #611: every caller of this function holds world_mutex, and the apply
			// below reaches a blocking render-thread dispatch. Its result was always
			// discarded here, so it can be split: build the contract now (pure
			// bookkeeping, needs the map entry) and dispatch it after the lock is
			// released. The queued entry holds its own renderer Ref, so it survives a
			// prune of this world in the meantime.
			if (r_deferred_work) {
				r_deferred_work->queue_apply(entry->renderer,
						SubmissionStore::build_contract(entry->submission_store.get_record().renderer_restore_state,
								entry->submission_store.get_record()));
			} else {
				// No queue supplied: keep the historical inline behaviour rather than
				// silently skipping the apply. The boundary check inside will report
				// the inversion.
				_apply_world_submission_to_renderer(*entry, entry->submission_store.get_record(),
						entry->submission_store.get_record().renderer_restore_state);
			}
		}
	}

	return entry;
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_or_create_world(World3D *p_world, bool p_require_renderer,
		RendererContractWorkQueue *r_deferred_work) {
	ERR_FAIL_NULL_V(p_world, nullptr);
	return _get_or_create_world_for_scenario(p_world->get_scenario(), p_require_renderer, r_deferred_work);
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_world_for_instance(ObjectID p_node_id,
		RendererContractWorkQueue *r_deferred_work) {
	Object *obj = ObjectDB::get_instance(p_node_id);
	Node3D *node = Object::cast_to<Node3D>(obj);
	if (!node) {
		return nullptr;
	}
	if (!node->is_inside_world()) {
		return nullptr;
	}
	World3D *world = node->get_world_3d().ptr();
	if (!world) {
		return nullptr;
	}
	return _get_or_create_world(world, true, r_deferred_work);
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_instance(ObjectID p_node_id) {
	for (KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.instance_store.has_instance(p_node_id)) {
			return &E.value;
		}
	}
	return nullptr;
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_world_for_effector(ObjectID p_effector_id) {
	Object *obj = ObjectDB::get_instance(p_effector_id);
	Node3D *node = Object::cast_to<Node3D>(obj);
	if (!node || !node->is_inside_world()) {
		return nullptr;
	}
	World3D *world = node->get_world_3d().ptr();
	if (!world) {
		return nullptr;
	}
	return _get_or_create_world(world, false);
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_effector(ObjectID p_effector_id) {
	for (KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.sphere_effector_store.has_effector(p_effector_id)) {
			return &E.value;
		}
	}
	return nullptr;
}

uint32_t GaussianSplatSceneDirector::_build_scene_effector_mask_for_record(const InstanceRecord &p_record,
		const LocalVector<SphereEffectorSelection> &p_payload) {
	if (p_payload.is_empty() || !p_record.scene_effectors_enabled || p_record.scene_effector_layer_mask == 0u) {
		return 0u;
	}
	if (p_record.scene_effector_scope_filter_present && !p_record.scene_effector_scope_filter_valid) {
		return 0u;
	}

	uint32_t mask = 0u;
	for (uint32_t i = 0; i < p_payload.size(); i++) {
		const SphereEffectorSelection &selection = p_payload[i];
		if ((selection.layer_mask & p_record.scene_effector_layer_mask) == 0u) {
			continue;
		}
		if (p_record.scene_effector_scope_filter_present) {
			if (selection.scope_root_id == ObjectID() || selection.scope_root_id != p_record.scene_effector_scope_root_id) {
				continue;
			}
		} else if (selection.scope_mode != SPHERE_EFFECTOR_SCOPE_WORLD) {
			// Implicit subtree containment: the effector carries its resolved
			// scope_root ObjectID (SCOPE_SUBTREE → effector's parent;
			// SCOPE_EXPLICIT_ROOT → the configured root). Check against the
			// cached ancestor chain on the record instead of walking the
			// live tree.
			if (selection.scope_root_id == ObjectID()) {
				continue;
			}
			bool in_scope = false;
			for (const ObjectID &ancestor_id : p_record.scene_tree_ancestor_ids) {
				if (ancestor_id == selection.scope_root_id) {
					in_scope = true;
					break;
				}
			}
			if (!in_scope) {
				continue;
			}
		}
		mask |= (1u << i);
	}
	return mask;
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_renderer(const GaussianSplatRenderer *p_renderer) {
	if (!p_renderer) {
		return nullptr;
	}
	for (KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.renderer.ptr() == p_renderer) {
			return &E.value;
		}
	}
	if (GaussianSplatting::debug_trace_is_enabled()) {
		GaussianSplatting::debug_trace_record_event("world_lookup",
				vformat("renderer=%d not found (worlds=%d)",
						(int64_t)(uintptr_t)p_renderer, (int)worlds.size()),
				true);
	}
	return nullptr;
}

const GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	if (!p_renderer) {
		GaussianSplatting::debug_trace_record_event("world_lookup", "renderer=NULL", true);
		return nullptr;
	}
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.renderer.ptr() == p_renderer) {
			return &E.value;
		}
	}
	if (GaussianSplatting::debug_trace_is_enabled()) {
		GaussianSplatting::debug_trace_record_event("world_lookup",
				vformat("renderer=%d not found (worlds=%d)",
						(int64_t)(uintptr_t)p_renderer, (int)worlds.size()),
				true);
	}
	return nullptr;
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_world_submission(ObjectID p_owner_id) {
	if (p_owner_id == ObjectID()) {
		return nullptr;
	}
	for (KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.submission_store.is_active() && E.value.submission_store.owner_id() == p_owner_id) {
			return &E.value;
		}
	}
	return nullptr;
}

const GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_world_submission(ObjectID p_owner_id) const {
	if (p_owner_id == ObjectID()) {
		return nullptr;
	}
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.submission_store.is_active() && E.value.submission_store.owner_id() == p_owner_id) {
			return &E.value;
		}
	}
	return nullptr;
}

bool GaussianSplatSceneDirector::InstanceStore::_populate_gaussian_data_from_asset(const Ref<GaussianSplatAsset> &p_asset, Ref<GaussianData> &r_data) {
	if (p_asset.is_null()) {
		return false;
	}

	if (p_asset->get_asset_type() == GaussianSplatAsset::ASSET_TYPE_DYNAMIC) {
		return p_asset->populate_gaussian_data(r_data);
	}

	Ref<GaussianData> shared_data = p_asset->get_gaussian_data();
	if (shared_data.is_null()) {
		return false;
	}
	r_data = shared_data;
	return true;
}

void GaussianSplatSceneDirector::InstanceStore::bump_generation() {
	_bump_instance_generation(instance_generation);
}

void GaussianSplatSceneDirector::InstanceStore::bump_asset_generation() {
	_bump_instance_asset_generation(instance_asset_generation);
}

void GaussianSplatSceneDirector::SphereEffectorStore::bump_generation() {
	_bump_instance_generation(sphere_effector_generation);
}

bool GaussianSplatSceneDirector::InstanceStore::retain_asset(const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id) {
	if (p_asset.is_null()) {
		return false;
	}
	uint32_t edited_version = 0;
#ifdef TOOLS_ENABLED
	edited_version = p_asset->get_edited_version();
#endif
	AssetRecord *record = asset_records.getptr(p_asset_id);
	if (!record) {
		AssetRecord new_record;
		new_record.asset = p_asset;
		if (!_populate_gaussian_data_from_asset(p_asset, new_record.data)) {
			GS_LOG_WARN_DEFAULT("[GaussianSplatSceneDirector] Failed to build GaussianData from asset.");
			return false;
		}
		new_record.edited_version = edited_version;
		new_record.refcount = 1;
		asset_records.insert(p_asset_id, new_record);
		_bump_instance_generation(instance_generation);
		_bump_instance_asset_generation(instance_asset_generation);
		return true;
	}

	record->asset = p_asset;
	if (record->data.is_null() || record->edited_version != edited_version) {
		Ref<GaussianData> refreshed_data;
		if (!_populate_gaussian_data_from_asset(p_asset, refreshed_data)) {
			GS_LOG_WARN_DEFAULT("[GaussianSplatSceneDirector] Failed to rebuild GaussianData from asset.");
			return false;
		}
		record->data = refreshed_data;
		record->edited_version = edited_version;
		_bump_instance_generation(instance_generation);
		_bump_instance_asset_generation(instance_asset_generation);
	}
	record->refcount++;
	return true;
}

bool GaussianSplatSceneDirector::InstanceStore::refresh_asset(const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id) {
	if (p_asset.is_null()) {
		return false;
	}
	AssetRecord *record = asset_records.getptr(p_asset_id);
	if (!record) {
		return false;
	}
	uint32_t edited_version = 0;
#ifdef TOOLS_ENABLED
	edited_version = p_asset->get_edited_version();
#endif
	if (!record->data.is_null() && record->edited_version == edited_version) {
		return true;
	}
	Ref<GaussianData> refreshed_data;
	if (!_populate_gaussian_data_from_asset(p_asset, refreshed_data)) {
		GS_LOG_WARN_DEFAULT("[GaussianSplatSceneDirector] Failed to refresh GaussianData from asset.");
		return false;
	}
	record->asset = p_asset;
	record->data = refreshed_data;
	record->edited_version = edited_version;
	_bump_instance_generation(instance_generation);
	_bump_instance_asset_generation(instance_asset_generation);
	return true;
}

void GaussianSplatSceneDirector::InstanceStore::release_asset(uint64_t p_asset_id) {
	AssetRecord *record = asset_records.getptr(p_asset_id);
	if (!record) {
		return;
	}
	if (record->refcount > 0) {
		record->refcount--;
	}
	if (record->refcount == 0) {
		asset_records.erase(p_asset_id);
		_bump_instance_asset_generation(instance_asset_generation);
	}
}

bool GaussianSplatSceneDirector::_is_world_submission_owner_live(ObjectID p_owner_id) {
	if (p_owner_id == ObjectID()) {
		return false;
	}
	return ObjectDB::get_instance(p_owner_id) != nullptr;
}

void GaussianSplatSceneDirector::SubmissionStore::store_submission(WorldSubmissionRecord &r_record,
		const WorldSubmission &p_submission) {
	r_record.owner_id = p_submission.owner_id;
	r_record.gaussian_data = p_submission.gaussian_data;
	r_record.payload_source = p_submission.payload_source;
	r_record.static_chunks = p_submission.static_chunks;
	r_record.bounds = p_submission.bounds;
	r_record.metadata = p_submission.metadata;
	r_record.has_desired_residency_hint = p_submission.has_desired_residency_hint;
	r_record.desired_residency_hint = p_submission.desired_residency_hint;
	r_record.desired_renderer_overrides = p_submission.desired_renderer_overrides;
	r_record.renderer_restore_state = GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot();
	r_record.active = true;
}

bool GaussianSplatSceneDirector::SubmissionStore::record_has_renderable_payload(
		const WorldSubmissionRecord &p_record) {
	const bool has_resident_data = p_record.gaussian_data.is_valid() &&
			p_record.gaussian_data->get_count() > 0;
	const bool has_file_backed_payload = p_record.payload_source.is_valid() &&
			p_record.payload_source->is_valid() &&
			p_record.payload_source->get_count() > 0;
	return has_resident_data || has_file_backed_payload;
}

void GaussianSplatSceneDirector::_copy_world_submission_record(const SharedWorld &p_world,
		const SubmissionStore::WorldSubmissionRecord &p_record, WorldSubmission *r_submission) {
	ERR_FAIL_NULL(r_submission);

	r_submission->owner_id = p_record.owner_id;
	r_submission->scenario = p_world.scenario;
	r_submission->gaussian_data = p_record.gaussian_data;
	r_submission->payload_source = p_record.payload_source;
	r_submission->static_chunks = p_record.static_chunks;
	r_submission->bounds = p_record.bounds;
	r_submission->metadata = p_record.metadata;
	r_submission->has_desired_residency_hint = p_record.has_desired_residency_hint;
	r_submission->desired_residency_hint = p_record.desired_residency_hint;
	r_submission->desired_renderer_overrides = p_record.desired_renderer_overrides;
}

void GaussianSplatSceneDirector::_restore_world_submission_renderer(SharedWorld &p_world,
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_snapshot) {
	if (!p_world.renderer.is_valid()) {
		return;
	}
	// #611: checked AFTER the early-out. With no renderer this function performs
	// no dispatch at all, so counting it would make the violation counter fire on
	// every headless run and stop meaning anything.
	_report_renderer_contract_lock_violation("_restore_world_submission_renderer");
	GaussianSplatRenderer *renderer = p_world.renderer.ptr();
	ERR_FAIL_NULL(renderer);

	if (!p_snapshot.valid) {
		renderer->clear_world_submission_contract();
		return;
	}

	const Error err = renderer->restore_world_submission_runtime_state(p_snapshot);
	if (err != OK) {
		GS_LOG_RENDERER_ERROR(vformat("[GaussianSplatSceneDirector] Failed to restore world submission renderer state (err=%d).", err));
	}
}

bool GaussianSplatSceneDirector::_apply_world_submission_to_renderer(SharedWorld &p_world,
		const SubmissionStore::WorldSubmissionRecord &p_record,
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state) {
	if (!p_record.active || !p_world.renderer.is_valid()) {
		return true;
	}
	// #611: checked AFTER the early-out — see the note in
	// _restore_world_submission_renderer.
	_report_renderer_contract_lock_violation("_apply_world_submission_to_renderer");

	GaussianSplatRenderer *renderer = p_world.renderer.ptr();
	ERR_FAIL_NULL_V(renderer, false);
	const GaussianSplatRenderer::WorldSubmissionContract contract =
			SubmissionStore::build_contract(p_renderer_state, p_record);
	const Error err = renderer->apply_world_submission_contract(contract);
	if (err != OK) {
		GS_LOG_RENDERER_ERROR(vformat("[GaussianSplatSceneDirector] Failed to apply world submission contract (err=%d).", err));
		return false;
	}
	return true;
}

bool GaussianSplatSceneDirector::_apply_world_submission_contract_unlocked(
		const Ref<GaussianSplatRenderer> &p_renderer,
		const GaussianSplatRenderer::WorldSubmissionContract &p_contract) {
	if (p_renderer.is_null()) {
		// Matches _apply_world_submission_to_renderer's early-out for a world with
		// no renderer: nothing to apply is not a failure, and must not be turned
		// into a rejected submission.
		return true;
	}
	// #611 PR B2: the inverse sense of the other boundary checks. Those report
	// when the lock IS held because they are still on the locked path; this one is
	// the path that fixed it, so the report is a regression alarm — it fires only
	// if someone reintroduces the inversion by calling this under world_mutex.
	// Either way the counter's meaning is unchanged: non-zero means a blocking
	// render-thread dispatch was issued from inside the critical section the
	// render thread itself needs.
	_report_renderer_contract_lock_violation("_apply_world_submission_contract_unlocked");

	// Identical error handling to _apply_world_submission_to_renderer, and that is
	// load-bearing rather than incidental: the two failure behaviours #611 requires
	// to stay distinct are produced INSIDE apply_world_submission_contract and are
	// carried out solely by this Error.
	//
	//   set_max_splats      -> warns and returns void on dispatch timeout, so the
	//                          contract still returns OK -> true -> the caller
	//                          COMMITS and the max_splats change is silently
	//                          dropped (renderer/render_quality_orchestrator.cpp).
	//   set_gaussian_data   -> returns ERR_BUSY on dispatch timeout, so the
	//                          contract returns non-OK -> false -> the caller
	//                          ROLLS BACK and REJECTS the submission
	//                          (renderer/render_data_orchestrator.cpp).
	//
	// Collapsing them — for example by treating any timeout uniformly, or by
	// returning true here on error — is a behavioural regression even though it
	// would still remove the lock inversion.
	const Error err = p_renderer->apply_world_submission_contract(p_contract);
	if (err != OK) {
		GS_LOG_RENDERER_ERROR(vformat("[GaussianSplatSceneDirector] Failed to apply world submission contract (err=%d).", err));
		return false;
	}
	return true;
}

bool GaussianSplatSceneDirector::_world_has_no_instances(const SharedWorld &p_world) const {
	return p_world.instance_store.is_empty();
}

bool GaussianSplatSceneDirector::_world_has_no_sphere_effectors(const SharedWorld &p_world) const {
	return p_world.sphere_effector_store.is_empty();
}

bool GaussianSplatSceneDirector::_world_submission_idle(const SharedWorld &p_world) const {
	return !p_world.submission_store.is_active();
}

bool GaussianSplatSceneDirector::_world_renderer_unshared(const SharedWorld &p_world) const {
	// A null renderer is trivially unshared (prune). Otherwise preserve the
	// SharedWorld while some external owner (for example a node that temporarily
	// left the tree, an active world node, or editor tooling) still holds the
	// shared renderer Ref -- re-registration can otherwise desynchronize the
	// director from that retained renderer. The `is_null()` short-circuit guards
	// the deref, so this is safe to evaluate in any order.
	return p_world.renderer.is_null() || p_world.renderer->get_reference_count() <= 1;
}

bool GaussianSplatSceneDirector::_should_prune_world(const SharedWorld &p_world) const {
	// #610 S2: prune iff every per-concern predicate agrees. `&&` preserves the
	// original short-circuit exactly -- the renderer predicate (the only one that
	// dereferences) is reached only after the first three pass, and it internally
	// short-circuits the null case, so behavior is byte-identical to the prior
	// sequential early-returns.
	return _world_has_no_instances(p_world) &&
			_world_has_no_sphere_effectors(p_world) &&
			_world_submission_idle(p_world) &&
			_world_renderer_unshared(p_world);
}

void GaussianSplatSceneDirector::_prune_world_if_unused(const RID &p_scenario,
		LocalVector<Ref<GaussianSplatRenderer>> &r_deferred_release) {
	SharedWorld *world = worlds.getptr(p_scenario);
	if (!world) {
		return;
	}
	if (!_should_prune_world(*world)) {
		return;
	}
	// #611: move the renderer Ref out of the map entry BEFORE erasing so that
	// ~GaussianSplatRenderer (which blocks on a render-thread dispatch) does not
	// run while world_mutex is held. r_deferred_release is owned by the caller and
	// destroyed only after world_mutex is released, so the actual teardown happens
	// outside the critical section.
	if (world->renderer.is_valid()) {
		r_deferred_release.push_back(std::move(world->renderer));
	}
	worlds.erase(p_scenario);
}


void GaussianSplatSceneDirector::register_instance(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset,
        const Transform3D &p_transform, float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow,
        float p_wind_intensity, uint32_t p_wind_mode, const Vector3 &p_wind_direction, float p_wind_frequency,
        bool p_visible, bool p_has_desired_residency_hint, int32_t p_desired_residency_hint,
        float p_effect_position_scale, float p_effect_opacity_scale) {
	// #611: declared before the lock so the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer) dispatches to the render thread only after world_mutex is
	// released. See RendererContractWorkQueue in the header for the ordering rules.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id, &deferred_renderer_work);
	if (!world) {
		GaussianSplatting::debug_trace_record_event("instance_reg", "FAIL: world=NULL", true);
		return;
	}
	// World-switch migration: if the node is already registered in a DIFFERENT
	// SharedWorld (the node's `World3D` changed without a tree removal), evict
	// the stale record first. Without this, `register_instance` called with
	// the node's new world leaves the record pinned in both worlds — stale
	// renderer state, duplicate instances, and effector matching against the
	// wrong world's ancestor chain.
	for (KeyValue<RID, SharedWorld> &E : worlds) {
		SharedWorld &other = E.value;
		if (&other == world) {
			continue;
		}
		uint64_t stale_asset_id = 0;
		if (!other.instance_store.remove(p_node_id, stale_asset_id)) {
			continue;
		}
		other.instance_store.bump_generation();
		other.instance_store.bump_asset_generation();
		if (stale_asset_id != 0) {
			other.instance_store.release_asset(stale_asset_id);
		}
		break; // an instance can only live in one SharedWorld at a time
	}
	// #611 PR B1: this used to call world->renderer->initialize() inline, which
	// opens with a blocking render-thread dispatch
	// (renderer/gaussian_splat_renderer.cpp:1613-1618) -- issued while this thread
	// holds world_mutex, which the render thread itself needs inside the
	// *_for_renderer builders. The call is deferred to the flush below (after the
	// lock is released) instead. Nothing between here and the end of this function
	// touches the renderer or GPU state -- the remainder is instance/asset record
	// bookkeeping -- and the flush still happens before register_instance returns,
	// so no caller can observe the difference.
	_initialize_world_renderer(*world, &deferred_renderer_work);
	if (p_asset.is_null()) {
		GaussianSplatting::debug_trace_record_event("instance_reg", "FAIL: asset=null", true);
		return;
	}
	// Use the full 64-bit ObjectID as the asset_records key. Truncating to
	// uint32_t (the old behaviour) made two assets whose ObjectIDs share the
	// low 32 bits alias the same asset record.
	const uint64_t asset_id = _asset_records_key(p_asset->get_instance_id());
	const float wind_intensity = MAX(0.0f, p_wind_intensity);
	const uint32_t wind_mode = MIN(p_wind_mode, (uint32_t)INSTANCE_WIND_FORCE_ENABLED);
	const float wind_frequency = MAX(0.0f, p_wind_frequency);
	const float effect_position_scale = MAX(0.0f, p_effect_position_scale);
	const float effect_opacity_scale = MAX(0.0f, p_effect_opacity_scale);
	if (asset_id == 0) {
		GaussianSplatting::debug_trace_record_event("instance_reg", "FAIL: asset_id=0", true);
		return;
	}
	GaussianSplatting::debug_trace_record_event("instance_reg",
			vformat("OK: asset_id=%s instances_before=%d", String::num_uint64(asset_id), world->instance_store.instance_count()),
			false);

	InstanceRecord *existing_record = world->instance_store.find_mutable(p_node_id);
	if (existing_record) {
		InstanceRecord &record = *existing_record;
		bool dirty = false;
		bool asset_selection_dirty = false;
		if (!record.transform.is_equal_approx(p_transform)) {
			record.transform = p_transform;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.opacity, p_opacity)) {
			record.opacity = p_opacity;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.lod_bias, p_lod_bias)) {
			record.lod_bias = p_lod_bias;
			dirty = true;
		}
		if (record.flags != p_flags) {
			record.flags = p_flags;
			dirty = true;
		}
		if (record.casts_shadow != p_casts_shadow) {
			record.casts_shadow = p_casts_shadow;
			dirty = true;
			asset_selection_dirty = true;
		}
		if (record.visible != p_visible) {
			record.visible = p_visible;
			dirty = true;
			asset_selection_dirty = true;
		}
		if (!Math::is_equal_approx(record.wind_intensity, wind_intensity)) {
			record.wind_intensity = wind_intensity;
			dirty = true;
		}
		if (record.wind_mode != wind_mode) {
			record.wind_mode = wind_mode;
			dirty = true;
		}
		if (!record.wind_direction.is_equal_approx(p_wind_direction)) {
			record.wind_direction = p_wind_direction;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.wind_frequency, wind_frequency)) {
			record.wind_frequency = wind_frequency;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.effect_position_scale, effect_position_scale)) {
			record.effect_position_scale = effect_position_scale;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.effect_opacity_scale, effect_opacity_scale)) {
			record.effect_opacity_scale = effect_opacity_scale;
			dirty = true;
		}
		if (record.has_desired_residency_hint != p_has_desired_residency_hint) {
			record.has_desired_residency_hint = p_has_desired_residency_hint;
			dirty = true;
		}
		if (record.desired_residency_hint != p_desired_residency_hint) {
			record.desired_residency_hint = p_desired_residency_hint;
			dirty = true;
		}
		if (record.asset_id == asset_id) {
			if (world->instance_store.has_asset(asset_id)) {
				if (!world->instance_store.refresh_asset(p_asset, asset_id)) {
					return;
				}
			} else {
				if (!world->instance_store.retain_asset(p_asset, asset_id)) {
					return;
				}
			}
		}
		if (record.asset_id != asset_id) {
			if (!world->instance_store.retain_asset(p_asset, asset_id)) {
				return;
			}
			world->instance_store.release_asset(record.asset_id);
			record.asset_id = asset_id;
			record.last_lod = 0;
			dirty = true;
			asset_selection_dirty = true;
		}
		record.dirty = record.dirty || dirty;
		if (dirty) {
			world->instance_store.bump_generation();
		}
		if (asset_selection_dirty) {
			world->instance_store.bump_asset_generation();
		}
		return;
	}

	if (!world->instance_store.retain_asset(p_asset, asset_id)) {
		return;
	}

	InstanceRecord record;
	record.node_id = p_node_id;
	record.transform = p_transform;
	record.opacity = p_opacity;
	record.lod_bias = p_lod_bias;
	record.wind_intensity = wind_intensity;
	record.wind_mode = wind_mode;
	record.wind_direction = p_wind_direction;
	record.wind_frequency = wind_frequency;
	record.effect_position_scale = effect_position_scale;
	record.effect_opacity_scale = effect_opacity_scale;
	record.asset_id = asset_id;
	record.flags = p_flags;
	record.last_lod = 0;
	record.casts_shadow = p_casts_shadow;
	record.visible = p_visible;
	record.has_desired_residency_hint = p_has_desired_residency_hint;
	record.desired_residency_hint = p_desired_residency_hint;
	record.dirty = true;

	world->instance_store.append(record);
	world->instance_store.bump_generation();
	world->instance_store.bump_asset_generation();
	GaussianSplatting::debug_trace_record_event("instance_reg",
			vformat("ADDED: instances_after=%d", world->instance_store.instance_count()),
			false);
}

void GaussianSplatSceneDirector::update_instance_transform(ObjectID p_node_id, const Transform3D &p_transform) {
	// #611: declared before the lock so the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer) dispatches to the render thread only after world_mutex is
	// released. See RendererContractWorkQueue in the header for the ordering rules.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id, &deferred_renderer_work);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}

	InstanceRecord *record_ptr = world->instance_store.find_mutable(p_node_id);
	if (!record_ptr) {
		return;
	}

	InstanceRecord &record = *record_ptr;
	if (record.transform.is_equal_approx(p_transform)) {
		return;
	}
	record.transform = p_transform;
	record.dirty = true;
	world->instance_store.bump_generation();
}

void GaussianSplatSceneDirector::update_instance_scene_effector_filter(ObjectID p_node_id, bool p_enabled,
		uint32_t p_layer_mask, bool p_scope_filter_present, bool p_scope_filter_valid,
		ObjectID p_scope_root_id, const LocalVector<ObjectID> &p_scene_tree_ancestor_ids) {
	// #611: declared before the lock so the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer) dispatches to the render thread only after world_mutex is
	// released. See RendererContractWorkQueue in the header for the ordering rules.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id, &deferred_renderer_work);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}
	InstanceRecord *record_ptr = world->instance_store.find_mutable(p_node_id);
	if (!record_ptr) {
		return;
	}
	InstanceRecord &record = *record_ptr;
	bool ancestors_changed = record.scene_tree_ancestor_ids.size() != p_scene_tree_ancestor_ids.size();
	if (!ancestors_changed) {
		for (uint32_t i = 0; i < p_scene_tree_ancestor_ids.size(); ++i) {
			if (record.scene_tree_ancestor_ids[i] != p_scene_tree_ancestor_ids[i]) {
				ancestors_changed = true;
				break;
			}
		}
	}
	const bool changed = ancestors_changed ||
			record.scene_effectors_enabled != p_enabled ||
			record.scene_effector_layer_mask != p_layer_mask ||
			record.scene_effector_scope_filter_present != p_scope_filter_present ||
			record.scene_effector_scope_filter_valid != p_scope_filter_valid ||
			record.scene_effector_scope_root_id != p_scope_root_id;
	if (!changed) {
		return;
	}
	record.scene_effectors_enabled = p_enabled;
	record.scene_effector_layer_mask = p_layer_mask;
	record.scene_effector_scope_filter_present = p_scope_filter_present;
	record.scene_effector_scope_filter_valid = p_scope_filter_valid;
	record.scene_effector_scope_root_id = p_scope_root_id;
	record.scene_tree_ancestor_ids = p_scene_tree_ancestor_ids;
	record.dirty = true;
	world->instance_store.bump_generation();
}

void GaussianSplatSceneDirector::update_instance_params(ObjectID p_node_id, float p_opacity, float p_lod_bias,
		uint32_t p_flags, bool p_casts_shadow, float p_wind_intensity, uint32_t p_wind_mode,
		const Vector3 &p_wind_direction, float p_wind_frequency, bool p_visible,
		bool p_has_desired_residency_hint, int32_t p_desired_residency_hint,
		float p_effect_position_scale, float p_effect_opacity_scale) {
	// #611: declared before the lock so the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer) dispatches to the render thread only after world_mutex is
	// released. See RendererContractWorkQueue in the header for the ordering rules.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id, &deferred_renderer_work);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}

	InstanceRecord *record_ptr = world->instance_store.find_mutable(p_node_id);
	if (!record_ptr) {
		return;
	}

	InstanceRecord &record = *record_ptr;
	const float wind_intensity = MAX(0.0f, p_wind_intensity);
	const uint32_t wind_mode = MIN(p_wind_mode, (uint32_t)INSTANCE_WIND_FORCE_ENABLED);
	const float wind_frequency = MAX(0.0f, p_wind_frequency);
	const float effect_position_scale = MAX(0.0f, p_effect_position_scale);
	const float effect_opacity_scale = MAX(0.0f, p_effect_opacity_scale);
	bool dirty = false;
	bool asset_selection_dirty = false;
	if (!Math::is_equal_approx(record.opacity, p_opacity)) {
		record.opacity = p_opacity;
		dirty = true;
	}
	if (!Math::is_equal_approx(record.lod_bias, p_lod_bias)) {
		record.lod_bias = p_lod_bias;
		dirty = true;
	}
	if (record.flags != p_flags) {
		record.flags = p_flags;
		dirty = true;
	}
	if (record.casts_shadow != p_casts_shadow) {
		record.casts_shadow = p_casts_shadow;
		dirty = true;
		asset_selection_dirty = true;
	}
	if (record.visible != p_visible) {
		record.visible = p_visible;
		dirty = true;
		asset_selection_dirty = true;
	}
	if (!Math::is_equal_approx(record.wind_intensity, wind_intensity)) {
		record.wind_intensity = wind_intensity;
		dirty = true;
	}
	if (record.wind_mode != wind_mode) {
		record.wind_mode = wind_mode;
		dirty = true;
	}
	if (!record.wind_direction.is_equal_approx(p_wind_direction)) {
		record.wind_direction = p_wind_direction;
		dirty = true;
	}
	if (!Math::is_equal_approx(record.wind_frequency, wind_frequency)) {
		record.wind_frequency = wind_frequency;
		dirty = true;
	}
	if (!Math::is_equal_approx(record.effect_position_scale, effect_position_scale)) {
		record.effect_position_scale = effect_position_scale;
		dirty = true;
	}
	if (!Math::is_equal_approx(record.effect_opacity_scale, effect_opacity_scale)) {
		record.effect_opacity_scale = effect_opacity_scale;
		dirty = true;
	}
	if (record.has_desired_residency_hint != p_has_desired_residency_hint) {
		record.has_desired_residency_hint = p_has_desired_residency_hint;
		dirty = true;
	}
	if (record.desired_residency_hint != p_desired_residency_hint) {
		record.desired_residency_hint = p_desired_residency_hint;
		dirty = true;
	}
	record.dirty = record.dirty || dirty;
	if (dirty) {
		world->instance_store.bump_generation();
	}
	if (asset_selection_dirty) {
		world->instance_store.bump_asset_generation();
	}
}

void GaussianSplatSceneDirector::unregister_instance(ObjectID p_node_id) {
	// #611/#628: declared before the lock so both the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer) and the blocking teardown of any renderer the prune below
	// frees dispatch/run only after world_mutex is released. The single
	// RendererContractWorkQueue owns both the deferred work and the release vector
	// and fixes their relative teardown order internally; see the header.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id, &deferred_renderer_work);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}

	uint64_t asset_id = 0;
	if (!world->instance_store.remove(p_node_id, asset_id)) {
		return;
	}
	world->instance_store.release_asset(asset_id);
	world->instance_store.bump_generation();
	world->instance_store.bump_asset_generation();

	_prune_world_if_unused(world->scenario, deferred_renderer_work.release_vector());
}

void GaussianSplatSceneDirector::register_instance_submission(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset,
		const Transform3D &p_transform, float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow,
		float p_wind_intensity, uint32_t p_wind_mode, const Vector3 &p_wind_direction, float p_wind_frequency,
		bool p_visible, bool p_has_desired_residency_hint, int32_t p_desired_residency_hint,
		float p_effect_position_scale, float p_effect_opacity_scale) {
	register_instance(p_node_id, p_asset, p_transform, p_opacity, p_lod_bias, p_flags, p_casts_shadow,
			p_wind_intensity, p_wind_mode, p_wind_direction, p_wind_frequency, p_visible,
			p_has_desired_residency_hint, p_desired_residency_hint, p_effect_position_scale,
			p_effect_opacity_scale);
}

void GaussianSplatSceneDirector::update_instance_submission_transform(ObjectID p_node_id, const Transform3D &p_transform) {
	update_instance_transform(p_node_id, p_transform);
}

void GaussianSplatSceneDirector::update_instance_submission_params(ObjectID p_node_id, float p_opacity, float p_lod_bias,
		uint32_t p_flags, bool p_casts_shadow, float p_wind_intensity, uint32_t p_wind_mode,
		const Vector3 &p_wind_direction, float p_wind_frequency, bool p_visible,
		bool p_has_desired_residency_hint, int32_t p_desired_residency_hint,
		float p_effect_position_scale, float p_effect_opacity_scale) {
	update_instance_params(p_node_id, p_opacity, p_lod_bias, p_flags, p_casts_shadow, p_wind_intensity,
			p_wind_mode, p_wind_direction, p_wind_frequency, p_visible,
			p_has_desired_residency_hint, p_desired_residency_hint, p_effect_position_scale,
			p_effect_opacity_scale);
}

void GaussianSplatSceneDirector::unregister_instance_submission(ObjectID p_node_id) {
	unregister_instance(p_node_id);
}

bool GaussianSplatSceneDirector::get_instance_submission(ObjectID p_node_id, InstanceSubmission *r_submission) const {
	ERR_FAIL_NULL_V(r_submission, false);

	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		const SharedWorld &world = E.value;
		const InstanceRecord *record_ptr = world.instance_store.find(p_node_id);
		if (!record_ptr) {
			continue;
		}

		const InstanceRecord &record = *record_ptr;
		const AssetRecord *asset_record = world.instance_store.find_asset(record.asset_id);

		r_submission->node_id = record.node_id;
		r_submission->scenario = world.scenario;
		r_submission->renderer = world.renderer;
		r_submission->asset = asset_record ? asset_record->asset : Ref<GaussianSplatAsset>();
		r_submission->transform = record.transform;
		r_submission->opacity = record.opacity;
		r_submission->lod_bias = record.lod_bias;
		r_submission->wind_intensity = record.wind_intensity;
		r_submission->wind_mode = record.wind_mode;
		r_submission->wind_direction = record.wind_direction;
		r_submission->wind_frequency = record.wind_frequency;
		r_submission->effect_position_scale = record.effect_position_scale;
		r_submission->effect_opacity_scale = record.effect_opacity_scale;
		r_submission->flags = record.flags;
		r_submission->last_lod = record.last_lod;
		r_submission->casts_shadow = record.casts_shadow;
		r_submission->visible = record.visible;
		r_submission->has_desired_residency_hint = record.has_desired_residency_hint;
		r_submission->desired_residency_hint = record.desired_residency_hint;
		return true;
	}

	return false;
}

void GaussianSplatSceneDirector::update_instance_lods_for_renderer(const GaussianSplatRenderer *p_renderer,
		const Vector3 &p_camera_pos, const LODConfig &p_lod_config, float p_hysteresis_zone) {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || world->instance_store.is_empty()) {
		return;
	}

	// Skip the whole O(instances) walk when nothing that affects LOD moved since
	// the last walk. instance_generation already bumps on every instance
	// add/remove/transform/param(bias) change, so animating content can never be
	// frozen here; camera position, LODConfig and the hysteresis zone are compared
	// directly. Exact (non-epsilon) comparison is intentional: any real change
	// must re-walk, and an epsilon could let a slow camera drift stall LOD.
	if (world->lod_walk_cache_valid &&
			world->lod_walk_last_generation == world->instance_store.generation() &&
			world->lod_walk_last_camera_pos == p_camera_pos &&
			world->lod_walk_last_hysteresis == p_hysteresis_zone &&
			world->lod_walk_last_config == p_lod_config) {
		return;
	}

	const int max_lod = MAX(0, p_lod_config.num_levels - 1);
	const bool use_fallback = p_hysteresis_zone <= 0.0f;
	const bool log_enabled = _is_scene_director_log_enabled();
	bool any_changed = false;

	LocalVector<InstanceRecord> &instance_records = world->instance_store.mutable_records();
	for (uint32_t i = 0; i < instance_records.size(); i++) {
		InstanceRecord &record = instance_records[i];
		const float distance = p_camera_pos.distance_to(record.transform.origin);
		const float bias = MAX(record.lod_bias, 0.0001f);
		const float effective_distance = distance * bias;
		int desired_lod = p_lod_config.calculate_lod_level(effective_distance);
		desired_lod = CLAMP(desired_lod, 0, max_lod);

		uint32_t current_lod = record.last_lod;
		if (current_lod > static_cast<uint32_t>(max_lod)) {
			current_lod = static_cast<uint32_t>(max_lod);
			record.last_lod = current_lod;
			record.dirty = true;
			any_changed = true;
		}
		if (desired_lod == static_cast<int>(current_lod)) {
			if (log_enabled) {
				GS_LOG_RENDERER_DEBUG(vformat("[InstanceLOD] node=%s asset=%s dist=%.3f bias=%.3f eff=%.3f lod=%u desired=%d (no change)",
						String::num_uint64((uint64_t)record.node_id), String::num_uint64(record.asset_id), distance, bias, effective_distance, current_lod, desired_lod));
			}
			continue;
		}

		if (desired_lod > static_cast<int>(current_lod)) {
			const float threshold = p_lod_config.get_distance_threshold(desired_lod);
			const float zone = use_fallback ? MAX(0.5f, 0.05f * threshold) : p_hysteresis_zone;
			if (effective_distance < threshold + zone) {
				if (log_enabled) {
					GS_LOG_RENDERER_DEBUG(vformat("[InstanceLOD] node=%s asset=%s dist=%.3f bias=%.3f eff=%.3f lod=%u desired=%d (hold-up)",
							String::num_uint64((uint64_t)record.node_id), String::num_uint64(record.asset_id), distance, bias, effective_distance, current_lod, desired_lod));
				}
				continue;
			}
		} else {
			const float threshold = p_lod_config.get_distance_threshold(static_cast<int>(current_lod));
			const float zone = use_fallback ? MAX(0.5f, 0.05f * threshold) : p_hysteresis_zone;
			if (effective_distance > threshold - zone) {
				if (log_enabled) {
					GS_LOG_RENDERER_DEBUG(vformat("[InstanceLOD] node=%s asset=%s dist=%.3f bias=%.3f eff=%.3f lod=%u desired=%d (hold-down)",
							String::num_uint64((uint64_t)record.node_id), String::num_uint64(record.asset_id), distance, bias, effective_distance, current_lod, desired_lod));
				}
				continue;
			}
		}

		record.last_lod = static_cast<uint32_t>(desired_lod);
		record.dirty = true;
		any_changed = true;
		if (log_enabled) {
			GS_LOG_RENDERER_DEBUG(vformat("[InstanceLOD] node=%s asset=%s dist=%.3f bias=%.3f eff=%.3f lod=%u -> %u",
					String::num_uint64((uint64_t)record.node_id), String::num_uint64(record.asset_id), distance, bias, effective_distance,
					current_lod, record.last_lod));
		}
	}
	if (any_changed) {
		world->instance_store.bump_generation();
	}

	// Memoize the inputs for next frame's early-out. Capture the generation AFTER
	// the potential bump above so the walk's own change doesn't force a redundant
	// re-walk next frame.
	world->lod_walk_cache_valid = true;
	world->lod_walk_last_camera_pos = p_camera_pos;
	world->lod_walk_last_generation = world->instance_store.generation();
	world->lod_walk_last_hysteresis = p_hysteresis_zone;
	world->lod_walk_last_config = p_lod_config;
}

void GaussianSplatSceneDirector::build_instance_buffer(LocalVector<InstanceDataGPU> &out) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	out.clear();

	uint32_t total_instances = 0;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		total_instances += E.value.instance_store.instance_count();
	}
	if (total_instances == 0) {
		return;
	}
	out.reserve(total_instances);

	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		const SharedWorld &world_const_ref = E.value;
		LocalVector<SphereEffectorSelection> scene_payload;
		_build_sorted_sphere_effector_payload(world_const_ref, scene_payload);
		for (const InstanceRecord &record : world_const_ref.instance_store.records()) {
			if (!record.visible) {
				continue;
			}
			const AssetRecord *asset_record = world_const_ref.instance_store.find_asset(record.asset_id);
			if (!asset_record || asset_record->data.is_null()) {
				continue;
			}
			InstanceDataGPU entry = {};

			const Basis &basis = record.transform.basis;
			const Vector3 scale = basis.get_scale();
			const float sx = Math::abs(scale.x);
			const float sy = Math::abs(scale.y);
			const float sz = Math::abs(scale.z);
			const float uniform_scale = MAX(sx, MAX(sy, sz));

			Quaternion rotation = basis.get_rotation_quaternion().normalized();
			Quaternion inv_rotation = rotation.inverse();

			entry.rotation[0] = rotation.x;
			entry.rotation[1] = rotation.y;
			entry.rotation[2] = rotation.z;
			entry.rotation[3] = rotation.w;

			entry.inv_rotation[0] = inv_rotation.x;
			entry.inv_rotation[1] = inv_rotation.y;
			entry.inv_rotation[2] = inv_rotation.z;
			entry.inv_rotation[3] = inv_rotation.w;

			entry.translation_scale[0] = record.transform.origin.x;
			entry.translation_scale[1] = record.transform.origin.y;
			entry.translation_scale[2] = record.transform.origin.z;
			entry.translation_scale[3] = uniform_scale;

			entry.params[0] = record.opacity;
			entry.params[1] = record.lod_bias;
			entry.params[2] = record.wind_intensity;
			entry.params[3] = float(record.wind_mode);

			// GPU instance slot is a 32-bit opaque tag used only for dense-id
			// remapping; the collision-free identity lives in the 64-bit
			// asset_records key, so truncation here is intentional.
			entry.ids[0] = static_cast<uint32_t>(record.asset_id);
			uint32_t flags = record.flags;
			if (rotation.is_equal_approx(Quaternion())) {
				flags |= GS_INSTANCE_FLAG_ROTATION_IDENTITY;
			}
			if (Math::is_equal_approx(uniform_scale, 1.0f)) {
				flags |= GS_INSTANCE_FLAG_SCALE_IDENTITY;
			}
			if (record.transform.origin.is_zero_approx()) {
				flags |= GS_INSTANCE_FLAG_TRANSLATION_ZERO;
			}
			entry.ids[1] = flags;

			entry.lod[0] = record.last_lod;
			entry.lod[1] = 0;
			entry.wind_params[0] = record.wind_direction.x;
			entry.wind_params[1] = record.wind_direction.y;
			entry.wind_params[2] = record.wind_direction.z;
			entry.wind_params[3] = record.wind_frequency;
			entry.effect_params[0] = record.effect_position_scale;
			entry.effect_params[1] = record.effect_opacity_scale;
			entry.effect_params[2] = _encode_u32_as_float_bits(_build_scene_effector_mask_for_record(record, scene_payload));
			entry.effect_params[3] = float(scene_payload.size());

			out.push_back(entry);
		}
	}
}

void GaussianSplatSceneDirector::build_instance_buffer_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<InstanceDataGPU> &out, bool p_shadow_casters_only,
		LocalVector<uint64_t> *r_submission_asset_ids) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	out.clear();
	if (r_submission_asset_ids) {
		r_submission_asset_ids->clear();
	}

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return;
	}

	// Build the scene-effector payload once up front so the world-submission
	// shim (below) and the per-instance path (further down) can encode masks
	// against the same slot ordering.
	LocalVector<SphereEffectorSelection> scene_payload;
	_build_sorted_sphere_effector_payload(*world, scene_payload);

	// World submission instance: when the world has an active world submission with
	// renderable data and no normal instances, produce a proper identity-transform
	// instance referencing the primary asset (id=0).  This replaces the synthetic
	// fallback shim that render_streaming_orchestrator previously injected.
	if (world->instance_store.is_empty()) {
		if (world->submission_store.is_active() &&
				SubmissionStore::record_has_renderable_payload(world->submission_store.get_record())) {
			InstanceDataGPU entry = {};
			entry.rotation[3] = 1.0f;
			entry.inv_rotation[3] = 1.0f;
			entry.translation_scale[3] = 1.0f;
			entry.params[0] = 1.0f; // opacity
			entry.params[1] = 1.0f; // lod_bias
			entry.params[2] = 1.0f; // wind_intensity
			entry.params[3] = 0.0f; // wind_mode
			entry.ids[0] = 0u; // primary asset id
			entry.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY |
					GS_INSTANCE_FLAG_SCALE_IDENTITY |
					GS_INSTANCE_FLAG_TRANSLATION_ZERO;
			entry.lod[0] = 0;
			entry.lod[1] = 0;
			entry.wind_params[0] = 0.0f;
			entry.wind_params[1] = 0.0f;
			entry.wind_params[2] = 0.0f;
			entry.wind_params[3] = 1.0f;
			entry.effect_params[0] = 1.0f;
			entry.effect_params[1] = 1.0f;
			// Encode the scene-effector mask so world-submitted content isn't
			// filtered out by the shader's `effector_meta.w > 0.5` gate.
			// World-submission renders have no node-side filter state, so
			// default to "accept every WORLD-scope effector" — matches what
			// a world-scope effector is meant to do (affect everything in
			// this renderer's scenario).
			uint32_t world_scope_mask = 0u;
			for (uint32_t i = 0; i < scene_payload.size(); ++i) {
				if (scene_payload[i].scope_mode == SPHERE_EFFECTOR_SCOPE_WORLD) {
					world_scope_mask |= (1u << i);
				}
			}
			entry.effect_params[2] = _encode_u32_as_float_bits(world_scope_mask);
			entry.effect_params[3] = float(scene_payload.size());
			out.push_back(entry);
			if (r_submission_asset_ids) {
				// Primary/world-submission asset id is 0 (kPrimary*AssetId).
				r_submission_asset_ids->push_back(0ULL);
			}
		}
		return;
	}

	const bool log_enabled = _is_scene_director_log_enabled();
	const bool trace_enabled = GaussianSplatting::debug_trace_is_enabled();
	out.reserve(world->instance_store.instance_count());
	uint32_t skipped_instances = 0;
	uint32_t traced_total = 0;
	uint32_t traced_rotation_identity = 0;
	uint32_t traced_scale_identity = 0;
	uint32_t traced_translation_zero = 0;
	uint32_t traced_fully_identity = 0;
	for (const InstanceRecord &record : world->instance_store.records()) {
		if (!record.visible) {
			continue;
		}
		if (p_shadow_casters_only && !record.casts_shadow) {
			continue;
		}
		const AssetRecord *asset_record = world->instance_store.find_asset(record.asset_id);
		if (!asset_record || asset_record->data.is_null()) {
			if (log_enabled) {
				GS_LOG_RENDERER_DEBUG(vformat("[InstanceBuffer] SKIP asset_id=%s record=%s data=%s",
						String::num_uint64(record.asset_id),
						asset_record ? "found" : "NULL",
						(asset_record && asset_record->data.is_valid()) ? "valid" : "null"));
			}
			skipped_instances++;
			continue;
		}
		InstanceDataGPU entry = {};

		const Basis &basis = record.transform.basis;
		const Vector3 scale = basis.get_scale();
		const float sx = Math::abs(scale.x);
		const float sy = Math::abs(scale.y);
		const float sz = Math::abs(scale.z);
		const float uniform_scale = MAX(sx, MAX(sy, sz));

		Quaternion rotation = basis.get_rotation_quaternion().normalized();
		Quaternion inv_rotation = rotation.inverse();

		entry.rotation[0] = rotation.x;
		entry.rotation[1] = rotation.y;
		entry.rotation[2] = rotation.z;
		entry.rotation[3] = rotation.w;

		entry.inv_rotation[0] = inv_rotation.x;
		entry.inv_rotation[1] = inv_rotation.y;
		entry.inv_rotation[2] = inv_rotation.z;
		entry.inv_rotation[3] = inv_rotation.w;

		entry.translation_scale[0] = record.transform.origin.x;
		entry.translation_scale[1] = record.transform.origin.y;
		entry.translation_scale[2] = record.transform.origin.z;
		entry.translation_scale[3] = uniform_scale;

		entry.params[0] = record.opacity;
		entry.params[1] = record.lod_bias;
		entry.params[2] = record.wind_intensity;
		entry.params[3] = float(record.wind_mode);

		// GPU tag is only 32 bits; it carries a TRANSIENT truncated asset id that
		// update_instance_buffer() overwrites with the resolved dense slot. The
		// collision-free 64-bit submission identity is published in parallel via
		// r_submission_asset_ids below (and lives authoritatively in asset_records).
		entry.ids[0] = static_cast<uint32_t>(record.asset_id);
		uint32_t flags = record.flags;
		if (rotation.is_equal_approx(Quaternion())) {
			flags |= GS_INSTANCE_FLAG_ROTATION_IDENTITY;
		}
		if (Math::is_equal_approx(uniform_scale, 1.0f)) {
			flags |= GS_INSTANCE_FLAG_SCALE_IDENTITY;
		}
		if (record.transform.origin.is_zero_approx()) {
			flags |= GS_INSTANCE_FLAG_TRANSLATION_ZERO;
		}
		entry.ids[1] = flags;

		entry.lod[0] = record.last_lod;
		entry.lod[1] = 0;
		entry.wind_params[0] = record.wind_direction.x;
		entry.wind_params[1] = record.wind_direction.y;
		entry.wind_params[2] = record.wind_direction.z;
		entry.wind_params[3] = record.wind_frequency;
		entry.effect_params[0] = record.effect_position_scale;
		entry.effect_params[1] = record.effect_opacity_scale;
		entry.effect_params[2] = _encode_u32_as_float_bits(_build_scene_effector_mask_for_record(record, scene_payload));
		entry.effect_params[3] = float(scene_payload.size());

		out.push_back(entry);
		if (r_submission_asset_ids) {
			r_submission_asset_ids->push_back(record.asset_id);
		}
		if (trace_enabled) {
			const bool rotation_identity = (flags & GS_INSTANCE_FLAG_ROTATION_IDENTITY) != 0u;
			const bool scale_identity = (flags & GS_INSTANCE_FLAG_SCALE_IDENTITY) != 0u;
			const bool translation_zero = (flags & GS_INSTANCE_FLAG_TRANSLATION_ZERO) != 0u;
			traced_total++;
			traced_rotation_identity += rotation_identity ? 1u : 0u;
			traced_scale_identity += scale_identity ? 1u : 0u;
			traced_translation_zero += translation_zero ? 1u : 0u;
			traced_fully_identity += (rotation_identity && scale_identity && translation_zero) ? 1u : 0u;
		}
		if (log_enabled) {
			GS_LOG_RENDERER_DEBUG(vformat("[InstanceBuffer] idx=%d node=%s asset=%s lod=%u flags=0x%08X pos=(%.3f,%.3f,%.3f) scale=%.3f",
					out.size() - 1,
					String::num_uint64((uint64_t)record.node_id), String::num_uint64(record.asset_id), record.last_lod, record.flags,
					entry.translation_scale[0], entry.translation_scale[1], entry.translation_scale[2], entry.translation_scale[3]));
		}
	}

	if (log_enabled) {
		GS_LOG_RENDERER_DEBUG(vformat("[InstanceBuffer] total_instances=%d (world=%d)",
				out.size(), world->instance_store.instance_count()));
	}

	if (trace_enabled) {
		GaussianSplatting::debug_trace_record_instance_buffer(out.size(), world->instance_store.instance_count(), skipped_instances);
		GaussianSplatting::debug_trace_record_instance_flags(traced_total, traced_rotation_identity, traced_scale_identity,
				traced_translation_zero, traced_fully_identity);
		if (skipped_instances > 0 || out.size() != world->instance_store.instance_count()) {
			GaussianSplatting::debug_trace_record_event("instance_buffer",
					vformat("build out=%d world=%d skipped=%d",
							out.size(), world->instance_store.instance_count(), skipped_instances),
					skipped_instances > 0);
		}
	}
}

// Shared grading→GPU conversion. Mirrors the enabled/disabled logic used by
// TileRenderParamsBuilder::build_params so the binding-stage shader sees identical
// parameter semantics whether it reads the legacy UBO default or the new SSBO.
void GaussianSplatSceneDirector::fill_instance_grading_entry(const Ref<ColorGradingResource> &p_grading, InstanceGradingGPU &r_entry) {
	if (p_grading.is_valid() && p_grading->get_enabled()) {
		r_entry.primary[0] = 1.0f; // enabled = true
		r_entry.primary[1] = p_grading->get_exposure();
		r_entry.primary[2] = p_grading->get_contrast();
		r_entry.primary[3] = p_grading->get_saturation();
		r_entry.secondary[0] = p_grading->get_temperature();
		r_entry.secondary[1] = p_grading->get_tint();
		r_entry.secondary[2] = p_grading->get_hue_shift();
		r_entry.secondary[3] = 0.0f; // reserved
	} else {
		r_entry.primary[0] = 0.0f; // enabled = false
		r_entry.primary[1] = 0.0f; // exposure = 0
		r_entry.primary[2] = 1.0f; // contrast = 1
		r_entry.primary[3] = 1.0f; // saturation = 1
		r_entry.secondary[0] = 0.0f; // temperature
		r_entry.secondary[1] = 0.0f; // tint
		r_entry.secondary[2] = 0.0f; // hue_shift
		r_entry.secondary[3] = 0.0f; // reserved
	}
}

void GaussianSplatSceneDirector::build_instance_grading_buffer_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<InstanceGradingGPU> &out, bool p_shadow_casters_only) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	out.clear();

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	// Renderer-wide fallback; mirrors the legacy single-slot RenderConfig::color_grading
	// semantics when a record has no per-instance grading ref. Passed by value to the
	// helper so the renderer read is confined to this function.
	Ref<ColorGradingResource> renderer_default;
	if (p_renderer) {
		renderer_default = const_cast<GaussianSplatRenderer *>(p_renderer)->get_color_grading();
	}

	if (!world) {
		return;
	}

	// World-submission single-instance shim: mirrors the same path in
	// build_instance_buffer_for_renderer so the shader always has a 1-row
	// grading buffer indexable at splat_ref.instance_id == 0.
	if (world->instance_store.is_empty()) {
		if (world->submission_store.is_active() &&
				SubmissionStore::record_has_renderable_payload(world->submission_store.get_record())) {
			InstanceGradingGPU entry = {};
			GaussianSplatSceneDirector::fill_instance_grading_entry(renderer_default, entry);
			out.push_back(entry);
		}
		return;
	}

	out.reserve(world->instance_store.instance_count());
	for (const InstanceRecord &record : world->instance_store.records()) {
		if (!record.visible) {
			continue;
		}
		if (p_shadow_casters_only && !record.casts_shadow) {
			continue;
		}
		const AssetRecord *asset_record = world->instance_store.find_asset(record.asset_id);
		if (!asset_record || asset_record->data.is_null()) {
			// Must match build_instance_buffer_for_renderer's skip logic exactly
			// so rows stay 1:1 with instance_id.
			continue;
		}
		InstanceGradingGPU entry = {};
		const Ref<ColorGradingResource> &grading = record.color_grading.is_valid()
				? record.color_grading
				: renderer_default;
		GaussianSplatSceneDirector::fill_instance_grading_entry(grading, entry);
		out.push_back(entry);
	}
}

bool GaussianSplatSceneDirector::update_instance_color_grading(ObjectID p_node_id,
		const Ref<ColorGradingResource> &p_grading, bool p_force_refresh) {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_instance(p_node_id);
	if (!world) {
		return false;
	}
	InstanceRecord *record_ptr = world->instance_store.find_mutable(p_node_id);
	if (!record_ptr) {
		return false;
	}
	InstanceRecord &record = *record_ptr;
	const bool ref_changed = record.color_grading != p_grading;
	if (!ref_changed && !p_force_refresh) {
		// Per-frame apply / repeat-push path on an unchanged ref. Skip the
		// generation bump entirely — every frame would otherwise bust sort/
		// raster caches just because an unrelated setting re-ran settings apply.
		return true;
	}
	record.color_grading = p_grading;
	record.dirty = true;
	// Bump the instance generation so downstream caches (sort/raster) see the
	// change. Fires when the ref actually changes, or when the caller explicitly
	// asserts "values behind this ref just mutated" via p_force_refresh=true
	// (used by the ColorGradingResource `changed` signal handler for slider edits).
	world->instance_store.bump_generation();
	return true;
}

Ref<ColorGradingResource> GaussianSplatSceneDirector::get_instance_color_grading(ObjectID p_node_id) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = const_cast<GaussianSplatSceneDirector *>(this)->_find_world_for_instance(p_node_id);
	if (!world) {
		return Ref<ColorGradingResource>();
	}
	const InstanceRecord *record_ptr = world->instance_store.find(p_node_id);
	if (!record_ptr) {
		return Ref<ColorGradingResource>();
	}
	return record_ptr->color_grading;
}

void GaussianSplatSceneDirector::invalidate_grading_for_renderer(const GaussianSplatRenderer *p_renderer) {
	// Always bump the renderer-wide grading defaults counter, even when there is
	// no SharedWorld for this renderer. Renderer-only / direct-data flows (no
	// director instances) need this so the streaming upload fingerprint changes
	// on default grading edits — their fallback rows read from the renderer's
	// get_color_grading() value and must refresh.
	if (p_renderer) {
		// Atomic increment — the streaming orchestrator reads this from the
		// render thread to compute upload fingerprints without holding the
		// director's world_mutex. Relaxed ordering is fine: the counter is
		// a monotonic "did anything change since last frame" beacon.
		const_cast<GaussianSplatRenderer *>(p_renderer)
				->get_resource_state().instance_grading_defaults_generation
				.fetch_add(1, std::memory_order_relaxed);
	}
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = const_cast<SharedWorld *>(_find_world_for_renderer(p_renderer));
	if (!world) {
		return;
	}
	// Bump the instance generation so build_instance_grading_buffer_for_renderer
	// re-runs next frame. Records without per-instance grading fall back to the
	// renderer-wide default at build time, so those rows need to re-upload when
	// the default changes even though no per-instance ref mutated.
	world->instance_store.bump_generation();
}

uint64_t GaussianSplatSceneDirector::compute_color_grading_signature_for_renderer(
		const GaussianSplatRenderer *p_renderer, bool p_shadow_casters_only) const {
	// FNV-1a-esque rolling hash over every per-instance grading tied to the renderer,
	// including the renderer-wide default used as the fallback. The sort/raster cache
	// invalidation path hashes this in, so any node grading edit busts the cache.
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	uint64_t seed = 1469598103934665603ull;
	auto mix_u64 = [&](uint64_t v) {
		seed ^= v;
		seed *= 1099511628211ull;
	};
	auto mix_f = [&](float f) {
		union {
			float f;
			uint32_t u;
		} c = { f };
		mix_u64(uint64_t(c.u));
	};
	auto mix_grading = [&](const Ref<ColorGradingResource> &g) {
		if (!g.is_valid()) {
			mix_u64(0ull);
			return;
		}
		mix_u64(1ull);
		mix_u64(reinterpret_cast<uint64_t>(g.ptr()));
		mix_u64(g->get_enabled() ? 1ull : 0ull);
		mix_f(g->get_exposure());
		mix_f(g->get_contrast());
		mix_f(g->get_saturation());
		mix_f(g->get_temperature());
		mix_f(g->get_tint());
		mix_f(g->get_hue_shift());
	};

	Ref<ColorGradingResource> renderer_default;
	if (p_renderer) {
		renderer_default = const_cast<GaussianSplatRenderer *>(p_renderer)->get_color_grading();
	}
	mix_grading(renderer_default);

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return seed;
	}

	if (world->instance_store.is_empty()) {
		// World-submission shim uses the renderer default; already mixed.
		return seed;
	}

	for (const InstanceRecord &record : world->instance_store.records()) {
		// Mirror the visibility/shadow/asset filters from
		// build_instance_grading_buffer_for_renderer so signature reflects the exact
		// set of gradings the shader will actually see. Without the shadow filter,
		// grading edits on non-shadow-caster nodes would spuriously bust the shadow
		// sort/raster cache.
		if (!record.visible) {
			continue;
		}
		if (p_shadow_casters_only && !record.casts_shadow) {
			continue;
		}
		const AssetRecord *asset_record = world->instance_store.find_asset(record.asset_id);
		if (!asset_record || asset_record->data.is_null()) {
			continue;
		}
		mix_grading(record.color_grading.is_valid() ? record.color_grading : renderer_default);
	}
	return seed;
}

void GaussianSplatSceneDirector::collect_instance_node_ids_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<ObjectID> &r_node_ids) const {
	r_node_ids.clear();
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return;
	}
	const LocalVector<InstanceRecord> &instance_records = world->instance_store.records();
	r_node_ids.reserve(instance_records.size());
	for (uint32_t i = 0; i < instance_records.size(); i++) {
		const ObjectID node_id = instance_records[i].node_id;
		if (node_id == ObjectID()) {
			continue;
		}
		r_node_ids.push_back(node_id);
	}
}

uint64_t GaussianSplatSceneDirector::get_instance_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return 0;
	}
	return world->instance_store.generation();
}

uint64_t GaussianSplatSceneDirector::get_instance_asset_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return 0;
	}
	return world->instance_store.asset_generation();
}

uint32_t GaussianSplatSceneDirector::get_instance_count_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return 0;
	}
	return world->instance_store.instance_count();
}

void GaussianSplatSceneDirector::register_sphere_effector(ObjectID p_effector_id, const Transform3D &p_transform,
		float p_radius, float p_strength, float p_falloff, float p_frequency, bool p_enabled,
		bool p_affect_position, bool p_affect_opacity, float p_opacity_strength, float p_target_opacity,
		uint32_t p_layer_mask, uint32_t p_scope_mode, ObjectID p_scope_root_id, int32_t p_priority) {
	update_sphere_effector(p_effector_id, p_transform, p_radius, p_strength, p_falloff, p_frequency,
			p_enabled, p_affect_position, p_affect_opacity, p_opacity_strength, p_target_opacity,
			p_layer_mask, p_scope_mode, p_scope_root_id, p_priority);
}

void GaussianSplatSceneDirector::update_sphere_effector(ObjectID p_effector_id, const Transform3D &p_transform,
		float p_radius, float p_strength, float p_falloff, float p_frequency, bool p_enabled,
		bool p_affect_position, bool p_affect_opacity, float p_opacity_strength, float p_target_opacity,
		uint32_t p_layer_mask, uint32_t p_scope_mode, ObjectID p_scope_root_id, int32_t p_priority) {
	if (p_effector_id == ObjectID()) {
		return;
	}

	// #611/#628: declared before the lock so the blocking teardown of any renderer
	// the prune below frees runs only after world_mutex is released. The
	// RendererContractWorkQueue owns the release vector the prune fills; see the header.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *target_world = _get_world_for_effector(p_effector_id);
	SharedWorld *existing_world = _find_world_for_effector(p_effector_id);
	if (!target_world) {
		target_world = existing_world;
	}
	if (!target_world) {
		return;
	}

	auto remove_effector_from_world = [&](SharedWorld *p_world) {
		if (!p_world) {
			return;
		}
		if (!p_world->sphere_effector_store.remove(p_effector_id)) {
			return;
		}
		p_world->sphere_effector_store.bump_generation();
		_prune_world_if_unused(p_world->scenario, deferred_renderer_work.release_vector());
	};

	if (existing_world && existing_world != target_world) {
		remove_effector_from_world(existing_world);
	}

	const String effector_context = "sphere effector " + String::num_uint64((uint64_t)p_effector_id);
	const float radius = _sanitize_non_negative_float(p_radius, 0.0f, effector_context, "radius");
	const float strength = _sanitize_finite_float(p_strength, 0.0f, effector_context, "strength");
	const float falloff = _sanitize_min_float(p_falloff, 2.0f, 0.001f, effector_context, "falloff");
	const float frequency = _sanitize_min_float(p_frequency, 2.0f, 0.1f, effector_context, "frequency");
	const float opacity_strength = CLAMP(
			_sanitize_finite_float(p_opacity_strength, 1.0f, effector_context, "opacity_strength"),
			0.0f, 1.0f);
	if (!Math::is_equal_approx(opacity_strength, p_opacity_strength) && Math::is_finite(p_opacity_strength)) {
		WARN_PRINT(vformat("[GaussianSplatSceneDirector] opacity_strength for %s was clamped to [0, 1].", effector_context));
	}
	const float target_opacity = CLAMP(
			_sanitize_finite_float(p_target_opacity, 0.0f, effector_context, "target_opacity"),
			0.0f, 1.0f);
	if (!Math::is_equal_approx(target_opacity, p_target_opacity) && Math::is_finite(p_target_opacity)) {
		WARN_PRINT(vformat("[GaussianSplatSceneDirector] target_opacity for %s was clamped to [0, 1].", effector_context));
	}
	uint32_t scope_mode = p_scope_mode;
	if (scope_mode > SPHERE_EFFECTOR_SCOPE_EXPLICIT_ROOT) {
		WARN_PRINT(vformat("[GaussianSplatSceneDirector] Invalid scope_mode %u for %s; falling back to SUBTREE.",
				scope_mode, effector_context));
		scope_mode = SPHERE_EFFECTOR_SCOPE_SUBTREE;
	}
	if (scope_mode == SPHERE_EFFECTOR_SCOPE_EXPLICIT_ROOT && p_scope_root_id == ObjectID()) {
		WARN_PRINT(vformat("[GaussianSplatSceneDirector] Explicit scope requested for %s without a scope root. The effector will not match until a root is provided.",
				effector_context));
	}

	if (SphereEffectorRecord *record_ptr = target_world->sphere_effector_store.find_mutable(p_effector_id)) {
		SphereEffectorRecord &record = *record_ptr;
		bool dirty = false;
		if (!record.transform.is_equal_approx(p_transform)) {
			record.transform = p_transform;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.radius, radius)) {
			record.radius = radius;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.strength, strength)) {
			record.strength = strength;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.falloff, falloff)) {
			record.falloff = falloff;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.frequency, frequency)) {
			record.frequency = frequency;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.opacity_strength, opacity_strength)) {
			record.opacity_strength = opacity_strength;
			dirty = true;
		}
		if (!Math::is_equal_approx(record.target_opacity, target_opacity)) {
			record.target_opacity = target_opacity;
			dirty = true;
		}
		if (record.enabled != p_enabled) {
			record.enabled = p_enabled;
			dirty = true;
		}
		if (record.affect_position != p_affect_position) {
			record.affect_position = p_affect_position;
			dirty = true;
		}
		if (record.affect_opacity != p_affect_opacity) {
			record.affect_opacity = p_affect_opacity;
			dirty = true;
		}
		if (record.layer_mask != p_layer_mask) {
			record.layer_mask = p_layer_mask;
			dirty = true;
		}
		if (record.scope_mode != scope_mode) {
			record.scope_mode = scope_mode;
			dirty = true;
		}
		if (record.scope_root_id != p_scope_root_id) {
			record.scope_root_id = p_scope_root_id;
			dirty = true;
		}
		if (record.priority != p_priority) {
			record.priority = p_priority;
			dirty = true;
		}
		if (dirty) {
			target_world->sphere_effector_store.bump_generation();
		}
		return;
	}

	SphereEffectorRecord record;
	record.effector_id = p_effector_id;
	record.transform = p_transform;
	record.radius = radius;
	record.strength = strength;
	record.falloff = falloff;
	record.frequency = frequency;
	record.opacity_strength = opacity_strength;
	record.target_opacity = target_opacity;
	record.layer_mask = p_layer_mask;
	record.scope_mode = scope_mode;
	record.scope_root_id = p_scope_root_id;
	record.priority = p_priority;
	record.enabled = p_enabled;
	record.affect_position = p_affect_position;
	record.affect_opacity = p_affect_opacity;

	// append() stamps registration_serial (from the store-owned counter) and the
	// lookup slot before storing the copy, matching the prior inline order.
	target_world->sphere_effector_store.append(record);
	target_world->sphere_effector_store.bump_generation();
}

void GaussianSplatSceneDirector::unregister_sphere_effector(ObjectID p_effector_id) {
	if (p_effector_id == ObjectID()) {
		return;
	}

	// #611/#628: declared before the lock so the blocking teardown of any renderer
	// the prune below frees runs only after world_mutex is released. The
	// RendererContractWorkQueue owns the release vector the prune fills; see the header.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_effector(p_effector_id);
	if (!world) {
		return;
	}

	if (!world->sphere_effector_store.remove(p_effector_id)) {
		return;
	}
	world->sphere_effector_store.bump_generation();
	_prune_world_if_unused(world->scenario, deferred_renderer_work.release_vector());
}

bool GaussianSplatSceneDirector::submit_world_submission(const WorldSubmission &p_submission) {
	if (p_submission.owner_id == ObjectID() || !p_submission.scenario.is_valid()) {
		return false;
	}

	// Runtime world path: renderer mutation, ownership arbitration, and rollback stay centralized here.
	//
	// #611 PR B2 — THREE-PHASE STRUCTURE. This function used to hold `world_mutex`
	// across `_apply_world_submission_to_renderer`, which reaches a *blocking*
	// render-thread dispatch while the render thread needs that same mutex inside
	// the `*_for_renderer` builders. Every other such site is now deferred past the
	// unlock (PR A, PR B1); this one cannot be, because its result gates the
	// commit/reject decision and a destructor cannot feed a value back into a
	// function that has already chosen its return value.
	//
	//   Phase 1  arbitrate  under world_mutex   -- decide, snapshot, build contract
	//   Phase 2  apply      world_mutex RELEASED -- the blocking dispatch
	//   Phase 3  commit     under world_mutex   -- RE-VALIDATE, then commit or reject
	//
	// serialized end-to-end by `world_submission_apply_mutex`, which the render
	// thread never acquires and which is always taken before `world_mutex`.
	//
	// Phase 0: serialize. Two concurrent submissions must not interleave their
	// phases, or the committed record could be one whose contract was not the last
	// applied to the renderer. Declared FIRST so it is released LAST — after the
	// deferred queue below has flushed.
	MutexLock submission_lock(world_submission_apply_mutex);

	// #611: declared before every lock scope below so the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer), and the rollback queued on the reject paths, dispatch to
	// the render thread only after world_mutex is released. See
	// RendererContractWorkQueue in the header for the ordering rules.
	RendererContractWorkQueue deferred_renderer_work;

	const RID scenario = p_submission.scenario;
	// Carried across the phases. Deliberately NOT a `SharedWorld *`: no pointer
	// into `worlds` survives the unlocked gap (the world may be pruned, and an
	// insert may rehash the map), which is why phase 3 re-looks-up by scenario RID.
	Ref<GaussianSplatRenderer> target_renderer;
	SubmissionStore::WorldSubmissionRecord candidate_record;
	GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot target_previous_renderer_state;
	GaussianSplatRenderer::WorldSubmissionContract contract;
	bool needs_apply = false;

	// ---------------- Phase 1: arbitrate, under world_mutex ----------------
	{
		GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
		SharedWorld *world = _get_or_create_world_for_scenario(scenario, true, &deferred_renderer_work);
		if (!world) {
			return false;
		}

		const SubmissionStore::WorldSubmissionRecord target_previous_record = world->submission_store.get_record();
		const bool same_owner = target_previous_record.active && target_previous_record.owner_id == p_submission.owner_id;
		if (target_previous_record.active && !same_owner) {
			if (_is_world_submission_owner_live(world->submission_store.owner_id())) {
				// Arbitration reject. No renderer mutation has happened, so there is
				// nothing to roll back — and the apply that
				// _get_or_create_world_for_scenario may have queued is still the
				// correct one (the previous record is still live), so it is
				// deliberately left to flush.
				return false;
			}
		}

		target_previous_renderer_state = world->renderer.is_valid()
				? world->renderer->snapshot_world_submission_runtime_state()
				: GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot();
		SubmissionStore::store_submission(candidate_record, p_submission);
		candidate_record.renderer_restore_state = target_previous_record.active
				? (target_previous_record.renderer_restore_state.valid
						? target_previous_record.renderer_restore_state
						: target_previous_renderer_state)
				: target_previous_renderer_state;

		// Mirrors _apply_world_submission_to_renderer's early-out: an inactive
		// record or a renderer-less world applies nothing and is not a failure.
		if (candidate_record.active && world->renderer.is_valid()) {
			// A strong Ref, so the renderer cannot be freed under phase 2 if this
			// world is pruned in the gap. See the prune retry at the end of this
			// function for the reference-count consequence of holding it.
			target_renderer = world->renderer;
			contract = SubmissionStore::build_contract(candidate_record.renderer_restore_state, candidate_record);
			needs_apply = true;
		}
	}

	// ---------------- Phase 2: apply, with world_mutex RELEASED ----------------
	// This is the whole point of the restructure: the blocking render-thread
	// dispatch now happens with the mutex the render thread needs left free.
	const bool applied = needs_apply
			? _apply_world_submission_contract_unlocked(target_renderer, contract)
			: true;

	// ---------------- Phase 3: commit, under world_mutex ----------------
	//
	// RE-VALIDATION. Phase 1's arbitration was true when it was made, and it is
	// still true with respect to other submissions (they are serialized behind
	// `submission_lock`). It is NOT necessarily still true with respect to
	// everything else that can run on the main thread while phase 2 was unlocked,
	// so each fact phase 1 relied on is re-established here rather than assumed:
	//
	//   R1  the world still exists          -- re-looked-up by scenario RID, because
	//                                          it may have been pruned and because
	//                                          a rehash invalidates stale pointers.
	//   R2  it still has the SAME renderer  -- phase 2 mutated `target_renderer`;
	//                                          if the world's renderer was swapped,
	//                                          committing would record a contract
	//                                          the live renderer never received.
	//   R3  the submission slot is still    -- a different, live owner may have
	//       ours to take                       taken it in the gap; stomping it
	//                                          would silently evict a live world.
	//   R4  the owner's previous world      -- re-resolved, not carried from phase
	//       is re-resolved                     1: the owner may have been re-homed.
	//
	// When re-validation fails the outcome is the SAME as an apply failure —
	// rollback and reject — because the renderer has already been mutated and we
	// have decided not to commit. That is deliberate: it keeps `false` meaning
	// exactly one thing to GaussianSplatWorld3D ("your submission was not
	// installed, and the renderer was put back"), rather than inventing a third
	// outcome the caller has no handling for.
	bool committed = false;
	{
		GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
		SharedWorld *world = worlds.getptr(scenario); // R1
		const bool world_still_valid = world != nullptr;
		// R2: pointer identity, not Ref equality — we care whether this is the very
		// object phase 2 dispatched against.
		//
		// SCOPED TO `needs_apply` DELIBERATELY. When phase 2 dispatched nothing
		// (the world had no renderer, the only way needs_apply is false — see
		// SubmissionStore::store_submission, which always sets active = true), there is
		// no mutation for a renderer swap to invalidate, and the commit is pure
		// bookkeeping. Checking identity anyway would reject whenever a renderer
		// merely *appeared* in the gap, and a rejection is not a retry: it reads to
		// GaussianSplatWorld3D as "another world owns this scenario"
		// (nodes/gaussian_splat_world_3d.cpp), which stops it registering until some
		// unrelated event re-triggers _resubmit_world_submission_if_registered().
		// Inventing that stall would be worse than the inversion being fixed here.
		//
		// It also keeps every headless lane bit-identical to the pre-change
		// behaviour: with no RenderingDevice no world ever has a renderer, so
		// needs_apply is always false and this check never runs.
		const bool renderer_unchanged = !needs_apply ||
				(world_still_valid && world->renderer.ptr() == target_renderer.ptr());
		bool slot_still_available = false;
		if (world_still_valid) {
			// R3: re-run phase 1's arbitration predicate against the CURRENT record.
			const SubmissionStore::WorldSubmissionRecord &current = world->submission_store.get_record();
			const bool same_owner = current.active && current.owner_id == p_submission.owner_id;
			slot_still_available = !current.active || same_owner ||
					!_is_world_submission_owner_live(current.owner_id);
		}

		if (applied && world_still_valid && renderer_unchanged && slot_still_available) {
			// #611: if _get_or_create_world_for_scenario lazily created this world's
			// renderer in phase 1, it queued an apply of the PREVIOUS record. The
			// commit about to be made supersedes it, and the queue flushes after
			// world_mutex is released, so leaving it in place would re-apply the old
			// contract on top of the new one. On every failure exit the previous
			// record is still the live one, so their queued apply is correct and is
			// deliberately left alone.
			//
			// ORDER: cancel() clears the ENTIRE queue, so it must run BEFORE the
			// previous-world restore is queued below — otherwise it would silently
			// drop that restore too, and the evicted world's renderer would keep a
			// contract it no longer owns.
			deferred_renderer_work.cancel();

			// R4: re-resolve the owner's previous world under this lock rather than
			// carrying phase 1's pointer across the gap.
			SharedWorld *previous_world = _find_world_for_world_submission(p_submission.owner_id);
			if (previous_world && previous_world != world) {
				// Read the restore state BEFORE clearing the record it lives in.
				// Result-discarded restore, so it fits the deferral pattern exactly and
				// goes on the queue instead of dispatching under the lock.
				deferred_renderer_work.queue_restore(previous_world->renderer,
						previous_world->submission_store.get_record().renderer_restore_state);
				previous_world->submission_store.reset();
			}

			world->submission_store.mutable_record() = candidate_record;
			committed = true;
		} else {
			// Rollback + reject. Queued rather than dispatched inline, because a
			// restore's result is discarded and the queue flushes with the lock
			// released.
			//
			// ORDER — front-inserted, and this is load-bearing. A rejection leaves
			// the director's previous record P active, so the renderer must end up
			// holding P's contract or the two diverge. When phase 1 lazily created
			// this renderer it queued an apply of P, and
			// `target_previous_renderer_state` was snapshotted from the still-blank
			// renderer BEFORE that apply ran. The pre-#611 code restored inline and
			// let the queued apply(P) flush afterwards, making P the last writer.
			// Appending here would invert that — (apply(P), restore(blank)) — and
			// leave the renderer cleared under a live record. Front-insertion
			// reproduces the original ordering exactly, and degenerates to a plain
			// append when nothing else is queued.
			//
			// `target_renderer` is the renderer phase 2 actually mutated. On an R2
			// failure that is no longer the world's renderer, and restoring it — not
			// the new one — is precisely right: we undo what we did, and leave what
			// we never touched alone.
			if (target_renderer.is_valid()) {
				deferred_renderer_work.queue_restore_first(target_renderer, target_previous_renderer_state);
			}
		}
	}
	// world_mutex is released here.

	// LOAD-BEARING INVARIANT for the reject exit (carried forward from PR A):
	// deferring the queued apply means `target_previous_renderer_state` is
	// snapshotted from a renderer that has NOT yet had the previous record applied.
	// That divergence cannot reach the committed record only because
	// `snapshot_world_submission_runtime_state()` hardcodes `snapshot.valid = true`
	// (renderer/render_data_orchestrator.cpp). `valid` being unconditionally true
	// is what makes `candidate_record.renderer_restore_state` resolve to
	// `target_previous_record.renderer_restore_state` — captured before any of this
	// — and therefore byte-identical to the pre-change value. If `valid` ever
	// becomes conditional, re-derive this; the reject path would otherwise silently
	// start restoring to a different state.

	// Flush explicitly rather than leaving it to the destructor: the renderer Ref
	// below must be dropped BEFORE the prune retry, and the queued restore must run
	// before that Ref potentially frees the renderer.
	deferred_renderer_work.flush();

	// #611 PR B2 — REFERENCE-COUNT CONSEQUENCE, and why the prune retry exists.
	//
	// Holding `target_renderer` across phase 2 pushes the renderer's reference
	// count to 2 (the world's Ref plus ours). `_should_prune_world` prunes only at
	// `get_reference_count() <= 1`, so a concurrent `release_world_submission` in
	// the unlocked gap would have found the count inflated and skipped its prune —
	// leaking an empty SharedWorld, which is the exact trap release_world_submission
	// documents at its own capture-after-prune-decision site.
	//
	// Dropping the Ref here is safe (world_mutex is released, so the potentially
	// blocking ~GaussianSplatRenderer cannot invert), and the retry afterwards is a
	// no-op whenever the world is still in use.
	target_renderer.unref();
	if (needs_apply) {
		try_prune_world_if_unused(scenario);
	}

	return committed;
}

void GaussianSplatSceneDirector::release_world_submission(ObjectID p_owner_id) {
	// #611/#628: declared before the lock so both the queued restore and the
	// blocking teardown of any renderer the prune below frees dispatch/run only
	// after world_mutex is released. The single RendererContractWorkQueue owns the
	// deferred work AND the release vector, and destroys them in the load-bearing
	// order (the queued restore flushes first, THEN the released Ref drops) — the
	// order the pre-deferral inline code had (restore, then prune/free). See the
	// header.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_world_submission(p_owner_id);
	if (!world) {
		return;
	}
	const RID scenario = world->scenario;
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot restore_state =
			world->submission_store.get_record().renderer_restore_state;
	world->submission_store.reset();
	// Alias the queue's owned release vector so the delicate capture-after-prune
	// logic below reads exactly as the reviewed pre-S6 code did. This is a
	// reference, not an owning local: it runs no ~GaussianSplatRenderer at scope
	// exit (the queue does, in the load-bearing order documented on its declaration).
	LocalVector<Ref<GaussianSplatRenderer>> &deferred_renderer_release = deferred_renderer_work.release_vector();
	const uint32_t released_before = deferred_renderer_release.size();
	_prune_world_if_unused(scenario, deferred_renderer_release);
	// #611: capture the renderer for the deferred restore only AFTER the prune
	// decision. Holding a Ref across _should_prune_world would push the renderer's
	// reference count past its `<= 1` threshold and silently stop pruning — the
	// deferral would then leak a SharedWorld per release.
	if (deferred_renderer_release.size() > released_before) {
		// Pruned: the world entry is gone and its last renderer Ref now lives in
		// the queue's release vector. Restore it before that Ref drops (the queue
		// flushes the restore before its release vector destructs).
		deferred_renderer_work.queue_restore(deferred_renderer_release[released_before], restore_state);
	} else if (SharedWorld *surviving = worlds.getptr(scenario)) {
		deferred_renderer_work.queue_restore(surviving->renderer, restore_state);
	}
}

void GaussianSplatSceneDirector::try_prune_world_if_unused(const RID &p_scenario) {
	if (!p_scenario.is_valid()) {
		return;
	}
	// #611/#628: declared before the lock so the blocking teardown of any renderer
	// the prune below frees runs only after world_mutex is released. The
	// RendererContractWorkQueue owns the release vector the prune fills; see the header.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	_prune_world_if_unused(p_scenario, deferred_renderer_work.release_vector());
}

bool GaussianSplatSceneDirector::has_shared_world_for_scenario(const RID &p_scenario) const {
	if (!p_scenario.is_valid()) {
		return false;
	}
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	return worlds.getptr(p_scenario) != nullptr;
}

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)
uint32_t GaussianSplatSceneDirector::test_asset_record_count_for_scenario(const RID &p_scenario) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world) {
		return 0;
	}
	return world->instance_store.asset_count();
}

bool GaussianSplatSceneDirector::test_has_asset_record_for_scenario(const RID &p_scenario, ObjectID p_asset_object_id) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world) {
		return false;
	}
	return world->instance_store.has_asset(_asset_records_key(p_asset_object_id));
}
#endif

void GaussianSplatSceneDirector::teardown_world_for_scenario(const RID &p_scenario) {
	// Explicit, idempotent F6-reload teardown. See header comment for rationale.
	// Drops every Ref the SharedWorld holds (renderer, asset records, world-submission
	// data) so the renderer dtor can run as soon as the about-to-be-deleted scene
	// tree nodes drop their own Refs (which they do in the dtors that follow
	// PREDELETE). Bypasses _should_prune_world() on purpose -- that check is
	// designed for in-tree unregistration where reference-holding peers may still
	// rebind; in the PREDELETE/F6 case the holders themselves are being destroyed.
	if (!p_scenario.is_valid()) {
		return;
	}
	// #611: the renderer's world-submission clear + teardown both block on a
	// render-thread dispatch, and the render thread can be blocked acquiring
	// world_mutex inside a *_for_renderer builder. Move the renderer Ref out of
	// the map under the lock, then run those blocking calls AFTER releasing the
	// lock. Declared before the lock so it drops after the MutexLock scope ends.
	Ref<GaussianSplatRenderer> deferred_renderer_release;
	{
		GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
		SharedWorld *entry = worlds.getptr(p_scenario);
		if (!entry) {
			// Already torn down (e.g. another peer on the same scenario beat us to
			// the punch) or never existed -- both are no-ops.
			return;
		}

		// Clear every Ref-holding field on the SharedWorld and move the renderer
		// Ref out so the map entry is no longer the owner. The moved-out Ref is the
		// last owner and is released only after world_mutex is dropped, below.
		entry->instance_store.clear();
		entry->sphere_effector_store.clear();
		entry->submission_store.reset();
		deferred_renderer_release = std::move(entry->renderer);

		// Erase the map entry -- last reference holder for everything above.
		worlds.erase(p_scenario);
	}

	// world_mutex is released. Drop the renderer's world-submission contract (so it
	// no longer points at the gaussian_data / payload_source) and then the renderer
	// itself; both may block waiting on the render thread.
	if (deferred_renderer_release.is_valid()) {
		deferred_renderer_release->clear_world_submission_contract();
	}
	// deferred_renderer_release drops here -> ~GaussianSplatRenderer runs outside
	// world_mutex.
}

void GaussianSplatSceneDirector::release_all_worlds() {
	// #329: same teardown `~GaussianSplatSceneDirector` performs via worlds.clear(),
	// but callable while the surrounding engine is still fully alive.
	//
	// The --gs-gpu-test harness owns the RenderingDevice it created, and that device
	// must be destroyed before Main::test_cleanup() deletes the Engine singleton
	// (RenderingDevice::~RenderingDevice reads Engine for GPU memory tracking).
	// Module uninitialization inside test_cleanup() is where the director would
	// otherwise drop its renderers -- i.e. strictly after the device is gone. Neither
	// teardown order works on its own; the harness therefore calls this first, while
	// BOTH the device and the GaussianSplatManager are live, which is exactly the
	// ordering #589 requires for renderer teardown.
	//
	// Snapshot the scenario keys under the lock, then reuse teardown_world_for_scenario()
	// per key with the lock released: that path already implements the #611 deferred
	// renderer-release discipline (dropping a renderer Ref under world_mutex is a
	// lock-order inversion against the render thread).
	LocalVector<RID> scenarios;
	{
		GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
		scenarios.reserve(worlds.size());
		for (const KeyValue<RID, SharedWorld> &E : worlds) {
			scenarios.push_back(E.key);
		}
	}

	for (const RID &scenario : scenarios) {
		teardown_world_for_scenario(scenario);
	}
}

bool GaussianSplatSceneDirector::get_world_submission(ObjectID p_owner_id, WorldSubmission *r_submission) const {
	ERR_FAIL_NULL_V(r_submission, false);

	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_world_submission(p_owner_id);
	if (!world || !world->submission_store.is_active()) {
		return false;
	}

	_copy_world_submission_record(*world, world->submission_store.get_record(), r_submission);
	return true;
}

bool GaussianSplatSceneDirector::get_world_submission_for_scenario(const RID &p_scenario, WorldSubmission *r_submission) const {
	ERR_FAIL_NULL_V(r_submission, false);

	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world || !world->submission_store.is_active()) {
		return false;
	}

	_copy_world_submission_record(*world, world->submission_store.get_record(), r_submission);
	return true;
}

bool GaussianSplatSceneDirector::has_world_submission_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || !world->submission_store.is_active()) {
		return false;
	}

	return SubmissionStore::record_has_renderable_payload(world->submission_store.get_record());
}

bool GaussianSplatSceneDirector::get_submission_residency_hint_for_renderer(const GaussianSplatRenderer *p_renderer,
		int32_t *r_hint, String *r_source) const {
	ERR_FAIL_NULL_V(r_hint, false);

	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	if (const SharedWorld *world = _find_world_for_renderer(p_renderer)) {
		const bool world_submission_has_renderable_data =
				SubmissionStore::record_has_renderable_payload(world->submission_store.get_record());
		if (world->submission_store.is_active() && world_submission_has_renderable_data &&
				world->submission_store.get_record().has_desired_residency_hint) {
			*r_hint = world->submission_store.get_record().desired_residency_hint;
			if (r_source) {
				*r_source = "world_submission";
			}
			return true;
		}

		bool found_instance_hint = false;
		int32_t instance_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
		for (const InstanceRecord &record : world->instance_store.records()) {
			if (!record.has_desired_residency_hint) {
				continue;
			}
			if (!found_instance_hint) {
				found_instance_hint = true;
				instance_hint = record.desired_residency_hint;
				continue;
			}
			if (instance_hint != record.desired_residency_hint) {
				if (r_source) {
					*r_source = "mixed_instance_submissions";
				}
				return false;
			}
		}
		if (found_instance_hint) {
			*r_hint = instance_hint;
			if (r_source) {
				*r_source = "instance_submission";
			}
			return true;
		}
	}

	if (r_source) {
		*r_source = "none";
	}
	return false;
}

GaussianSplatSceneDirector::SubmissionCounts GaussianSplatSceneDirector::get_submission_counts() const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);

	SubmissionCounts counts;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		counts.instance_submissions += E.value.instance_store.instance_count();
		if (E.value.submission_store.is_active()) {
			counts.world_submissions++;
		}
	}
	return counts;
}

namespace {

static int _metadata_int(const Dictionary &p_metadata, const StringName &p_key, int p_default) {
	if (!p_metadata.has(p_key)) {
		return p_default;
	}
	const Variant value = p_metadata[p_key];
	if (value.get_type() == Variant::FLOAT) {
		return int((double)value);
	}
	return int(value);
}

static double _metadata_double(const Dictionary &p_metadata, const StringName &p_key, double p_default) {
	if (!p_metadata.has(p_key)) {
		return p_default;
	}
	const Variant value = p_metadata[p_key];
	if (value.get_type() == Variant::INT) {
		return double(int64_t(value));
	}
	return (double)value;
}

static bool _asset_requests_full_fidelity_runtime(const Ref<GaussianSplatAsset> &p_asset) {
	if (p_asset.is_null()) {
		return false;
	}
	const Dictionary import_metadata = p_asset->get_import_metadata();
	const int import_max_splats = _metadata_int(import_metadata, StringName("max_splats"), -1);
	const double density_multiplier = _metadata_double(import_metadata, StringName("density_multiplier"), 1.0);
	return import_max_splats == 0 && density_multiplier >= 0.999;
}

} // namespace

void GaussianSplatSceneDirector::collect_instance_assets_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<InstanceAssetRegistration> &out, bool p_shadow_casters_only) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	out.clear();

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || world->instance_store.assets_empty()) {
		return;
	}

	HashSet<uint64_t> selected_asset_ids;
	selected_asset_ids.reserve(world->instance_store.asset_count());
	for (const InstanceRecord &record : world->instance_store.records()) {
		if (!record.visible) {
			continue;
		}
		if (p_shadow_casters_only && !record.casts_shadow) {
			continue;
		}
		if (record.asset_id != 0) {
			selected_asset_ids.insert(record.asset_id);
		}
	}

	out.reserve(selected_asset_ids.size());
	for (const uint64_t &asset_id : selected_asset_ids) {
		const AssetRecord *record = world->instance_store.find_asset(asset_id);
		if (!record || record->data.is_null()) {
			continue;
		}
		InstanceAssetRegistration entry;
		// Carry the FULL 64-bit submission identity through to the renderer remap.
		// This is the collision-free key into PublishedInstanceAssetRemap; truncating
		// it here would alias two assets whose ObjectIDs share the low 32 bits.
		entry.asset_id = asset_id;
		entry.data = record->data;
		entry.edited_version = record->edited_version;
		entry.requests_full_fidelity_runtime = _asset_requests_full_fidelity_runtime(record->asset);
		out.push_back(entry);
	}
}

void GaussianSplatSceneDirector::collect_registered_assets_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<InstanceAssetRegistration> &out) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	out.clear();

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || world->instance_store.assets_empty()) {
		return;
	}

	out.reserve(world->instance_store.asset_count());
	for (const KeyValue<uint64_t, AssetRecord> &E : world->instance_store.assets()) {
		if (E.key == 0 || E.value.data.is_null()) {
			continue;
		}
		InstanceAssetRegistration entry;
		// Carry the FULL 64-bit asset_records key (host submission identity) through
		// to the renderer remap so distinct ObjectIDs never alias a dense slot.
		entry.asset_id = E.key;
		entry.data = E.value.data;
		entry.edited_version = E.value.edited_version;
		entry.requests_full_fidelity_runtime = _asset_requests_full_fidelity_runtime(E.value.asset);
		out.push_back(entry);
	}
}



Ref<GaussianSplatRenderer> GaussianSplatSceneDirector::get_shared_renderer(World3D *p_world) {
	// #611: declared before the lock so the apply that
	// _get_or_create_world_for_scenario may queue (when it lazily creates this
	// world's renderer) dispatches to the render thread only after world_mutex is
	// released. See RendererContractWorkQueue in the header for the ordering rules.
	RendererContractWorkQueue deferred_renderer_work;
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	SharedWorld *world = _get_or_create_world(p_world, true, &deferred_renderer_work);
	if (!world) {
		return Ref<GaussianSplatRenderer>();
	}
	return world->renderer;
}
