/**************************************************************************/
/*  test_sh_encoding.h                                                    */
/*  Host round-trip tests for the signed SH storage (#1054).              */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_SH_ENCODING_H
#define GAUSSIAN_SPLATTING_TEST_SH_ENCODING_H

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/streaming_quantization.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gaussian_splat_renderer.h"

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#include <cstring>
#include <limits>

// #1054 / docs/architecture/adr-splat-colour-encoding.md (option E, evidence item 1).
// SH coefficients are signed. Both GPU packers now store each coefficient triplet as three
// 10-bit two's-complement integers scaled by one per-splat magnitude in the sh_dc.w lane.
// These cases pack through the production packers and decode through
// gs_decode_sh_snorm10(), the C++ mirror of the GLSL decode_sh_snorm10() that
// tests/ci/check_gaussian_layout_sync.py pins to the shader. The bound checked is the
// encoding's stated worst case: |decoded - original| <= scale / (2 * 511) per component.
//
// Before the fix every negative channel was clamped to 0 by the unsigned RGB9E5 packer, so
// the sign assertions below are the defect's reproduction; the bound assertions pin precision.

namespace TestSHEncoding {

inline Gaussian make_sh_gaussian(const Vector3 &p_c0, const Vector3 &p_c1, const Vector3 &p_c2) {
	Gaussian g;
	g.position = Vector3(0.5f, 0.5f, 0.5f);
	g.opacity = 1.0f;
	g.scale = Vector3(0.1f, 0.1f, 0.1f);
	g.area = 1.0f;
	g.rotation = Quaternion();
	g.sh_dc = Color(0.1f, -0.2f, 0.3f, 1.0f);
	g.sh_1[0] = p_c0;
	g.sh_1[1] = p_c1;
	g.sh_1[2] = p_c2;
	g.normal = Vector3(0.0f, 1.0f, 0.0f);
	g.stroke_age = 0.0f;
	g.brush_axes = Vector2();
	g.painterly_meta = 0u;
	g.render_meta = 0u;
	return g;
}

inline ChunkQuantizationInfo make_unit_chunk() {
	ChunkQuantizationInfo info;
	info.position_min = Vector3();
	info.position_max = Vector3(1, 1, 1);
	info.position_range = Vector3(1, 1, 1);
	info.scale_min = Vector3();
	info.scale_max = Vector3(1, 1, 1);
	info.scale_range = Vector3(1, 1, 1);
	info.position_bits = 16;
	info.scale_bits = 12;
	info.scales_quantized = false;
	return info;
}

inline float max_abs_component(const Vector3 *p_coeffs, uint32_t p_count) {
	float m = 0.0f;
	for (uint32_t i = 0; i < p_count; i++) {
		m = MAX(m, MAX(Math::abs(p_coeffs[i].x), MAX(Math::abs(p_coeffs[i].y), Math::abs(p_coeffs[i].z))));
	}
	return m;
}

inline uint32_t word_bits(float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(uint32_t));
	return bits;
}

// Checks every decoded coefficient against the original within the SNORM10 bound, and that
// the sign of every component larger than one quantization step survives.
inline void check_round_trip(const Vector3 *p_expected, const uint32_t *p_words, uint32_t p_count, float p_scale,
		const char *p_label) {
	const float bound = p_scale / (2.0f * float(GS_SH_SNORM10_MAX)) + p_scale * 1e-6f + 1e-7f;
	const float step = p_scale / float(GS_SH_SNORM10_MAX);
	for (uint32_t i = 0; i < p_count; i++) {
		const Vector3 decoded = gs_decode_sh_snorm10(p_words[i], p_scale);
		for (int axis = 0; axis < 3; axis++) {
			const float want = p_expected[i][axis];
			const float got = decoded[axis];
			CHECK_MESSAGE(Math::abs(got - want) <= bound,
					vformat("%s: coefficient %d axis %d decoded %f, expected %f (bound %f)", p_label, i, axis, got, want, bound));
			if (Math::abs(want) > step) {
				CHECK_MESSAGE((got < 0.0f) == (want < 0.0f),
						vformat("%s: coefficient %d axis %d lost its sign (%f -> %f)", p_label, i, axis, want, got));
			}
		}
	}
}

} // namespace TestSHEncoding

TEST_CASE("[GaussianSplatting][SHEncoding] Negative band-1 SH survives the unquantized packer (#1054 reproduction)") {
	// The audit probe's splat: before the fix sh_1[0] decoded as (0, 0.2, 0).
	const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(-0.3f, 0.2f, -0.1f), Vector3(), Vector3());
	PackedGaussian packed = {};
	SHCompressionMetrics metrics;
	pack_gaussian(g, packed, metrics, nullptr, 3, 0);

	CHECK(gs_get_sh_encoding(packed.sh_metadata) == GS_SH_ENCODING_SNORM10_SPLAT_SCALE);
	CHECK(packed.sh.dc[3] == doctest::Approx(0.3f)); // per-splat scale = largest |component|
	const Vector3 decoded = gs_decode_sh_snorm10(TestSHEncoding::word_bits(packed.sh.encoded[0]), packed.sh.dc[3]);
	CHECK(decoded.x < -0.29f);
	CHECK(decoded.y > 0.19f);
	CHECK(decoded.z < -0.09f);
}

TEST_CASE("[GaussianSplatting][SHEncoding] Negative band-1 SH survives the quantized packer (#1054 reproduction)") {
	const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(-0.3f, 0.2f, -0.1f), Vector3(), Vector3());
	PackedGaussianQuantized packed = {};
	SHCompressionMetrics metrics;
	pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, packed, metrics, nullptr, 3, 0);

	CHECK(packed.sh_dc[3] == doctest::Approx(0.3f));
	const Vector3 decoded = gs_decode_sh_snorm10(packed.sh_encoded[0], packed.sh_dc[3]);
	CHECK(decoded.x < -0.29f);
	CHECK(decoded.y > 0.19f);
	CHECK(decoded.z < -0.09f);
}

TEST_CASE("[GaussianSplatting][SHEncoding] Round-trip within scale/1022 for negative, positive, mixed, tiny and large coefficients") {
	struct Case {
		const char *label;
		Vector3 first[3];
		Vector3 high[9];
	};
	const Case cases[] = {
		{ "all negative", { Vector3(-0.4f, -0.2f, -0.05f), Vector3(-0.1f, -0.3f, -0.2f), Vector3(-0.01f, -0.02f, -0.03f) },
				{ Vector3(-0.2f, -0.1f, -0.3f), Vector3(-0.05f, -0.06f, -0.07f), Vector3(-0.3f, -0.3f, -0.3f), Vector3(-0.001f, -0.002f, -0.003f),
						Vector3(-0.15f, -0.25f, -0.35f), Vector3(-0.4f, -0.1f, -0.2f), Vector3(-0.08f, -0.09f, -0.1f), Vector3(-0.2f, -0.2f, -0.2f),
						Vector3(-0.33f, -0.11f, -0.22f) } },
		{ "all positive", { Vector3(0.4f, 0.2f, 0.05f), Vector3(0.1f, 0.3f, 0.2f), Vector3(0.01f, 0.02f, 0.03f) },
				{ Vector3(0.2f, 0.1f, 0.3f), Vector3(0.05f, 0.06f, 0.07f), Vector3(0.3f, 0.3f, 0.3f), Vector3(0.001f, 0.002f, 0.003f),
						Vector3(0.15f, 0.25f, 0.35f), Vector3(0.4f, 0.1f, 0.2f), Vector3(0.08f, 0.09f, 0.1f), Vector3(0.2f, 0.2f, 0.2f),
						Vector3(0.33f, 0.11f, 0.22f) } },
		{ "mixed sign", { Vector3(-0.4f, 0.2f, -0.05f), Vector3(0.1f, -0.3f, 0.2f), Vector3(-0.01f, 0.02f, -0.03f) },
				{ Vector3(0.2f, -0.1f, 0.3f), Vector3(-0.05f, 0.06f, -0.07f), Vector3(0.3f, -0.3f, 0.3f), Vector3(-0.001f, 0.002f, -0.003f),
						Vector3(0.15f, -0.25f, 0.35f), Vector3(-0.4f, 0.1f, -0.2f), Vector3(0.08f, -0.09f, 0.1f), Vector3(-0.2f, 0.2f, -0.2f),
						Vector3(0.33f, -0.11f, 0.22f) } },
		{ "tiny", { Vector3(1e-4f, -1e-4f, 5e-5f), Vector3(-2e-5f, 3e-5f, -1e-4f), Vector3() },
				{ Vector3(-1e-4f, 1e-4f, 0.0f), Vector3(), Vector3(), Vector3(), Vector3(), Vector3(), Vector3(), Vector3(), Vector3() } },
		{ "large", { Vector3(-4.0f, 4.0f, -2.5f), Vector3(3.9f, -3.9f, 0.5f), Vector3(-0.25f, 1.0f, -4.0f) },
				{ Vector3(4.0f, -4.0f, 4.0f), Vector3(-1.0f, 2.0f, -3.0f), Vector3(0.1f, -0.1f, 0.2f), Vector3(-4.0f, -4.0f, -4.0f),
						Vector3(2.0f, 2.0f, -2.0f), Vector3(), Vector3(-0.5f, 0.5f, -0.5f), Vector3(3.0f, -3.0f, 3.0f), Vector3(-2.2f, 1.1f, 3.3f) } },
	};

	for (const Case &c : cases) {
		const Gaussian g = TestSHEncoding::make_sh_gaussian(c.first[0], c.first[1], c.first[2]);

		// Unquantized: 3 first-order + 9 higher-order = all 12 slots.
		PackedGaussian packed = {};
		SHCompressionMetrics metrics;
		pack_gaussian(g, packed, metrics, c.high, 3, 9);
		Vector3 expected[12];
		for (int i = 0; i < 3; i++) {
			expected[i] = c.first[i];
		}
		for (int i = 0; i < 9; i++) {
			expected[3 + i] = c.high[i];
		}
		CHECK(gs_get_sh_encoding(packed.sh_metadata) == GS_SH_ENCODING_SNORM10_SPLAT_SCALE);
		CHECK(packed.sh.dc[3] == TestSHEncoding::max_abs_component(expected, 12));
		uint32_t words[12];
		for (int i = 0; i < 12; i++) {
			words[i] = TestSHEncoding::word_bits(packed.sh.encoded[i]);
			CHECK_MESSAGE((words[i] >> 30u) == 0u, "bits 30-31 of an SH word must stay zero");
		}
		TestSHEncoding::check_round_trip(expected, words, 12, packed.sh.dc[3], c.label);
		// The DC colour lanes stay FP32 and untouched.
		CHECK(packed.sh.dc[0] == g.sh_dc.r);
		CHECK(packed.sh.dc[1] == g.sh_dc.g);
		CHECK(packed.sh.dc[2] == g.sh_dc.b);

		// Quantized: 3 first-order + 3 higher-order = the fixed 6 slots; the scale covers
		// only the coefficients actually stored.
		PackedGaussianQuantized quantized = {};
		SHCompressionMetrics qmetrics;
		pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, quantized, qmetrics, c.high, 3, 9);
		CHECK(quantized.sh_dc[3] == TestSHEncoding::max_abs_component(expected, 6));
		TestSHEncoding::check_round_trip(expected, quantized.sh_encoded, 6, quantized.sh_dc[3], c.label);
	}
}

TEST_CASE("[GaussianSplatting][SHEncoding] Scale covers exactly the stored coefficients (coefficient_limit)") {
	// A large coefficient beyond the slot limit must not coarsen the stored ones.
	const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(-0.1f, 0.05f, 0.02f), Vector3(0.03f, -0.04f, 0.01f), Vector3());
	const Vector3 high[1] = { Vector3(100.0f, -100.0f, 100.0f) };
	PackedGaussian packed = {};
	SHCompressionMetrics metrics;
	pack_gaussian(g, packed, metrics, high, 3, 1, /*coefficient_limit=*/2);
	CHECK(((packed.sh_metadata & GS_SH_METADATA_ENCODED_COUNT_MASK) >> 16u) == 2u);
	CHECK(packed.sh.dc[3] == doctest::Approx(0.1f));
	const Vector3 expected[2] = { g.sh_1[0], g.sh_1[1] };
	uint32_t words[2] = { TestSHEncoding::word_bits(packed.sh.encoded[0]), TestSHEncoding::word_bits(packed.sh.encoded[1]) };
	TestSHEncoding::check_round_trip(expected, words, 2, packed.sh.dc[3], "coefficient_limit");
	CHECK(packed.sh.encoded[2] == 0.0f);
}

TEST_CASE("[GaussianSplatting][SHEncoding] All-zero and non-finite coefficients encode to zero words with a finite scale") {
	{
		const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(), Vector3(), Vector3());
		PackedGaussian packed = {};
		SHCompressionMetrics metrics;
		pack_gaussian(g, packed, metrics, nullptr, 3, 0);
		CHECK(packed.sh.dc[3] == 0.0f);
		for (int i = 0; i < 3; i++) {
			CHECK(TestSHEncoding::word_bits(packed.sh.encoded[i]) == 0u);
		}
		// A zero word decodes to zero whatever the scale.
		CHECK(gs_decode_sh_snorm10(0u, 0.0f) == Vector3());
		CHECK(gs_decode_sh_snorm10(0u, 3.0f) == Vector3());
	}
	{
		const float inf = std::numeric_limits<float>::infinity();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(nan, -0.2f, inf), Vector3(0.1f, -inf, 0.0f), Vector3());
		PackedGaussianQuantized packed = {};
		SHCompressionMetrics metrics;
		pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, packed, metrics, nullptr, 3, 0);
		CHECK(Math::is_finite(packed.sh_dc[3]));
		CHECK(packed.sh_dc[3] == doctest::Approx(0.2f)); // non-finite components count as 0
		const Vector3 c0 = gs_decode_sh_snorm10(packed.sh_encoded[0], packed.sh_dc[3]);
		CHECK(c0.x == 0.0f);
		CHECK(c0.y == doctest::Approx(-0.2f));
		CHECK(c0.z == 0.0f);
	}
}

TEST_CASE("[GaussianSplatting][SHEncoding] Encoding ids: SNORM10 is new and the retired RGB9E5 id is not reused") {
	CHECK(GS_SH_ENCODING_SNORM10_SPLAT_SCALE != GS_SH_ENCODING_RGB9E5);
	CHECK(GS_SH_ENCODING_SNORM10_SPLAT_SCALE != 0u); // 0 means "no SH" to the shader
	CHECK(GS_SH_ENCODING_SNORM10_SPLAT_SCALE <= 0x7Fu); // fits the 7-bit metadata field
	// The decoder mirror's extremes: +-511 steps map to +-scale exactly.
	CHECK(gs_decode_sh_snorm10(0x1FFu, 2.0f).x == doctest::Approx(2.0f));
	CHECK(gs_decode_sh_snorm10(0x201u, 2.0f).x == doctest::Approx(-2.0f)); // -511 in 10-bit two's complement
}


// ---------------------------------------------------------------------------------------------
// GPU readback (ADR evidence item 2). One opaque splat, rendered three times with its band-1
// Y(1,0) red coefficient negative / zero / positive; the camera looks down -Z so that basis term
// is non-zero. Before #1054 the negative render was pixel-identical to the zero render (the
// unsigned packer clamped it to 0). The case asserts ordering and symmetry of the red channel.
// Tagged [SceneTree][RequiresGPU] (RendererSceneTree GPU batch) and FAILs, never skips, when the
// harness does not provide the GPU environment it promises.
// ---------------------------------------------------------------------------------------------
namespace TestSHEncoding {

inline RID create_readback_texture(RenderingDevice *p_rd, const Vector2i &p_size) {
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	format.width = p_size.x;
	format.height = p_size.y;
	format.depth = 1;
	format.array_layers = 1;
	format.mipmaps = 1;
	format.samples = RD::TEXTURE_SAMPLES_1;
	format.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	return p_rd->texture_create(format, RD::TextureView());
}

// Renders one splat whose Y(1,0) red coefficient is p_coeff and returns the centre pixel's red
// channel in [0, 1] through r_red. Returns false (with a message) if any step of the render or
// readback fails, so the caller can FAIL instead of asserting on garbage.
inline bool render_center_red(RenderingDevice *p_rd, float p_coeff, float &r_red, String &r_why) {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(p_rd);
	if (!renderer.is_valid()) {
		r_why = "renderer did not instantiate";
		return false;
	}
	renderer->initialize();
	renderer->set_painterly_enabled(false);

	LocalVector<Gaussian> gaussians;
	gaussians.resize(1);
	Gaussian &g = gaussians[0];
	g = Gaussian{};
	g.position = Vector3(0.0f, 0.0f, -3.0f);
	g.scale = Vector3(0.6f, 0.6f, 0.6f);
	g.opacity = 0.995f;
	g.rotation = Quaternion();
	g.normal = Vector3(0.0f, 0.0f, 1.0f);
	g.area = g.scale.x * g.scale.y;
	g.sh_dc = Color(0.0f, 0.0f, 0.0f, 1.0f); // displays as 0.5 grey under the linear DC decode
	g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
	g.sh_1[0] = Vector3();
	g.sh_1[1] = Vector3(p_coeff, 0.0f, 0.0f); // Y(1,0): basis = SH_C1 * view_dir.z
	g.sh_1[2] = Vector3();

	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);
	renderer->set_max_splats(1);
	if (renderer->set_gaussian_data(data) != OK) {
		r_why = "set_gaussian_data failed";
		return false;
	}

	const Vector2i resolution(64, 64);
	Projection projection;
	projection.set_perspective(60.0f, 1.0f, 0.1f, 50.0f);
	bool rendered = false;
	for (int attempt = 0; attempt < 8 && !rendered; attempt++) {
		rendered = renderer->render_for_view(Transform3D(), projection, RID(), resolution);
	}
	if (!rendered) {
		r_why = "render_for_view never reported a rendered frame";
		return false;
	}

	RID target = create_readback_texture(p_rd, resolution);
	if (!target.is_valid()) {
		r_why = "readback texture creation failed";
		return false;
	}
	const bool copied = renderer->copy_final_texture_to_target(target, resolution);
	Vector<uint8_t> pixels;
	if (copied) {
		pixels = p_rd->texture_get_data(target, 0);
	}
	p_rd->free(target);
	renderer.unref();
	if (!copied) {
		r_why = "copy_final_texture_to_target failed";
		return false;
	}
	if (pixels.size() != resolution.x * resolution.y * 4) {
		r_why = vformat("readback returned %d bytes, expected %d", pixels.size(), resolution.x * resolution.y * 4);
		return false;
	}
	const int idx = ((resolution.y / 2) * resolution.x + resolution.x / 2) * 4;
	r_red = float(pixels[idx]) / 255.0f;
	return true;
}

} // namespace TestSHEncoding

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Negative band-1 SH renders darker than zero SH through the GPU (#1054)") {
	if (RenderingServer::get_singleton() == nullptr) {
		FAIL("RenderingServer unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return;
	}
	GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
	if (manager == nullptr) {
		FAIL("GaussianSplatManager unavailable - module registration always provides one");
		return;
	}
	RenderingDevice *rd = manager->get_primary_rendering_device();
	if (rd == nullptr) {
		FAIL("RenderingDevice unavailable in a [RequiresGPU] case");
		return;
	}

	float red_neg = 0.0f, red_zero = 0.0f, red_pos = 0.0f;
	String why;
	if (!TestSHEncoding::render_center_red(rd, -0.4f, red_neg, why)) {
		FAIL(vformat("negative-SH render: %s", why));
		return;
	}
	if (!TestSHEncoding::render_center_red(rd, 0.0f, red_zero, why)) {
		FAIL(vformat("zero-SH render: %s", why));
		return;
	}
	if (!TestSHEncoding::render_center_red(rd, 0.4f, red_pos, why)) {
		FAIL(vformat("positive-SH render: %s", why));
		return;
	}
	MESSAGE(vformat("centre red: negative %f, zero %f, positive %f", red_neg, red_zero, red_pos));

	// Premise: the splat covers the centre and its DC decodes to mid grey.
	CHECK_MESSAGE(red_zero > 0.25f, "the zero-SH splat does not cover the centre pixel");
	CHECK_MESSAGE(red_zero < 0.75f, "the zero-SH splat is not mid grey");
	// #1054: the negative coefficient must darken red exactly as the positive one brightens it.
	CHECK_MESSAGE(red_neg < red_zero - 0.03f, "negative band-1 SH did not darken red (clamped to zero?)");
	CHECK_MESSAGE(red_pos > red_zero + 0.03f, "positive band-1 SH did not brighten red");
	CHECK_MESSAGE(Math::abs((red_zero - red_neg) - (red_pos - red_zero)) < 0.04f,
			"negative and positive SH deltas are not symmetric");
}

#endif // GAUSSIAN_SPLATTING_TEST_SH_ENCODING_H
