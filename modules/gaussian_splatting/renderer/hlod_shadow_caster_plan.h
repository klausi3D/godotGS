#ifndef GS_HLOD_SHADOW_CASTER_PLAN_H
#define GS_HLOD_SHADOW_CASTER_PLAN_H

// Splat shadow casters on the HLOD route (#1095, #1163; docs/architecture/adr-one-route-many-producers.md, F5 "light
// views"). Device-free planning for the light-view caster pass, shared by the renderer and the CPU doctests.
//
// Caster list: the main view's HLOD draw list, exactly as that view uploaded it (world records of its cut plus the
// admitted node instances' records, one DrawnNodeGPU per node or chunk). Every caster the shadow map receives is a splat
// the color view drew; objects the budget rule refused cast nothing, and nothing outside the view's cut casts (the
// light-view cut is a later slice). Forward+ renders the shadow passes of a frame BEFORE that frame's GS main view, so a
// shadow pass consumes the record of the previous main view: the draw list, rows and atlas the GPU still holds, with
// camera offsets formed against that view's camera position (recorded here).
//
// Representation: depth only, written straight into Godot's directional shadow atlas rect with Godot's own projection
// and depth convention (reverse-Z, depth correction, pancake clamp), so every mesh receiver samples it unchanged. A
// fragment of a splat is an occluder when its Gaussian alpha reaches kAlphaThreshold.

#include "hlod_world_instances.h"

#include "core/math/projection.h"
#include "core/math/rect2.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"

#include <cstdint>

namespace gs_shadow_caster {

// alpha = opacity * exp(-r^2 / 2) >= threshold marks an occluding fragment (threshold coverage, #1095 section 1).
static constexpr float kAlphaThreshold = 0.5f;
// Godot's pancake clamp for depth casters (scene_forward_clustered.glsl, MODE_RENDER_DEPTH).
static constexpr float kPancakeDepth = 0.9999f;
// Low-pass dilation added to the projected 2D covariance, in shadow-map texels squared (as the color raster does).
static constexpr float kLowPassTexels2 = 0.3f;
// Instance slots one draw may issue. Slots are draw_count * max_chunk_splats (the depth pass's 2D dispatch, flattened).
static constexpr uint64_t kMaxInstanceSlots = uint64_t(1) << 31;

enum class Refusal : uint8_t {
	NONE = 0,
	NOT_DIRECTIONAL, // Spot / omni passes: perspective light projections are not cast yet.
	NO_ACTIVE_HLOD_VIEW, // No main view published an active HLOD draw list since the bundle was (re)built.
	QUANTIZED_PAYLOAD, // The caster shader reads the raw relative payload only.
	EMPTY_DRAW_LIST,
	INSTANCE_SLOT_OVERFLOW,
	BUNDLE_CHANGED, // The relative bundle's buffers are not the ones the record names (rebuilt or released).
	PIPELINE_UNAVAILABLE,
	INVALID_ATLAS_RECT,
};

inline const char *refusal_reason(Refusal p_refusal) {
	switch (p_refusal) {
		case Refusal::NONE:
			return "cast";
		case Refusal::NOT_DIRECTIONAL:
			return "shadow_caster_light_type_not_directional";
		case Refusal::NO_ACTIVE_HLOD_VIEW:
			return "shadow_caster_no_active_hlod_view";
		case Refusal::QUANTIZED_PAYLOAD:
			return "shadow_caster_quantized_payload_unsupported";
		case Refusal::EMPTY_DRAW_LIST:
			return "shadow_caster_empty_draw_list";
		case Refusal::INSTANCE_SLOT_OVERFLOW:
			return "shadow_caster_instance_slot_overflow";
		case Refusal::BUNDLE_CHANGED:
			return "shadow_caster_bundle_changed";
		case Refusal::PIPELINE_UNAVAILABLE:
			return "shadow_caster_pipeline_unavailable";
		case Refusal::INVALID_ATLAS_RECT:
			return "shadow_caster_invalid_atlas_rect";
	}
	return "shadow_caster_unknown";
}

// What the last main view uploaded (written on the render thread by prepare_hlod_frame, read by the shadow passes).
struct FrameRecord {
	bool valid = false;
	bool quantized = false;
	uint32_t draw_count = 0; // World records + instance records in the uploaded list.
	uint32_t world_records = 0;
	uint32_t instance_records = 0;
	uint32_t max_chunk_splats = 0; // Largest node/chunk payload of the bundle (the per-record slot stride).
	uint64_t world_splats = 0; // Splats of the world records (sum of the selected nodes' payloads).
	uint64_t instance_splats = 0; // Splats of the admitted objects whose rows cast (refused objects are not in the list).
	Vector3 camera; // Camera position the list's offsets were formed against (origin 0: the route refuses a shift).
	uint64_t origin_revision = 0;
	uint64_t renderer_frame = 0;
	uint64_t bundle_builds = 0; // ResourceState::hlod_bundle_builds when recorded.
	// ResourceState::hlod_draw_list_uploads when the list was uploaded. A later upload rewrites the list the record names;
	// an upload-free rebind (the streamed world's end-of-frame idle restore zeroes only the GPU counter) does not.
	uint64_t draw_list_upload = 0;
	// Per bundle row (InstanceDataGPU index): 1 = the row casts. Row 0 follows the world's own cast_shadow (Godot gives the
	// shared renderer a shadow pass when ANY of its Gaussian instances casts, so that alone says nothing about the
	// world); rows 1..N follow their node's cast_shadow.
	LocalVector<uint32_t> row_casts;

	uint64_t casters() const { return world_splats + instance_splats; }
};

// Per-row cast flags for the rows as uploaded (row 0 = the world). Row 0 casts iff the world casts; a node row casts iff
// its stable key is among the director's visible && casts_shadow records.
inline void build_row_casts(const LocalVector<uint64_t> &p_row_keys, bool p_world_casts,
		const LocalVector<uint64_t> &p_caster_keys, LocalVector<uint32_t> &r_row_casts) {
	r_row_casts.resize(p_row_keys.size());
	for (uint32_t r = 0; r < p_row_keys.size(); ++r) {
		uint32_t casts = r == 0 ? (p_world_casts ? 1u : 0u) : 0u;
		for (uint32_t k = 0; r != 0 && k < p_caster_keys.size() && !casts; ++k) {
			casts = p_caster_keys[k] == p_row_keys[r] ? 1u : 0u;
		}
		r_row_casts[r] = casts;
	}
}

// Splats of the admitted objects whose rows cast: whole assets (the budget rule admits or refuses an object whole), so
// the caster set is exactly what the color view draws minus the rows with cast_shadow off; a refused object casts
// nothing.
inline uint64_t admitted_instance_splats(const gs_hlod_bundle::InstanceRegion &p_region,
		const gs_hlod_bundle::ObjectDemand *p_demands, uint32_t p_demand_count, const gs_hlod_bundle::Admission &p_admission,
		const LocalVector<uint32_t> &p_row_casts) {
	uint64_t splats = 0;
	for (uint32_t index : p_admission.admitted) {
		if (index >= p_demand_count || p_demands[index].asset_index >= p_region.assets.size()) {
			continue;
		}
		const uint32_t row = p_demands[index].row;
		if (row < p_row_casts.size() && p_row_casts[row] != 0) {
			splats += p_region.assets[p_demands[index].asset_index].splat_count;
		}
	}
	return splats;
}

// Completes a record for the list a main view uploaded: `p_world_records` world records (their payloads summing to
// `p_world_record_splats`) followed by the instance records the record already counts (instance_records,
// instance_splats, row_casts). The world's splats are casters only when row 0 casts. An idle streamed world uploads
// its instance records alone (p_world_records = 0): the objects it still draws keep casting (ADR 4.4).
inline void complete_record(FrameRecord &r_record, uint32_t p_world_records, uint64_t p_world_record_splats) {
	const bool world_casts = !r_record.row_casts.is_empty() && r_record.row_casts[0] != 0;
	r_record.world_records = p_world_records;
	r_record.world_splats = world_casts ? p_world_record_splats : 0;
	r_record.draw_count = p_world_records + r_record.instance_records;
	r_record.valid = true;
}

struct PassPlan {
	Refusal refusal = Refusal::NONE;
	uint32_t instance_slots = 0;
	uint64_t casters = 0;

	bool draws() const { return refusal == Refusal::NONE; }
};

// One light pass. The light type follows from the projection Godot hands the pass: directional cascades are orthographic.
inline PassPlan plan_pass(const FrameRecord &p_record, bool p_orthographic_light, bool p_bundle_current) {
	PassPlan plan;
	if (!p_orthographic_light) {
		plan.refusal = Refusal::NOT_DIRECTIONAL;
	} else if (!p_record.valid) {
		plan.refusal = Refusal::NO_ACTIVE_HLOD_VIEW;
	} else if (!p_bundle_current) {
		plan.refusal = Refusal::BUNDLE_CHANGED;
	} else if (p_record.quantized) {
		plan.refusal = Refusal::QUANTIZED_PAYLOAD;
	} else if (p_record.draw_count == 0 || p_record.max_chunk_splats == 0 || p_record.casters() == 0) {
		plan.refusal = Refusal::EMPTY_DRAW_LIST;
	} else {
		const uint64_t slots = uint64_t(p_record.draw_count) * uint64_t(p_record.max_chunk_splats);
		if (slots > kMaxInstanceSlots) {
			plan.refusal = Refusal::INSTANCE_SLOT_OVERFLOW;
		} else {
			plan.instance_slots = uint32_t(slots);
			plan.casters = p_record.casters();
		}
	}
	return plan;
}

// Godot's shadow-pass clip transform (render_forward_clustered.cpp): _render_shadow_pass hands _render_shadow_append
// flip_y = true for a directional pass, _render_shadow_append sets scene_data.flip_y = !p_flip_y, and
// RenderSceneDataRD::update_ubo applies set_depth_correction(scene_data.flip_y). So mesh casters of a directional
// cascade use correction(false): clip = correction(!p_flip_y) * light_projection * light_transform^-1 * world.
// The GS hook passes the same p_flip_y (true for directional). The receivers sample with
// rectm * bias * correction(false, true, false) * camera * modelview (light_storage.cpp), which this matches texel for
// texel. A correction(true) here mirrored every cascade about its light-space centre line (#1095 slice 1 review).
inline Projection godot_shadow_clip(const Projection &p_light_projection, const Transform3D &p_light_transform, bool p_flip_y) {
	Projection correction;
	correction.set_depth_correction(!p_flip_y);
	return correction * p_light_projection * Projection(p_light_transform.affine_inverse());
}

// The same transform for positions relative to the frame camera C (world = p + C), as the HLOD draw list stores them.
inline Projection frame_relative_shadow_clip(const Projection &p_light_projection, const Transform3D &p_light_transform,
		bool p_flip_y, const Vector3 &p_frame_camera) {
	return godot_shadow_clip(p_light_projection, p_light_transform, p_flip_y) * Projection(Transform3D(Basis(), p_frame_camera));
}

// Where Godot's directional receivers sample a world point: atlas-normalised uv and depth of the shadow matrix of
// light_storage.cpp (rectm * bias * correction(false, true, false) * camera * light_transform^-1), for a cascade rect
// in normalised atlas coordinates. Used by the CPU tests to check the caster against the receiver.
inline Vector3 godot_directional_receiver_sample(const Projection &p_light_projection, const Transform3D &p_light_transform,
		const Rect2 &p_atlas_rect_normalized, const Vector3 &p_world) {
	Projection correction;
	correction.set_depth_correction(false, true, false);
	Projection bias;
	bias.set_light_bias();
	Projection rectm;
	rectm.set_light_atlas_rect(p_atlas_rect_normalized);
	const Projection shadow = rectm * bias * correction * p_light_projection * Projection(p_light_transform.affine_inverse());
	return shadow.xform(p_world);
}

// Where the caster writes a clip-space point: the draw list's viewport is the cascade rect (RenderingDevice
// draw_list_begin region), framebuffer row 0 at ndc y = -1, so uv = rect.pos + (ndc.xy * 0.5 + 0.5) * rect.size.
inline Vector3 caster_atlas_sample(const Projection &p_clip, const Rect2 &p_atlas_rect_normalized, const Vector3 &p_point) {
	const Vector3 ndc = p_clip.xform(p_point);
	return Vector3(p_atlas_rect_normalized.position.x + (ndc.x * 0.5f + 0.5f) * p_atlas_rect_normalized.size.x,
			p_atlas_rect_normalized.position.y + (ndc.y * 0.5f + 0.5f) * p_atlas_rect_normalized.size.y, ndc.z);
}

// Pancake: casters in front of the cascade's near plane are clamped, never clipped (Godot's mesh casters do the same).
inline float pancake_depth(float p_ndc_z) {
	return p_ndc_z >= kPancakeDepth ? kPancakeDepth : p_ndc_z;
}

// Per-renderer-frame caster accounting (stats shadow_caster_*). Shadow passes of a frame run before its main view, at
// renderer frame counter F; get_render_stats() after that frame shows frame_count F + 1, so shadow_caster_frame is
// frame_count - 1 exactly like sort_dispatch_frame.
struct FrameStats {
	uint64_t frame = UINT64_MAX;
	uint32_t passes_cast = 0;
	uint32_t passes_refused = 0;
	uint64_t casters = 0; // Sum over the frame's cast passes (a caster counts once per cascade it is drawn into).
	uint64_t instance_slots = 0;
	Refusal last_refusal = Refusal::NONE;
	uint64_t total_passes_cast = 0;
	uint64_t total_passes_refused = 0;

	void record(uint64_t p_frame, const PassPlan &p_plan) {
		if (frame != p_frame) {
			frame = p_frame;
			passes_cast = passes_refused = 0;
			casters = instance_slots = 0;
			last_refusal = Refusal::NONE;
		}
		if (p_plan.draws()) {
			++passes_cast;
			++total_passes_cast;
			casters += p_plan.casters;
			instance_slots += p_plan.instance_slots;
		} else {
			++passes_refused;
			++total_passes_refused;
			last_refusal = p_plan.refusal;
		}
	}
};

} // namespace gs_shadow_caster

#endif // GS_HLOD_SHADOW_CASTER_PLAN_H
