#pragma once

// Shared deterministic GSQ input generator. No test registrations: safe for
// force-link anchored test translation units.
#include "../io/gsq/gsq_codec.h"
#include "core/templates/local_vector.h"

namespace TestGSQCodec {

using namespace gs_gsq;

// Fixed-seed generator; no global RNG state.
struct Rng {
	uint64_t state;
	explicit Rng(uint64_t p_seed) :
			state(p_seed * 0x9E3779B97F4A7C15ull + 1u) {}
	uint64_t next() {
		state ^= state << 13u;
		state ^= state >> 7u;
		state ^= state << 17u;
		return state;
	}
	float unit() {
		return float(next() >> 40u) / float(1u << 24u);
	}
	float range(float p_lo, float p_hi) {
		return p_lo + (p_hi - p_lo) * unit();
	}
};

struct Fixture {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh_high;
	uint32_t degree = 0;
};

inline Fixture make_fixture(uint32_t p_count, uint32_t p_degree, float p_extent, uint64_t p_seed) {
	Fixture f;
	f.degree = p_degree;
	f.gaussians.resize(p_count);
	const uint32_t high_order = sh_high_order_for_degree(p_degree);
	f.sh_high.resize(p_count * high_order);
	Rng rng(p_seed);
	for (uint32_t i = 0; i < p_count; i++) {
		Gaussian g;
		g.position = Vector3(rng.range(100.0f, 100.0f + p_extent), rng.range(-5.0f, 5.0f), rng.range(-p_extent, 0.0f));
		g.scale = Vector3(rng.range(0.01f, 0.5f), rng.range(0.01f, 0.5f), rng.range(0.001f, 0.02f));
		g.opacity = rng.unit();
		Quaternion q(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
		g.rotation = q.normalized();
		g.sh_dc = Color(rng.range(-1.5f, 1.5f), rng.range(-1.5f, 1.5f), rng.range(-1.5f, 1.5f), 1.0f);
		// Every third splat has no view dependence: its zero SH must stay exactly zero.
		const bool flat = i % 3u == 0u;
		if (p_degree > 0u) {
			for (Vector3 &v : g.sh_1) {
				v = flat ? Vector3() : Vector3(rng.range(-0.4f, 0.4f), rng.range(-0.4f, 0.4f), rng.range(-0.4f, 0.4f));
			}
		}
		for (uint32_t k = 0; k < high_order; k++) {
			f.sh_high[i * high_order + k] = flat ? Vector3() : Vector3(rng.range(-0.1f, 0.1f), rng.range(-0.1f, 0.1f), rng.range(-0.1f, 0.1f));
		}
		g.normal = Vector3(0, 1, 0);
		g.area = 1.0f;
		g.stroke_age = 0.25f;
		g.brush_axes = Vector2(1.0f, 2.0f);
		g.painterly_meta = 7u;
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		f.gaussians[i] = g;
	}
	return f;
}

} // namespace TestGSQCodec
