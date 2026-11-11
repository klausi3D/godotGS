#include "gaussian_splat_hlod_tiled_bake.h"

namespace gs_hlod {

Error TiledAutoTree::build(const TiledBakeSource &p_source, const TiledBakeParams &p_params,
		BakeResult &r_result, TiledBakeReport &r_report, String *r_error) {
	// D2-2 phase 1 deliberately has no tiling, scheduling or bake implementation.
	// The registered host tests must fail at runtime, not at compile/link time.
	(void)p_source;
	(void)p_params;
	r_result = BakeResult();
	r_report = TiledBakeReport();
	if (r_error) {
		*r_error = "D2-2 tiled bake is not implemented (tests-first stub).";
	}
	return ERR_UNAVAILABLE;
}

} // namespace gs_hlod
