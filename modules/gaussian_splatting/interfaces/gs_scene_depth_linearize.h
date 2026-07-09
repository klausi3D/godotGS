#ifndef GS_SCENE_DEPTH_LINEARIZE_H
#define GS_SCENE_DEPTH_LINEARIZE_H

#include "core/math/projection.h"

// Scene-depth linearization pair for converting a raw NDC depth sample to positive
// view-space z: perspective uses view_z = mul / (add - raw); orthographic consumers
// use the z_near/z_far pair directly. ONE derivation shared by the whole-pixel
// composite (viewport_blit) and the per-splat raster clip (compositing slice D) so
// both sides convert scene depth with bit-identical math — a divergence would make
// the raster clip and the composite guard disagree at silhouettes.
struct GSSceneDepthLinearize {
	float mul = 0.0f;
	float add = 1.0f;
};

inline GSSceneDepthLinearize gs_derive_scene_depth_linearize(const Projection &p_cam_projection,
		bool p_orthogonal, float p_z_near, float p_z_far) {
	GSSceneDepthLinearize out;
	out.mul = p_z_near;
	out.add = p_z_far;
	if (!p_orthogonal) {
		Projection correction;
		correction.set_depth_correction(false);
		Projection temp = correction * p_cam_projection;
		out.mul = -temp.columns[3][2];
		out.add = temp.columns[2][2];
		if (out.mul * out.add < 0.0f) {
			out.add = -out.add;
		}
	}
	return out;
}

#endif // GS_SCENE_DEPTH_LINEARIZE_H
