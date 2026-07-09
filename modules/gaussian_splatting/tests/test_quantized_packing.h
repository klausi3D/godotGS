/**************************************************************************/
/*  test_quantized_packing.h                                              */
/*  Round-trip unit tests for pack_gaussian_quantized() (80-byte layout). */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_QUANTIZED_PACKING_H
#define GAUSSIAN_SPLATTING_TEST_QUANTIZED_PACKING_H

#include "test_macros.h"

#include "../core/streaming_quantization.h"
#include "../renderer/float16_utils.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/quantization_config.h"

// These exercise pack_gaussian_quantized() against the CPU dequantizers, which use the
// identical 1/((1<<bits)-1) normalization as the GLSL side (quantization_dequant.glsl):
// dequantize_position/scale are therefore a faithful reference for the shader. The end-
// to-end GLSL match (rendered pixels) is covered separately by the GPU-harness PSNR test.

namespace TestQuantizedPacking {

inline ChunkQuantizationInfo make_chunk(const Vector3 &pos_min, const Vector3 &pos_max,
		const Vector3 &scale_min, const Vector3 &scale_max, uint32_t pos_bits, uint32_t scale_bits,
		bool quantize_scale) {
	ChunkQuantizationInfo info;
	info.position_min = pos_min;
	info.position_max = pos_max;
	info.scale_min = scale_min;
	info.scale_max = scale_max;
	info.position_bits = pos_bits;
	info.scale_bits = scale_bits;
	info.scales_quantized = quantize_scale;
	const float epsilon = 1e-6f;
	info.position_range = pos_max - pos_min;
	info.position_range.x = MAX(info.position_range.x, epsilon);
	info.position_range.y = MAX(info.position_range.y, epsilon);
	info.position_range.z = MAX(info.position_range.z, epsilon);
	info.scale_range = scale_max - scale_min;
	info.scale_range.x = MAX(info.scale_range.x, epsilon);
	info.scale_range.y = MAX(info.scale_range.y, epsilon);
	info.scale_range.z = MAX(info.scale_range.z, epsilon);
	return info;
}

inline Gaussian make_gaussian(const Vector3 &pos, const Vector3 &scale, const Quaternion &rot,
		float opacity, const Color &dc) {
	Gaussian g;
	g.position = pos;
	g.opacity = opacity;
	g.scale = scale;
	g.area = 1.0f;
	g.rotation = rot;
	g.sh_dc = dc;
	for (int i = 0; i < 3; i++) {
		g.sh_1[i] = Vector3();
	}
	g.normal = Vector3(0.0f, 1.0f, 0.0f);
	g.stroke_age = 0.0f;
	g.brush_axes = Vector2();
	g.painterly_meta = 0u;
	g.render_meta = 0u;
	return g;
}

} // namespace TestQuantizedPacking

TEST_CASE("[GaussianSplatting][Quantized] Position round-trips within the per-axis step bound") {
	const ChunkQuantizationInfo chunk = TestQuantizedPacking::make_chunk(
			Vector3(-5.0f, -2.0f, 10.0f), Vector3(5.0f, 8.0f, 30.0f),
			Vector3(), Vector3(1, 1, 1), 16, 12, false);
	const float max_err = chunk.get_max_position_error();

	// Interior, both boundaries, and a mid value must all land within half a step.
	const Vector3 samples[] = {
		Vector3(-5.0f, -2.0f, 10.0f), // min corner -> quantized 0
		Vector3(5.0f, 8.0f, 30.0f), // max corner -> quantized (2^16-1)
		Vector3(0.0f, 3.0f, 20.0f), // interior
		Vector3(-4.9f, 7.99f, 10.01f), // near boundaries
	};
	for (const Vector3 &p : samples) {
		Gaussian g = TestQuantizedPacking::make_gaussian(p, Vector3(1, 1, 1), Quaternion(), 0.5f, Color(0.1f, 0.2f, 0.3f, 1.0f));
		PackedGaussianQuantized packed;
		SHCompressionMetrics metrics;
		pack_gaussian_quantized(g, chunk, 7, packed, metrics);

		const Vector3 rt = chunk.dequantize_position(
				packed.quantized_position[0], packed.quantized_position[1], packed.quantized_position[2]);
		CHECK(Math::abs(rt.x - p.x) <= max_err + 1e-5f);
		CHECK(Math::abs(rt.y - p.y) <= max_err + 1e-5f);
		CHECK(Math::abs(rt.z - p.z) <= max_err + 1e-5f);
	}
}

TEST_CASE("[GaussianSplatting][Quantized] chunk_id, opacity, and sh_dc are stored bit-exact") {
	const ChunkQuantizationInfo chunk = TestQuantizedPacking::make_chunk(
			Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(), Vector3(1, 1, 1), 16, 12, false);
	const Color dc(0.123456f, -0.98765f, 42.5f, 0.7f);
	Gaussian g = TestQuantizedPacking::make_gaussian(Vector3(0.5f, 0.5f, 0.5f), Vector3(1, 1, 1), Quaternion(), 0.813f, dc);
	PackedGaussianQuantized packed;
	SHCompressionMetrics metrics;
	pack_gaussian_quantized(g, chunk, 4321, packed, metrics);

	CHECK(packed.chunk_id == uint16_t(4321));
	CHECK(packed.opacity == 0.813f); // FP32, exact
	CHECK(packed.sh_dc[0] == dc.r); // FP32, exact
	CHECK(packed.sh_dc[1] == dc.g);
	CHECK(packed.sh_dc[2] == dc.b);
	CHECK(packed.sh_dc[3] == dc.a);
}

TEST_CASE("[GaussianSplatting][Quantized] Scale quantizes only when the chunk enables it") {
	// scales_quantized == false -> the packer must emit zeros (the shader substitutes 1.0).
	const ChunkQuantizationInfo off = TestQuantizedPacking::make_chunk(
			Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(0.01f, 0.01f, 0.01f), Vector3(2, 2, 2), 16, 12, false);
	Gaussian g = TestQuantizedPacking::make_gaussian(Vector3(0.5f, 0.5f, 0.5f), Vector3(1.0f, 1.5f, 0.5f), Quaternion(), 0.5f, Color());
	PackedGaussianQuantized packed;
	SHCompressionMetrics metrics;
	pack_gaussian_quantized(g, off, 0, packed, metrics);
	CHECK(packed.quantized_scale[0] == 0);
	CHECK(packed.quantized_scale[1] == 0);
	CHECK(packed.quantized_scale[2] == 0);

	// scales_quantized == true -> round-trips within the scale step bound.
	const ChunkQuantizationInfo on = TestQuantizedPacking::make_chunk(
			Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(0.5f, 0.5f, 0.5f), Vector3(2.0f, 2.0f, 2.0f), 16, 12, true);
	const float max_err = on.get_max_scale_error();
	PackedGaussianQuantized packed_on;
	SHCompressionMetrics metrics_on;
	pack_gaussian_quantized(g, on, 0, packed_on, metrics_on);
	const Vector3 rt = on.dequantize_scale(
			packed_on.quantized_scale[0], packed_on.quantized_scale[1], packed_on.quantized_scale[2]);
	CHECK(Math::abs(rt.x - 1.0f) <= max_err + 1e-5f);
	CHECK(Math::abs(rt.y - 1.5f) <= max_err + 1e-5f);
	CHECK(Math::abs(rt.z - 0.5f) <= max_err + 1e-5f);
}

TEST_CASE("[GaussianSplatting][Quantized] Rotation and normal survive the FP16 round-trip") {
	const ChunkQuantizationInfo chunk = TestQuantizedPacking::make_chunk(
			Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(), Vector3(1, 1, 1), 16, 12, false);
	const Quaternion q = Quaternion(0.18257418f, 0.36514837f, 0.54772256f, 0.73029674f); // normalized (1,2,3,4)
	Gaussian g = TestQuantizedPacking::make_gaussian(Vector3(0.5f, 0.5f, 0.5f), Vector3(1, 1, 1), q, 0.5f, Color());
	g.normal = Vector3(0.3f, -0.6f, 0.74f);
	g.stroke_age = 2.5f;
	PackedGaussianQuantized packed;
	SHCompressionMetrics metrics;
	pack_gaussian_quantized(g, chunk, 0, packed, metrics);

	const float rot_tol = 1e-3f; // FP16 mantissa is ~11 bits
	CHECK(Math::abs(Float16Utils::half_to_float(packed.rotation[0]) - q.x) <= rot_tol);
	CHECK(Math::abs(Float16Utils::half_to_float(packed.rotation[1]) - q.y) <= rot_tol);
	CHECK(Math::abs(Float16Utils::half_to_float(packed.rotation[2]) - q.z) <= rot_tol);
	CHECK(Math::abs(Float16Utils::half_to_float(packed.rotation[3]) - q.w) <= rot_tol);

	float nx = 0.0f, ny = 0.0f, nz = 0.0f, stroke = 0.0f;
	Float16Utils::unpack_half2(packed.normal_xy, nx, ny);
	Float16Utils::unpack_half2(packed.normal_z_stroke, nz, stroke);
	CHECK(Math::abs(nx - 0.3f) <= rot_tol);
	CHECK(Math::abs(ny - (-0.6f)) <= rot_tol);
	CHECK(Math::abs(nz - 0.74f) <= rot_tol);
	CHECK(Math::abs(stroke - 2.5f) <= 5e-3f);
}

TEST_CASE("[GaussianSplatting][Quantized] Higher-order SH fills the fixed 6-slot array, unused slots zeroed") {
	const ChunkQuantizationInfo chunk = TestQuantizedPacking::make_chunk(
			Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(), Vector3(1, 1, 1), 16, 12, false);
	Gaussian g = TestQuantizedPacking::make_gaussian(Vector3(0.5f, 0.5f, 0.5f), Vector3(1, 1, 1), Quaternion(), 0.5f, Color());
	g.sh_1[0] = Vector3(0.5f, 0.25f, 0.125f);
	g.sh_1[1] = Vector3(0.1f, 0.2f, 0.3f);
	g.sh_1[2] = Vector3();
	PackedGaussianQuantized packed;
	SHCompressionMetrics metrics;
	// 3 first-order coeffs, no higher-order: slots 0-2 encode, 3-5 stay zero.
	pack_gaussian_quantized(g, chunk, 0, packed, metrics, nullptr, 3, 0);
	CHECK(packed.sh_encoded[0] != 0u); // non-zero coeff -> non-zero RGB9E5
	CHECK(packed.sh_encoded[1] != 0u);
	CHECK(packed.sh_encoded[3] == 0u); // unused
	CHECK(packed.sh_encoded[4] == 0u);
	CHECK(packed.sh_encoded[5] == 0u);

	// Deterministic: identical inputs pack byte-identically.
	PackedGaussianQuantized packed2;
	SHCompressionMetrics metrics2;
	pack_gaussian_quantized(g, chunk, 0, packed2, metrics2, nullptr, 3, 0);
	CHECK(memcmp(&packed, &packed2, sizeof(PackedGaussianQuantized)) == 0);
}

TEST_CASE("[GaussianSplatting][Quantized] Non-finite inputs are floored deterministically") {
	const ChunkQuantizationInfo chunk = TestQuantizedPacking::make_chunk(
			Vector3(-1, -1, -1), Vector3(1, 1, 1), Vector3(0.1f, 0.1f, 0.1f), Vector3(1, 1, 1), 16, 12, true);
	const float nan = NAN;
	const float inf = INFINITY;
	Gaussian g = TestQuantizedPacking::make_gaussian(
			Vector3(nan, inf, -inf), Vector3(nan, 0.5f, inf),
			Quaternion(nan, 0.0f, 0.0f, 1.0f), nan, Color(nan, inf, -inf, nan));
	g.normal = Vector3(nan, inf, 0.5f);
	g.stroke_age = inf;
	g.area = nan;
	PackedGaussianQuantized packed;
	SHCompressionMetrics metrics;
	pack_gaussian_quantized(g, chunk, 9, packed, metrics);

	// Quantized position components must be valid uint16 (no UB cast of NaN).
	CHECK(packed.quantized_position[0] <= 0xFFFF);
	// Opacity/sh_dc floored to a finite value.
	CHECK(Math::is_finite(packed.opacity));
	CHECK(Math::is_finite(packed.sh_dc[0]));
	CHECK(Math::is_finite(packed.sh_dc[3]));
	// Non-finite rotation floors to identity so the shader's normalize() cannot NaN.
	CHECK(Float16Utils::half_to_float(packed.rotation[3]) == 1.0f);
	CHECK(Float16Utils::half_to_float(packed.rotation[0]) == 0.0f);
	// Area FP16 finite.
	CHECK(Math::is_finite(Float16Utils::half_to_float(packed.area_fp16)));

	// Determinism under non-finite inputs.
	PackedGaussianQuantized packed2;
	SHCompressionMetrics metrics2;
	pack_gaussian_quantized(g, chunk, 9, packed2, metrics2);
	CHECK(memcmp(&packed, &packed2, sizeof(PackedGaussianQuantized)) == 0);
}

TEST_CASE("[GaussianSplatting][Quantized] Degenerate chunk bounds do not divide by zero") {
	// Zero-extent bounds get epsilon-floored ranges; a single-point chunk must pack finitely.
	const ChunkQuantizationInfo chunk = TestQuantizedPacking::make_chunk(
			Vector3(3, 3, 3), Vector3(3, 3, 3), Vector3(1, 1, 1), Vector3(1, 1, 1), 16, 12, true);
	Gaussian g = TestQuantizedPacking::make_gaussian(Vector3(3, 3, 3), Vector3(1, 1, 1), Quaternion(), 0.5f, Color());
	PackedGaussianQuantized packed;
	SHCompressionMetrics metrics;
	pack_gaussian_quantized(g, chunk, 0, packed, metrics);
	const Vector3 rt = chunk.dequantize_position(
			packed.quantized_position[0], packed.quantized_position[1], packed.quantized_position[2]);
	CHECK(Math::is_finite(rt.x));
	CHECK(Math::abs(rt.x - 3.0f) <= 1e-3f);
}

TEST_CASE("[GaussianSplatting][Quantized] position_bits is capped at 16 (uint16 storage), not 24") {
	// Regression: the 80-byte PackedGaussianQuantized stores quantized_position as
	// uint16[3]. position_bits > 16 would quantize to > 65535 and silently truncate on
	// the CPU store while GLSL dequantized at the full 1/((1<<bits)-1) scale -> corrupted
	// positions. The config (the single source of the bits used by both the packer and
	// the uploaded ChunkQuantization) must cap at 16.
	QuantizationConfig cfg;

	cfg.position_bits = 16;
	CHECK(cfg.validate()); // 16 is the max valid value.

	cfg.position_bits = 17;
	CHECK_FALSE(cfg.validate()); // above the uint16 storage limit -> invalid.
	CHECK(cfg.get_validation_errors().contains("Position bits must be <= 16"));

	cfg.position_bits = 24;
	CHECK_FALSE(cfg.validate()); // the old (silently-truncating) max is now rejected.

	cfg.position_bits = 8;
	CHECK(cfg.validate()); // lower bound still valid.
}

#endif // GAUSSIAN_SPLATTING_TEST_QUANTIZED_PACKING_H
