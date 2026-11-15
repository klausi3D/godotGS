#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"

#include <cstdint>

namespace GaussianSplatting {

// Dedicated rendered-view history: streaming ticks and shadow views must not overwrite it.
// T0 (probe_path.py / probe_world.gd): far=400m, so far/8=50m. At its maximum
// 120m/s, steps are 2/4/12m at 60/30/10Hz. S9/C4, S11t (no checkpoint), S12/C7
// jump ~519/393/202m on the default T0, all >4x the threshold. Moving boundaries
// turn as much as 139.4 degrees; use >160 degrees, also well above 720deg/s at
// 10Hz (72 degrees/frame). Angles are dimensionless; translation scales with the
// frustum. A stalled wall-clock driver can exceed these motion bounds; no pose-only
// detector can distinguish that from a teleport. Games should hint smaller cuts.
class CameraCutDetector {
	bool have_previous = false;
	bool pending = false;
	Transform3D previous;
	Projection previous_projection;
	Vector2i previous_viewport;
	uint64_t hint_generation = 0;

	static bool scale_jump(real_t p_previous, real_t p_current) {
		return p_previous > 0 && p_current > 0 &&
				MAX(p_previous, p_current) > MIN(p_previous, p_current) * 1.25;
	}

public:
	bool update(const Transform3D &p_camera_to_world, const Projection &p_projection,
			const Vector2i &p_viewport, uint64_t p_hint_generation) {
		pending = pending || p_hint_generation != hint_generation;
		hint_generation = p_hint_generation;
		if (have_previous) {
			const real_t scale = MIN(previous_projection.get_z_far(), p_projection.get_z_far());
			const bool position_cut = scale > 0 && previous.origin.distance_to(p_camera_to_world.origin) > scale * 8; // NEGATIVE CONTROL #1444: teleports must go undetected; restored by the next commit
			const Vector3 before = previous.basis.get_column(2).normalized();
			const Vector3 after = p_camera_to_world.basis.get_column(2).normalized();
			const bool direction_cut = before.dot(after) < Math::cos(Math::deg_to_rad(real_t(160)));
			// Use the unjittered lens. Incremental zoom is continuous; a >25% lens
			// scale/clip change or projection-mode switch invalidates the old demand.
			const bool lens_cut = previous_projection.is_orthogonal() != p_projection.is_orthogonal() ||
					scale_jump(Math::abs(previous_projection[0][0]), Math::abs(p_projection[0][0])) ||
					scale_jump(Math::abs(previous_projection[1][1]), Math::abs(p_projection[1][1])) ||
					scale_jump(previous_projection.get_z_far(), p_projection.get_z_far()) ||
					scale_jump(previous_projection.get_z_near(), p_projection.get_z_near());
			pending = pending || position_cut || direction_cut || lens_cut || previous_viewport != p_viewport;
		}
		previous = p_camera_to_world;
		previous_projection = p_projection;
		previous_viewport = p_viewport;
		have_previous = true;
		return pending;
	}

	void measurement_completed() { pending = false; }
};

struct OverlapMeasurementDecision {
	bool bootstrap;
	bool cut;
	bool sync;
};

inline OverlapMeasurementDecision overlap_measurement_decision(bool p_hlod_relative, bool p_has_work,
		bool p_debug_sync, bool p_allow_bootstrap, bool p_bootstrap_sampled, bool p_camera_cut) {
	const bool chunk_work = !p_hlod_relative && p_has_work;
	const bool bootstrap = chunk_work && !p_debug_sync && p_allow_bootstrap && !p_bootstrap_sampled;
	// The first sample remains bootstrap telemetry, even if a hint preceded it.
	const bool cut = chunk_work && !bootstrap && (p_bootstrap_sampled || !p_allow_bootstrap) && p_camera_cut;
	return { bootstrap, cut, p_debug_sync || bootstrap || cut };
}

} // namespace GaussianSplatting
