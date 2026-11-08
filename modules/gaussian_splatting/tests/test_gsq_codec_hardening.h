#pragma once

// GSQ page codec hardening (BV-C1b; docs/architecture/adr-beyond-vram-worlds.md D-12). CPU only.
//
// A deterministic mutation fuzz of malformed pages, golden digests pinned across compilers, the
// SPZ smallest-three quantizer as the rotation oracle, and the D-12 error bounds at every
// allowed bit depth. Every case is tagged [WorldIO], so it runs in the strict
// "GaussianSplatting [WorldIO]" lane of tests/ci/run_module_tests.py, never in [untagged].

#include "test_gsq_codec.h"
#include "test_macros.h"

#include "../io/gsq/gsq_codec.h"
#include "../thirdparty/spz/splat-utils.h"

#include "core/crypto/crypto_core.h"
#include "core/io/marshalls.h"
#include "core/templates/local_vector.h"

#include <cmath>
#include <cstring>

namespace spz {
// The SPZ reference quantizer. thirdparty/spz/load-spz.cc defines it with external linkage
// outside its anonymous namespace but declares it in no header.
void packQuaternionSmallestThree(uint8_t r[4], const float rotation[4], const CoordinateConverter &c);
} // namespace spz

namespace TestGSQCodecHardening {

using namespace gs_gsq;
using TestGSQCodec::decode_all;
using TestGSQCodec::encode;
using TestGSQCodec::Errors;
using TestGSQCodec::Fixture;
using TestGSQCodec::make_fixture;
using TestGSQCodec::measure;
using TestGSQCodec::Rng;

// Header offsets of gsq_codec.h, needed to aim mutations and to re-seal a page.
constexpr uint32_t kStoredBytesOffset = 12u;
constexpr uint32_t kBitDepthOffset = 16u; // u8[3] SH bits, then u8 opacity bits
constexpr uint32_t kDigestOffset = 32u;

// -ffast-math may assume finite values, so finiteness is read from the bits.
inline bool finite_bits(float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	return (bits & 0x7F800000u) != 0x7F800000u;
}

inline bool same_bits(float p_a, float p_b) {
	return memcmp(&p_a, &p_b, sizeof(float)) == 0;
}

inline String digest_hex(const uint8_t *p_digest) {
	return String::hex_encode_buffer(p_digest, int(kDigestBytes));
}

// Recomputes the stored SHA-256 over [0, 32) and [64, stored bytes), so a mutant passes
// verify_page() and reaches the frames and the dequantizers.
inline void reseal(LocalVector<uint8_t> &r_page) {
	if (r_page.size() < kPageHeaderBytes) {
		return;
	}
	const uint32_t stored = decode_uint32(r_page.ptr() + kStoredBytesOffset);
	if (stored < kPageHeaderBytes || stored > r_page.size()) {
		return;
	}
	CryptoCore::SHA256Context ctx;
	ctx.start();
	ctx.update(r_page.ptr(), kDigestOffset);
	ctx.update(r_page.ptr() + kHeaderBytes, stored - kHeaderBytes);
	ctx.finish(r_page.ptr() + kDigestOffset);
}

// What every decoded splat must satisfy, corrupt page or not (D-12 semantic read validation):
// finite values, position and DC inside the header ranges, opacity in [0, 1], a positive scale,
// SH within the band maxima, a non-zero rotation, and the page constants as stored. The
// tolerance covers the Q24 fixed point the ranges pass through.
inline uint32_t count_semantic_violations(const PageInfo &p_info, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count) {
	const float tolerance = 1.0f / float(1u << 23u);
	uint32_t violations = 0u;
	for (uint32_t i = 0u; i < p_count; i++) {
		const Gaussian &g = p_gaussians[i];
		bool ok = true;
		for (int axis = 0; axis < 3; axis++) {
			ok = ok && finite_bits(g.position[axis]) && g.position[axis] >= p_info.position_min[axis] - tolerance &&
					g.position[axis] <= p_info.position_max[axis] + tolerance;
			ok = ok && finite_bits(g.sh_dc[axis]) && g.sh_dc[axis] >= p_info.dc_min[axis] - tolerance &&
					g.sh_dc[axis] <= p_info.dc_max[axis] + tolerance;
			ok = ok && finite_bits(g.scale[axis]) && g.scale[axis] > 0.0f;
			if (p_info.has_normals) {
				ok = ok && finite_bits(g.normal[axis]) && std::abs(g.normal[axis]) <= 1.0f + tolerance;
			} else {
				ok = ok && same_bits(g.normal[axis], p_info.constants.normal[axis]);
			}
			for (int v = 0; v < 3; v++) {
				const float value = g.sh_1[v][axis];
				ok = ok && finite_bits(value) && std::abs(value) <= p_info.sh_max[0] + tolerance;
			}
			for (uint32_t k = 0u; k < p_info.sh_high_order; k++) {
				const float value = p_sh_high[uint64_t(i) * p_info.sh_high_order + k][axis];
				ok = ok && finite_bits(value) && std::abs(value) <= p_info.sh_max[k < 5u ? 1 : 2] + tolerance;
			}
		}
		ok = ok && finite_bits(g.opacity) && g.opacity >= 0.0f && g.opacity <= 1.0f;
		ok = ok && finite_bits(g.rotation.x) && finite_bits(g.rotation.y) && finite_bits(g.rotation.z) && finite_bits(g.rotation.w);
		const double length_squared = double(g.rotation.x) * g.rotation.x + double(g.rotation.y) * g.rotation.y +
				double(g.rotation.z) * g.rotation.z + double(g.rotation.w) * g.rotation.w;
		ok = ok && length_squared > 0.25 && length_squared < 2.0;
		ok = ok && same_bits(g.area, p_info.constants.area) && same_bits(g.sh_dc.a, p_info.constants.sh_dc.a) &&
				same_bits(g.stroke_age, p_info.constants.stroke_age) && same_bits(g.brush_axes.x, p_info.constants.brush_axes.x) &&
				same_bits(g.brush_axes.y, p_info.constants.brush_axes.y) && g.painterly_meta == p_info.constants.painterly_meta &&
				g.render_meta == p_info.constants.render_meta;
		if (!ok) {
			violations++;
		}
	}
	return violations;
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] fuzz: every mutated page is refused with a gsq reason or decodes inside its header ranges") {
	// Three base pages: 21-bit positions with SH3; two sub-blocks with per-splat normals; and SH
	// stored at 8 bits, so a header that claims fewer bits meets bytes past its own range.
	LocalVector<LocalVector<uint8_t>> bases;
	bases.resize(3);
	String reason;
	const Fixture sh3 = make_fixture(300, 3, 60.0f, 81);
	Fixture normals = make_fixture(4100, 0, 12.0f, 82);
	Rng normal_rng(83);
	for (Gaussian &g : normals.gaussians) {
		g.normal = Vector3(normal_rng.range(-1, 1), normal_rng.range(-1, 1), normal_rng.range(-1, 1)).normalized();
	}
	const Fixture wide_sh = make_fixture(800, 1, 16.0f, 84);
	EncodeParams wide_params;
	wide_params.sh_bits[0] = 8;
	wide_params.sh_bits[1] = 8;
	wide_params.sh_bits[2] = 8;
	if (encode(sh3, bases[0], &reason) != OK || encode(normals, bases[1], &reason) != OK ||
			encode_page(wide_sh.gaussians.ptr(), nullptr, wide_sh.gaussians.size(), 1, wide_params, bases[2], reason) != OK) {
		FAIL("a base page did not encode: ", reason.utf8().get_data());
		return;
	}

	// Values that sit on the edges of the header checks.
	const uint32_t interesting[] = { 0u, 1u, 2u, 0xFFu, 0xFFFFu, 0x10000u, 0x10001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
		0x7F800000u, 0x7FC00000u, 0xFF800000u, 0x3F800000u, kPageHeaderBytes, kSubBlockSplats };
	constexpr uint32_t kMutantsPerPage = 600u;
	DecodeContext ctx;
	Rng rng(0xC1Bu);
	uint32_t refused_header = 0u;
	uint32_t refused_frames = 0u;
	uint32_t decoded = 0u;
	uint32_t unsealed_accepted = 0u;
	for (uint32_t p = 0u; p < bases.size(); p++) {
		const LocalVector<uint8_t> &base = bases[p];
		for (uint32_t m = 0u; m < kMutantsPerPage; m++) {
			LocalVector<uint8_t> page = base;
			const uint32_t op = uint32_t(rng.next() % 8u);
			switch (op) {
				case 0: { // one bit anywhere
					const uint32_t bit = uint32_t(rng.next() % (uint64_t(page.size()) * 8u));
					page[bit / 8u] ^= uint8_t(1u << (bit % 8u));
				} break;
				case 1: { // one bit of the header
					const uint32_t bit = uint32_t(rng.next() % (kPageHeaderBytes * 8u));
					page[bit / 8u] ^= uint8_t(1u << (bit % 8u));
				} break;
				case 2: { // a random word of the header
					const uint32_t offset = uint32_t(rng.next() % (kPageHeaderBytes / 4u)) * 4u;
					encode_uint32(uint32_t(rng.next()), page.ptr() + offset);
				} break;
				case 3: { // an edge value in a word of the header
					const uint32_t offset = uint32_t(rng.next() % (kPageHeaderBytes / 4u)) * 4u;
					encode_uint32(interesting[rng.next() % (sizeof(interesting) / sizeof(interesting[0]))], page.ptr() + offset);
				} break;
				case 4: { // a run of random bytes inside the frames
					const uint32_t length = 1u + uint32_t(rng.next() % 16u);
					const uint32_t offset = kPageHeaderBytes + uint32_t(rng.next() % (page.size() - kPageHeaderBytes - length + 1u));
					for (uint32_t k = 0u; k < length; k++) {
						page[offset + k] = uint8_t(rng.next());
					}
				} break;
				case 5: // cut anywhere
					page.resize(uint32_t(rng.next() % page.size()));
					break;
				case 6: { // trailing bytes past the stored size
					const uint32_t extra = 1u + uint32_t(rng.next() % 64u);
					for (uint32_t k = 0u; k < extra; k++) {
						page.push_back(uint8_t(rng.next()));
					}
				} break;
				default: // a bit depth: the dequantizer tables are built from these bytes
					page[kBitDepthOffset + uint32_t(rng.next() % 4u)] = uint8_t(rng.next() % 20u);
					break;
			}
			const bool sealed = (rng.next() & 1u) != 0u;
			if (sealed) {
				reseal(page);
			}
			CAPTURE(p);
			CAPTURE(m);
			CAPTURE(op);
			CAPTURE(sealed);

			PageInfo info;
			if (parse_page_header(page.ptr(), page.size(), info, reason) != OK) {
				CHECK(reason.begins_with("gsq_"));
				Gaussian one;
				Vector3 one_sh[12];
				CHECK(ctx.decode_range(page.ptr(), page.size(), 0, 1, &one, one_sh, reason, false) != OK);
				CHECK(reason.begins_with("gsq_"));
				refused_header++;
				continue;
			}
			// A parsed header bounds what a caller allocates from it.
			CHECK(info.splat_count <= kMaxPageSplats);
			CHECK(info.sub_block_count <= kMaxSubBlocks);
			CHECK(info.stored_bytes <= page.size());

			LocalVector<Gaussian> out;
			LocalVector<Vector3> out_sh;
			out.resize(info.splat_count);
			out_sh.resize(info.splat_count * info.sh_high_order);
			bool any_decoded = false;
			for (const bool verify : { true, false }) {
				CAPTURE(verify);
				if (ctx.decode_range(page.ptr(), page.size(), 0, info.splat_count, out.ptr(), out_sh.ptr(), reason, verify) != OK) {
					CHECK(reason.begins_with("gsq_"));
					continue;
				}
				any_decoded = true;
				CHECK(count_semantic_violations(info, out.ptr(), out_sh.ptr(), info.splat_count) == 0u);
				if (verify && !sealed) {
					// Without a re-seal the page digest only passes when no stored byte changed.
					unsealed_accepted++;
					const bool unchanged = info.stored_bytes <= base.size() &&
							memcmp(page.ptr(), base.ptr(), info.stored_bytes) == 0 &&
							decode_uint32(base.ptr() + kStoredBytesOffset) == info.stored_bytes;
					CHECK(unchanged);
				}
			}
			if (any_decoded) {
				decoded++;
			} else {
				refused_frames++;
			}
		}
	}

	// The fuzz must reach every stage, or it proves less than it claims.
	MESSAGE("GSQ fuzz: ", refused_header, " refused by the header, ", refused_frames, " by digest or frames, ", decoded,
			" decoded, ", unsealed_accepted, " unsealed pages accepted with the digest");
	CHECK(refused_header >= 200u);
	CHECK(refused_frames >= 100u);
	CHECK(decoded >= 200u);
	CHECK(unsealed_accepted >= 50u);

	// After every refusal above, the shared context still decodes each base page exactly as a fresh one.
	for (uint32_t p = 0u; p < bases.size(); p++) {
		CAPTURE(p);
		DecodeContext fresh;
		Fixture out_shared;
		Fixture out_fresh;
		if (decode_all(bases[p], ctx, out_shared) != OK || decode_all(bases[p], fresh, out_fresh) != OK) {
			FAIL("a base page no longer decodes");
			continue;
		}
		uint8_t digest_shared[kDigestBytes];
		uint8_t digest_fresh[kDigestBytes];
		decoded_digest(out_shared.gaussians.ptr(), out_shared.sh_high.ptr(), out_shared.gaussians.size(), out_shared.degree, digest_shared);
		decoded_digest(out_fresh.gaussians.ptr(), out_fresh.sh_high.ptr(), out_fresh.gaussians.size(), out_fresh.degree, digest_fresh);
		CHECK(memcmp(digest_shared, digest_fresh, kDigestBytes) == 0);
	}
}

// Golden inputs are built from integers only. A float made from an integer below 2^24 and a power
// of two is exact, so the inputs cannot differ between compilers, -ffast-math or FMA contraction,
// and a changed digest can only come from the codec.
struct IntRng {
	uint32_t state;
	explicit IntRng(uint32_t p_seed) :
			state(p_seed) {}
	uint32_t next() {
		state = state * 1664525u + 1013904223u;
		return state;
	}
	int32_t in(int32_t p_lo, int32_t p_hi) {
		return p_lo + int32_t((next() >> 8u) % uint32_t(p_hi - p_lo + 1));
	}
};

inline float dyadic(int32_t p_value, int p_shift) {
	return std::ldexp(float(p_value), -p_shift);
}

inline Fixture make_golden_fixture(uint32_t p_count, uint32_t p_degree, int32_t p_extent_m, bool p_normals, uint32_t p_seed) {
	Fixture f;
	f.degree = p_degree;
	f.gaussians.resize(p_count);
	const uint32_t high_order = sh_high_order_for_degree(p_degree);
	f.sh_high.resize(p_count * high_order);
	IntRng r(p_seed);
	for (uint32_t i = 0u; i < p_count; i++) {
		Gaussian g;
		g.position = Vector3(dyadic(r.in(0, p_extent_m * 1024), 10), dyadic(r.in(-4096, 4096), 10), dyadic(r.in(-p_extent_m * 1024, 0), 10));
		g.scale = Vector3(dyadic(r.in(16, 1023), 11), dyadic(r.in(16, 1023), 11), dyadic(r.in(1, 64), 12));
		g.opacity = dyadic(r.in(0, 65536), 16);
		g.rotation = Quaternion(dyadic(r.in(-1024, 1024), 10), dyadic(r.in(-1024, 1024), 10), dyadic(r.in(-1024, 1024), 10), dyadic(r.in(-1024, 1024), 10));
		if (g.rotation.x == 0.0f && g.rotation.y == 0.0f && g.rotation.z == 0.0f && g.rotation.w == 0.0f) {
			g.rotation.w = 1.0f;
		}
		g.sh_dc = Color(dyadic(r.in(-3072, 3072), 11), dyadic(r.in(-3072, 3072), 11), dyadic(r.in(-3072, 3072), 11), 1.0f);
		const bool flat = i % 3u == 0u;
		if (p_degree > 0u) {
			for (Vector3 &v : g.sh_1) {
				v = flat ? Vector3() : Vector3(dyadic(r.in(-1024, 1024), 12), dyadic(r.in(-1024, 1024), 12), dyadic(r.in(-1024, 1024), 12));
			}
		}
		for (uint32_t k = 0u; k < high_order; k++) {
			f.sh_high[i * high_order + k] = flat ? Vector3() : Vector3(dyadic(r.in(-512, 512), 12), dyadic(r.in(-512, 512), 12), dyadic(r.in(-512, 512), 12));
		}
		g.normal = p_normals ? Vector3(dyadic(r.in(-1024, 1024), 10), dyadic(r.in(-1024, 1024), 10), dyadic(r.in(-1024, 1024), 10)) : Vector3(0, 1, 0);
		g.area = 1.0f;
		g.stroke_age = 0.25f;
		g.brush_axes = Vector2(1.0f, 2.0f);
		g.painterly_meta = 7u;
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		f.gaussians[i] = g;
	}
	return f;
}

// The page bytes depend on the zstd build; the decoded floats do not. Re-pin the stored digests
// and page sizes, with a reason in the PR, only when thirdparty/zstd changes.
constexpr uint32_t kGoldenZstdVersion = 10507u;

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] golden digests: fixed integer-built pages give pinned bytes on every compiler") {
	struct Golden {
		const char *name;
		uint32_t count;
		uint32_t degree;
		int32_t extent_m;
		bool normals;
		uint32_t seed;
		uint32_t position_bits;
		uint32_t page_bytes;
		const char *decoded_sha256;
		const char *stored_sha256;
	};
	const Golden goldens[] = {
		{ "SH3, 21-bit positions, two sub-blocks", 4500u, 3u, 40, false, 0xB1C1u, 21u, 0u, "unpinned", "unpinned" },
		{ "SH1, 16-bit positions, per-splat normals", 700u, 1u, 12, true, 0xB1C2u, 16u, 0u, "unpinned", "unpinned" },
	};
	const bool zstd_pinned = zstd_library_version() == kGoldenZstdVersion;
	if (!zstd_pinned) {
		FAIL("zstd ", zstd_library_version(), " is not the pinned ", kGoldenZstdVersion,
				": the stored digests below cannot be checked; re-pin them for this zstd");
	}
	DecodeContext ctx;
	for (const Golden &g : goldens) {
		CAPTURE(g.name);
		const Fixture src = make_golden_fixture(g.count, g.degree, g.extent_m, g.normals, g.seed);
		LocalVector<uint8_t> page;
		String reason;
		if (encode(src, page, &reason) != OK) {
			FAIL("encode refused: ", reason.utf8().get_data());
			continue;
		}
		PageInfo info;
		CHECK(verify_page(page.ptr(), page.size(), info, reason) == OK);
		CHECK(info.position_bits == g.position_bits);
		CHECK(info.has_normals == g.normals);
		Fixture out;
		if (decode_all(page, ctx, out, true, &reason) != OK) {
			FAIL("decode refused: ", reason.utf8().get_data());
			continue;
		}
		uint8_t digest[kDigestBytes];
		decoded_digest(out.gaussians.ptr(), out.sh_high.ptr(), out.gaussians.size(), out.degree, digest);
		const String decoded_hex = digest_hex(digest);
		const String stored_hex = digest_hex(info.stored_digest);
		// Printed on every run, so a re-pin and a cross-compiler comparison read them from the log.
		MESSAGE("GSQ golden '", g.name, "': page_bytes=", page.size(), " decoded_sha256=", decoded_hex.utf8().get_data(),
				" stored_sha256=", stored_hex.utf8().get_data());
		CHECK_MESSAGE(decoded_hex == String(g.decoded_sha256), "decoded digest moved: ", decoded_hex.utf8().get_data());
		if (zstd_pinned) {
			CHECK(page.size() == g.page_bytes);
			CHECK_MESSAGE(stored_hex == String(g.stored_sha256), "stored digest moved: ", stored_hex.utf8().get_data());
		}
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] rotation: smallest-three agrees with the SPZ reference quantizer and stays within 0.28 degrees") {
	// One quantization step of a stored component: sqrt(1/2) / 511.
	const double step = std::sqrt(0.5) / 511.0;
	LocalVector<Quaternion> probes;
	const Quaternion fixed[] = {
		Quaternion(0, 0, 0, 1), Quaternion(1, 0, 0, 0), Quaternion(0, -1, 0, 0), Quaternion(0, 0, 0, -1),
		Quaternion(0.5f, 0.5f, 0.5f, 0.5f), Quaternion(-0.5f, 0.5f, -0.5f, 0.5f), Quaternion(0.70710678f, 0.70710678f, 0, 0),
		Quaternion(0, -0.6f, 0.8f, 0), Quaternion(3e-3f, 4e-3f, 0, 0), Quaternion(300.0f, -400.0f, 120.0f, 7.0f),
	};
	for (const Quaternion &q : fixed) {
		probes.push_back(q);
	}
	Rng rng(71);
	for (int i = 0; i < 20000; i++) {
		const Quaternion q(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
		if (q.length_squared() > 1e-4f) {
			probes.push_back(q);
		}
	}
	// Near (1/2, 1/2, 1/2, 1/2), where the rebuilt largest component carries the most error.
	const float h = float(step);
	for (int i = 0; i < 4000; i++) {
		probes.push_back(Quaternion(0.5f + rng.range(-1.5f, 1.5f) * h, 0.5f + rng.range(-1.5f, 1.5f) * h,
				0.5f + rng.range(-1.5f, 1.5f) * h, -0.5f + rng.range(-1.5f, 1.5f) * h));
	}

	Fixture src = make_fixture(probes.size(), 0, 16.0f, 72);
	for (uint32_t i = 0u; i < probes.size(); i++) {
		src.gaussians[i].rotation = probes[i];
	}
	LocalVector<uint8_t> page;
	String reason;
	DecodeContext ctx;
	Fixture out;
	if (encode(src, page, &reason) != OK || decode_all(page, ctx, out, true, &reason) != OK) {
		FAIL("round trip refused: ", reason.utf8().get_data());
		return;
	}

	const auto angle_degrees = [](const double *p_a, const double *p_b) {
		double dot = 0.0;
		double aa = 0.0;
		double bb = 0.0;
		for (int c = 0; c < 4; c++) {
			dot += p_a[c] * p_b[c];
			aa += p_a[c] * p_a[c];
			bb += p_b[c] * p_b[c];
		}
		const double cosine = MIN(1.0, std::abs(dot) / std::sqrt(aa * bb));
		return 2.0 * std::acos(cosine) * 180.0 / Math::PI;
	};
	double worst_gsq = 0.0;
	double worst_spz = 0.0;
	double worst_disagreement = 0.0;
	uint32_t agree = 0u;
	const spz::CoordinateConverter identity{};
	for (uint32_t i = 0u; i < probes.size(); i++) {
		const Quaternion &q = probes[i];
		const Quaternion &d = out.gaussians[i].rotation;
		const double source[4] = { q.x, q.y, q.z, q.w };
		const double gsq[4] = { d.x, d.y, d.z, d.w };
		const float spz_in[4] = { q.x, q.y, q.z, q.w };
		uint8_t packed[4];
		spz::packQuaternionSmallestThree(packed, spz_in, identity);
		float spz_out[4];
		spz::unpackQuaternionSmallestThree(spz_out, packed);
		const double reference[4] = { spz_out[0], spz_out[1], spz_out[2], spz_out[3] };

		worst_gsq = MAX(worst_gsq, angle_degrees(source, gsq));
		worst_spz = MAX(worst_spz, angle_degrees(source, reference));
		// Both store the same grid; q and -q are one rotation.
		double dot = 0.0;
		for (int c = 0; c < 4; c++) {
			dot += gsq[c] * reference[c];
		}
		const double sign = dot < 0.0 ? -1.0 : 1.0;
		double disagreement = 0.0;
		for (int c = 0; c < 4; c++) {
			disagreement = MAX(disagreement, std::abs(gsq[c] - sign * reference[c]));
		}
		worst_disagreement = MAX(worst_disagreement, disagreement);
		if (disagreement <= 1e-5) {
			agree++;
		}
	}
	MESSAGE("GSQ rotation: worst ", worst_gsq, " deg (SPZ ", worst_spz, " deg), ", agree, " of ", probes.size(),
			" probes on the SPZ grid point, worst disagreement ", worst_disagreement);
	// D-12: rotation error at most about 0.28 degrees. The near-half probes must reach the region
	// where that bound is tight, or the bound is not exercised.
	CHECK(worst_gsq <= 0.28);
	CHECK(worst_gsq >= 0.2);
	CHECK(worst_gsq <= worst_spz + 1e-3);
	// The integer quantizer and SPZ's float one round alike except within float rounding of a tie;
	// a different rounding rule moves most probes, a different grid moves all of them.
	CHECK(agree >= probes.size() - probes.size() / 200u);
	CHECK(worst_disagreement <= 2.0 * step);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] D-12 error bounds: 0.25 mm position step, then half a step at every bit depth of scale, opacity, DC and 1 B SH") {
	DecodeContext ctx;
	String reason;

	// Position: 16 bits while that steps at most 0.25 mm on the longest axis, else 21 bits.
	struct PositionCase {
		float extent;
		uint32_t bits;
	};
	for (const PositionCase c : { PositionCase{ 16.0f, 16u }, PositionCase{ 16.5f, 21u }, PositionCase{ 400.0f, 21u } }) {
		CAPTURE(c.extent);
		const Fixture src = make_fixture(5000, 0, c.extent, 91);
		LocalVector<uint8_t> page;
		Fixture out;
		if (encode(src, page, &reason) != OK || decode_all(page, ctx, out, true, &reason) != OK) {
			FAIL("round trip refused: ", reason.utf8().get_data());
			continue;
		}
		PageInfo info;
		CHECK(parse_page_header(page.ptr(), page.size(), info, reason) == OK);
		CHECK(info.position_bits == c.bits);
		const Errors e = measure(src, out);
		// Half of 0.25 mm, plus one float ULP at the largest coordinate (x < 100 + extent).
		const float ulp = std::ldexp(1.0f, std::ilogb(100.0f + c.extent) - 23);
		for (int axis = 0; axis < 3; axis++) {
			CHECK(e.position[axis] <= 0.000125f + ulp);
		}
	}

	// SH 2..8 bits (one byte per coefficient whatever the depth) and opacity 8..16 bits.
	const Fixture src = make_fixture(3000, 3, 16.0f, 92);
	float band_max[3] = {};
	float log_lo[3] = { 1e9f, 1e9f, 1e9f };
	float log_hi[3] = { -1e9f, -1e9f, -1e9f };
	float dc_lo[3] = { 1e9f, 1e9f, 1e9f };
	float dc_hi[3] = { -1e9f, -1e9f, -1e9f };
	const uint32_t high_order = sh_high_order_for_degree(3);
	for (uint32_t i = 0u; i < src.gaussians.size(); i++) {
		const Gaussian &g = src.gaussians[i];
		for (int axis = 0; axis < 3; axis++) {
			for (int v = 0; v < 3; v++) {
				band_max[0] = MAX(band_max[0], std::abs(g.sh_1[v][axis]));
			}
			for (uint32_t k = 0u; k < high_order; k++) {
				const int band = k < 5u ? 1 : 2;
				band_max[band] = MAX(band_max[band], std::abs(src.sh_high[i * high_order + k][axis]));
			}
			log_lo[axis] = MIN(log_lo[axis], std::log2(g.scale[axis]));
			log_hi[axis] = MAX(log_hi[axis], std::log2(g.scale[axis]));
			dc_lo[axis] = MIN(dc_lo[axis], g.sh_dc[axis]);
			dc_hi[axis] = MAX(dc_hi[axis], g.sh_dc[axis]);
		}
	}
	const float fixed_slack = 4.0f / float(1u << 24u);
	const float dc_max_q[3] = { 2047.0f, 2047.0f, 1023.0f };
	for (uint32_t i = 0u; i <= 8u; i++) {
		EncodeParams params;
		const uint8_t sh_bits = uint8_t(2u + MIN(i, 6u));
		params.sh_bits[0] = sh_bits;
		params.sh_bits[1] = sh_bits;
		params.sh_bits[2] = sh_bits;
		params.opacity_bits = uint8_t(8u + i);
		CAPTURE(sh_bits);
		CAPTURE(params.opacity_bits);
		LocalVector<uint8_t> page;
		Fixture out;
		if (encode_page(src.gaussians.ptr(), src.sh_high.ptr(), src.gaussians.size(), 3, params, page, reason) != OK ||
				decode_all(page, ctx, out, true, &reason) != OK) {
			FAIL("round trip refused: ", reason.utf8().get_data());
			continue;
		}
		const Errors e = measure(src, out);
		const float sh_q = float((1u << (sh_bits - 1u)) - 1u);
		for (int band = 0; band < 3; band++) {
			CHECK(e.sh[band] <= band_max[band] / sh_q * 0.5f * 1.001f + fixed_slack);
		}
		CHECK(e.zero_sh_broken == 0u);
		CHECK(e.opacity <= 0.5f / float((1u << params.opacity_bits) - 1u) * 1.001f + fixed_slack);
		for (int axis = 0; axis < 3; axis++) {
			// log2 scale is Q16 (1.5e-5) before its 10-bit quantization.
			CHECK(e.log_scale[axis] <= (log_hi[axis] - log_lo[axis]) / 1023.0f * 0.5f + 3e-5f);
			CHECK(e.dc[axis] <= (dc_hi[axis] - dc_lo[axis]) / dc_max_q[axis] * 0.5f * 1.001f + fixed_slack);
		}
		CHECK(e.constants_broken == 0u);
	}

	// One byte per SH coefficient and 16-bit opacity are the ceilings of the format.
	LocalVector<uint8_t> page;
	EncodeParams params;
	params.sh_bits[2] = 9;
	CHECK(encode_page(src.gaussians.ptr(), src.sh_high.ptr(), src.gaussians.size(), 3, params, page, reason) != OK);
	CHECK(reason.begins_with("gsq_invalid_params"));
	params = EncodeParams();
	params.opacity_bits = 17;
	CHECK(encode_page(src.gaussians.ptr(), src.sh_high.ptr(), src.gaussians.size(), 3, params, page, reason) != OK);
	CHECK(reason.begins_with("gsq_invalid_params"));
}

} // namespace TestGSQCodecHardening
