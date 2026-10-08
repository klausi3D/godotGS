/**************************************************************************/
/*  test_sh_encoding.h                                                    */
/*  Host round-trip tests for the signed SH storage (#1054).              */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_SH_ENCODING_H
#define GAUSSIAN_SPLATTING_TEST_SH_ENCODING_H

#include "gs_test_pump.h"
#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/streaming_quantization.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/quantization_config.h"

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// Compile the exact production polynomial source into the native test.
// The independent reference below uses a recurrence, not those polynomials.
namespace GSProductionSHBasis {
using uint = uint32_t;
using std::min;
#define vec3 Vector3
#define GS_SH_OUT
#include "../shaders/includes/gs_sh_basis.glsl"
#undef GS_SH_OUT
#undef vec3
}

namespace GSReferenceSHBasis {
static double real_sh(int l, int m, const Vector3 &dir) {
    const int order = m < 0 ? -m : m;
    const double z = dir.z;
    double pmm = 1.0;
    for (int k = 1; k <= order; k++) {
        pmm *= -(2*k - 1)*std::sqrt(std::max(0.0, 1.0 - z*z));
    }
    double plm = pmm;
    if (l > order) {
        double previous = pmm;
        plm = (2*order + 1)*z*pmm;
        for (int degree = order + 2; degree <= l; degree++) {
            double next = ((2*degree - 1)*z*plm - (degree + order - 1)*previous)/(degree - order);
            previous = plm;
            plm = next;
        }
    }
    double ratio = 1.0;
    for (int k = l - order + 1; k <= l + order; k++) ratio /= k;
    double result = std::sqrt((2*l + 1)*ratio/(4.0*Math_PI))*plm;
    if (m != 0) {
        const double phi = std::atan2(double(dir.y), double(dir.x));
        result *= std::sqrt(2.0)*(m < 0 ? std::sin(order*phi) : std::cos(order*phi));
    }
    return result;
}
}

TEST_CASE("[GaussianSplatting][SHEncoding] Production SH0-4 basis agrees with independent Legendre recurrence") {
    for (int sample = 0; sample < 130; sample++) {
        Vector3 dir;
        if (sample < 2) {
            dir = Vector3(0, 0, sample == 0 ? 1 : -1);
        } else {
            const double z = 1.0 - 2.0*(double(sample - 2) + 0.5)/128.0;
            const double phi = double(sample - 2)*2.39996322972865332;
            const double radius = std::sqrt(1.0 - z*z);
            dir = Vector3(radius*std::cos(phi), radius*std::sin(phi), z);
        }
        for (uint32_t band = 0; band <= 5; band++) {
            float basis[25];
            GSProductionSHBasis::gs_compute_real_sh_basis(dir, band, basis);
            for (int l = 0; l <= 4; l++) {
                for (int m = -l; m <= l; m++) {
                    const int index = l*l + l + m;
                    const double expected = uint32_t(l) <= band ? GSReferenceSHBasis::real_sh(l, m, dir) : 0.0;
                    CAPTURE(sample, band, l, m);
                    CHECK(std::abs(double(basis[index]) - expected) < 2e-6);
                }
            }
        }
    }
}

TEST_CASE("[GaussianSplatting][SHEncoding] SH3 sign and normalized m2 regression") {
    const Vector3 dir = Vector3(1, 2, 3).normalized();
    float basis[25];
    GSProductionSHBasis::gs_compute_real_sh_basis(dir, 3, basis);
    CHECK(basis[11] < 0.0f);
    CHECK(basis[13] < 0.0f);
    CHECK(std::abs(double(basis[14]) - GSReferenceSHBasis::real_sh(3, 2, dir)) < 2e-6);
    for (int i = 16; i < 25; i++) CHECK(basis[i] == 0.0f);
}

TEST_CASE("[GaussianSplatting][SHEncoding] Production SH metadata bounds compact prefixes") {
    struct Counts { uint32_t first, high, encoded, expected_first, expected_high; };
    const Counts cases[] = {
        { 3, 21, 24, 3, 21 }, { 1, 5, 6, 1, 5 }, { 0, 5, 5, 0, 5 },
        { 2, 12, 14, 2, 12 }, { 3, 21, 0, 0, 0 }, { 3, 21, 2, 2, 0 },
        { 255, 255, 24, 3, 21 }, { 0, 255, 24, 0, 21 }, { 1, 255, 24, 1, 21 }
    };
    for (const Counts &c : cases) {
        const uint32_t first = GSProductionSHBasis::gs_sh_first_count(c.first, c.encoded);
        const uint32_t high = GSProductionSHBasis::gs_sh_high_count(c.high, c.first, c.encoded);
        CHECK(first == c.expected_first);
        CHECK(high == c.expected_high);
        CHECK(first + high <= c.encoded);
        CHECK(first + high <= 24u);
    }
}

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
	const Vector3 decoded = gs_decode_sh_snorm10(packed.sh.encoded[0], packed.sh.dc[3]);
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
			words[i] = packed.sh.encoded[i];
			CHECK_MESSAGE((words[i] >> 30u) == 0u, "bits 30-31 of an SH word must stay zero");
		}
		TestSHEncoding::check_round_trip(expected, words, 12, packed.sh.dc[3], c.label);
		// The DC colour lanes stay FP32 and untouched.
		CHECK(packed.sh.dc[0] == g.sh_dc.r);
		CHECK(packed.sh.dc[1] == g.sh_dc.g);
		CHECK(packed.sh.dc[2] == g.sh_dc.b);

		// Quantized: the same 12 authored terms survive; remaining slots are zero.
		// Scale covers every stored coefficient.
		PackedGaussianQuantized quantized = {};
		SHCompressionMetrics qmetrics;
		pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, quantized, qmetrics, c.high, 3, 9);
		CHECK(quantized.sh_dc[3] == TestSHEncoding::max_abs_component(expected, 12));
		TestSHEncoding::check_round_trip(expected, quantized.sh_encoded, 12, quantized.sh_dc[3], c.label);
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
	uint32_t words[2] = { packed.sh.encoded[0], packed.sh.encoded[1] };
	TestSHEncoding::check_round_trip(expected, words, 2, packed.sh.dc[3], "coefficient_limit");
	CHECK(packed.sh.encoded[2] == 0u);
}

TEST_CASE("[GaussianSplatting][SHEncoding] All-zero and non-finite coefficients encode to zero words with a finite scale") {
	{
		const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(), Vector3(), Vector3());
		PackedGaussian packed = {};
		SHCompressionMetrics metrics;
		pack_gaussian(g, packed, metrics, nullptr, 3, 0);
		CHECK(packed.sh.dc[3] == 0.0f);
		for (int i = 0; i < 3; i++) {
			CHECK(packed.sh.encoded[i] == 0u);
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
// GPU readback (ADR evidence item 2; #1054 signed storage + #1063 view direction). One opaque
// splat is rendered with a single band-1 coefficient in the red channel and compared with the
// REFERENCE formula (Inria 3DGS computeColorFromSH): delta_red = c * basis_k(dir), where
// dir = normalize(splat - camera) and basis = (-SH_C1*y, SH_C1*z, -SH_C1*x) for slots 0/1/2.
// Before #1054 a negative c rendered exactly like c = 0; before #1063 every band-1 delta had the
// opposite sign. The red channel is sampled at the splat's measured coverage centroid (robust to
// the readback's row order). Tagged [SceneTree][RequiresGPU] (RendererSceneTree GPU batch);
// FAILs, never skips, when the harness does not provide the GPU environment it promises.
// ---------------------------------------------------------------------------------------------
namespace TestSHEncoding {

static constexpr float REF_SH_C1 = 0.4886025119029199f;

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

// One opaque splat at p_position with band-1 red coefficients p_band1 (slot k in component k),
// DC at mid grey. Red carries the coefficient under test. Slot 2's green coefficient only keeps
// band 1 complete: GaussianData derives sh_degree from the highest non-zero band-1 slot, and the
// quantized route gates bands by it (ChunkMetaGPU.sh_limit), so a lone slot-0 or slot-1
// coefficient would otherwise render as DC only on that route. Red is unaffected.
inline Ref<GaussianData> make_band1_splat(const Vector3 &p_position, const Vector3 &p_band1) {
	LocalVector<Gaussian> gaussians;
	gaussians.resize(1);
	Gaussian &g = gaussians[0];
	g = Gaussian{};
	g.position = p_position;
	g.scale = Vector3(0.6f, 0.6f, 0.6f);
	g.opacity = 0.995f;
	g.rotation = Quaternion();
	g.normal = Vector3(0.0f, 0.0f, 1.0f);
	g.area = g.scale.x * g.scale.y;
	g.sh_dc = Color(0.0f, 0.0f, 0.0f, 1.0f); // displays as 0.5 grey under the linear DC decode
	g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
	g.sh_1[0] = Vector3(p_band1.x, 0.0f, 0.0f);
	g.sh_1[1] = Vector3(p_band1.y, 0.0f, 0.0f);
	g.sh_1[2] = Vector3(p_band1.z, 0.05f, 0.0f);

	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);
	return data;
}

// Mean red over the 3x3 pixels around the alpha-coverage centroid of an RGBA8 readback (the
// background is transparent), so the sample is robust to the readback's row order.
inline bool sample_centroid_red(const Vector<uint8_t> &p_pixels, const Vector2i &p_size, float &r_red, String &r_why) {
	if (p_pixels.size() != p_size.x * p_size.y * 4) {
		r_why = vformat("readback returned %d bytes, expected %d", p_pixels.size(), p_size.x * p_size.y * 4);
		return false;
	}
	double sum_w = 0.0, sum_x = 0.0, sum_y = 0.0;
	for (int y = 0; y < p_size.y; y++) {
		for (int x = 0; x < p_size.x; x++) {
			const double a = p_pixels[(y * p_size.x + x) * 4 + 3] / 255.0;
			if (a > 0.5) {
				sum_w += a;
				sum_x += a * x;
				sum_y += a * y;
			}
		}
	}
	if (sum_w < 9.0) {
		r_why = vformat("the splat covers too few opaque pixels (alpha mass %f)", sum_w);
		return false;
	}
	const int cx = int(Math::round(sum_x / sum_w));
	const int cy = int(Math::round(sum_y / sum_w));
	float red = 0.0f;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int x = CLAMP(cx + dx, 0, p_size.x - 1);
			const int y = CLAMP(cy + dy, 0, p_size.y - 1);
			red += p_pixels[(y * p_size.x + x) * 4] / 255.0f;
		}
	}
	r_red = red / 9.0f;
	return true;
}

// Renders make_band1_splat(p_position, p_band1) with the camera at the origin looking down -Z
// and returns sample_centroid_red() of the final texture. False (with a reason) on any
// render/readback failure, or when the route under test (quantized or not) was not the one taken.
inline bool render_splat_red(RenderingDevice *p_rd, const Vector3 &p_position, const Vector3 &p_band1,
		bool p_expect_quantized, float &r_red, String &r_why) {
	const Ref<GaussianData> data = make_band1_splat(p_position, p_band1);
	if (data->get_sh_degree() != 1) {
		r_why = vformat("premise: fixture sh_degree is %d, expected 1 (band 1 would be gated off)", data->get_sh_degree());
		return false;
	}
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(p_rd);
	if (!renderer.is_valid()) {
		r_why = "renderer did not instantiate";
		return false;
	}
	renderer->initialize();
	renderer->set_painterly_enabled(false);
	renderer->set_max_splats(1);
	if (renderer->set_gaussian_data(data) != OK) {
		r_why = "set_gaussian_data failed";
		return false;
	}

	const Vector2i resolution(96, 96);
	Projection projection;
	projection.set_perspective(60.0f, 1.0f, 0.1f, 50.0f);
	const TestGaussianSplatting::GSPumpOutcome outcome = TestGaussianSplatting::gs_pump_until([&]() {
		return renderer->render_for_view(Transform3D(), projection, RID(), resolution);
	}, 10000000, 1);
	if (!outcome.ready()) {
		r_why = "render_for_view never reported a rendered frame: " + outcome.describe();
		return false;
	}
	const bool quantized = bool(renderer->get_render_stats().get("raster_feature_quantized_storage", false));
	if (quantized != p_expect_quantized) {
		r_why = vformat("premise: raster_feature_quantized_storage is %s, expected %s (the route under test was not taken)",
				quantized ? "true" : "false", p_expect_quantized ? "true" : "false");
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
	return sample_centroid_red(pixels, resolution, r_red, r_why);
}

// Reference band-1 basis (Inria forward.cu) for the direction from the camera (origin) to p_pos.
inline Vector3 reference_band1_basis(const Vector3 &p_pos) {
	const Vector3 dir = p_pos.normalized();
	return Vector3(-REF_SH_C1 * dir.y, REF_SH_C1 * dir.z, -REF_SH_C1 * dir.x);
}

inline void check_band1_against_reference(RenderingDevice *p_rd, const Vector3 &p_position, bool p_quantized, const char *p_label) {
	float red_zero = 0.0f;
	String why;
	if (!render_splat_red(p_rd, p_position, Vector3(), p_quantized, red_zero, why)) {
		FAIL(vformat("%s zero-SH render: %s", p_label, why));
		return;
	}
	CHECK_MESSAGE(red_zero > 0.4f, vformat("%s premise: zero-SH splat not mid grey (%f)", p_label, red_zero));
	CHECK_MESSAGE(red_zero < 0.6f, vformat("%s premise: zero-SH splat not mid grey (%f)", p_label, red_zero));
	const Vector3 basis = reference_band1_basis(p_position);
	for (int slot = 0; slot < 3; slot++) {
		for (const float c : { -0.4f, 0.4f }) {
			Vector3 band1;
			band1[slot] = c;
			float red = 0.0f;
			if (!render_splat_red(p_rd, p_position, band1, p_quantized, red, why)) {
				FAIL(vformat("%s slot %d c=%f render: %s", p_label, slot, c, why));
				return;
			}
			const float expected = c * basis[slot];
			const float measured = red - red_zero;
			MESSAGE(vformat("%s slot %d c=%+.2f: measured %+f expected %+f", p_label, slot, c, measured, expected));
			CHECK_MESSAGE(Math::abs(measured - expected) < 0.02f,
					vformat("%s slot %d c=%+.2f: red delta %+f != reference %+f (sign flipped or clamped?)", p_label, slot, c, measured, expected));
		}
	}
}

} // namespace TestSHEncoding

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Band-1 SH renders the reference colour delta on-axis and off-axis, unquantized and quantized (#1054, #1063)") {
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
	const QuantizationConfig saved_quantization = g_quantization_config;
	g_quantization_config.per_chunk_quantization = false;
	TestSHEncoding::check_band1_against_reference(rd, Vector3(0.0f, 0.0f, -3.0f), false, "unquantized on-axis");
	TestSHEncoding::check_band1_against_reference(rd, Vector3(1.2f, 0.9f, -3.0f), false, "unquantized off-axis");
	g_quantization_config.per_chunk_quantization = true;
	TestSHEncoding::check_band1_against_reference(rd, Vector3(1.2f, 0.9f, -3.0f), true, "quantized off-axis");
	g_quantization_config = saved_quantization;
}

#include "../renderer/gpu_record_sizes.h"

TEST_CASE("[GaussianSplatting][SHEncoding] GPU byte sizing rejects overflow and covers every aligned upload byte") {
	for (uint32_t count : { 0u, 1u, 2u, 3u, 15u, 16u, 17u, 255u, 256u, 257u }) {
		uint32_t bytes = 0;
		const bool valid = gs_gpu_record_buffer_size(count, sizeof(PackedGaussian), 256, bytes);
		CHECK(valid);
		if (!valid) {
			continue;
		}
		CHECK_EQ(bytes % 256, 0u);
		CHECK(uint64_t(bytes) >= uint64_t(count) * sizeof(PackedGaussian));
		CHECK(uint64_t(bytes) < uint64_t(count) * sizeof(PackedGaussian) + 256);
		const uint32_t scratch_count = gs_gpu_upload_scratch_count(bytes, sizeof(PackedGaussian));
		CHECK(uint64_t(scratch_count) * sizeof(PackedGaussian) >= bytes);
		if (scratch_count > 0) {
			CHECK(uint64_t(scratch_count - 1) * sizeof(PackedGaussian) < bytes);
		}
	}
	for (uint32_t stride : { uint32_t(sizeof(PackedGaussian)), uint32_t(sizeof(PackedGaussian) + sizeof(uint32_t)) }) {
		const uint32_t max_aligned = UINT32_MAX & ~255u;
		const uint32_t largest_count = max_aligned / stride;
		uint32_t bytes = 0;
		CHECK(gs_gpu_record_buffer_size(largest_count, stride, 256, bytes));
		CHECK_FALSE(gs_gpu_record_buffer_size(largest_count + 1, stride, 256, bytes));
		CHECK_EQ(bytes, 0u);
		CHECK_FALSE(gs_gpu_record_buffer_size(25000000, stride, 256, bytes));
		CHECK_FALSE(gs_gpu_record_buffer_size(UINT32_MAX, stride, 256, bytes));
	}
	uint32_t bytes = 123;
	CHECK_FALSE(gs_gpu_record_buffer_size(1, 0, 256, bytes));
	CHECK_FALSE(gs_gpu_record_buffer_size(1, 176, 0, bytes));
	CHECK_FALSE(gs_gpu_record_buffer_size(1, 176, 3, bytes));
	CHECK_EQ(bytes, 0u);
}

TEST_CASE("[GaussianSplatting][SHEncoding] Both GPU layouts preserve every SH0–4 term and zero unused storage") {
	Vector3 expected[24];
	for (uint32_t i = 0; i < 24; i++) {
		expected[i] = Vector3(float(i + 1) / 32.0f, -float(i + 2) / 64.0f, float(24 - i) / 48.0f);
	}
	const Gaussian g = TestSHEncoding::make_sh_gaussian(expected[0], expected[1], expected[2]);
	for (uint32_t degree = 0; degree <= 4; degree++) {
		const uint32_t total = (degree + 1) * (degree + 1) - 1;
		const uint32_t first = MIN(total, 3u);
		const uint32_t high = total - first;
		PackedGaussian normal_a, normal_b;
		PackedGaussianQuantized quant_a, quant_b;
		memset(&normal_a, 0xA5, sizeof(normal_a));
		memset(&normal_b, 0x5A, sizeof(normal_b));
		memset(&quant_a, 0xA5, sizeof(quant_a));
		memset(&quant_b, 0x5A, sizeof(quant_b));
		SHCompressionMetrics metrics_a, metrics_b, qmetrics_a, qmetrics_b;
		pack_gaussian(g, normal_a, metrics_a, expected + 3, first, high);
		pack_gaussian(g, normal_b, metrics_b, expected + 3, first, high);
		pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, quant_a, qmetrics_a, expected + 3, first, high);
		pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, quant_b, qmetrics_b, expected + 3, first, high);
		CHECK_EQ(memcmp(&normal_a, &normal_b, sizeof(normal_a)), 0);
		CHECK_EQ(memcmp(&quant_a, &quant_b, sizeof(quant_a)), 0);
		CHECK_EQ((normal_a.sh_metadata & GS_SH_METADATA_ENCODED_COUNT_MASK) >> 16u, total);
		CHECK_EQ(metrics_a.coefficient_count, total);
		CHECK_EQ(qmetrics_a.coefficient_count, total);
		TestSHEncoding::check_round_trip(expected, normal_a.sh.encoded, total, normal_a.sh.dc[3], "normal SH0–4");
		TestSHEncoding::check_round_trip(expected, quant_a.sh_encoded, total, quant_a.sh_dc[3], "quantized SH0–4");
		for (uint32_t i = total; i < 24; i++) {
			CHECK_EQ(normal_a.sh.encoded[i], 0u);
			CHECK_EQ(quant_a.sh_encoded[i], 0u);
		}
		CHECK_EQ(quant_a._tail_padding[0], 0u);
		CHECK_EQ(quant_a._tail_padding[1], 0u);
	}
}

TEST_CASE("[GaussianSplatting][SHEncoding] SH4 storage respects explicit limits in both layouts") {
	Vector3 high[21];
	for (uint32_t i = 0; i < 21; i++) {
		high[i] = Vector3(float(i + 1), -float(i + 2), float(i + 3));
	}
	const Gaussian g = TestSHEncoding::make_sh_gaussian(Vector3(0.1f, -0.2f, 0.3f), Vector3(), Vector3());
	for (uint32_t limit : { 0u, 3u, 8u, 15u, 24u, UINT32_MAX }) {
		PackedGaussian normal;
		PackedGaussianQuantized quantized;
		SHCompressionMetrics metrics, qmetrics;
		pack_gaussian(g, normal, metrics, high, 3, 21, limit);
		pack_gaussian_quantized(g, TestSHEncoding::make_unit_chunk(), 0, quantized, qmetrics, high, 3, 21, limit);
		const uint32_t stored = MIN(limit, 24u);
		CHECK_EQ(metrics.coefficient_count, stored);
		CHECK_EQ(qmetrics.coefficient_count, stored);
		CHECK_EQ(normal.sh.dc[3], quantized.sh_dc[3]);
		for (uint32_t i = 0; i < stored; i++) {
			CHECK_EQ(normal.sh.encoded[i], quantized.sh_encoded[i]);
		}
		for (uint32_t i = stored; i < 24; i++) {
			CHECK_EQ(normal.sh.encoded[i], 0u);
			CHECK_EQ(quantized.sh_encoded[i], 0u);
		}
	}
}

#endif // GAUSSIAN_SPLATTING_TEST_SH_ENCODING_H
