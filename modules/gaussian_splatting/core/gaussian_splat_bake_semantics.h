#pragma once

#include "gaussian_splat_hlod_bake.h"
#include "core/variant/dictionary.h"

namespace gs_hlod {
// ADR dynamics 6.1: independent of the partition and merge grids.
constexpr double kIslandHoleCellSizeM = 0.25;
// volumes are serializable descriptors returned by the authoring nodes.
// source contains PLY property-presence and island side data (not GPU lanes).
Error prepare_semantic_bake(const BakeInput &p_input, const Array &p_volumes,
        const Dictionary &p_source, const String &p_island_directory,
        SplatList &r_world, Dictionary &r_metadata, Dictionary &r_report, String &r_reason);
}
