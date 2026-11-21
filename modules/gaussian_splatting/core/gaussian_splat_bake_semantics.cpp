#include "gaussian_splat_bake_semantics.h"

namespace gs_hlod {
// Tests-first seam: the parent copies input, without semantic assignment/extraction.
Error prepare_semantic_bake(const BakeInput &p_input, const Array &, const Dictionary &,
        const String &, SplatList &r_world, Dictionary &, Dictionary &, String &) {
    r_world.sh_high_order_count = p_input.sh_high_order_count;
    return r_world.append({p_input.gaussians, p_input.sh_high_order, p_input.count, p_input.sh_high_order_count}, 0, p_input.count);
}
}
