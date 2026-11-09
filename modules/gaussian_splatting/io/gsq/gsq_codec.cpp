#include "gsq_codec.h"

#include "core/crypto/crypto_core.h"
#include "core/io/marshalls.h"

#include <zstd.h>

#include <chrono>
#include <cmath>
#include <cstring>

// ZSTD_compress2 and the parameter API (risk K11).
#if ZSTD_VERSION_NUMBER < 10400
#error "GSQ needs zstd 1.4.0 or newer."
#endif

namespace gs_gsq {

namespace {

constexpr uint32_t kMagic = 0x31515347u; // "GSQ1"
constexpr uint8_t kFlagPosition21 = 1u << 0;
constexpr uint8_t kFlagNormals = 1u << 1;

// Offsets into the 256 B page header (see gsq_codec.h).
constexpr uint32_t kOffsetDigest = 32u;
constexpr uint32_t kOffsetFrames = 64u;
constexpr uint32_t kOffsetPosition = 128u;
constexpr uint32_t kOffsetLogScale = 152u;
constexpr uint32_t kOffsetDc = 176u;
constexpr uint32_t kOffsetShMax = 200u;
constexpr uint32_t kOffsetNormal = 212u;
constexpr uint32_t kOffsetFloats = 224u;
constexpr uint32_t kOffsetMetas = 244u;
constexpr uint32_t kOffsetReserved = 252u;
static_assert(kOffsetReserved + 4u == kPageHeaderBytes, "GSQ header layout");
static_assert(kMaxPageSplats / kSubBlockSplats == kMaxSubBlocks, "GSQ sub-block table");

// Fixed point for linear attributes: Q24, so 6e-8 resolution, with |value| < 2^38.
constexpr int kFixedFrac = 24;
constexpr uint32_t kFixedMaxExponent = 127u + 38u;
// Dequant steps carry this many extra fractional bits.
constexpr int kStepShift = 22;
// A page-wide range stays below 2^17 units (131 km): with kStepShift and the 21-bit maximum of
// q, range << kStepShift and range * q both stay below 2^63.
constexpr int64_t kMaxRangeFixed = int64_t(1) << (17 + kFixedFrac);
// log2(scale) in Q16. Below 2^-32 is the floor (it includes zero); 2^31 is refused.
constexpr int32_t kLogFloor = -32 * 65536;
constexpr int32_t kLogCeil = 31 * 65536;
constexpr uint32_t kScaleMaxQ = 1023u;
constexpr uint32_t kDcMaxQ[3] = { 2047u, 2047u, 1023u };
constexpr uint32_t kNormalMaxQ = 32767u;
// Normals may overshoot unit length by rounding; anything past this is not a normal.
constexpr uint32_t kNormalMaxBits = 0x3F880000u; // 1.0625f

// Smallest-three: index in the top two bits and three 10-bit signed components of Q30 values.
constexpr int kQuatFrac = 30;
constexpr int64_t kSqrtHalfQ30 = 759250125;
constexpr uint32_t kQuatMaxQ = 511u;

// 2^(2^-k) in Q30 for k = 1..16. Part of the format: they define the decoded scale.
constexpr uint64_t kPow2Table[16] = {
	1518500250u, 1276901417u, 1170923762u, 1121280436u, 1097253708u, 1085434106u, 1079572136u, 1076653033u,
	1075196443u, 1074468888u, 1074105294u, 1073923544u, 1073832680u, 1073787251u, 1073764537u, 1073753181u
};

Error fail(String &r_reason, const char *p_reason, const String &p_detail = String()) {
	r_reason = p_detail.is_empty() ? String(p_reason) : String(p_reason) + ": " + p_detail;
	return ERR_INVALID_DATA;
}

inline uint32_t float_bits(float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

inline float bits_float(uint32_t p_bits) {
	float value;
	memcpy(&value, &p_bits, sizeof(value));
	return value;
}

inline uint32_t bits_exponent(uint32_t p_bits) {
	return (p_bits >> 23u) & 0xFFu;
}

inline bool bits_finite(uint32_t p_bits) {
	return bits_exponent(p_bits) != 0xFFu;
}

// Also false for non-finite values, whose exponent is the largest.
inline bool bits_fit_fixed(uint32_t p_bits) {
	return bits_exponent(p_bits) < kFixedMaxExponent;
}

// Rounds half away from zero, so the mapping is symmetric and monotonic.
inline int64_t bits_to_fixed(uint32_t p_bits, int p_frac) {
	const uint32_t biased = bits_exponent(p_bits);
	uint64_t mantissa = p_bits & 0x7FFFFFu;
	int exponent = -149;
	if (biased != 0u) {
		mantissa |= 0x800000u;
		exponent = int(biased) - 150;
	}
	const int shift = exponent + p_frac;
	uint64_t magnitude = 0u;
	if (shift >= 0) {
		magnitude = mantissa << shift;
	} else if (shift > -32) {
		magnitude = (mantissa + (uint64_t(1) << (-shift - 1))) >> -shift;
	}
	return (p_bits >> 31u) != 0u ? -int64_t(magnitude) : int64_t(magnitude);
}

inline uint64_t magnitude64(int64_t p_value) {
	return p_value < 0 ? uint64_t(-p_value) : uint64_t(p_value);
}

inline int highest_bit(uint64_t p_value) {
	int bit = 0;
	for (int step = 32; step > 0; step >>= 1) {
		if ((p_value >> step) != 0u) {
			p_value >>= step;
			bit += step;
		}
	}
	return bit;
}

// p_value / 2^p_frac, correctly rounded to nearest-even. Every caller stays in the normal
// float range: |p_value| < 2^63 with p_frac in [-1, 62].
inline float fixed_to_float(int64_t p_value, int p_frac) {
	if (p_value == 0) {
		return 0.0f;
	}
	const uint32_t sign = p_value < 0 ? 0x80000000u : 0u;
	const uint64_t magnitude = magnitude64(p_value);
	const int top = highest_bit(magnitude);
	uint32_t biased = uint32_t(top - p_frac + 127);
	uint64_t mantissa;
	if (top > 23) {
		const int shift = top - 23;
		mantissa = magnitude >> shift;
		const uint64_t rest = magnitude & ((uint64_t(1) << shift) - 1u);
		const uint64_t half = uint64_t(1) << (shift - 1);
		if (rest > half || (rest == half && (mantissa & 1u) != 0u)) {
			mantissa++;
			if (mantissa == (uint64_t(1) << 24)) {
				mantissa >>= 1;
				biased++;
			}
		}
	} else {
		mantissa = magnitude << (23 - top);
	}
	return bits_float(sign | (biased << 23u) | (uint32_t(mantissa) & 0x7FFFFFu));
}

// Floor of the square root for p_value < 2^62. The floating point sqrt is only a seed: the
// result is the unique integer the two loops settle on, whatever the seed was.
inline uint64_t integer_sqrt(uint64_t p_value) {
	uint64_t root = uint64_t(std::sqrt(double(p_value)));
	while (root * root > p_value) {
		root--;
	}
	while ((root + 1u) * (root + 1u) <= p_value) {
		root++;
	}
	return root;
}

// log2 of a non-negative float in Q16, by repeated squaring of the mantissa.
inline int32_t log2_q16(uint32_t p_bits) {
	const int32_t biased = int32_t(bits_exponent(p_bits));
	if (biased < 127 - 32) {
		return kLogFloor;
	}
	uint64_t mantissa = uint64_t(0x80000000u) | (uint64_t(p_bits & 0x7FFFFFu) << 8u); // Q31
	int32_t fraction = 0;
	for (int32_t bit = 0x8000; bit != 0; bit >>= 1) {
		mantissa = (mantissa * mantissa) >> 31u;
		if ((mantissa >> 32u) != 0u) {
			mantissa >>= 1u;
			fraction |= bit;
		}
	}
	return (biased - 127) * 65536 + fraction;
}

inline float pow2_q16(int32_t p_log) {
	const int32_t whole = p_log >= 0 ? p_log / 65536 : -((-p_log + 65535) / 65536);
	const uint32_t fraction = uint32_t(p_log - whole * 65536);
	uint64_t value = uint64_t(1) << 30u;
	for (uint32_t k = 0u; k < 16u; k++) {
		if ((fraction & (0x8000u >> k)) != 0u) {
			value = (value * kPow2Table[k] + (uint64_t(1) << 29u)) >> 30u;
		}
	}
	return fixed_to_float(int64_t(value), 30 - whole);
}

// Linear integer map of [lo, hi] onto [0, max_q]. Decoding multiplies by a step derived once
// per page, so it needs no division per value. The result never leaves [lo, hi].
struct LinearMap {
	int64_t lo = 0;
	int64_t hi = 0;
	int64_t step = 0;
	uint32_t max_q = 1u;

	void init(int64_t p_lo, int64_t p_hi, uint32_t p_max_q) {
		lo = p_lo;
		hi = p_hi;
		max_q = p_max_q;
		step = (((p_hi - p_lo) << kStepShift) + p_max_q / 2u) / p_max_q;
	}
	inline uint32_t quantize(int64_t p_value) const {
		const uint64_t range = uint64_t(hi - lo);
		if (range == 0u) {
			return 0u;
		}
		return uint32_t((uint64_t(p_value - lo) * max_q + range / 2u) / range);
	}
	// Any integer is safe here: a corrupt page cannot leave the range or overflow.
	inline int64_t dequantize(uint32_t p_q) const {
		const uint64_t q = p_q < max_q ? p_q : max_q;
		const int64_t value = lo + int64_t((q * uint64_t(step)) >> kStepShift);
		return value < hi ? value : hi;
	}
};

inline uint32_t zigzag_delta(uint32_t p_value, uint32_t p_previous, uint32_t p_bits) {
	const uint32_t size = 1u << p_bits;
	const uint32_t wrapped = (p_value - p_previous) & (size - 1u);
	return wrapped < size / 2u ? wrapped * 2u : (size - wrapped) * 2u - 1u;
}

inline uint32_t apply_zigzag_delta(uint32_t p_zigzag, uint32_t p_previous, uint32_t p_bits) {
	const uint32_t next = (p_zigzag & 1u) != 0u ? p_previous - (p_zigzag + 1u) / 2u : p_previous + p_zigzag / 2u;
	return next & ((1u << p_bits) - 1u);
}

// Normalizes in integers: the components are aligned on the largest exponent, so any finite
// non-zero quaternion works. Ties pick the lowest index.
bool quaternion_to_smallest_three(const Quaternion &p_rotation, uint32_t r_stored[3], uint32_t &r_index) {
	const uint32_t bits[4] = { float_bits(p_rotation.x), float_bits(p_rotation.y), float_bits(p_rotation.z), float_bits(p_rotation.w) };
	uint32_t top = 0u;
	for (uint32_t bit : bits) {
		top = MAX(top, bits_exponent(bit));
	}
	const int frac = 30 - (int(top) - 126);
	int64_t c[4];
	uint64_t sum = 0u;
	for (int i = 0; i < 4; i++) {
		c[i] = bits_to_fixed(bits[i], frac);
		sum += uint64_t(c[i] * c[i]);
	}
	if (sum == 0u) {
		return false;
	}
	const int64_t norm = int64_t(integer_sqrt(sum));
	r_index = 0u;
	for (uint32_t i = 0u; i < 4u; i++) {
		c[i] = c[i] * (int64_t(1) << kQuatFrac) / norm;
		if (magnitude64(c[i]) > magnitude64(c[r_index])) {
			r_index = i;
		}
	}
	const bool flip = c[r_index] < 0;
	uint32_t out = 0u;
	for (uint32_t i = 0u; i < 4u; i++) {
		if (i == r_index) {
			continue;
		}
		const int64_t value = flip ? -c[i] : c[i];
		const uint64_t magnitude = magnitude64(value);
		const uint32_t q = MIN(uint32_t((magnitude * kQuatMaxQ + uint64_t(kSqrtHalfQ30) / 2u) / uint64_t(kSqrtHalfQ30)), kQuatMaxQ);
		r_stored[out++] = value < 0 ? kQuatMaxQ - q : kQuatMaxQ + q;
	}
	return true;
}

// Byte offsets of the planes of one sub-block with n splats.
struct Planes {
	uint32_t position_planes = 0; // per axis
	uint32_t sh_coefficients = 0;
	bool has_normals = false;
	uint32_t n = 0;

	uint32_t bytes_per_splat() const {
		return position_planes * 3u + 14u + sh_coefficients + (has_normals ? 6u : 0u);
	}
	uint32_t scale() const { return position_planes * 3u * n; }
	uint32_t rotation() const { return scale() + 4u * n; }
	uint32_t opacity() const { return rotation() + 4u * n; }
	uint32_t dc() const { return opacity() + 2u * n; }
	uint32_t sh() const { return dc() + 4u * n; }
	uint32_t normal() const { return sh() + sh_coefficients * n; }
};

inline uint32_t sh_band(uint32_t p_coefficient) {
	return p_coefficient < 9u ? 0u : (p_coefficient < 24u ? 1u : 2u);
}

inline uint32_t sh_coefficient_count(uint32_t p_degree) {
	return p_degree == 0u ? 0u : 9u + 3u * sh_high_order_for_degree(p_degree);
}

inline uint32_t sub_block_count(uint32_t p_splats) {
	return (p_splats + kSubBlockSplats - 1u) / kSubBlockSplats;
}

inline void put_float(uint8_t *p_dst, float p_value) {
	encode_uint32(float_bits(p_value), p_dst);
}

inline float get_float(const uint8_t *p_src) {
	return bits_float(decode_uint32(p_src));
}

void hash_stored_bytes(const uint8_t *p_page, uint32_t p_stored_bytes, uint8_t r_digest[kDigestBytes]) {
	CryptoCore::SHA256Context ctx;
	ctx.start();
	ctx.update(p_page, kOffsetDigest);
	ctx.update(p_page + kHeaderBytes, p_stored_bytes - kHeaderBytes);
	ctx.finish(r_digest);
}

struct ZstdCCtx {
	ZSTD_CCtx *ctx = ZSTD_createCCtx();
	~ZstdCCtx() {
		ZSTD_freeCCtx(ctx);
	}
};

// Tracks the extent of one float attribute by exact fixed-point comparison.
struct Extent {
	bool any = false;
	int64_t lo = 0;
	int64_t hi = 0;
	uint32_t lo_bits = 0u;
	uint32_t hi_bits = 0u;

	inline void add(uint32_t p_bits) {
		const int64_t value = bits_to_fixed(p_bits, kFixedFrac);
		if (!any || value < lo) {
			lo = value;
			lo_bits = p_bits;
		}
		if (!any || value > hi) {
			hi = value;
			hi_bits = p_bits;
		}
		any = true;
	}
};

inline bool same_bits(float p_a, float p_b) {
	return float_bits(p_a) == float_bits(p_b);
}

bool constants_match(const Gaussian &p_a, const Gaussian &p_b) {
	return same_bits(p_a.area, p_b.area) && same_bits(p_a.sh_dc.a, p_b.sh_dc.a) &&
			same_bits(p_a.stroke_age, p_b.stroke_age) && same_bits(p_a.brush_axes.x, p_b.brush_axes.x) &&
			same_bits(p_a.brush_axes.y, p_b.brush_axes.y) && p_a.painterly_meta == p_b.painterly_meta &&
			p_a.render_meta == p_b.render_meta;
}

inline float sh_value(const Gaussian &p_gaussian, const Vector3 *p_high, uint32_t p_coefficient) {
	const uint32_t vector = p_coefficient / 3u;
	const uint32_t axis = p_coefficient % 3u;
	return vector < 3u ? p_gaussian.sh_1[vector][axis] : p_high[vector - 3u][axis];
}

} // namespace

uint32_t zstd_header_version() {
	return ZSTD_VERSION_NUMBER;
}

uint32_t zstd_library_version() {
	return ZSTD_versionNumber();
}

Error encode_page(const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count, uint32_t p_sh_degree,
		const EncodeParams &p_params, LocalVector<uint8_t> &r_page, String &r_reason) {
	r_page.clear();
	if (p_count == 0u || p_gaussians == nullptr) {
		return fail(r_reason, "gsq_empty_page");
	}
	if (p_count > kMaxPageSplats) {
		return fail(r_reason, "gsq_page_too_large", vformat("%d splats", p_count));
	}
	if (p_sh_degree > kMaxShDegree) {
		return fail(r_reason, "gsq_sh_degree_unsupported", vformat("degree %d", p_sh_degree));
	}
	const uint32_t high_order = sh_high_order_for_degree(p_sh_degree);
	if (high_order > 0u && p_sh_high == nullptr) {
		return fail(r_reason, "gsq_sh_layout", "high-order SH missing");
	}
	for (uint8_t bits : p_params.sh_bits) {
		if (bits < 2u || bits > 8u) {
			return fail(r_reason, "gsq_invalid_params", "sh_bits");
		}
	}
	if (p_params.opacity_bits < 8u || p_params.opacity_bits > 16u) {
		return fail(r_reason, "gsq_invalid_params", "opacity_bits");
	}
	const uint32_t step_bits = float_bits(p_params.pos_step_m);
	if (!bits_fit_fixed(step_bits) || (step_bits >> 31u) != 0u || bits_to_fixed(step_bits, kFixedFrac) <= 0) {
		return fail(r_reason, "gsq_invalid_params", "pos_step_m");
	}
	if (p_params.zstd_level < 1 || p_params.zstd_level > ZSTD_maxCLevel()) {
		return fail(r_reason, "gsq_invalid_params", "zstd_level");
	}

	// Page scan: refuse what the page cannot carry, then fix the ranges.
	Extent position[3];
	Extent dc[3];
	uint32_t sh_max_bits[3] = { 0u, 0u, 0u };
	int32_t log_lo[3] = { kLogCeil, kLogCeil, kLogCeil };
	int32_t log_hi[3] = { kLogFloor, kLogFloor, kLogFloor };
	LocalVector<int32_t> log_scale;
	log_scale.resize(p_count * 3u);
	const uint32_t coefficients = sh_coefficient_count(p_sh_degree);
	const Gaussian &first = p_gaussians[0];
	const uint32_t constant_bits[] = { float_bits(first.area), float_bits(first.sh_dc.a), float_bits(first.stroke_age),
		float_bits(first.brush_axes.x), float_bits(first.brush_axes.y) };
	for (uint32_t bits : constant_bits) {
		if (!bits_finite(bits)) {
			return fail(r_reason, "gsq_non_finite", "page constant");
		}
	}
	bool has_normals = false;
	for (uint32_t i = 0u; i < p_count; i++) {
		const Gaussian &g = p_gaussians[i];
		if (!constants_match(first, g)) {
			return fail(r_reason, "gsq_heterogeneous_field", vformat("splat %d differs from the page constants", i));
		}
		for (int axis = 0; axis < 3; axis++) {
			const uint32_t pos = float_bits(g.position[axis]);
			const uint32_t color = float_bits(g.sh_dc[axis]);
			const uint32_t scale = float_bits(g.scale[axis]);
			const uint32_t normal = float_bits(g.normal[axis]);
			if (!bits_finite(pos) || !bits_finite(color) || !bits_finite(scale) || !bits_finite(normal)) {
				return fail(r_reason, "gsq_non_finite", vformat("splat %d", i));
			}
			if (!bits_fit_fixed(pos) || !bits_fit_fixed(color)) {
				return fail(r_reason, "gsq_field_out_of_range", vformat("splat %d position or DC", i));
			}
			if (((scale >> 31u) != 0u && (scale & 0x7FFFFFFFu) != 0u) || bits_exponent(scale) >= 127u + 31u) {
				return fail(r_reason, "gsq_field_out_of_range", vformat("splat %d scale", i));
			}
			if ((normal & 0x7FFFFFFFu) > kNormalMaxBits) {
				return fail(r_reason, "gsq_field_out_of_range", vformat("splat %d normal", i));
			}
			has_normals = has_normals || normal != float_bits(first.normal[axis]);
			position[axis].add(pos);
			dc[axis].add(color);
			const int32_t log_value = log2_q16(scale & 0x7FFFFFFFu);
			log_scale[i * 3u + uint32_t(axis)] = log_value;
			log_lo[axis] = MIN(log_lo[axis], log_value);
			log_hi[axis] = MAX(log_hi[axis], log_value);
		}
		const uint32_t opacity = float_bits(g.opacity);
		if (!bits_finite(opacity)) {
			return fail(r_reason, "gsq_non_finite", vformat("splat %d opacity", i));
		}
		if ((opacity >> 31u) != 0u ? (opacity & 0x7FFFFFFFu) != 0u : opacity > 0x3F800000u) {
			return fail(r_reason, "gsq_field_out_of_range", vformat("splat %d opacity", i));
		}
		const uint32_t rotation[4] = { float_bits(g.rotation.x), float_bits(g.rotation.y), float_bits(g.rotation.z), float_bits(g.rotation.w) };
		for (uint32_t bits : rotation) {
			if (!bits_finite(bits)) {
				return fail(r_reason, "gsq_non_finite", vformat("splat %d rotation", i));
			}
		}
		const Vector3 *high = p_sh_high != nullptr ? p_sh_high + uint64_t(i) * high_order : nullptr;
		for (uint32_t c = 0u; c < coefficients; c++) {
			const uint32_t bits = float_bits(sh_value(g, high, c));
			if (!bits_finite(bits)) {
				return fail(r_reason, "gsq_non_finite", vformat("splat %d SH", i));
			}
			if (!bits_fit_fixed(bits)) {
				return fail(r_reason, "gsq_field_out_of_range", vformat("splat %d SH", i));
			}
			const uint32_t band = sh_band(c);
			sh_max_bits[band] = MAX(sh_max_bits[band], bits & 0x7FFFFFFFu);
		}
		if (coefficients == 0u) {
			for (const Vector3 &v : g.sh_1) {
				if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f) {
					return fail(r_reason, "gsq_sh_layout", "first-order SH at degree 0");
				}
			}
		}
	}

	// D-12: u16 unless that would step coarser than pos_step_m on the longest axis.
	int64_t longest = 0;
	for (uint32_t axis = 0u; axis < 3u; axis++) {
		longest = MAX(longest, position[axis].hi - position[axis].lo);
		if (dc[axis].hi - dc[axis].lo >= kMaxRangeFixed || bits_to_fixed(sh_max_bits[axis], kFixedFrac) >= kMaxRangeFixed) {
			return fail(r_reason, "gsq_field_out_of_range", "DC or SH range");
		}
	}
	if (longest >= kMaxRangeFixed) {
		return fail(r_reason, "gsq_field_out_of_range", "position range");
	}
	const int64_t step_fixed = bits_to_fixed(step_bits, kFixedFrac);
	const uint32_t position_bits = longest > step_fixed * 65535 ? 21u : 16u;

	LinearMap position_map[3];
	LinearMap scale_map[3];
	LinearMap dc_map[3];
	LinearMap sh_map[3];
	LinearMap opacity_map;
	LinearMap normal_map;
	for (uint32_t axis = 0u; axis < 3u; axis++) {
		position_map[axis].init(position[axis].lo, position[axis].hi, (1u << position_bits) - 1u);
		scale_map[axis].init(log_lo[axis], log_hi[axis], kScaleMaxQ);
		dc_map[axis].init(dc[axis].lo, dc[axis].hi, kDcMaxQ[axis]);
		sh_map[axis].init(0, bits_to_fixed(sh_max_bits[axis], kFixedFrac), (1u << (p_params.sh_bits[axis] - 1u)) - 1u);
	}
	opacity_map.init(0, int64_t(1) << kFixedFrac, (1u << p_params.opacity_bits) - 1u);
	normal_map.init(0, int64_t(1) << kFixedFrac, kNormalMaxQ);

	const uint32_t blocks = sub_block_count(p_count);
	uint64_t bound = kPageHeaderBytes;
	Planes planes;
	planes.position_planes = position_bits == 21u ? 3u : 2u;
	planes.sh_coefficients = coefficients;
	planes.has_normals = has_normals;
	for (uint32_t b = 0u; b < blocks; b++) {
		bound += ZSTD_compressBound(MIN(p_count - b * kSubBlockSplats, kSubBlockSplats) * planes.bytes_per_splat());
	}
	r_page.resize(uint32_t(bound));
	uint8_t *page = r_page.ptr();
	memset(page, 0, kPageHeaderBytes);

	// One context per page: the output must not depend on what the context compressed before
	// (W7), and encoding is not the hot path.
	ZstdCCtx cctx;
	if (cctx.ctx == nullptr ||
			ZSTD_isError(ZSTD_CCtx_setParameter(cctx.ctx, ZSTD_c_compressionLevel, p_params.zstd_level)) ||
			ZSTD_isError(ZSTD_CCtx_setParameter(cctx.ctx, ZSTD_c_checksumFlag, 1))) {
		r_page.clear();
		return fail(r_reason, "gsq_zstd_error", "context");
	}

	LocalVector<uint8_t> raw;
	uint64_t cursor = kPageHeaderBytes;
	for (uint32_t b = 0u; b < blocks; b++) {
		const uint32_t base = b * kSubBlockSplats;
		const uint32_t n = MIN(p_count - base, kSubBlockSplats);
		planes.n = n;
		const uint32_t raw_bytes = n * planes.bytes_per_splat();
		raw.resize(raw_bytes);
		uint8_t *out = raw.ptr();
		uint32_t previous[3] = { 0u, 0u, 0u };
		for (uint32_t i = 0u; i < n; i++) {
			const Gaussian &g = p_gaussians[base + i];
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				const uint32_t q = position_map[axis].quantize(bits_to_fixed(float_bits(g.position[axis]), kFixedFrac));
				const uint32_t delta = zigzag_delta(q, previous[axis], position_bits);
				previous[axis] = q;
				for (uint32_t plane = 0u; plane < planes.position_planes; plane++) {
					out[(axis * planes.position_planes + plane) * n + i] = uint8_t(delta >> (8u * plane));
				}
			}
			uint32_t high_bits = 0u;
			uint32_t dc_high = 0u;
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				const uint32_t scale_q = scale_map[axis].quantize(log_scale[(base + i) * 3u + axis]);
				out[planes.scale() + axis * n + i] = uint8_t(scale_q);
				high_bits |= (scale_q >> 8u) << (axis * 2u);
				const uint32_t color_q = dc_map[axis].quantize(bits_to_fixed(float_bits(g.sh_dc[axis]), kFixedFrac));
				out[planes.dc() + axis * n + i] = uint8_t(color_q);
				dc_high |= (color_q >> 8u) << (axis * 3u);
			}
			out[planes.scale() + 3u * n + i] = uint8_t(high_bits);
			out[planes.dc() + 3u * n + i] = uint8_t(dc_high);

			uint32_t stored[3];
			uint32_t index = 0u;
			if (!quaternion_to_smallest_three(g.rotation, stored, index)) {
				r_page.clear();
				return fail(r_reason, "gsq_invalid_rotation", vformat("splat %d is a zero quaternion", base + i));
			}
			uint32_t rotation_high = index << 6u;
			for (uint32_t c = 0u; c < 3u; c++) {
				out[planes.rotation() + c * n + i] = uint8_t(stored[c]);
				rotation_high |= (stored[c] >> 8u) << (c * 2u);
			}
			out[planes.rotation() + 3u * n + i] = uint8_t(rotation_high);

			const uint32_t opacity_q = opacity_map.quantize(bits_to_fixed(float_bits(g.opacity), kFixedFrac));
			out[planes.opacity() + i] = uint8_t(opacity_q);
			out[planes.opacity() + n + i] = uint8_t(opacity_q >> 8u);

			const Vector3 *high = p_sh_high != nullptr ? p_sh_high + uint64_t(base + i) * high_order : nullptr;
			for (uint32_t c = 0u; c < coefficients; c++) {
				const uint32_t bits = float_bits(sh_value(g, high, c));
				const LinearMap &map = sh_map[sh_band(c)];
				const uint32_t q = map.quantize(bits_to_fixed(bits & 0x7FFFFFFFu, kFixedFrac));
				out[planes.sh() + c * n + i] = uint8_t((bits >> 31u) != 0u ? map.max_q - q : map.max_q + q);
			}
			if (has_normals) {
				for (uint32_t axis = 0u; axis < 3u; axis++) {
					const uint32_t bits = float_bits(g.normal[axis]);
					const uint32_t q = normal_map.quantize(MIN(bits_to_fixed(bits & 0x7FFFFFFFu, kFixedFrac), int64_t(1) << kFixedFrac));
					const uint32_t value = (bits >> 31u) != 0u ? kNormalMaxQ - q : kNormalMaxQ + q;
					out[planes.normal() + axis * 2u * n + i] = uint8_t(value);
					out[planes.normal() + (axis * 2u + 1u) * n + i] = uint8_t(value >> 8u);
				}
			}
		}
		const size_t written = ZSTD_compress2(cctx.ctx, page + cursor, size_t(bound - cursor), raw.ptr(), raw_bytes);
		if (ZSTD_isError(written)) {
			const String detail = ZSTD_getErrorName(written);
			r_page.clear();
			return fail(r_reason, "gsq_zstd_error", detail);
		}
		encode_uint32(uint32_t(written), page + kOffsetFrames + b * 4u);
		cursor += written;
	}

	const uint32_t stored_bytes = uint32_t(cursor);
	encode_uint32(kMagic, page);
	encode_uint16(uint16_t(kCodecId), page + 4);
	page[6] = uint8_t(p_sh_degree);
	page[7] = uint8_t((position_bits == 21u ? kFlagPosition21 : 0u) | (has_normals ? kFlagNormals : 0u));
	encode_uint32(p_count, page + 8);
	encode_uint32(stored_bytes, page + 12);
	memcpy(page + 16, p_params.sh_bits, 3);
	page[19] = p_params.opacity_bits;
	encode_uint16(uint16_t(kSubBlockSplats), page + 20);
	encode_uint16(uint16_t(blocks), page + 22);
	for (uint32_t axis = 0u; axis < 3u; axis++) {
		encode_uint32(position[axis].lo_bits, page + kOffsetPosition + axis * 4u);
		encode_uint32(position[axis].hi_bits, page + kOffsetPosition + 12u + axis * 4u);
		encode_uint32(uint32_t(log_lo[axis]), page + kOffsetLogScale + axis * 4u);
		encode_uint32(uint32_t(log_hi[axis]), page + kOffsetLogScale + 12u + axis * 4u);
		encode_uint32(dc[axis].lo_bits, page + kOffsetDc + axis * 4u);
		encode_uint32(dc[axis].hi_bits, page + kOffsetDc + 12u + axis * 4u);
		encode_uint32(sh_max_bits[axis], page + kOffsetShMax + axis * 4u);
		put_float(page + kOffsetNormal + axis * 4u, has_normals ? 0.0f : first.normal[axis]);
	}
	put_float(page + kOffsetFloats, first.area);
	put_float(page + kOffsetFloats + 4u, first.sh_dc.a);
	put_float(page + kOffsetFloats + 8u, first.stroke_age);
	put_float(page + kOffsetFloats + 12u, first.brush_axes.x);
	put_float(page + kOffsetFloats + 16u, first.brush_axes.y);
	encode_uint32(first.painterly_meta, page + kOffsetMetas);
	encode_uint32(first.render_meta, page + kOffsetMetas + 4u);
	hash_stored_bytes(page, stored_bytes, page + kOffsetDigest);
	r_page.resize(stored_bytes);
	return OK;
}

Error parse_page_header(const uint8_t *p_page, uint64_t p_page_bytes, PageInfo &r_info, String &r_reason) {
	r_info = PageInfo();
	if (p_page == nullptr || p_page_bytes < kPageHeaderBytes) {
		return fail(r_reason, "gsq_truncated", "no header");
	}
	if (decode_uint32(p_page) != kMagic) {
		return fail(r_reason, "gsq_bad_magic");
	}
	if (decode_uint16(p_page + 4) != kCodecId) {
		return fail(r_reason, "gsq_bad_codec");
	}
	const uint32_t degree = p_page[6];
	const uint8_t flags = p_page[7];
	const uint32_t count = decode_uint32(p_page + 8);
	const uint32_t stored = decode_uint32(p_page + 12);
	if (degree > kMaxShDegree || (flags & ~(kFlagPosition21 | kFlagNormals)) != 0u || count == 0u ||
			count > kMaxPageSplats || decode_uint16(p_page + 20) != kSubBlockSplats ||
			decode_uint16(p_page + 22) != sub_block_count(count) || decode_uint32(p_page + kOffsetReserved) != 0u) {
		return fail(r_reason, "gsq_bad_header");
	}
	for (uint32_t i = 24u; i < kOffsetDigest; i++) {
		if (p_page[i] != 0u) {
			return fail(r_reason, "gsq_bad_header", "reserved");
		}
	}
	if (stored < kPageHeaderBytes || stored > p_page_bytes) {
		return fail(r_reason, "gsq_truncated", vformat("%d stored bytes in %d", stored, p_page_bytes));
	}
	r_info.splat_count = count;
	r_info.sh_degree = degree;
	r_info.sh_high_order = sh_high_order_for_degree(degree);
	r_info.position_bits = (flags & kFlagPosition21) != 0u ? 21u : 16u;
	r_info.has_normals = (flags & kFlagNormals) != 0u;
	r_info.stored_bytes = stored;
	r_info.sub_block_count = sub_block_count(count);
	for (int i = 0; i < 3; i++) {
		r_info.sh_bits[i] = p_page[16 + i];
		if (r_info.sh_bits[i] < 2u || r_info.sh_bits[i] > 8u) {
			return fail(r_reason, "gsq_bad_header", "sh_bits");
		}
	}
	r_info.opacity_bits = p_page[19];
	if (r_info.opacity_bits < 8u || r_info.opacity_bits > 16u) {
		return fail(r_reason, "gsq_bad_header", "opacity_bits");
	}

	Planes planes;
	planes.position_planes = r_info.position_bits == 21u ? 3u : 2u;
	planes.sh_coefficients = sh_coefficient_count(degree);
	planes.has_normals = r_info.has_normals;
	uint64_t total = kPageHeaderBytes;
	for (uint32_t b = 0u; b < kMaxSubBlocks; b++) {
		const uint32_t frame = decode_uint32(p_page + kOffsetFrames + b * 4u);
		if (b >= r_info.sub_block_count) {
			if (frame != 0u) {
				return fail(r_reason, "gsq_bad_table", "unused entry");
			}
			continue;
		}
		const uint32_t n = MIN(count - b * kSubBlockSplats, kSubBlockSplats);
		if (frame == 0u || frame > ZSTD_compressBound(n * planes.bytes_per_splat())) {
			return fail(r_reason, "gsq_bad_table", vformat("sub-block %d", b));
		}
		r_info.frame_bytes[b] = frame;
		total += frame;
	}
	if (total != stored) {
		return fail(r_reason, "gsq_bad_table", "frames do not add up to the stored size");
	}

	for (uint32_t axis = 0u; axis < 3u; axis++) {
		const uint32_t position_lo = decode_uint32(p_page + kOffsetPosition + axis * 4u);
		const uint32_t position_hi = decode_uint32(p_page + kOffsetPosition + 12u + axis * 4u);
		const uint32_t dc_lo = decode_uint32(p_page + kOffsetDc + axis * 4u);
		const uint32_t dc_hi = decode_uint32(p_page + kOffsetDc + 12u + axis * 4u);
		const uint32_t sh_max = decode_uint32(p_page + kOffsetShMax + axis * 4u);
		if (!bits_fit_fixed(position_lo) || !bits_fit_fixed(position_hi) || !bits_fit_fixed(dc_lo) ||
				!bits_fit_fixed(dc_hi) || !bits_fit_fixed(sh_max) || (sh_max >> 31u) != 0u) {
			return fail(r_reason, "gsq_bad_range", vformat("axis %d", axis));
		}
		const int64_t position_range = bits_to_fixed(position_hi, kFixedFrac) - bits_to_fixed(position_lo, kFixedFrac);
		const int64_t dc_range = bits_to_fixed(dc_hi, kFixedFrac) - bits_to_fixed(dc_lo, kFixedFrac);
		if (position_range < 0 || position_range >= kMaxRangeFixed || dc_range < 0 || dc_range >= kMaxRangeFixed ||
				bits_to_fixed(sh_max, kFixedFrac) >= kMaxRangeFixed) {
			return fail(r_reason, "gsq_bad_range", vformat("axis %d", axis));
		}
		r_info.log_scale_min[axis] = int32_t(decode_uint32(p_page + kOffsetLogScale + axis * 4u));
		r_info.log_scale_max[axis] = int32_t(decode_uint32(p_page + kOffsetLogScale + 12u + axis * 4u));
		if (r_info.log_scale_min[axis] < kLogFloor || r_info.log_scale_max[axis] > kLogCeil ||
				r_info.log_scale_min[axis] > r_info.log_scale_max[axis]) {
			return fail(r_reason, "gsq_bad_range", vformat("scale axis %d", axis));
		}
		r_info.position_min[axis] = bits_float(position_lo);
		r_info.position_max[axis] = bits_float(position_hi);
		r_info.dc_min[axis] = bits_float(dc_lo);
		r_info.dc_max[axis] = bits_float(dc_hi);
		r_info.sh_max[axis] = bits_float(sh_max);
		const uint32_t normal = decode_uint32(p_page + kOffsetNormal + axis * 4u);
		if (!bits_finite(normal)) {
			return fail(r_reason, "gsq_bad_range", "normal");
		}
		r_info.constants.normal[axis] = bits_float(normal);
	}
	for (uint32_t i = 0u; i < 5u; i++) {
		if (!bits_finite(decode_uint32(p_page + kOffsetFloats + i * 4u))) {
			return fail(r_reason, "gsq_bad_range", "page constant");
		}
	}
	r_info.constants.area = get_float(p_page + kOffsetFloats);
	r_info.constants.sh_dc.a = get_float(p_page + kOffsetFloats + 4u);
	r_info.constants.stroke_age = get_float(p_page + kOffsetFloats + 8u);
	r_info.constants.brush_axes.x = get_float(p_page + kOffsetFloats + 12u);
	r_info.constants.brush_axes.y = get_float(p_page + kOffsetFloats + 16u);
	r_info.constants.painterly_meta = decode_uint32(p_page + kOffsetMetas);
	r_info.constants.render_meta = decode_uint32(p_page + kOffsetMetas + 4u);
	memcpy(r_info.stored_digest, p_page + kOffsetDigest, kDigestBytes);
	return OK;
}

Error verify_page(const uint8_t *p_page, uint64_t p_page_bytes, PageInfo &r_info, String &r_reason) {
	const Error err = parse_page_header(p_page, p_page_bytes, r_info, r_reason);
	if (err != OK) {
		return err;
	}
	uint8_t digest[kDigestBytes];
	hash_stored_bytes(p_page, r_info.stored_bytes, digest);
	if (memcmp(digest, r_info.stored_digest, kDigestBytes) != 0) {
		return fail(r_reason, "gsq_digest_mismatch");
	}
	return OK;
}

void decoded_digest(const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count, uint32_t p_sh_degree,
		uint8_t r_digest[kDigestBytes]) {
	CryptoCore::SHA256Context ctx;
	ctx.start();
	uint8_t prefix[8];
	encode_uint32(p_count, prefix);
	encode_uint32(p_sh_degree, prefix + 4);
	ctx.update(prefix, sizeof(prefix));
	const uint32_t high_order = sh_high_order_for_degree(p_sh_degree);
	for (uint32_t i = 0u; i < p_count; i++) {
		const Gaussian &g = p_gaussians[i];
		const float fields[] = { g.position.x, g.position.y, g.position.z, g.opacity, g.scale.x, g.scale.y, g.scale.z, g.area,
			g.rotation.x, g.rotation.y, g.rotation.z, g.rotation.w, g.sh_dc.r, g.sh_dc.g, g.sh_dc.b, g.sh_dc.a,
			g.sh_1[0].x, g.sh_1[0].y, g.sh_1[0].z, g.sh_1[1].x, g.sh_1[1].y, g.sh_1[1].z, g.sh_1[2].x, g.sh_1[2].y, g.sh_1[2].z,
			g.normal.x, g.normal.y, g.normal.z, g.stroke_age, g.brush_axes.x, g.brush_axes.y };
		uint8_t buffer[sizeof(fields) + 8u + 12u * 12u];
		uint32_t used = 0u;
		for (float field : fields) {
			put_float(buffer + used, field);
			used += 4u;
		}
		encode_uint32(g.painterly_meta, buffer + used);
		encode_uint32(g.render_meta, buffer + used + 4u);
		used += 8u;
		for (uint32_t k = 0u; k < high_order; k++) {
			const Vector3 &v = p_sh_high[uint64_t(i) * high_order + k];
			put_float(buffer + used, v.x);
			put_float(buffer + used + 4u, v.y);
			put_float(buffer + used + 8u, v.z);
			used += 12u;
		}
		ctx.update(buffer, used);
	}
	ctx.finish(r_digest);
}

DecodeContext::DecodeContext() {
	dctx = ZSTD_createDCtx();
}

DecodeContext::~DecodeContext() {
	ZSTD_freeDCtx(dctx);
}

Error DecodeContext::decode_range(const uint8_t *p_page, uint64_t p_page_bytes, uint32_t p_first, uint32_t p_count,
		Gaussian *r_gaussians, Vector3 *r_sh_high, String &r_reason, bool p_verify_digest) {
	last_zstd_nsec = 0;
	if (dctx == nullptr) {
		return fail(r_reason, "gsq_zstd_error", "context");
	}
	PageInfo info;
	const Error err = p_verify_digest ? verify_page(p_page, p_page_bytes, info, r_reason)
									  : parse_page_header(p_page, p_page_bytes, info, r_reason);
	if (err != OK) {
		return err;
	}
	if (p_count == 0u || uint64_t(p_first) + p_count > info.splat_count || r_gaussians == nullptr ||
			(info.sh_high_order > 0u && r_sh_high == nullptr)) {
		return fail(r_reason, "gsq_range_out_of_bounds", vformat("[%d, +%d) of %d", p_first, p_count, info.splat_count));
	}

	LinearMap position_map[3];
	LinearMap dc_map[3];
	for (uint32_t axis = 0u; axis < 3u; axis++) {
		position_map[axis].init(bits_to_fixed(float_bits(info.position_min[axis]), kFixedFrac),
				bits_to_fixed(float_bits(info.position_max[axis]), kFixedFrac), (1u << info.position_bits) - 1u);
		dc_map[axis].init(bits_to_fixed(float_bits(info.dc_min[axis]), kFixedFrac),
				bits_to_fixed(float_bits(info.dc_max[axis]), kFixedFrac), kDcMaxQ[axis]);
		LinearMap scale_map;
		scale_map.init(info.log_scale_min[axis], info.log_scale_max[axis], kScaleMaxQ);
		for (uint32_t q = 0u; q <= kScaleMaxQ; q++) {
			scale_lut[axis][q] = pow2_q16(int32_t(scale_map.dequantize(q)));
		}
		// Bytes past 2 * max_q exist only in corrupt pages; they saturate.
		LinearMap sh_map;
		const uint32_t sh_q = (1u << (info.sh_bits[axis] - 1u)) - 1u;
		sh_map.init(0, bits_to_fixed(float_bits(info.sh_max[axis]), kFixedFrac), sh_q);
		for (uint32_t stored = 0u; stored < 256u; stored++) {
			const float magnitude = fixed_to_float(sh_map.dequantize(stored > sh_q ? stored - sh_q : sh_q - stored), kFixedFrac);
			sh_lut[axis][stored] = stored < sh_q ? -magnitude : magnitude;
		}
	}
	LinearMap opacity_map;
	opacity_map.init(0, int64_t(1) << kFixedFrac, (1u << info.opacity_bits) - 1u);
	LinearMap normal_map;
	normal_map.init(0, int64_t(1) << kFixedFrac, kNormalMaxQ);
	LinearMap quat_map;
	quat_map.init(0, kSqrtHalfQ30, kQuatMaxQ);

	Planes planes;
	planes.position_planes = info.position_bits == 21u ? 3u : 2u;
	planes.sh_coefficients = sh_coefficient_count(info.sh_degree);
	planes.has_normals = info.has_normals;

	const uint32_t end = p_first + p_count;
	uint64_t cursor = kPageHeaderBytes;
	for (uint32_t b = 0u; b < info.sub_block_count; b++) {
		const uint32_t base = b * kSubBlockSplats;
		const uint32_t n = MIN(info.splat_count - base, kSubBlockSplats);
		const uint32_t frame = info.frame_bytes[b];
		if (base + n <= p_first) {
			cursor += frame;
			continue;
		}
		if (base >= end) {
			break;
		}
		planes.n = n;
		const uint32_t raw_bytes = n * planes.bytes_per_splat();
		raw.resize(raw_bytes);
		const std::chrono::steady_clock::time_point zstd_begin = std::chrono::steady_clock::now();
		const size_t written = ZSTD_decompressDCtx(dctx, raw.ptr(), raw_bytes, p_page + cursor, frame);
		last_zstd_nsec += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - zstd_begin)
						.count());
		if (ZSTD_isError(written)) {
			return fail(r_reason, "gsq_frame_corrupt", vformat("sub-block %d: %s", b, ZSTD_getErrorName(written)));
		}
		if (written != raw_bytes) {
			return fail(r_reason, "gsq_frame_corrupt", vformat("sub-block %d is short", b));
		}
		cursor += frame;

		const uint8_t *in = raw.ptr();
		uint32_t previous[3] = { 0u, 0u, 0u };
		const uint32_t stop = MIN(end - base, n);
		const uint32_t start = p_first > base ? p_first - base : 0u;
		for (uint32_t i = 0u; i < stop; i++) {
			// Deltas run from the start of the sub-block, wanted splat or not.
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				uint32_t zigzag = 0u;
				for (uint32_t plane = 0u; plane < planes.position_planes; plane++) {
					zigzag |= uint32_t(in[(axis * planes.position_planes + plane) * n + i]) << (8u * plane);
				}
				previous[axis] = apply_zigzag_delta(zigzag, previous[axis], info.position_bits);
			}
			if (i < start) {
				continue;
			}
			const uint32_t index = base + i - p_first;
			Gaussian &g = r_gaussians[index];
			g = info.constants;
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				g.position[axis] = fixed_to_float(position_map[axis].dequantize(previous[axis]), kFixedFrac);
			}

			const uint32_t scale_high = in[planes.scale() + 3u * n + i];
			const uint32_t dc_high = in[planes.dc() + 3u * n + i];
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				const uint32_t scale_q = uint32_t(in[planes.scale() + axis * n + i]) | (((scale_high >> (axis * 2u)) & 3u) << 8u);
				g.scale[axis] = scale_lut[axis][scale_q];
				const uint32_t color_q = uint32_t(in[planes.dc() + axis * n + i]) | (((dc_high >> (axis * 3u)) & 7u) << 8u);
				g.sh_dc[axis] = fixed_to_float(dc_map[axis].dequantize(color_q), kFixedFrac);
			}

			const uint32_t rotation_high = in[planes.rotation() + 3u * n + i];
			const uint32_t largest = rotation_high >> 6u;
			int64_t components[4];
			uint64_t sum = 0u;
			uint32_t source = 0u;
			for (uint32_t c = 0u; c < 4u; c++) {
				if (c == largest) {
					continue;
				}
				const uint32_t stored = uint32_t(in[planes.rotation() + source * n + i]) | (((rotation_high >> (source * 2u)) & 3u) << 8u);
				const int64_t magnitude = quat_map.dequantize(stored > kQuatMaxQ ? stored - kQuatMaxQ : kQuatMaxQ - stored);
				components[c] = stored < kQuatMaxQ ? -magnitude : magnitude;
				sum += uint64_t(magnitude * magnitude);
				source++;
			}
			const uint64_t unit = uint64_t(1) << (2 * kQuatFrac);
			components[largest] = sum < unit ? int64_t(integer_sqrt(unit - sum)) : 0;
			g.rotation.x = fixed_to_float(components[0], kQuatFrac);
			g.rotation.y = fixed_to_float(components[1], kQuatFrac);
			g.rotation.z = fixed_to_float(components[2], kQuatFrac);
			g.rotation.w = fixed_to_float(components[3], kQuatFrac);

			const uint32_t opacity_q = uint32_t(in[planes.opacity() + i]) | (uint32_t(in[planes.opacity() + n + i]) << 8u);
			g.opacity = fixed_to_float(opacity_map.dequantize(opacity_q), kFixedFrac);

			for (uint32_t c = 0u; c < planes.sh_coefficients; c++) {
				const float value = sh_lut[sh_band(c)][in[planes.sh() + c * n + i]];
				if (c < 9u) {
					g.sh_1[c / 3u][c % 3u] = value;
				} else {
					r_sh_high[uint64_t(index) * info.sh_high_order + (c / 3u - 3u)][c % 3u] = value;
				}
			}
			if (planes.has_normals) {
				for (uint32_t axis = 0u; axis < 3u; axis++) {
					const uint32_t stored = uint32_t(in[planes.normal() + axis * 2u * n + i]) | (uint32_t(in[planes.normal() + (axis * 2u + 1u) * n + i]) << 8u);
					const int64_t magnitude = normal_map.dequantize(stored > kNormalMaxQ ? stored - kNormalMaxQ : kNormalMaxQ - stored);
					g.normal[axis] = fixed_to_float(stored < kNormalMaxQ ? -magnitude : magnitude, kFixedFrac);
				}
			}
		}
	}
	return OK;
}

} // namespace gs_gsq
