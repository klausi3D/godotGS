#ifndef GAUSSIAN_SCENE_DIRECTOR_INTERNAL_H
#define GAUSSIAN_SCENE_DIRECTOR_INTERNAL_H

// Small file-local helpers shared between gaussian_splat_scene_director.cpp and
// its partial-class translation units (e.g. scene_director_sphere_effectors.cpp).
// These were previously `static` free functions inside the god-file; they are
// hoisted here unchanged so a sibling TU can see them without duplicating the
// definition. Keep this header minimal — it is NOT a public module interface.

#include "gaussian_splat_scene_director.h"

#include <cstdint>

// Scope specificity ranking used to break priority ties when ordering sphere
// effectors. A scoped effector (subtree / explicit-root) is "more specific"
// than a world-scoped one; explicit-root outranks subtree. Used by the
// render-path payload builder in scene_director_sphere_effectors.cpp; kept in
// this shared header so the ranking has a single definition any sibling
// translation unit can reuse without redefining it.
static inline uint32_t _get_effector_scope_specificity(uint32_t p_scope_mode, bool p_has_scope_root) {
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

#endif // GAUSSIAN_SCENE_DIRECTOR_INTERNAL_H
