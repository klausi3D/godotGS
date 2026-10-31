#include "gaussian_splat_world_frame.h"

#include <cmath>

namespace gs_world_frame {

namespace {
bool checked_add(int64_t p_a, int64_t p_b, int64_t &r_out) {
	if ((p_b > 0 && p_a > INT64_MAX - p_b) || (p_b < 0 && p_a < INT64_MIN - p_b)) { return false; }
	r_out = p_a + p_b;
	return true;
}
bool checked_sub(int64_t p_a, int64_t p_b, int64_t &r_out) {
	if ((p_b < 0 && p_a > INT64_MAX + p_b) || (p_b > 0 && p_a < INT64_MIN + p_b)) { return false; }
	r_out = p_a - p_b;
	return true;
}
bool in_range(int64_t p_cell) { return p_cell >= -MAX_CELL && p_cell <= MAX_CELL; }

// Moves `r_local` into [0, EDGE) and carries whole cells into `r_cell`. Subtracting a multiple of the
// power-of-two edge from a double is exact while the value stays below 2^52 edges.
bool normalize_axis(int64_t &r_cell, double &r_local) {
	if (!std::isfinite(r_local)) { return false; }
	const double carry = std::floor(r_local / LATTICE_EDGE);
	if (std::abs(carry) > double(MAX_CELL)) { return false; }
	int64_t next = 0;
	if (!checked_add(r_cell, int64_t(carry), next) || !in_range(next)) { return false; }
	double local = r_local - carry * LATTICE_EDGE;
	if (local >= LATTICE_EDGE) { // Rounding guard at the upper edge.
		local -= LATTICE_EDGE;
		if (!checked_add(next, 1, next) || !in_range(next)) { return false; }
	} else if (local < 0.0) {
		local += LATTICE_EDGE;
		if (!checked_add(next, -1, next) || !in_range(next)) { return false; }
	}
	r_cell = next;
	r_local = local;
	return true;
}

// floor(a / b) for b > 0.
int64_t floor_div(int64_t p_a, int64_t p_b) {
	int64_t q = p_a / p_b;
	if ((p_a % p_b) != 0 && p_a < 0) { --q; }
	return q;
}
} // namespace

bool PreciseCoord::from_doubles(const double p_value[3], PreciseCoord &r_out) {
	PreciseCoord out;
	for (int a = 0; a < 3; ++a) {
		if (!std::isfinite(p_value[a])) { return false; }
		const double cell = std::floor(p_value[a] / LATTICE_EDGE);
		if (std::abs(cell) > double(MAX_CELL)) { return false; }
		out.cell[a] = int64_t(cell);
		out.local[a] = p_value[a] - cell * LATTICE_EDGE; // Exact: a multiple of the edge is subtracted.
		if (!normalize_axis(out.cell[a], out.local[a])) { return false; }
	}
	r_out = out;
	return true;
}

bool PreciseCoord::from_tree_cell_center(const double p_origin[3], int32_t p_e, int64_t p_ix, int64_t p_iy, int64_t p_iz,
		PreciseCoord &r_out) {
	// Cell edges are 2^e; the lattice edge is 2^LATTICE_EXP. Keep e in a range where every product is exact.
	if (p_e < -40 || p_e > 50) { return false; }
	PreciseCoord out;
	if (!from_doubles(p_origin, out)) { return false; }
	const int64_t index[3] = { p_ix, p_iy, p_iz };
	const double half_edge = std::ldexp(1.0, p_e - 1);
	for (int a = 0; a < 3; ++a) {
		int64_t whole = 0; // Whole lattice cells contributed by index * 2^e.
		double rest = 0.0; // Remainder in metres, < LATTICE_EDGE.
		if (p_e >= LATTICE_EXP) {
			const int64_t scale = int64_t(1) << (p_e - LATTICE_EXP);
			if (index[a] > MAX_CELL / scale || index[a] < -MAX_CELL / scale) { return false; }
			whole = index[a] * scale;
		} else {
			const int64_t per_cell = int64_t(1) << (LATTICE_EXP - p_e);
			whole = floor_div(index[a], per_cell);
			rest = double(index[a] - whole * per_cell) * std::ldexp(1.0, p_e);
		}
		if (!checked_add(out.cell[a], whole, out.cell[a]) || !in_range(out.cell[a])) { return false; }
		out.local[a] += rest + half_edge;
		if (!normalize_axis(out.cell[a], out.local[a])) { return false; }
	}
	r_out = out;
	return true;
}

bool PreciseCoord::offset_from(const PreciseCoord &p_base, double r_offset[3]) const {
	double result[3];
	for (int a = 0; a < 3; ++a) {
		int64_t dc = 0;
		if (!checked_sub(cell[a], p_base.cell[a], dc)) { return false; }
		result[a] = double(dc) * LATTICE_EDGE + (local[a] - p_base.local[a]);
		if (!std::isfinite(result[a])) { return false; }
	}
	for (int a = 0; a < 3; ++a) { r_offset[a] = result[a]; }
	return true;
}

bool PreciseCoord::translated(const double p_delta[3], PreciseCoord &r_out) const {
	PreciseCoord out = *this;
	for (int a = 0; a < 3; ++a) {
		if (!std::isfinite(p_delta[a])) { return false; }
		out.local[a] += p_delta[a];
		if (!normalize_axis(out.cell[a], out.local[a])) { return false; }
	}
	r_out = out;
	return true;
}

void PreciseCoord::to_doubles(double r_out[3]) const {
	for (int a = 0; a < 3; ++a) { r_out[a] = double(cell[a]) * LATTICE_EDGE + local[a]; }
}

bool PreciseCoord::operator==(const PreciseCoord &p_other) const {
	for (int a = 0; a < 3; ++a) {
		if (cell[a] != p_other.cell[a] || local[a] != p_other.local[a]) { return false; }
	}
	return true;
}

bool PrecisePlacement::from_frame_transform(const Transform3D &p_transform, const PreciseCoord &p_frame_origin,
		PrecisePlacement &r_out) {
	const double delta[3] = { double(p_transform.origin.x), double(p_transform.origin.y), double(p_transform.origin.z) };
	PreciseCoord translation;
	if (!p_frame_origin.translated(delta, translation)) { return false; }
	r_out.basis = p_transform.basis;
	r_out.translation = translation;
	return true;
}

bool PrecisePlacement::to_frame_transform(const PreciseCoord &p_frame_origin, Transform3D &r_out) const {
	double rel[3];
	if (!translation.offset_from(p_frame_origin, rel)) { return false; }
	const Vector3 origin = Vector3(real_t(rel[0]), real_t(rel[1]), real_t(rel[2]));
	if (!origin.is_finite()) { return false; }
	r_out = Transform3D(basis, origin);
	return true;
}

bool camera_relative_offset(const PrecisePlacement &p_placement, const PreciseCoord &p_node_center,
		const PreciseCoord &p_camera, Vector3 &r_offset) {
	// T - camera from the integer addresses (exact), then R s c in double. c is split into whole cells and
	// the in-cell remainder so the partial products stay small relative to their sum.
	double t_minus_camera[3];
	if (!p_placement.translation.offset_from(p_camera, t_minus_camera)) { return false; }
	double c_whole[3], c_local[3];
	for (int a = 0; a < 3; ++a) {
		c_whole[a] = double(p_node_center.cell[a]) * LATTICE_EDGE;
		c_local[a] = p_node_center.local[a];
	}
	const Basis &b = p_placement.basis;
	double total[3];
	for (int row = 0; row < 3; ++row) {
		const double r0 = b.rows[row][0], r1 = b.rows[row][1], r2 = b.rows[row][2];
		const double whole = r0 * c_whole[0] + r1 * c_whole[1] + r2 * c_whole[2];
		const double local = r0 * c_local[0] + r1 * c_local[1] + r2 * c_local[2];
		total[row] = (t_minus_camera[row] + whole) + local;
	}
	const Vector3 out = Vector3(real_t(total[0]), real_t(total[1]), real_t(total[2]));
	if (!out.is_finite()) { return false; }
	r_offset = out;
	return true;
}

bool WorldOriginContext::set_origin(const PreciseCoord &p_origin) {
	for (int a = 0; a < 3; ++a) {
		if (!std::isfinite(p_origin.local[a]) || p_origin.local[a] < 0.0 || p_origin.local[a] >= LATTICE_EDGE ||
				!in_range(p_origin.cell[a])) {
			return false;
		}
	}
	if (p_origin == origin) { return true; }
	const PreciseCoord old = origin;
	origin = p_origin;
	++revision;
	// Copy: a hook may add or remove hooks while being notified.
	LocalVector<OriginShiftHook *> notify = hooks;
	for (uint32_t i = 0; i < notify.size(); ++i) {
		notify[i]->on_world_origin_changed(old, origin, revision);
	}
	return true;
}

bool WorldOriginContext::shift_origin_by(const double p_delta[3]) {
	PreciseCoord next;
	if (!origin.translated(p_delta, next)) { return false; }
	return set_origin(next);
}

void WorldOriginContext::add_hook(OriginShiftHook *p_hook) {
	if (!p_hook) { return; }
	for (uint32_t i = 0; i < hooks.size(); ++i) {
		if (hooks[i] == p_hook) { return; }
	}
	hooks.push_back(p_hook);
}

void WorldOriginContext::remove_hook(OriginShiftHook *p_hook) {
	for (uint32_t i = 0; i < hooks.size(); ++i) {
		if (hooks[i] == p_hook) {
			hooks.remove_at(i);
			return;
		}
	}
}

} // namespace gs_world_frame
