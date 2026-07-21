/**
 * @file scene_director_sphere_effectors.cpp
 * @brief Sphere-effector coordination for GaussianSplatSceneDirector.
 *
 * Extracted verbatim from gaussian_splat_scene_director.cpp to shrink that
 * god-file. All functions here are GaussianSplatSceneDirector member methods
 * declared in gaussian_splat_scene_director.h — this is a partial-class
 * translation unit, mirroring the streaming_lod_policy.cpp pattern. No new
 * declarations are introduced.
 *
 * Contains the render-thread payload builder, the renderer-facing payload /
 * count / generation accessors, and the main-thread per-instance debug-state
 * query. The shared file-local helper `_get_effector_scope_specificity` lives
 * in core/scene_director_internal.h; it was hoisted there during extraction so
 * this TU can reach it without duplicating the definition.
 */

#include "gaussian_splat_scene_director.h"

#include "scene_director_internal.h"
#include "core/math/math_funcs.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "scene/3d/node_3d.h"
#include "scene/main/node.h"

void GaussianSplatSceneDirector::_build_sorted_sphere_effector_payload(const SharedWorld &p_world,
		LocalVector<SphereEffectorSelection> &r_out) {
	r_out.clear();
	if (p_world.sphere_effector_store.is_empty()) {
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
	candidates.reserve(p_world.sphere_effector_store.count());
	for (const SphereEffectorRecord &record : p_world.sphere_effector_store.records()) {
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
		// plus bump the store's generation so cached instance bitmasks
		// invalidate and rebuild without the dropped effector. #610 S4 leaves
		// this write-through-const behavior verbatim (D5 remains open); only
		// the generation bump is now routed through the owned store method.
		if (record.scope_mode != SPHERE_EFFECTOR_SCOPE_WORLD) {
			const bool scope_alive = record.scope_root_id != ObjectID() &&
					Object::cast_to<Node>(ObjectDB::get_instance(record.scope_root_id)) != nullptr;
			if (!scope_alive) {
				if (record.scope_root_valid) {
					const_cast<SphereEffectorRecord &>(record).scope_root_valid = false;
					const_cast<SharedWorld &>(p_world).sphere_effector_store.bump_generation();
				}
				continue;
			}
			if (!record.scope_root_valid) {
				// Flipped back to alive (rare — node re-registered at the same ID).
				const_cast<SphereEffectorRecord &>(record).scope_root_valid = true;
				const_cast<SharedWorld &>(p_world).sphere_effector_store.bump_generation();
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

void GaussianSplatSceneDirector::build_sphere_effector_payload_for_renderer(const GaussianSplatRenderer *p_renderer,
		LocalVector<SphereEffectorSelection> &out, uint32_t *r_total_scene_effectors) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
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
		*r_total_scene_effectors = world->sphere_effector_store.count();
	}
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

	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = nullptr;
	const InstanceRecord *record_ptr = nullptr;
	for (const KeyValue<RID, SharedWorld> &E : worlds) {
		const InstanceRecord *found = E.value.instance_store.find(p_node_id);
		if (found) {
			world = &E.value;
			record_ptr = found;
			break;
		}
	}
	if (!world) {
		return state;
	}

	const InstanceRecord &record = *record_ptr;
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
	for (const SphereEffectorRecord &effector : world->sphere_effector_store.records()) {
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
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	return world ? world->sphere_effector_store.count() : 0u;
}

uint64_t GaussianSplatSceneDirector::get_sphere_effector_generation_for_renderer(const GaussianSplatRenderer *p_renderer) const {
	GaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);
	const SharedWorld *world = _find_world_for_renderer(p_renderer);
	return world ? world->sphere_effector_store.generation() : 0ull;
}
