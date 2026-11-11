#ifndef GAUSSIAN_SPLAT_HLOD_BAKE_BOUNDS_H
#define GAUSSIAN_SPLAT_HLOD_BAKE_BOUNDS_H

#include "gaussian_splat_hlod_merge.h"

#include <cfloat>

namespace gs_hlod {
namespace bake_bounds {

// Shared by in-core baking and tiled stitching: all accumulation is in double,
// and stored float offsets contain exactly the bounds the validator reconstructs.
struct BoundsAccumulator {
	double lo[3] = { DBL_MAX, DBL_MAX, DBL_MAX };
	double hi[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
	void add(const double p_lo[3], const double p_hi[3]);
	bool valid() const { return lo[0] <= hi[0]; }
};

float round_down_offset(double p_center, double p_target);
float round_up_offset(double p_center, double p_target);
float round_up(double p_value);

// p_shift converts payload positions into the accumulator/midpoint's frame.
// Leaves are absolute; interior payloads are relative to their node cell centre.
void add_payload_bounds(const SplatSpan &p_span, const double p_shift[3], BoundsAccumulator &r_bounds);
double payload_radius(const SplatSpan &p_span, const double p_shift[3], const double p_mid[3]);

} // namespace bake_bounds
} // namespace gs_hlod

#endif // GAUSSIAN_SPLAT_HLOD_BAKE_BOUNDS_H
