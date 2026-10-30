#ifndef GS_HLOD_DRAWN_NODE_H
#define GS_HLOD_DRAWN_NODE_H

#include "core/math/plane.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/vector.h"
#include "gaussian_gpu_layout.h"

#include <cstdint>

// Host construction of the per-draw GPU record of the HLOD route (DrawnNodeGPU; package C2). Pure and device free so
// the layout and the numbers are testable on the CPU.
namespace gs_hlod_gpu {

// `p_camera_offset` is DrawRecord::camera_offset: R s c + T - camera, formed in double by
// gs_world_frame::camera_relative_offset and converted to float last. It is view dependent, so it is written here and
// never into the shared ChunkMetaGPU payload.
inline DrawnNodeGPU make_drawn_node(uint32_t p_instance_id, uint32_t p_chunk_id, const Vector3 &p_camera_offset) {
	DrawnNodeGPU node = {};
	node.instance_id = p_instance_id;
	node.chunk_id = p_chunk_id;
	node.fade = 1.0f; // S6 cross-fade is not wired; fully opaque.
	node.reserved = 0u;
	node.camera_offset[0] = p_camera_offset.x;
	node.camera_offset[1] = p_camera_offset.y;
	node.camera_offset[2] = p_camera_offset.z;
	node.camera_offset[3] = 0.0f;
	return node;
}

// Frustum planes for the camera-relative depth pass: the planes of the camera-translated frustum (rotation of the
// camera, no translation). The four side planes pass through the origin (d == 0), which is where the camera sits in
// that frame; near and far keep d = +-distance (a few metres to km, never the camera's distance from the world
// origin, which is the large number a float would round).
inline Vector<Plane> camera_relative_frustum_planes(const Projection &p_projection, const Transform3D &p_camera_to_world) {
	return p_projection.get_projection_planes(Transform3D(p_camera_to_world.basis, Vector3()));
}

// Cache key for state that depends on the HLOD frame (cached sort order, sort reuse). Outside the HLOD route the
// generation is returned unchanged, so the canonical routes keep their keys. In the HLOD route two different origin
// revisions give different keys, so a cache built against one origin can never be served against another.
inline uint64_t origin_keyed_generation(uint64_t p_generation, bool p_hlod_relative, uint64_t p_origin_revision) {
	if (!p_hlod_relative) {
		return p_generation;
	}
	uint64_t mixed = p_generation;
	mixed ^= p_origin_revision + 0x9e3779b97f4a7c15ULL + (mixed << 6) + (mixed >> 2);
	return mixed;
}

} // namespace gs_hlod_gpu

#endif // GS_HLOD_DRAWN_NODE_H
