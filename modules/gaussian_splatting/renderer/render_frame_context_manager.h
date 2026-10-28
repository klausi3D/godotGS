#ifndef GAUSSIAN_RENDER_FRAME_CONTEXT_MANAGER_H
#define GAUSSIAN_RENDER_FRAME_CONTEXT_MANAGER_H

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "servers/rendering/rendering_device.h"

#include "../lod/hlod_view_set.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#include <atomic>

// RenderSceneBuffersRD owns this token. It owns no renderer, render-buffer Ref or RID.
class GaussianHlodViewLifetime : public RenderBufferCustomDataRD {
    GDCLASS(GaussianHlodViewLifetime, RenderBufferCustomDataRD);
    uint64_t epoch = 0;
protected:
    static void _bind_methods() {}
public:
    void configure(RenderSceneBuffersRD *p_buffers) override;
    void free_data() override;
    uint64_t get_epoch() const { return epoch; }
};

// Lifetime identity of one render view: which RenderSceneBuffersRD, its render target and the lifetime
// epoch of its GaussianHlodViewLifetime token (bumped by configure and free_data).
struct HlodViewIdentity {
	ObjectID buffers;
	RID target;
	uint64_t epoch = 0;
	bool is_valid() const { return buffers.is_valid() && target.is_valid() && epoch != 0; }
};

// Canonical-route fallback for a non-success readiness or admission result. `reason == nullptr` means
// the cut is active. The counters are per-frame increments for the existing hlod_* telemetry.
struct HlodFallback {
	const char *reason = nullptr;
	uint32_t budget_bound_frames = 0;
	uint32_t root_dropped_frames = 0;
};

struct HlodViewOutcome {
	HlodFallback fallback; // reason == nullptr: active cut, `records` are valid until the next call.
	gs_hlod_runtime::ViewSet::AdmitStatus admit = gs_hlod_runtime::ViewSet::REFUSED_INVALID;
	bool admitted_now = false; // This call (re-)admitted the view.
	gs_hlod_runtime::ViewSet::ViewResult view;
	const gs_hlod_runtime::DrawRecord *records = nullptr;
	uint32_t record_count = 0;
	bool active() const { return fallback.reason == nullptr; }
};

class RenderFrameContextManager {
public:
	// One shared ViewSet budget slot per live render view; matches the four views the renderer keeps.
	static constexpr uint32_t HLOD_MAX_VIEWS = 4;

	struct FrameState {
		std::atomic<uint32_t> visible_splat_count{0};
		uint32_t frame_counter = 0;
		float sort_time_ms = 0.0f;
		float render_time_ms = 0.0f;
		// Monotonic wall-clock seconds sampled once per render frame and
		// propagated to shader animation phase (wind, sphere effectors).
		// Previously the shader derived phase from `frame_counter / 60`,
		// which beat against script-side animations driven by real
		// `_process(delta)` on non-60Hz displays or under frame drops —
		// visible as jitter layered on the intended oscillation. Sampling
		// once per frame keeps the tile and depth stages in lockstep.
		double animation_time_seconds = 0.0;
	};

	// Returns monotonic seconds since the first call. Used by the render
	// pipeline to sample a single animation-time value per frame before
	// setting shader uniforms; see FrameState::animation_time_seconds.
	static double sample_render_animation_time_seconds();

	struct ViewState {
		Transform3D last_camera_to_world_transform;
		Projection last_camera_projection;
		Vector3 last_camera_position = Vector3(0, 0, 5);
		Size2i manual_viewport_override = Size2i();
		RD::DataFormat manual_viewport_format_override = RD::DATA_FORMAT_MAX;
		RD::DataFormat active_viewport_color_format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		bool using_scene_data = false;
	};

	RenderFrameContextManager();

	FrameState &get_frame_state() { return frame_state; }
	const FrameState &get_frame_state() const { return frame_state; }
	ViewState &get_view_state() { return view_state; }
	const ViewState &get_view_state() const { return view_state; }

	// Whether the buffers/epoch of an identity are still the live ones. The default asks ObjectDB and the
	// lifetime token; tests install their own. Used only when a new view is admitted, never per frame.
	typedef bool (*HlodLivenessCheck)(const HlodViewIdentity &p_identity);
	static bool default_hlod_liveness(const HlodViewIdentity &p_identity);
	void set_hlod_liveness_check(HlodLivenessCheck p_check) { hlod_liveness = p_check ? p_check : &default_hlod_liveness; }
	// Pure part of the identity lookup: builds the identity from a token and the buffers' id/target.
	static bool hlod_identity_from_token(const GaussianHlodViewLifetime *p_token, ObjectID p_buffers, RID p_target, HlodViewIdentity &r_identity);
	// Drops bindings (and their reservations) whose buffers no longer resolve or whose epoch is stale.
	uint32_t evict_stale_hlod_bindings();
	// Releases the runtime snapshot reference held by the reusable selection input.
	void release_hlod_snapshot_data();
	// Reads (creating on first use) the lifetime token of `p_buffers`. False when the buffers have no
	// render target or the custom-data slot belongs to another owner.
	static bool acquire_hlod_identity(RenderSceneBuffersRD *p_buffers, HlodViewIdentity &r_identity);
	// ViewSet key of a view: derived from the buffers id and its lifetime epoch.
	static uint64_t hlod_view_key(const HlodViewIdentity &p_identity);
	// Admits (or reuses) the ViewSet view of `p_identity`, runs one selection and maps the result to
	// either an active cut or an explicit canonical fallback. `p_source` identifies the snapshot object.
	// `p_snapshot.views` is replaced by `p_spec` (whose view_key is overwritten with the derived key).
	// Cut state lives only in the ViewSet; this class keeps only the lifetime bindings.
	HlodViewOutcome select_hlod_view(const HlodViewIdentity &p_identity, uint64_t p_source,
			gs_hlod_runtime::SelectionSnapshot &p_snapshot, gs_hlod_runtime::ViewSpec p_spec);
	uint32_t hlod_bound_view_count() const;
	const gs_hlod_runtime::ViewSet &get_hlod_view_set() const { return hlod_view_set; }
	// Reusable input for select_hlod_view: the caller refills it each frame (same sizes: no allocation).
	// The runtime snapshot reference is released by release_hlod_snapshot_data() after every prepare.
	gs_hlod_runtime::SelectionSnapshot &get_hlod_snapshot() { return hlod_snapshot; }
	void clear_hlod_views();

	static HlodFallback hlod_fallback_for_admission(gs_hlod_runtime::ViewSet::AdmitStatus p_status);
	static HlodFallback hlod_fallback_for_snapshot(gs_hlod_runtime::ViewSet::SnapshotStatus p_status);
	static HlodFallback hlod_fallback_for_view(const gs_hlod_runtime::ViewSet::ViewResult &p_result);

	void reset_frame_state();
	void reset_view_state_defaults();
	void reset();

private:
	// Lifetime adapter only: which ViewSet view belongs to which buffers/target/epoch/snapshot. All cut
	// state (histories, policy, published records) is owned by `hlod_view_set`.
	struct HlodBinding {
		bool used = false;
		HlodViewIdentity identity;
		uint64_t source = 0;
		uint64_t last_used = 0;
	};
	HlodBinding hlod_bindings[HLOD_MAX_VIEWS];
	HlodLivenessCheck hlod_liveness = &default_hlod_liveness;
	gs_hlod_runtime::ViewSet hlod_view_set;
	gs_hlod_runtime::SelectionSnapshot hlod_snapshot;
	uint64_t hlod_view_clock = 0;
	void drop_hlod_binding(HlodBinding &r_binding);
	FrameState frame_state;
	ViewState view_state;
};

#endif
