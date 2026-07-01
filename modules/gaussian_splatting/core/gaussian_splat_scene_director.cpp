#include "gaussian_splat_scene_director.h"

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

static bool _is_scene_director_log_enabled() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return false;
	}
	if (ps->get_setting("rendering/gaussian_splatting/debug/enable_all_debug", false)) {
		return true;
	}
	if (ps->get_setting("rendering/gaussian_splatting/debug/enable_frame_logging", false)) {
		return true;
	}
	return ps->get_setting("rendering/gaussian_splatting/debug/enable_data_logging", false);
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
static const StringName &NODE_SCENE_EFFECTORS_ENABLED_PROPERTY() { static const StringName s("rendering/scene_effectors_enabled"); return s; }
static const StringName &NODE_SCENE_EFFECTOR_LAYER_MASK_PROPERTY() { static const StringName s("rendering/scene_effector_layer_mask"); return s; }
static const StringName &NODE_SCENE_EFFECTOR_SCOPE_ROOT_PROPERTY() { static const StringName s("rendering/scene_effector_scope_root"); return s; }
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

struct NodeSceneEffectorFilterState {
	bool enabled = true;
	uint32_t layer_mask = 1u;
	bool has_scope_filter = false;
	bool scope_filter_valid = true;
	ObjectID scope_root_id;
};

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

static ObjectID _resolve_scope_root_from_path(Node *p_owner, const NodePath &p_scope_path, bool *r_has_scope_filter, bool *r_scope_valid) {
	if (r_has_scope_filter) {
		*r_has_scope_filter = !p_scope_path.is_empty();
	}
	if (r_scope_valid) {
		*r_scope_valid = true;
	}
	if (!p_owner || p_scope_path.is_empty()) {
		return ObjectID();
	}

	Node *scope_root = p_owner->get_node_or_null(p_scope_path);
	if (!scope_root) {
		if (r_scope_valid) {
			*r_scope_valid = false;
		}
		return ObjectID();
	}
	if (!(scope_root == p_owner || scope_root->is_ancestor_of(p_owner))) {
		if (r_scope_valid) {
			*r_scope_valid = false;
		}
		return ObjectID();
	}
	return scope_root->get_instance_id();
}

static NodeSceneEffectorFilterState _get_node_scene_effector_filter_state(const Node3D *p_node) {
	NodeSceneEffectorFilterState filter;
	if (!p_node) {
		return filter;
	}

	bool valid = false;
	const Variant enabled_variant = p_node->get(NODE_SCENE_EFFECTORS_ENABLED_PROPERTY(), &valid);
	if (valid) {
		if (enabled_variant.get_type() == Variant::BOOL) {
			filter.enabled = (bool)enabled_variant;
		} else if (enabled_variant.get_type() == Variant::INT) {
			filter.enabled = int64_t(enabled_variant) != 0;
		}
	}

	valid = false;
	const Variant mask_variant = p_node->get(NODE_SCENE_EFFECTOR_LAYER_MASK_PROPERTY(), &valid);
	if (valid) {
		if (mask_variant.get_type() == Variant::INT) {
			const int64_t raw_mask = int64_t(mask_variant);
			filter.layer_mask = raw_mask < 0 ? 0u : uint32_t(raw_mask);
		}
	}

	valid = false;
	const Variant scope_variant = p_node->get(NODE_SCENE_EFFECTOR_SCOPE_ROOT_PROPERTY(), &valid);
	if (valid && scope_variant.get_type() == Variant::NODE_PATH) {
		const NodePath scope_path = scope_variant;
		filter.scope_root_id = _resolve_scope_root_from_path(
				const_cast<Node3D *>(p_node), scope_path,
				&filter.has_scope_filter, &filter.scope_filter_valid);
	}

	return filter;
}

static uint32_t _get_effector_scope_specificity(uint32_t p_scope_mode, bool p_has_scope_root) {
	if (!p_has_scope_root) {
		return 0u;
	}
	switch (p_scope_mode) {
		case GaussianSplatSceneDirector::SPHERE_EFFECTOR_SCOPE_EXPLICIT_ROOT:
			return 2u;
		case GaussianSplatSceneDirector::SPHERE_EFFECTOR_SCOPE_SUBTREE:
			return 1u;
		default:
			return 0u;
	}
}

static float _encode_u32_as_float_bits(uint32_t p_value) {
	float encoded = 0.0f;
	static_assert(sizeof(encoded) == sizeof(p_value), "Expected float/u32 bit widths to match");
	memcpy(&encoded, &p_value, sizeof(encoded));
	return encoded;
}

static bool _node_matches_scene_effector_selection(const Node3D *p_node,
		const NodeSceneEffectorFilterState &p_filter,
		const GaussianSplatSceneDirector::SphereEffectorSelection &p_selection) {
	if (!p_node || !p_filter.enabled || p_filter.layer_mask == 0u) {
		return false;
	}
	if (p_filter.has_scope_filter && !p_filter.scope_filter_valid) {
		return false;
	}
	if ((p_selection.layer_mask & p_filter.layer_mask) == 0u) {
		return false;
	}
	if (p_filter.has_scope_filter) {
		return p_selection.scope_root_id != ObjectID() && p_selection.scope_root_id == p_filter.scope_root_id;
	}
	if (p_selection.scope_mode == GaussianSplatSceneDirector::SPHERE_EFFECTOR_SCOPE_WORLD) {
		return true;
	}
	Node *scope_root = Object::cast_to<Node>(ObjectDB::get_instance(p_selection.scope_root_id));
	return scope_root && (scope_root == p_node || scope_root->is_ancestor_of(p_node));
}


GaussianSplatRenderer::WorldSubmissionContract GaussianSplatSceneDirector::_build_world_submission_contract(
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state,
		const SharedWorld::WorldSubmissionRecord &p_record) {
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

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_or_create_world_for_scenario(const RID &p_scenario, bool p_require_renderer) {
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
		if (entry->world_submission.active) {
			const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot renderer_state =
					entry->renderer->snapshot_world_submission_runtime_state();
			if (!entry->world_submission.renderer_restore_state.valid) {
				entry->world_submission.renderer_restore_state = renderer_state;
			}
			_apply_world_submission_to_renderer(*entry, entry->world_submission,
					entry->world_submission.renderer_restore_state);
		}
	}

	return entry;
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_or_create_world(World3D *p_world, bool p_require_renderer) {
	ERR_FAIL_NULL_V(p_world, nullptr);
	return _get_or_create_world_for_scenario(p_world->get_scenario(), p_require_renderer);
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_world_for_instance(ObjectID p_node_id) {
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
	return _get_or_create_world(world);
}

GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_find_world_for_instance(ObjectID p_node_id) {
	for (KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.instance_lookup.has(p_node_id)) {
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
		if (E.value.sphere_effector_lookup.has(p_effector_id)) {
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

void GaussianSplatSceneDirector::_build_sorted_sphere_effector_payload(const SharedWorld &p_world,
		LocalVector<SphereEffectorSelection> &r_out) {
	r_out.clear();
	if (p_world.sphere_effectors.is_empty()) {
		return;
	}

	struct Candidate {
		SphereEffectorSelection selection;
		uint64_t registration_serial = 0u;
		uint32_t specificity = 0u;
	};

	// Sort deterministically by (priority, specificity, registration serial,
	// effector_id). Uses the record's monotonic registration serial instead of
	// the effector's scene-path — the path would require a live `get_path()`
	// call from the render thread (not thread-safe) and would reorder
	// equal-priority slots on rename without bumping any generation, leaving
	// cached per-instance masks pointing at the wrong slots.
	auto candidate_precedes = [](const Candidate &p_a, const Candidate &p_b) {
		if (p_a.selection.priority != p_b.selection.priority) {
			return p_a.selection.priority > p_b.selection.priority;
		}
		if (p_a.specificity != p_b.specificity) {
			return p_a.specificity > p_b.specificity;
		}
		if (p_a.registration_serial != p_b.registration_serial) {
			return p_a.registration_serial < p_b.registration_serial;
		}
		return (uint64_t)p_a.selection.effector_id < (uint64_t)p_b.selection.effector_id;
	};

	LocalVector<Candidate> candidates;
	candidates.reserve(p_world.sphere_effectors.size());
	for (const SphereEffectorRecord &record : p_world.sphere_effectors) {
		// Filter ineligible records BEFORE the slot cap so ineligible
		// effectors can't consume a top-N slot and push a valid effector out.
		//
		// Rejected here:
		//   - disabled / zero-radius (GPU rejects in the packers anyway)
		//   - non-finite transform origins (same)
		//   - can't-contribute: even if the affect_* flags are set, the
		//     effector is inert when position strength is zero AND opacity
		//     can't move the splat (opacity_strength==0 or target==1.0).
		//     Without this, e.g. an affect_opacity-only effector with
		//     target_opacity=1.0 still wins slots but contributes nothing.
		if (!record.enabled || record.radius <= 0.0f ||
				(!record.affect_position && !record.affect_opacity)) {
			continue;
		}
		const Vector3 &center = record.transform.origin;
		if (!Math::is_finite(center.x) || !Math::is_finite(center.y) || !Math::is_finite(center.z)) {
			continue;
		}
		const bool position_can_contribute = record.affect_position && !Math::is_zero_approx(record.strength);
		// target_opacity==1.0 is the neutral "fully opaque" target; combined
		// with non-zero opacity_strength it still produces no visible change
		// for already-opaque splats, so treat it as inert here. Matches the
		// diagnostic gate in `get_scene_effector_debug_state_for_instance`.
		const bool opacity_can_contribute = record.affect_opacity &&
				!Math::is_zero_approx(record.opacity_strength) &&
				!Math::is_equal_approx(record.target_opacity, 1.0f);
		if (!position_can_contribute && !opacity_can_contribute) {
			continue;
		}

		// SCOPE_SUBTREE / SCOPE_EXPLICIT_ROOT carry a resolved `scope_root_id`
		// populated on the main thread at register/update time. Revalidate
		// that the ObjectID still resolves to a live Node — scope-root
		// deletion after sync would otherwise keep the effector eligible
		// while matching no instances. `ObjectDB::get_instance()` is
		// thread-safe, and we mutate the cached `scope_root_valid` flag
		// under `world_mutex` (via const_cast — the world is actually
		// addressable through the non-const caller that holds the mutex)
		// plus bump `sphere_effector_generation` so cached instance
		// bitmasks invalidate and rebuild without the dropped effector.
		if (record.scope_mode != SPHERE_EFFECTOR_SCOPE_WORLD) {
			const bool scope_alive = record.scope_root_id != ObjectID() &&
					Object::cast_to<Node>(ObjectDB::get_instance(record.scope_root_id)) != nullptr;
			if (!scope_alive) {
				if (record.scope_root_valid) {
					const_cast<SphereEffectorRecord &>(record).scope_root_valid = false;
					const_cast<SharedWorld &>(p_world).sphere_effector_generation++;
				}
				continue;
			}
			if (!record.scope_root_valid) {
				// Flipped back to alive (rare — node re-registered at the same ID).
				const_cast<SphereEffectorRecord &>(record).scope_root_valid = true;
				const_cast<SharedWorld &>(p_world).sphere_effector_generation++;
			}
		}

		Candidate candidate;
		candidate.selection.effector_id = record.effector_id;
		candidate.selection.scenario = p_world.scenario;
		candidate.selection.transform = record.transform;
		candidate.selection.center = record.transform.origin;
		candidate.selection.radius = record.radius;
		candidate.selection.strength = record.strength;
		candidate.selection.falloff = record.falloff;
		candidate.selection.frequency = record.frequency;
		candidate.selection.opacity_strength = record.opacity_strength;
		candidate.selection.target_opacity = record.target_opacity;
		candidate.selection.layer_mask = record.layer_mask;
		candidate.selection.scope_mode = record.scope_mode;
		candidate.selection.scope_root_id = record.scope_root_id;
		candidate.selection.priority = record.priority;
		candidate.selection.enabled = record.enabled;
		candidate.selection.affect_position = record.affect_position;
		candidate.selection.affect_opacity = record.affect_opacity;
		candidate.registration_serial = record.registration_serial;
		candidate.specificity = _get_effector_scope_specificity(record.scope_mode, record.scope_root_id != ObjectID());

		int insert_at = candidates.size();
		while (insert_at > 0 && candidate_precedes(candidate, candidates[insert_at - 1])) {
			insert_at--;
		}
		candidates.insert(insert_at, candidate);
	}

	const uint32_t capped_count = MIN<uint32_t>(candidates.size(), GS_MAX_SPHERE_EFFECTORS);
	r_out.reserve(capped_count);
	for (uint32_t i = 0; i < capped_count; i++) {
		r_out.push_back(candidates[i].selection);
	}
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
		if (E.value.world_submission.active && E.value.world_submission.owner_id == p_owner_id) {
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
		if (E.value.world_submission.active && E.value.world_submission.owner_id == p_owner_id) {
			return &E.value;
		}
	}
	return nullptr;
}

bool GaussianSplatSceneDirector::_populate_gaussian_data_from_asset(const Ref<GaussianSplatAsset> &p_asset, Ref<GaussianData> &r_data) {
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

bool GaussianSplatSceneDirector::_retain_asset_record(SharedWorld &p_world, const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id) {
	if (p_asset.is_null()) {
		return false;
	}
	uint32_t edited_version = 0;
#ifdef TOOLS_ENABLED
	edited_version = p_asset->get_edited_version();
#endif
	SharedWorld::AssetRecord *record = p_world.asset_records.getptr(p_asset_id);
	if (!record) {
		SharedWorld::AssetRecord new_record;
		new_record.asset = p_asset;
		if (!_populate_gaussian_data_from_asset(p_asset, new_record.data)) {
			GS_LOG_WARN_DEFAULT("[GaussianSplatSceneDirector] Failed to build GaussianData from asset.");
			return false;
		}
		new_record.edited_version = edited_version;
		new_record.refcount = 1;
		p_world.asset_records.insert(p_asset_id, new_record);
		_bump_instance_generation(p_world.instance_generation);
		_bump_instance_asset_generation(p_world.instance_asset_generation);
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
		_bump_instance_generation(p_world.instance_generation);
		_bump_instance_asset_generation(p_world.instance_asset_generation);
	}
	record->refcount++;
	return true;
}

bool GaussianSplatSceneDirector::_refresh_asset_record(SharedWorld &p_world, const Ref<GaussianSplatAsset> &p_asset, uint64_t p_asset_id) {
	if (p_asset.is_null()) {
		return false;
	}
	SharedWorld::AssetRecord *record = p_world.asset_records.getptr(p_asset_id);
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
	_bump_instance_generation(p_world.instance_generation);
	_bump_instance_asset_generation(p_world.instance_asset_generation);
	return true;
}

void GaussianSplatSceneDirector::_release_asset_record(SharedWorld &p_world, uint64_t p_asset_id) {
	SharedWorld::AssetRecord *record = p_world.asset_records.getptr(p_asset_id);
	if (!record) {
		return;
	}
	if (record->refcount > 0) {
		record->refcount--;
	}
	if (record->refcount == 0) {
		p_world.asset_records.erase(p_asset_id);
		_bump_instance_asset_generation(p_world.instance_asset_generation);
	}
}

bool GaussianSplatSceneDirector::_is_world_submission_owner_live(ObjectID p_owner_id) {
	if (p_owner_id == ObjectID()) {
		return false;
	}
	return ObjectDB::get_instance(p_owner_id) != nullptr;
}

void GaussianSplatSceneDirector::_store_world_submission_record(SharedWorld::WorldSubmissionRecord &r_record,
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

bool GaussianSplatSceneDirector::_world_submission_record_has_renderable_payload(
		const SharedWorld::WorldSubmissionRecord &p_record) {
	const bool has_resident_data = p_record.gaussian_data.is_valid() &&
			p_record.gaussian_data->get_count() > 0;
	const bool has_file_backed_payload = p_record.payload_source.is_valid() &&
			p_record.payload_source->is_valid() &&
			p_record.payload_source->get_count() > 0;
	return has_resident_data || has_file_backed_payload;
}

void GaussianSplatSceneDirector::_copy_world_submission_record(const SharedWorld &p_world,
		const SharedWorld::WorldSubmissionRecord &p_record, WorldSubmission *r_submission) {
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
		const SharedWorld::WorldSubmissionRecord &p_record,
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot &p_renderer_state) {
	if (!p_record.active || !p_world.renderer.is_valid()) {
		return true;
	}

	GaussianSplatRenderer *renderer = p_world.renderer.ptr();
	ERR_FAIL_NULL_V(renderer, false);
	const GaussianSplatRenderer::WorldSubmissionContract contract =
			_build_world_submission_contract(p_renderer_state, p_record);
	const Error err = renderer->apply_world_submission_contract(contract);
	if (err != OK) {
		GS_LOG_RENDERER_ERROR(vformat("[GaussianSplatSceneDirector] Failed to apply world submission contract (err=%d).", err));
		return false;
	}
	return true;
}

bool GaussianSplatSceneDirector::_should_prune_world(const SharedWorld &p_world) const {
	if (!p_world.instances.is_empty()) {
		return false;
	}
	if (!p_world.sphere_effectors.is_empty()) {
		return false;
	}
	if (p_world.world_submission.active) {
		return false;
	}
	if (p_world.renderer.is_null()) {
		return true;
	}

	// Preserve the SharedWorld while some external owner (for example a node that
	// temporarily left the tree, an active world node, or editor tooling) still
	// holds the shared renderer Ref. Otherwise re-registration can desynchronize
	// the director from that retained renderer.
	return p_world.renderer->get_reference_count() <= 1;
}

void GaussianSplatSceneDirector::_prune_world_if_unused(const RID &p_scenario) {
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world) {
		return;
	}
	if (!_should_prune_world(*world)) {
		return;
	}
	worlds.erase(p_scenario);
}


void GaussianSplatSceneDirector::register_instance(ObjectID p_node_id, const Ref<GaussianSplatAsset> &p_asset,
        const Transform3D &p_transform, float p_opacity, float p_lod_bias, uint32_t p_flags, bool p_casts_shadow,
        float p_wind_intensity, uint32_t p_wind_mode, const Vector3 &p_wind_direction, float p_wind_frequency,
        bool p_visible, bool p_has_desired_residency_hint, int32_t p_desired_residency_hint,
        float p_effect_position_scale, float p_effect_opacity_scale) {
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id);
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
		uint32_t *stale_index = other.instance_lookup.getptr(p_node_id);
		if (!stale_index || *stale_index >= other.instances.size()) {
			continue;
		}
		const uint32_t idx = *stale_index;
		const uint64_t stale_asset_id = other.instances[idx].asset_id;
		const uint32_t last_index = other.instances.size() - 1;
		if (idx != last_index) {
			other.instances[idx] = other.instances[last_index];
			other.instance_lookup[other.instances[idx].node_id] = idx;
		}
		other.instances.remove_at(last_index);
		other.instance_lookup.erase(p_node_id);
		_bump_instance_generation(other.instance_generation);
		_bump_instance_asset_generation(other.instance_asset_generation);
		if (stale_asset_id != 0) {
			_release_asset_record(other, stale_asset_id);
		}
		break; // an instance can only live in one SharedWorld at a time
	}
	if (world->renderer.is_valid()) {
		const auto &resource_state = world->renderer->get_resource_state();
		if (!resource_state.gpu_resources_initialized && !resource_state.gpu_initialization_pending) {
			world->renderer->initialize();
		}
	}
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
			vformat("OK: asset_id=%s instances_before=%d", String::num_uint64(asset_id), world->instances.size()),
			false);

	uint32_t *index_ptr = world->instance_lookup.getptr(p_node_id);
	if (index_ptr && *index_ptr < world->instances.size()) {
		InstanceRecord &record = world->instances[*index_ptr];
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
			if (world->asset_records.has(asset_id)) {
				if (!_refresh_asset_record(*world, p_asset, asset_id)) {
					return;
				}
			} else {
				if (!_retain_asset_record(*world, p_asset, asset_id)) {
					return;
				}
			}
		}
		if (record.asset_id != asset_id) {
			if (!_retain_asset_record(*world, p_asset, asset_id)) {
				return;
			}
			_release_asset_record(*world, record.asset_id);
			record.asset_id = asset_id;
			record.last_lod = 0;
			dirty = true;
			asset_selection_dirty = true;
		}
		record.dirty = record.dirty || dirty;
		if (dirty) {
			_bump_instance_generation(world->instance_generation);
		}
		if (asset_selection_dirty) {
			_bump_instance_asset_generation(world->instance_asset_generation);
		}
		return;
	}

	if (!_retain_asset_record(*world, p_asset, asset_id)) {
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

	world->instance_lookup[p_node_id] = world->instances.size();
	world->instances.push_back(record);
	_bump_instance_generation(world->instance_generation);
	_bump_instance_asset_generation(world->instance_asset_generation);
	GaussianSplatting::debug_trace_record_event("instance_reg",
			vformat("ADDED: instances_after=%d", world->instances.size()),
			false);
}

void GaussianSplatSceneDirector::update_instance_transform(ObjectID p_node_id, const Transform3D &p_transform) {
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}

	uint32_t *index_ptr = world->instance_lookup.getptr(p_node_id);
	if (!index_ptr || *index_ptr >= world->instances.size()) {
		return;
	}

	InstanceRecord &record = world->instances[*index_ptr];
	if (record.transform.is_equal_approx(p_transform)) {
		return;
	}
	record.transform = p_transform;
	record.dirty = true;
	_bump_instance_generation(world->instance_generation);
}

void GaussianSplatSceneDirector::update_instance_scene_effector_filter(ObjectID p_node_id, bool p_enabled,
		uint32_t p_layer_mask, bool p_scope_filter_present, bool p_scope_filter_valid,
		ObjectID p_scope_root_id, const LocalVector<ObjectID> &p_scene_tree_ancestor_ids) {
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}
	uint32_t *index_ptr = world->instance_lookup.getptr(p_node_id);
	if (!index_ptr || *index_ptr >= world->instances.size()) {
		return;
	}
	InstanceRecord &record = world->instances[*index_ptr];
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
	_bump_instance_generation(world->instance_generation);
}

void GaussianSplatSceneDirector::update_instance_params(ObjectID p_node_id, float p_opacity, float p_lod_bias,
		uint32_t p_flags, bool p_casts_shadow, float p_wind_intensity, uint32_t p_wind_mode,
		const Vector3 &p_wind_direction, float p_wind_frequency, bool p_visible,
		bool p_has_desired_residency_hint, int32_t p_desired_residency_hint,
		float p_effect_position_scale, float p_effect_opacity_scale) {
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}

	uint32_t *index_ptr = world->instance_lookup.getptr(p_node_id);
	if (!index_ptr || *index_ptr >= world->instances.size()) {
		return;
	}

	InstanceRecord &record = world->instances[*index_ptr];
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
		_bump_instance_generation(world->instance_generation);
	}
	if (asset_selection_dirty) {
		_bump_instance_asset_generation(world->instance_asset_generation);
	}
}

void GaussianSplatSceneDirector::unregister_instance(ObjectID p_node_id) {
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_world_for_instance(p_node_id);
	if (!world) {
		world = _find_world_for_instance(p_node_id);
	}
	if (!world) {
		return;
	}

	uint32_t *index_ptr = world->instance_lookup.getptr(p_node_id);
	if (!index_ptr || *index_ptr >= world->instances.size()) {
		return;
	}

	uint32_t index = *index_ptr;
	const uint64_t asset_id = world->instances[index].asset_id;
	uint32_t last_index = world->instances.size() - 1;
	if (index != last_index) {
		world->instances[index] = world->instances[last_index];
		world->instance_lookup[world->instances[index].node_id] = index;
	}
	world->instances.remove_at(last_index);
	world->instance_lookup.erase(p_node_id);
	_release_asset_record(*world, asset_id);
	_bump_instance_generation(world->instance_generation);
	_bump_instance_asset_generation(world->instance_asset_generation);

	_prune_world_if_unused(world->scenario);
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

	MutexLock lock(world_mutex);
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		const SharedWorld &world = E.value;
		const uint32_t *index_ptr = world.instance_lookup.getptr(p_node_id);
		if (!index_ptr || *index_ptr >= world.instances.size()) {
			continue;
		}

		const InstanceRecord &record = world.instances[*index_ptr];
		const SharedWorld::AssetRecord *asset_record = world.asset_records.getptr(record.asset_id);

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
	MutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || world->instances.is_empty()) {
		return;
	}

	const int max_lod = MAX(0, p_lod_config.num_levels - 1);
	const bool use_fallback = p_hysteresis_zone <= 0.0f;
	const bool log_enabled = _is_scene_director_log_enabled();
	bool any_changed = false;

	for (uint32_t i = 0; i < world->instances.size(); i++) {
		InstanceRecord &record = world->instances[i];
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
		_bump_instance_generation(world->instance_generation);
	}
}

void GaussianSplatSceneDirector::build_instance_buffer(LocalVector<InstanceDataGPU> &out) const {
	MutexLock lock(world_mutex);
	out.clear();

	uint32_t total_instances = 0;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		total_instances += E.value.instances.size();
	}
	if (total_instances == 0) {
		return;
	}
	out.reserve(total_instances);

	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		const SharedWorld &world_const_ref = E.value;
		LocalVector<SphereEffectorSelection> scene_payload;
		_build_sorted_sphere_effector_payload(world_const_ref, scene_payload);
		for (const InstanceRecord &record : world_const_ref.instances) {
			if (!record.visible) {
				continue;
			}
			const SharedWorld::AssetRecord *asset_record = world_const_ref.asset_records.getptr(record.asset_id);
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
	MutexLock lock(world_mutex);
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
	if (world->instances.is_empty()) {
		if (world->world_submission.active &&
				_world_submission_record_has_renderable_payload(world->world_submission)) {
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
	out.reserve(world->instances.size());
	uint32_t skipped_instances = 0;
	uint32_t traced_total = 0;
	uint32_t traced_rotation_identity = 0;
	uint32_t traced_scale_identity = 0;
	uint32_t traced_translation_zero = 0;
	uint32_t traced_fully_identity = 0;
	for (const InstanceRecord &record : world->instances) {
		if (!record.visible) {
			continue;
		}
		if (p_shadow_casters_only && !record.casts_shadow) {
			continue;
		}
		const SharedWorld::AssetRecord *asset_record = world->asset_records.getptr(record.asset_id);
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
				out.size(), world->instances.size()));
	}

	if (trace_enabled) {
		GaussianSplatting::debug_trace_record_instance_buffer(out.size(), world->instances.size(), skipped_instances);
		GaussianSplatting::debug_trace_record_instance_flags(traced_total, traced_rotation_identity, traced_scale_identity,
				traced_translation_zero, traced_fully_identity);
		if (skipped_instances > 0 || out.size() != world->instances.size()) {
			GaussianSplatting::debug_trace_record_event("instance_buffer",
					vformat("build out=%d world=%d skipped=%d",
							out.size(), world->instances.size(), skipped_instances),
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
	MutexLock lock(world_mutex);
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
	if (world->instances.is_empty()) {
		if (world->world_submission.active &&
				_world_submission_record_has_renderable_payload(world->world_submission)) {
			InstanceGradingGPU entry = {};
			GaussianSplatSceneDirector::fill_instance_grading_entry(renderer_default, entry);
			out.push_back(entry);
		}
		return;
	}

	out.reserve(world->instances.size());
	for (const InstanceRecord &record : world->instances) {
		if (!record.visible) {
			continue;
		}
		if (p_shadow_casters_only && !record.casts_shadow) {
			continue;
		}
		const SharedWorld::AssetRecord *asset_record = world->asset_records.getptr(record.asset_id);
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
	MutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_instance(p_node_id);
	if (!world) {
		return false;
	}
	const uint32_t *pidx = world->instance_lookup.getptr(p_node_id);
	if (!pidx) {
		return false;
	}
	InstanceRecord &record = world->instances[*pidx];
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
	_bump_instance_generation(world->instance_generation);
	return true;
}

Ref<ColorGradingResource> GaussianSplatSceneDirector::get_instance_color_grading(ObjectID p_node_id) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = const_cast<GaussianSplatSceneDirector *>(this)->_find_world_for_instance(p_node_id);
	if (!world) {
		return Ref<ColorGradingResource>();
	}
	const uint32_t *pidx = world->instance_lookup.getptr(p_node_id);
	if (!pidx) {
		return Ref<ColorGradingResource>();
	}
	return world->instances[*pidx].color_grading;
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
	MutexLock lock(world_mutex);
	SharedWorld *world = const_cast<SharedWorld *>(_find_world_for_renderer(p_renderer));
	if (!world) {
		return;
	}
	// Bump the instance generation so build_instance_grading_buffer_for_renderer
	// re-runs next frame. Records without per-instance grading fall back to the
	// renderer-wide default at build time, so those rows need to re-upload when
	// the default changes even though no per-instance ref mutated.
	_bump_instance_generation(world->instance_generation);
}

uint64_t GaussianSplatSceneDirector::compute_color_grading_signature_for_renderer(
		const GaussianSplatRenderer *p_renderer, bool p_shadow_casters_only) const {
	// FNV-1a-esque rolling hash over every per-instance grading tied to the renderer,
	// including the renderer-wide default used as the fallback. The sort/raster cache
	// invalidation path hashes this in, so any node grading edit busts the cache.
	MutexLock lock(world_mutex);
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

	if (world->instances.is_empty()) {
		// World-submission shim uses the renderer default; already mixed.
		return seed;
	}

	for (const InstanceRecord &record : world->instances) {
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
		const SharedWorld::AssetRecord *asset_record = world->asset_records.getptr(record.asset_id);
		if (!asset_record || asset_record->data.is_null()) {
			continue;
		}
		mix_grading(record.color_grading.is_valid() ? record.color_grading : renderer_default);
	}
	return seed;
}

uint64_t GaussianSplatSceneDirector::get_instance_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return 0;
	}
	return world->instance_generation;
}

uint64_t GaussianSplatSceneDirector::get_instance_asset_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return 0;
	}
	return world->instance_asset_generation;
}

uint32_t GaussianSplatSceneDirector::get_instance_count_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return 0;
	}
	return world->instances.size();
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

	MutexLock lock(world_mutex);
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
		uint32_t *lookup = p_world->sphere_effector_lookup.getptr(p_effector_id);
		if (!lookup || *lookup >= p_world->sphere_effectors.size()) {
			return;
		}
		const uint32_t index = *lookup;
		const uint32_t last_index = p_world->sphere_effectors.size() - 1;
		if (index != last_index) {
			p_world->sphere_effectors[index] = p_world->sphere_effectors[last_index];
			p_world->sphere_effector_lookup[p_world->sphere_effectors[index].effector_id] = index;
		}
		p_world->sphere_effectors.remove_at(last_index);
		p_world->sphere_effector_lookup.erase(p_effector_id);
		_bump_instance_generation(p_world->sphere_effector_generation);
		_prune_world_if_unused(p_world->scenario);
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

	uint32_t *lookup = target_world->sphere_effector_lookup.getptr(p_effector_id);
	if (lookup && *lookup < target_world->sphere_effectors.size()) {
		SphereEffectorRecord &record = target_world->sphere_effectors[*lookup];
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
			_bump_instance_generation(target_world->sphere_effector_generation);
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
	record.registration_serial = ++target_world->sphere_effector_registration_serial;
	record.enabled = p_enabled;
	record.affect_position = p_affect_position;
	record.affect_opacity = p_affect_opacity;

	target_world->sphere_effector_lookup[p_effector_id] = target_world->sphere_effectors.size();
	target_world->sphere_effectors.push_back(record);
	_bump_instance_generation(target_world->sphere_effector_generation);
}

void GaussianSplatSceneDirector::unregister_sphere_effector(ObjectID p_effector_id) {
	if (p_effector_id == ObjectID()) {
		return;
	}

	MutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_effector(p_effector_id);
	if (!world) {
		return;
	}

	uint32_t *lookup = world->sphere_effector_lookup.getptr(p_effector_id);
	if (!lookup || *lookup >= world->sphere_effectors.size()) {
		return;
	}

	const uint32_t index = *lookup;
	const uint32_t last_index = world->sphere_effectors.size() - 1;
	if (index != last_index) {
		world->sphere_effectors[index] = world->sphere_effectors[last_index];
		world->sphere_effector_lookup[world->sphere_effectors[index].effector_id] = index;
	}
	world->sphere_effectors.remove_at(last_index);
	world->sphere_effector_lookup.erase(p_effector_id);
	_bump_instance_generation(world->sphere_effector_generation);
	_prune_world_if_unused(world->scenario);
}

void GaussianSplatSceneDirector::build_sphere_effector_payload_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<SphereEffectorSelection> &out, uint32_t *r_total_scene_effectors) const {
	MutexLock lock(world_mutex);
	out.clear();
	if (r_total_scene_effectors) {
		*r_total_scene_effectors = 0u;
	}

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world) {
		return;
	}

	_build_sorted_sphere_effector_payload(*world, out);
	if (r_total_scene_effectors) {
		// Raw count under the same lock as the payload build — main-thread
		// mutations between the two reads cannot create a skew now.
		*r_total_scene_effectors = world->sphere_effectors.size();
	}
}

bool GaussianSplatSceneDirector::get_primary_sphere_effector_for_instance(ObjectID p_node_id,
		SphereEffectorSelection *r_selection) const {
	ERR_FAIL_NULL_V(r_selection, false);

	MutexLock lock(world_mutex);
	Object *node_object = ObjectDB::get_instance(p_node_id);
	Node3D *node = Object::cast_to<Node3D>(node_object);
	if (!node || !node->is_inside_world()) {
		scene_effector_multi_match_warned_nodes.erase(p_node_id);
		return false;
	}

	const SharedWorld *world = nullptr;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.instance_lookup.has(p_node_id)) {
			world = &E.value;
			break;
		}
	}
	if (!world) {
		const Ref<World3D> node_world = node->get_world_3d();
		if (node_world.is_valid()) {
			world = worlds.getptr(node_world->get_scenario());
		}
	}
	if (!world || world->sphere_effectors.is_empty()) {
		scene_effector_multi_match_warned_nodes.erase(p_node_id);
		return false;
	}

	const NodeSceneEffectorFilterState filter = _get_node_scene_effector_filter_state(node);
	if (!filter.enabled || filter.layer_mask == 0u || (filter.has_scope_filter && !filter.scope_filter_valid)) {
		scene_effector_multi_match_warned_nodes.erase(p_node_id);
		return false;
	}

	struct Candidate {
		SphereEffectorSelection selection;
		uint64_t registration_serial = 0u;
		uint32_t specificity = 0u;
	};

	// Tie-break by registration serial (stable, monotonic) instead of node
	// path — keeps the main-thread diagnostic path's ordering in sync with
	// the render-path `_build_sorted_sphere_effector_payload`.
	auto candidate_precedes = [](const Candidate &p_a, const Candidate &p_b) {
		if (p_a.selection.priority != p_b.selection.priority) {
			return p_a.selection.priority > p_b.selection.priority;
		}
		if (p_a.specificity != p_b.specificity) {
			return p_a.specificity > p_b.specificity;
		}
		if (p_a.registration_serial != p_b.registration_serial) {
			return p_a.registration_serial < p_b.registration_serial;
		}
		if ((uint64_t)p_a.selection.effector_id != (uint64_t)p_b.selection.effector_id) {
			return (uint64_t)p_a.selection.effector_id < (uint64_t)p_b.selection.effector_id;
		}
		return false;
	};

	LocalVector<Candidate> matches;
	for (const SphereEffectorRecord &record : world->sphere_effectors) {
		// Mirror the candidate filter from `_build_sorted_sphere_effector_payload()`:
		// the compatibility API must not surface effectors that the render path
		// already drops (zero-radius, non-finite center, disabled). Keeping the
		// same gate here means `get_primary_sphere_effector_for_instance()` can't
		// return something the GPU immediately rejects.
		if (!record.enabled || record.radius <= 0.0f ||
				(!record.affect_position && !record.affect_opacity)) {
			continue;
		}
		const Vector3 &center = record.transform.origin;
		if (!Math::is_finite(center.x) || !Math::is_finite(center.y) || !Math::is_finite(center.z)) {
			continue;
		}
		// Mirror the inert-channel gate from `_build_sorted_sphere_effector_payload()`
		// so the compatibility API doesn't report a primary that the render path
		// would filter out. Includes the `target_opacity != 1.0` check: neutral
		// target on an opacity-only effector contributes nothing to already-opaque
		// splats, and the payload builder drops those entries.
		const bool position_can_contribute = record.affect_position && !Math::is_zero_approx(record.strength);
		const bool opacity_can_contribute = record.affect_opacity &&
				!Math::is_zero_approx(record.opacity_strength) &&
				!Math::is_equal_approx(record.target_opacity, 1.0f);
		if (!position_can_contribute && !opacity_can_contribute) {
			continue;
		}
		if ((record.layer_mask & filter.layer_mask) == 0u) {
			continue;
		}

		Object *effector_object = ObjectDB::get_instance(record.effector_id);
		Node *effector_node = Object::cast_to<Node>(effector_object);

		ObjectID effective_scope_root_id;
		bool effector_scope_valid = true;
		switch (record.scope_mode) {
			case SPHERE_EFFECTOR_SCOPE_WORLD:
				break;
			case SPHERE_EFFECTOR_SCOPE_SUBTREE:
				if (!effector_node || !effector_node->get_parent()) {
					effector_scope_valid = false;
				} else {
					effective_scope_root_id = effector_node->get_parent()->get_instance_id();
				}
				break;
			case SPHERE_EFFECTOR_SCOPE_EXPLICIT_ROOT: {
				effective_scope_root_id = record.scope_root_id;
				Node *scope_root = Object::cast_to<Node>(ObjectDB::get_instance(record.scope_root_id));
				effector_scope_valid = scope_root != nullptr;
			} break;
			default:
				effector_scope_valid = false;
				break;
		}

		if (filter.has_scope_filter) {
			if (!effector_scope_valid || effective_scope_root_id != filter.scope_root_id) {
				continue;
			}
		} else if (record.scope_mode != SPHERE_EFFECTOR_SCOPE_WORLD) {
			Node *scope_root = Object::cast_to<Node>(ObjectDB::get_instance(effective_scope_root_id));
			if (!effector_scope_valid || !scope_root || !(scope_root == node || scope_root->is_ancestor_of(node))) {
				continue;
			}
		}

		Candidate candidate;
		candidate.selection.effector_id = record.effector_id;
		candidate.selection.scenario = world->scenario;
		candidate.selection.transform = record.transform;
		candidate.selection.center = record.transform.origin;
		candidate.selection.radius = record.radius;
		candidate.selection.strength = record.strength;
		candidate.selection.falloff = record.falloff;
		candidate.selection.frequency = record.frequency;
		candidate.selection.opacity_strength = record.opacity_strength;
		candidate.selection.target_opacity = record.target_opacity;
		candidate.selection.layer_mask = record.layer_mask;
		candidate.selection.scope_mode = record.scope_mode;
		candidate.selection.scope_root_id = effective_scope_root_id;
		candidate.selection.priority = record.priority;
		candidate.selection.enabled = record.enabled;
		candidate.selection.affect_position = record.affect_position;
		candidate.selection.affect_opacity = record.affect_opacity;
		candidate.registration_serial = record.registration_serial;
		candidate.specificity = _get_effector_scope_specificity(record.scope_mode, effective_scope_root_id != ObjectID());

		int insert_at = matches.size();
		while (insert_at > 0 && candidate_precedes(candidate, matches[insert_at - 1])) {
			insert_at--;
		}
		matches.insert(insert_at, candidate);
	}

	if (matches.is_empty()) {
		scene_effector_multi_match_warned_nodes.erase(p_node_id);
		return false;
	}

	*r_selection = matches[0].selection;
	r_selection->matched_effector_count = matches.size();
	if (matches.size() > 1) {
		if (!scene_effector_multi_match_warned_nodes.has(p_node_id)) {
			scene_effector_multi_match_warned_nodes.insert(p_node_id);
			WARN_PRINT(vformat("[GaussianSplatSceneDirector] Node '%s' matched %d scene sphere effectors. "
					"The compatibility query returns only the highest-priority deterministic match; the renderer can bind up to %d per pass.",
					node->get_path(), matches.size(), GS_MAX_SPHERE_EFFECTORS));
		}
	} else {
		scene_effector_multi_match_warned_nodes.erase(p_node_id);
	}
	return true;
}

bool GaussianSplatSceneDirector::get_scene_effector_match_summary_for_instance(ObjectID p_node_id,
		uint32_t *r_match_count, bool *r_position_active, bool *r_opacity_active) const {
	// Delegate to the richer debug-state accessor so the summary and the
	// Dictionary readout can't drift in their scope/subtree handling.
	// Specifically, the subtree-without-explicit-node-scope case is resolved
	// via the InstanceRecord's cached ancestor chain — matching the actual
	// render-path behavior instead of dropping every non-WORLD effector.
	const Dictionary state = get_scene_effector_debug_state_for_instance(p_node_id);
	const uint32_t matched_count = uint32_t(int64_t(state.get(StringName("matched_count"), 0)));
	if (r_match_count) {
		*r_match_count = matched_count;
	}
	if (r_position_active) {
		*r_position_active = bool(state.get(StringName("matched_position_active"), false));
	}
	if (r_opacity_active) {
		*r_opacity_active = bool(state.get(StringName("matched_opacity_active"), false));
	}
	return matched_count > 0u;
}

Dictionary GaussianSplatSceneDirector::get_scene_effector_debug_state_for_instance(ObjectID p_node_id) const {
	Dictionary state;
	state["matched_count"] = 0;
	state["bound_count"] = 0;
	state["truncated"] = false;
	state["position_active"] = false;
	state["opacity_active"] = false;
	state["selected_effector_ids"] = Array();
	state["selected_effector_names"] = PackedStringArray();
	state["effective_layer_mask"] = int64_t(0);
	state["scope_filter_present"] = false;
	state["scope_filter_valid"] = true;
	state["effective_scope_root_id"] = int64_t(0);
	state["matched_position_active"] = false;
	state["matched_opacity_active"] = false;

	MutexLock lock(world_mutex);
	const SharedWorld *world = nullptr;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		if (E.value.instance_lookup.has(p_node_id)) {
			world = &E.value;
			break;
		}
	}
	if (!world) {
		return state;
	}

	const uint32_t *index_ptr = world->instance_lookup.getptr(p_node_id);
	if (!index_ptr || *index_ptr >= world->instances.size()) {
		return state;
	}
	const InstanceRecord &record = world->instances[*index_ptr];
	state["effective_layer_mask"] = int64_t(record.scene_effector_layer_mask);
	state["scope_filter_present"] = record.scene_effector_scope_filter_present;
	state["scope_filter_valid"] = record.scene_effector_scope_filter_valid;
	state["effective_scope_root_id"] = int64_t((uint64_t)record.scene_effector_scope_root_id);

	if (!record.scene_effectors_enabled || record.scene_effector_layer_mask == 0u ||
			(record.scene_effector_scope_filter_present && !record.scene_effector_scope_filter_valid)) {
		return state;
	}

	// Helper: effector's scope is currently valid iff its cached scope_root_id
	// still resolves to a live Node. ObjectDB lookups are thread-safe and catch
	// the case where a scope-root node was deleted after the effector synced.
	auto effector_scope_alive = [](const SphereEffectorRecord &eff) -> bool {
		if (eff.scope_mode == SPHERE_EFFECTOR_SCOPE_WORLD) {
			return true;
		}
		if (eff.scope_root_id == ObjectID()) {
			return false;
		}
		return Object::cast_to<Node>(ObjectDB::get_instance(eff.scope_root_id)) != nullptr;
	};

	uint32_t matched_count = 0u;
	bool matched_position_active = false;
	bool matched_opacity_active = false;
	for (const SphereEffectorRecord &effector : world->sphere_effectors) {
		if (!effector.enabled || (!effector.affect_position && !effector.affect_opacity)) {
			continue;
		}
		if ((effector.layer_mask & record.scene_effector_layer_mask) == 0u) {
			continue;
		}
		if (effector.scope_mode != SPHERE_EFFECTOR_SCOPE_WORLD && !effector_scope_alive(effector)) {
			continue;
		}
		if (record.scene_effector_scope_filter_present) {
			if (effector.scope_root_id == ObjectID() ||
					effector.scope_root_id != record.scene_effector_scope_root_id) {
				continue;
			}
		} else if (effector.scope_mode != SPHERE_EFFECTOR_SCOPE_WORLD) {
			if (effector.scope_root_id == ObjectID()) {
				continue;
			}
			bool in_scope = false;
			for (const ObjectID &ancestor_id : record.scene_tree_ancestor_ids) {
				if (ancestor_id == effector.scope_root_id) {
					in_scope = true;
					break;
				}
			}
			if (!in_scope) {
				continue;
			}
		}

		matched_count++;
		// Radius <= 0 is inert on the GPU (see gs_apply_sphere_effectors'
		// `effector_sphere.w <= 1e-6` early-out). Exclude from active flags so
		// the diagnostic matches the shader.
		const bool effector_radius_nonzero = effector.radius > 1e-6f;
		if (effector_radius_nonzero && effector.affect_position && record.effect_position_scale > 0.0f &&
				!Math::is_zero_approx(effector.strength)) {
			matched_position_active = true;
		}
		if (effector_radius_nonzero && effector.affect_opacity && record.effect_opacity_scale > 0.0f && record.opacity > 0.0f &&
				!Math::is_zero_approx(effector.opacity_strength) &&
				!Math::is_equal_approx(effector.target_opacity, 1.0f)) {
			matched_opacity_active = true;
		}
	}

	// Bind-side stats: how many matched effectors actually survived the
	// renderer's payload build + mask packing (limited by GS_MAX_SPHERE_EFFECTORS,
	// invalid scope roots, etc). `_build_sorted_sphere_effector_payload` does the
	// same scope_root liveness check via ObjectDB so the counts stay consistent.
	LocalVector<SphereEffectorSelection> payload;
	_build_sorted_sphere_effector_payload(*world, payload);
	const uint32_t mask = _build_scene_effector_mask_for_record(record, payload);
	uint32_t bound_count = 0u;
	Array selected_effector_ids;
	PackedStringArray selected_effector_names;
	bool bound_position_active = false;
	bool bound_opacity_active = false;
	for (uint32_t i = 0; i < payload.size(); i++) {
		if ((mask & (1u << i)) == 0u) {
			continue;
		}
		bound_count++;
		selected_effector_ids.push_back(int64_t((uint64_t)payload[i].effector_id));

		String effector_name = String::num_uint64((uint64_t)payload[i].effector_id);
		if (Object *effector_object = ObjectDB::get_instance(payload[i].effector_id)) {
			if (Node *effector_node = Object::cast_to<Node>(effector_object)) {
				effector_name = String(effector_node->get_name());
			}
		}
		selected_effector_names.push_back(effector_name);

		// Mirror the shader's early-out: `gs_apply_sphere_effectors` skips any
		// slot with `effector_sphere.w <= 1e-6` (i.e. radius <= 0), so a
		// matched/bound zero-radius effector contributes nothing. Treat it
		// as inactive here so `is_scene_effector_*_active()` doesn't over-
		// report "active" relative to what the GPU actually does.
		const bool radius_nonzero = payload[i].radius > 1e-6f;
		if (radius_nonzero && payload[i].affect_position && record.effect_position_scale > 0.0f &&
				!Math::is_zero_approx(payload[i].strength)) {
			bound_position_active = true;
		}
		if (radius_nonzero && payload[i].affect_opacity && record.effect_opacity_scale > 0.0f && record.opacity > 0.0f &&
				!Math::is_zero_approx(payload[i].opacity_strength) &&
				!Math::is_equal_approx(payload[i].target_opacity, 1.0f)) {
			bound_opacity_active = true;
		}
	}

	state["matched_count"] = int64_t(matched_count);
	state["bound_count"] = int64_t(bound_count);
	state["truncated"] = matched_count > bound_count;
	state["position_active"] = bound_position_active;
	state["opacity_active"] = bound_opacity_active;
	state["selected_effector_ids"] = selected_effector_ids;
	state["selected_effector_names"] = selected_effector_names;
	state["matched_position_active"] = matched_position_active;
	state["matched_opacity_active"] = matched_opacity_active;
	return state;
}

uint32_t GaussianSplatSceneDirector::get_sphere_effector_count_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	return world ? world->sphere_effectors.size() : 0u;
}

uint64_t GaussianSplatSceneDirector::get_sphere_effector_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	return world ? world->sphere_effector_generation : 0ull;
}

bool GaussianSplatSceneDirector::submit_world_submission(const WorldSubmission &p_submission) {
	if (p_submission.owner_id == ObjectID() || !p_submission.scenario.is_valid()) {
		return false;
	}

	// Runtime world path: renderer mutation, ownership arbitration, and rollback stay centralized here.
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_or_create_world_for_scenario(p_submission.scenario);
	if (!world) {
		return false;
	}

	SharedWorld *previous_world = _find_world_for_world_submission(p_submission.owner_id);
	const SharedWorld::WorldSubmissionRecord target_previous_record = world->world_submission;
	const bool same_owner = target_previous_record.active && target_previous_record.owner_id == p_submission.owner_id;
	if (target_previous_record.active && !same_owner) {
		if (_is_world_submission_owner_live(world->world_submission.owner_id)) {
			return false;
		}
	}

	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot target_previous_renderer_state =
			world->renderer.is_valid()
					? world->renderer->snapshot_world_submission_runtime_state()
					: GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot();
	SharedWorld::WorldSubmissionRecord candidate_record;
	_store_world_submission_record(candidate_record, p_submission);
	candidate_record.renderer_restore_state = target_previous_record.active
			? (target_previous_record.renderer_restore_state.valid
					? target_previous_record.renderer_restore_state
					: target_previous_renderer_state)
			: target_previous_renderer_state;
	if (!_apply_world_submission_to_renderer(*world, candidate_record, candidate_record.renderer_restore_state)) {
		_restore_world_submission_renderer(*world, target_previous_renderer_state);
		return false;
	}

	if (previous_world && previous_world != world) {
		_restore_world_submission_renderer(*previous_world, previous_world->world_submission.renderer_restore_state);
		previous_world->world_submission = SharedWorld::WorldSubmissionRecord();
	}

	world->world_submission = candidate_record;
	return true;
}

void GaussianSplatSceneDirector::release_world_submission(ObjectID p_owner_id) {
	MutexLock lock(world_mutex);
	SharedWorld *world = _find_world_for_world_submission(p_owner_id);
	if (!world) {
		return;
	}
	const RID scenario = world->scenario;
	_restore_world_submission_renderer(*world, world->world_submission.renderer_restore_state);
	world->world_submission = SharedWorld::WorldSubmissionRecord();
	_prune_world_if_unused(scenario);
}

void GaussianSplatSceneDirector::try_prune_world_if_unused(const RID &p_scenario) {
	if (!p_scenario.is_valid()) {
		return;
	}
	MutexLock lock(world_mutex);
	_prune_world_if_unused(p_scenario);
}

bool GaussianSplatSceneDirector::has_shared_world_for_scenario(const RID &p_scenario) const {
	if (!p_scenario.is_valid()) {
		return false;
	}
	MutexLock lock(world_mutex);
	return worlds.getptr(p_scenario) != nullptr;
}

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)
uint32_t GaussianSplatSceneDirector::test_asset_record_count_for_scenario(const RID &p_scenario) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world) {
		return 0;
	}
	return world->asset_records.size();
}

bool GaussianSplatSceneDirector::test_has_asset_record_for_scenario(const RID &p_scenario, ObjectID p_asset_object_id) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world) {
		return false;
	}
	return world->asset_records.has(_asset_records_key(p_asset_object_id));
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
	MutexLock lock(world_mutex);
	SharedWorld *entry = worlds.getptr(p_scenario);
	if (!entry) {
		// Already torn down (e.g. another peer on the same scenario beat us to
		// the punch) or never existed -- both are no-ops.
		return;
	}

	// Drop the renderer's world-submission contract first so the renderer no
	// longer points at the gaussian_data / payload_source we are about to
	// release. If the renderer is the last owner this is a no-op once we unref.
	if (entry->renderer.is_valid()) {
		entry->renderer->clear_world_submission_contract();
	}

	// Clear every Ref-holding field on the SharedWorld so the only owner of
	// the renderer/data is the about-to-be-erased map entry.
	entry->instances.clear();
	entry->instance_lookup.clear();
	entry->sphere_effectors.clear();
	entry->sphere_effector_lookup.clear();
	entry->asset_records.clear();
	entry->world_submission = SharedWorld::WorldSubmissionRecord();
	entry->renderer.unref();

	// Erase the map entry -- last reference holder for everything above.
	worlds.erase(p_scenario);
}

bool GaussianSplatSceneDirector::get_world_submission(ObjectID p_owner_id, WorldSubmission *r_submission) const {
	ERR_FAIL_NULL_V(r_submission, false);

	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_world_submission(p_owner_id);
	if (!world || !world->world_submission.active) {
		return false;
	}

	_copy_world_submission_record(*world, world->world_submission, r_submission);
	return true;
}

bool GaussianSplatSceneDirector::get_world_submission_for_scenario(const RID &p_scenario, WorldSubmission *r_submission) const {
	ERR_FAIL_NULL_V(r_submission, false);

	MutexLock lock(world_mutex);
	const SharedWorld *world = worlds.getptr(p_scenario);
	if (!world || !world->world_submission.active) {
		return false;
	}

	_copy_world_submission_record(*world, world->world_submission, r_submission);
	return true;
}

bool GaussianSplatSceneDirector::has_world_submission_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	MutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || !world->world_submission.active) {
		return false;
	}

	return _world_submission_record_has_renderable_payload(world->world_submission);
}

bool GaussianSplatSceneDirector::get_submission_residency_hint_for_renderer(const GaussianSplatRenderer *p_renderer,
		int32_t *r_hint, String *r_source) const {
	ERR_FAIL_NULL_V(r_hint, false);

	MutexLock lock(world_mutex);
	if (const SharedWorld *world = _find_world_for_renderer(p_renderer)) {
		const bool world_submission_has_renderable_data =
				_world_submission_record_has_renderable_payload(world->world_submission);
		if (world->world_submission.active && world_submission_has_renderable_data &&
				world->world_submission.has_desired_residency_hint) {
			*r_hint = world->world_submission.desired_residency_hint;
			if (r_source) {
				*r_source = "world_submission";
			}
			return true;
		}

		bool found_instance_hint = false;
		int32_t instance_hint = SUBMISSION_RESIDENCY_HINT_RESIDENT;
		for (const InstanceRecord &record : world->instances) {
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
	MutexLock lock(world_mutex);

	SubmissionCounts counts;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		counts.instance_submissions += E.value.instances.size();
		if (E.value.world_submission.active) {
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
	MutexLock lock(world_mutex);
	out.clear();

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || world->asset_records.is_empty()) {
		return;
	}

	HashSet<uint64_t> selected_asset_ids;
	selected_asset_ids.reserve(world->asset_records.size());
	for (const InstanceRecord &record : world->instances) {
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
		const SharedWorld::AssetRecord *record = world->asset_records.getptr(asset_id);
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
	MutexLock lock(world_mutex);
	out.clear();

	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	if (!world || world->asset_records.is_empty()) {
		return;
	}

	out.reserve(world->asset_records.size());
	for (const KeyValue<uint64_t, SharedWorld::AssetRecord> &E : world->asset_records) {
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
	MutexLock lock(world_mutex);
	SharedWorld *world = _get_or_create_world(p_world);
	if (!world) {
		return Ref<GaussianSplatRenderer>();
	}
	return world->renderer;
}
