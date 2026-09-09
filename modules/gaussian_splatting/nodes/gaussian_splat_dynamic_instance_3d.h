/**
 * @file gaussian_splat_dynamic_instance_3d.h
 * @brief DEPRECATED one-release compatibility shim — use GaussianSplatNode3D.
 *
 * This class exists ONLY to keep serialized scenes that reference
 * `GaussianSplatDynamicInstance3D` loadable for one release after the
 * canonical implementation was removed (commit "remove dynamic instance
 * node"). It inherits GaussianSplatNode3D verbatim and adds nothing —
 * `splat_asset`, `gaussian_data`, `ply_file_path` (compat), registration,
 * and rendering all forward to the canonical path. Instantiation emits
 * WARN_DEPRECATED so users know to migrate. The class will be removed
 * in a future release.
 */

#ifndef GAUSSIAN_SPLAT_DYNAMIC_INSTANCE_3D_H
#define GAUSSIAN_SPLAT_DYNAMIC_INSTANCE_3D_H

#ifndef DISABLE_DEPRECATED

#include "gaussian_splat_node_3d.h"

class GaussianSplatDynamicInstance3D : public GaussianSplatNode3D {
    GDCLASS(GaussianSplatDynamicInstance3D, GaussianSplatNode3D);

protected:
    static void _bind_methods() {}

public:
    // Surfaces the deprecation in the scene tree (yellow warning triangle) for scenes
    // that already contain this node. The ctor's WARN_DEPRECATED_MSG only reaches the
    // Output log, which users routinely never open.
    PackedStringArray get_configuration_warnings() const override;

    GaussianSplatDynamicInstance3D();
};

#endif // DISABLE_DEPRECATED

#endif // GAUSSIAN_SPLAT_DYNAMIC_INSTANCE_3D_H
