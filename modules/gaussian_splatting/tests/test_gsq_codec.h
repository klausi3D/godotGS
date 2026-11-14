#pragma once

// GSQ page codec core (BV-C1a; docs/architecture/adr-beyond-vram-worlds.md D-12). CPU only.
//
// Round trip within the per-attribute error bound, digest determinism, range decode that
// touches only its sub-blocks, a corrupt page refused without a crash, one context per thread,
// explicit refusals, and the zstd link check of risk K11. Fuzzing, golden digests, the SPZ
// oracle and the bit-depth error bounds are BV-C1b, in test_gsq_codec_hardening.h.

#include "test_macros.h"
#include "gs_gsq_codec_test_fixture.h"

#include "../io/gsq/gsq_codec.h"

#include "core/os/thread.h"
#include "core/templates/local_vector.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace TestGSQCodec {

using namespace gs_gsq;

inline Error encode(const Fixture &p_f, LocalVector<uint8_t> &r_page, String *r_reason = nullptr) {
	String reason;
	const Error err = encode_page(p_f.gaussians.ptr(), p_f.sh_high.ptr(), p_f.gaussians.size(), p_f.degree, EncodeParams(), r_page, reason);
	if (r_reason != nullptr) {
		*r_reason = reason;
	}
	return err;
}

inline Error decode_all(const LocalVector<uint8_t> &p_page, DecodeContext &p_ctx, Fixture &r_out, bool p_verify = true, String *r_reason = nullptr) {
	PageInfo info;
	String reason;
	Error err = parse_page_header(p_page.ptr(), p_page.size(), info, reason);
	if (err == OK) {
		r_out.degree = info.sh_degree;
		r_out.gaussians.resize(info.splat_count);
		r_out.sh_high.resize(info.splat_count * info.sh_high_order);
		err = p_ctx.decode_range(p_page.ptr(), p_page.size(), 0, info.splat_count, r_out.gaussians.ptr(), r_out.sh_high.ptr(), reason, p_verify);
	}
	if (r_reason != nullptr) {
		*r_reason = reason;
	}
	return err;
}

inline bool digest_equal(const uint8_t *p_a, const uint8_t *p_b) {
	return memcmp(p_a, p_b, kDigestBytes) == 0;
}

// Largest absolute error per attribute over a decoded page.
struct Errors {
	float position[3] = {};
	float log_scale[3] = {};
	float opacity = 0.0f;
	float dc[3] = {};
	float sh[3] = {};
	float rotation_cos = 1.0f;
	float rotation_norm = 0.0f;
	uint32_t zero_sh_broken = 0;
	uint32_t constants_broken = 0;
};

inline Errors measure(const Fixture &p_src, const Fixture &p_out) {
	Errors e;
	const uint32_t high_order = sh_high_order_for_degree(p_src.degree);
	for (uint32_t i = 0; i < p_src.gaussians.size(); i++) {
		const Gaussian &a = p_src.gaussians[i];
		const Gaussian &b = p_out.gaussians[i];
		for (int axis = 0; axis < 3; axis++) {
			e.position[axis] = MAX(e.position[axis], std::abs(a.position[axis] - b.position[axis]));
			e.log_scale[axis] = MAX(e.log_scale[axis], std::abs(std::log2(a.scale[axis]) - std::log2(b.scale[axis])));
			e.dc[axis] = MAX(e.dc[axis], std::abs(a.sh_dc[axis] - b.sh_dc[axis]));
		}
		e.opacity = MAX(e.opacity, std::abs(a.opacity - b.opacity));
		const float cosine = std::abs(a.rotation.dot(b.rotation));
		e.rotation_cos = MIN(e.rotation_cos, cosine);
		e.rotation_norm = MAX(e.rotation_norm, std::abs(b.rotation.length() - 1.0f));
		for (int v = 0; v < 3; v++) {
			for (int axis = 0; axis < 3; axis++) {
				e.sh[0] = MAX(e.sh[0], std::abs(a.sh_1[v][axis] - b.sh_1[v][axis]));
				if (a.sh_1[v][axis] == 0.0f && b.sh_1[v][axis] != 0.0f) {
					e.zero_sh_broken++;
				}
			}
		}
		for (uint32_t k = 0; k < high_order; k++) {
			const Vector3 &s = p_src.sh_high[i * high_order + k];
			const Vector3 &d = p_out.sh_high[i * high_order + k];
			for (int axis = 0; axis < 3; axis++) {
				e.sh[k < 5u ? 1 : 2] = MAX(e.sh[k < 5u ? 1 : 2], std::abs(s[axis] - d[axis]));
				if (s[axis] == 0.0f && d[axis] != 0.0f) {
					e.zero_sh_broken++;
				}
			}
		}
		if (a.area != b.area || a.stroke_age != b.stroke_age || a.brush_axes != b.brush_axes || a.painterly_meta != b.painterly_meta ||
				a.render_meta != b.render_meta || a.sh_dc.a != b.sh_dc.a || a.normal != b.normal) {
			e.constants_broken++;
		}
	}
	return e;
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] round trip stays inside the D-12 error bound of every attribute, in both position modes") {
	struct Case {
		float extent;
		uint32_t bits;
	};
	// 16 m steps finer than 0.25 mm at 16 bits; 60 m does not, and takes 21.
	for (const Case c : { Case{ 16.0f, 16u }, Case{ 60.0f, 21u } }) {
		const Fixture src = make_fixture(10000, 3, c.extent, 11);
		LocalVector<uint8_t> page;
		String reason;
		if (encode(src, page, &reason) != OK) {
			FAIL("encode refused: ", reason.utf8().get_data());
			continue;
		}
		PageInfo info;
		CHECK(parse_page_header(page.ptr(), page.size(), info, reason) == OK);
		CHECK(info.position_bits == c.bits);
		CHECK(info.sub_block_count == 3u);
		CHECK_FALSE(info.has_normals);

		DecodeContext ctx;
		Fixture out;
		if (decode_all(page, ctx, out, true, &reason) != OK) {
			FAIL("decode refused: ", reason.utf8().get_data());
			continue;
		}
		const Errors e = measure(src, out);
		const float steps = float((1u << c.bits) - 1u);
		// Half a step, plus one float ULP at the magnitude of these positions.
		const float x_bound = c.extent / steps * 0.51f + 1.6e-5f;
		CHECK(e.position[0] <= x_bound);
		CHECK(e.position[2] <= x_bound);
		CHECK(e.position[1] <= 10.0f / steps * 0.51f + 1e-6f);
		// Half a step of each per-page log2 range, 10 bits.
		CHECK(e.log_scale[0] <= (std::log2(0.5f) - std::log2(0.01f)) / 1023.0f * 0.52f);
		CHECK(e.log_scale[2] <= (std::log2(0.02f) - std::log2(0.001f)) / 1023.0f * 0.52f);
		CHECK(e.opacity <= 0.51f / 65535.0f);
		CHECK(e.dc[0] <= 3.0f / 2047.0f * 0.51f);
		CHECK(e.dc[2] <= 3.0f / 1023.0f * 0.51f);
		CHECK(e.sh[0] <= 0.4f / 31.0f * 0.51f);
		CHECK(e.sh[1] <= 0.1f / 15.0f * 0.51f);
		CHECK(e.sh[2] <= 0.1f / 15.0f * 0.51f);
		// Half a step is 6.9e-4 per stored component. The worst case is (1/2, 1/2, 1/2, 1/2), where
		// the rebuilt largest component adds 2.1e-3: cos >= 1 - 2.9e-6, about 0.28 degrees.
		CHECK(e.rotation_cos >= 0.999997f);
		CHECK(e.rotation_norm <= 2e-6f);
		CHECK(e.zero_sh_broken == 0u);
		CHECK(e.constants_broken == 0u);
		// Order is the caller's: a prefix stays a valid sample.
		CHECK(out.gaussians[9999].position.distance_to(src.gaussians[9999].position) < 0.01f);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] page limits: a full page, per-splat normals, one splat") {
	// 65,536 splats fill all 16 table entries of the fixed header.
	const Fixture full = make_fixture(kMaxPageSplats, 0, 16.0f, 5);
	LocalVector<uint8_t> page;
	CHECK(encode(full, page) == OK);
	PageInfo info;
	String reason;
	CHECK(verify_page(page.ptr(), page.size(), info, reason) == OK);
	CHECK(info.sub_block_count == kMaxSubBlocks);
	DecodeContext ctx;
	Fixture out;
	CHECK(decode_all(page, ctx, out) == OK);
	if (out.gaussians.size() != kMaxPageSplats) {
		FAIL("expected a full page, got ", out.gaussians.size());
		return;
	}
	CHECK(out.gaussians[65535].position.distance_to(full.gaussians[65535].position) < 0.001f);

	// Normals that differ take the per-splat planes, without a caller decision.
	Fixture normals = make_fixture(5000, 1, 16.0f, 6);
	Rng rng(7);
	for (Gaussian &g : normals.gaussians) {
		g.normal = Vector3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)).normalized();
	}
	CHECK(encode(normals, page) == OK);
	CHECK(decode_all(page, ctx, out) == OK);
	CHECK(parse_page_header(page.ptr(), page.size(), info, reason) == OK);
	CHECK(info.has_normals);
	float worst = 0.0f;
	for (uint32_t i = 0; i < normals.gaussians.size(); i++) {
		worst = MAX(worst, normals.gaussians[i].normal.distance_to(out.gaussians[i].normal));
	}
	CHECK(worst <= 1.0f / 32767.0f);

	// One splat: every range is empty, so the value survives to the Q24 fixed-point resolution.
	const Fixture one = make_fixture(1, 2, 16.0f, 8);
	CHECK(encode(one, page) == OK);
	CHECK(decode_all(page, ctx, out) == OK);
	CHECK(out.gaussians[0].position.distance_to(one.gaussians[0].position) <= 1e-6f);
	CHECK(std::abs(out.gaussians[0].sh_dc.r - one.gaussians[0].sh_dc.r) <= 1e-7f);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] digests: same input same bytes and same decoded digest, one changed splat changes both") {
	const Fixture src = make_fixture(9000, 3, 60.0f, 21);
	LocalVector<uint8_t> a;
	LocalVector<uint8_t> b;
	CHECK(encode(src, a) == OK);
	CHECK(encode(src, b) == OK);
	CHECK(a.size() == b.size());
	CHECK(a.size() > kPageHeaderBytes);
	CHECK(memcmp(a.ptr(), b.ptr(), MIN(a.size(), b.size())) == 0);

	// Independent contexts; the second has decoded something else first.
	DecodeContext first;
	DecodeContext second;
	Fixture scratch;
	LocalVector<uint8_t> other;
	CHECK(encode(make_fixture(5000, 1, 16.0f, 22), other) == OK);
	CHECK(decode_all(other, second, scratch) == OK);
	Fixture out_a;
	Fixture out_b;
	CHECK(decode_all(a, first, out_a) == OK);
	CHECK(decode_all(a, second, out_b) == OK);
	uint8_t digest_a[kDigestBytes];
	uint8_t digest_b[kDigestBytes];
	decoded_digest(out_a.gaussians.ptr(), out_a.sh_high.ptr(), out_a.gaussians.size(), out_a.degree, digest_a);
	decoded_digest(out_b.gaussians.ptr(), out_b.sh_high.ptr(), out_b.gaussians.size(), out_b.degree, digest_b);
	CHECK(digest_equal(digest_a, digest_b));

	// One high-order coefficient of the last splat, by far more than a quantization step.
	Fixture changed = src;
	changed.sh_high[changed.sh_high.size() - 1].x += 0.05f;
	LocalVector<uint8_t> c;
	CHECK(encode(changed, c) == OK);
	Fixture out_c;
	CHECK(decode_all(c, first, out_c) == OK);
	uint8_t digest_c[kDigestBytes];
	decoded_digest(out_c.gaussians.ptr(), out_c.sh_high.ptr(), out_c.gaussians.size(), out_c.degree, digest_c);
	CHECK_FALSE(digest_equal(digest_a, digest_c));
	PageInfo info_a;
	PageInfo info_c;
	String reason;
	CHECK(verify_page(a.ptr(), a.size(), info_a, reason) == OK);
	CHECK(verify_page(c.ptr(), c.size(), info_c, reason) == OK);
	CHECK_FALSE(digest_equal(info_a.stored_digest, info_c.stored_digest));
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] a range decodes to the same splats as the whole page and touches only its own sub-blocks") {
	const Fixture src = make_fixture(12000, 3, 60.0f, 31);
	LocalVector<uint8_t> page;
	CHECK(encode(src, page) == OK);
	PageInfo info;
	String reason;
	CHECK(parse_page_header(page.ptr(), page.size(), info, reason) == OK);
	DecodeContext ctx;
	Fixture whole;
	CHECK(decode_all(page, ctx, whole) == OK);
	if (info.sub_block_count != 3u || whole.gaussians.size() != 12000u) {
		FAIL("fixture did not make three sub-blocks");
		return;
	}

	// Mid-block start (deltas must run from the block start) across one boundary.
	const uint32_t first = 4000;
	const uint32_t count = 300;
	const uint32_t high_order = info.sh_high_order;
	LocalVector<Gaussian> range;
	LocalVector<Vector3> range_sh;
	range.resize(count);
	range_sh.resize(count * high_order);
	CHECK(ctx.decode_range(page.ptr(), page.size(), first, count, range.ptr(), range_sh.ptr(), reason) == OK);
	uint8_t digest_range[kDigestBytes];
	uint8_t digest_whole[kDigestBytes];
	decoded_digest(range.ptr(), range_sh.ptr(), count, 3, digest_range);
	decoded_digest(whole.gaussians.ptr() + first, whole.sh_high.ptr() + first * high_order, count, 3, digest_whole);
	CHECK(digest_equal(digest_range, digest_whole));

	// Sub-block 2 holds no splat of the range: corrupting it must not matter without the page
	// digest, and must be seen as soon as the range reaches it.
	LocalVector<uint8_t> broken = page;
	const uint64_t last_frame = page.size() - info.frame_bytes[2];
	broken[uint32_t(last_frame) + info.frame_bytes[2] / 2u] ^= 0x40u;
	CHECK(ctx.decode_range(broken.ptr(), broken.size(), first, count, range.ptr(), range_sh.ptr(), reason, false) == OK);
	decoded_digest(range.ptr(), range_sh.ptr(), count, 3, digest_range);
	CHECK(digest_equal(digest_range, digest_whole));
	CHECK(ctx.decode_range(broken.ptr(), broken.size(), 11000, 10, range.ptr(), range_sh.ptr(), reason, false) != OK);
	CHECK(reason.begins_with("gsq_frame_corrupt"));

	CHECK(ctx.decode_range(page.ptr(), page.size(), 11990, 11, range.ptr(), range_sh.ptr(), reason) != OK);
	CHECK(reason.begins_with("gsq_range_out_of_bounds"));
	CHECK(ctx.decode_range(page.ptr(), page.size(), 0, 0, range.ptr(), range_sh.ptr(), reason) != OK);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] a corrupt page is refused with a reason and no crash, and the context stays usable") {
	const Fixture src = make_fixture(6000, 2, 16.0f, 41);
	LocalVector<uint8_t> page;
	CHECK(encode(src, page) == OK);
	DecodeContext ctx;
	Fixture out;
	String reason;

	const uint32_t frame_byte = kPageHeaderBytes + 100u;
	struct Corruption {
		const char *name;
		uint32_t offset;
		uint8_t mask;
		const char *with_digest;
		const char *without_digest; // null: the header cannot tell, the decode must still end safely
	};
	const Corruption cases[] = {
		{ "magic", 0u, 0x01u, "gsq_bad_magic", "gsq_bad_magic" },
		{ "splat count", 9u, 0x80u, "gsq_bad_header", "gsq_bad_header" },
		{ "stored bytes", 14u, 0x10u, "gsq_truncated", "gsq_truncated" },
		{ "frame table", 65u, 0x40u, "gsq_bad_table", "gsq_bad_table" },
		{ "position range", 131u, 0x7Fu, "gsq_digest_mismatch", nullptr },
		{ "frame payload", frame_byte, 0x10u, "gsq_digest_mismatch", "gsq_frame_corrupt" },
	};
	for (const Corruption &c : cases) {
		CAPTURE(c.name);
		LocalVector<uint8_t> broken = page;
		broken[c.offset] ^= c.mask;
		CHECK(decode_all(broken, ctx, out, true, &reason) != OK);
		CHECK(reason.begins_with(c.with_digest));
		const Error err = decode_all(broken, ctx, out, false, &reason);
		if (c.without_digest != nullptr) {
			CHECK(err != OK);
			CHECK(reason.begins_with(c.without_digest));
		}
	}

	// Cut anywhere: inside the header and inside the last frame.
	for (const uint32_t size : { 0u, 10u, kPageHeaderBytes - 1u, kPageHeaderBytes, page.size() - 1u }) {
		CAPTURE(size);
		PageInfo info;
		CHECK(verify_page(page.ptr(), size, info, reason) != OK);
		CHECK(reason.begins_with("gsq_truncated"));
	}

	CHECK(decode_all(page, ctx, out) == OK);
	const Errors e = measure(src, out);
	CHECK(e.opacity <= 0.51f / 65535.0f);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] one context per worker: concurrent decodes of one page agree with a serial decode") {
	const Fixture src = make_fixture(20000, 3, 60.0f, 51);
	LocalVector<uint8_t> page;
	CHECK(encode(src, page) == OK);
	DecodeContext serial;
	Fixture out;
	CHECK(decode_all(page, serial, out) == OK);
	uint8_t expected[kDigestBytes];
	decoded_digest(out.gaussians.ptr(), out.sh_high.ptr(), out.gaussians.size(), out.degree, expected);

	struct Work {
		const LocalVector<uint8_t> *page = nullptr;
		const uint8_t *expected = nullptr;
		std::atomic<uint32_t> mismatches{ 0 };
	} work;
	work.page = &page;
	work.expected = expected;
	const auto worker = [](void *p_user) {
		Work *w = static_cast<Work *>(p_user);
		DecodeContext ctx;
		for (int round = 0; round < 8; round++) {
			Fixture local;
			uint8_t digest[kDigestBytes];
			if (decode_all(*w->page, ctx, local, false) != OK) {
				w->mismatches.fetch_add(1);
				continue;
			}
			decoded_digest(local.gaussians.ptr(), local.sh_high.ptr(), local.gaussians.size(), local.degree, digest);
			if (!digest_equal(digest, w->expected)) {
				w->mismatches.fetch_add(1);
			}
		}
	};
	Thread threads[4];
	for (Thread &t : threads) {
		t.start(worker, &work);
	}
	for (Thread &t : threads) {
		t.wait_to_finish();
	}
	CHECK(work.mismatches.load() == 0u);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] what a page cannot carry is refused with a gsq reason, never dropped") {
	const Fixture base = make_fixture(100, 1, 16.0f, 61);
	LocalVector<uint8_t> page;
	String reason;

	const auto refused = [&](const Fixture &p_f, const char *p_reason) {
		CHECK(encode(p_f, page, &reason) != OK);
		CHECK(reason.begins_with(p_reason));
		CHECK(page.is_empty());
	};
	Fixture f = base;
	f.gaussians[50].area = 2.0f;
	refused(f, "gsq_heterogeneous_field");
	f = base;
	f.gaussians[50].render_meta = 0u;
	refused(f, "gsq_heterogeneous_field");
	f = base;
	f.gaussians[50].position.y = std::nanf("");
	refused(f, "gsq_non_finite");
	f = base;
	f.gaussians[50].rotation = Quaternion(0, 0, 0, 0);
	refused(f, "gsq_invalid_rotation");
	f = base;
	f.gaussians[50].opacity = 1.5f;
	refused(f, "gsq_field_out_of_range");
	f = base;
	f.gaussians[50].scale.x = -0.5f;
	refused(f, "gsq_field_out_of_range");
	f = base;
	f.gaussians[0].position.x = -100000.0f; // page range past 131 km
	f.gaussians[1].position.x = 100000.0f;
	refused(f, "gsq_field_out_of_range");
	f = base;
	f.degree = 0;
	refused(f, "gsq_sh_layout");

	CHECK(encode_page(base.gaussians.ptr(), nullptr, 100, 4, EncodeParams(), page, reason) != OK);
	CHECK(reason.begins_with("gsq_sh_degree_unsupported"));
	CHECK(encode_page(base.gaussians.ptr(), nullptr, 100, 2, EncodeParams(), page, reason) != OK);
	CHECK(reason.begins_with("gsq_sh_layout"));
	CHECK(encode_page(base.gaussians.ptr(), nullptr, 0, 1, EncodeParams(), page, reason) != OK);
	CHECK(reason.begins_with("gsq_empty_page"));
	CHECK(encode_page(base.gaussians.ptr(), nullptr, kMaxPageSplats + 1u, 1, EncodeParams(), page, reason) != OK);
	CHECK(reason.begins_with("gsq_page_too_large"));
	EncodeParams params;
	params.sh_bits[1] = 1;
	CHECK(encode_page(base.gaussians.ptr(), nullptr, 100, 1, params, page, reason) != OK);
	CHECK(reason.begins_with("gsq_invalid_params"));
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] zstd link check: the codec's symbols link and header and library agree (K11)") {
	// A missing symbol fails this binary's link. An ABI mismatch between the header the module
	// compiled against and the library it linked is caught here, at run time.
	CHECK(zstd_header_version() == zstd_library_version());
	CHECK(zstd_library_version() >= 10400u);
}

} // namespace TestGSQCodec
