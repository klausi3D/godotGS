#pragma once

// GSQ page builder, the writer's codec stage (BV-C3; docs/architecture/adr-beyond-vram-worlds.md
// D-12, §4.4 W12/W15, §6.2). CPU only.
//
// Page order: across pages importance order is kept (each page holds the next run of the
// chunk's importance order, so a page prefix is a sample prefix); inside a page the splats are
// in Morton order (checked against an independent Morton order of an integer grid). Page
// constants: a painterly chunk with mixed palettes, brush axes and stroke ages is split into
// constant groups, every page encodes, the counters show the split, and a field the codec cannot
// carry fails the chunk visibly. W12: a page damaged after encoding is refused before use.
// W15: each record carries the page's stored digest and the digest of its decoded splats.

#include "test_macros.h"

#include "test_gsq_codec.h"

#include "../io/gs_world_writer.h"
#include "../io/gsq/gsq_page_builder.h"

#include "core/templates/local_vector.h"

#include <cmath>
#include <cstring>

namespace TestGSQPageBuilder {

using namespace gs_gsq;
using TestGSQCodec::Fixture;
using TestGSQCodec::make_fixture;
using TestGSQCodec::Rng;

inline Error build(PageBuilder &p_builder, const Fixture &p_f, uint32_t p_chunk, LocalVector<BuiltPage> &r_pages, String &r_reason) {
	return p_builder.build_chunk(p_chunk, p_f.gaussians.ptr(), p_f.sh_high.ptr(), p_f.gaussians.size(), r_pages, r_reason);
}

inline bool same_constants(const Gaussian &p_a, const Gaussian &p_b) {
	return memcmp(&p_a.area, &p_b.area, 4) == 0 && memcmp(&p_a.sh_dc.a, &p_b.sh_dc.a, 4) == 0 &&
			memcmp(&p_a.stroke_age, &p_b.stroke_age, 4) == 0 && memcmp(&p_a.brush_axes, &p_b.brush_axes, 8) == 0 &&
			p_a.painterly_meta == p_b.painterly_meta && p_a.render_meta == p_b.render_meta;
}

// Every source index appears in exactly one page.
inline bool covers_once(const LocalVector<BuiltPage> &p_pages, uint32_t p_count) {
	LocalVector<uint8_t> seen;
	seen.resize(p_count);
	memset(seen.ptr(), 0, p_count);
	uint32_t total = 0u;
	for (const BuiltPage &page : p_pages) {
		for (uint32_t source : page.source_index) {
			if (source >= p_count || seen[source] != 0u) {
				return false;
			}
			seen[source] = 1u;
			total++;
		}
	}
	return total == p_count;
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] page builder: page order follows importance, a prefix of pages is a prefix of the chunk; W15 digests") {
	const Fixture src = make_fixture(10000, 3, 32.0f, 21);
	PageBuilder builder;
	String reason;
	CHECK(builder.configure(EncodeParams(), 3, reason) == OK);
	LocalVector<BuiltPage> pages;
	if (build(builder, src, 5, pages, reason) != OK) {
		FAIL("build refused: ", reason.utf8().get_data());
		return;
	}
	// 10000 = 4096 + 4096 + 1808: whole-page granularity.
	if (pages.size() != 3u) {
		FAIL("expected 3 pages, got ", pages.size());
		return;
	}
	CHECK(covers_once(pages, 10000));
	const uint32_t expected_counts[3] = { 4096u, 4096u, 1808u };
	DecodeContext ctx;
	for (uint32_t p = 0; p < 3u; p++) {
		const BuiltPage &page = pages[p];
		INFO("page ", p);
		CHECK(page.record.chunk_index == 5u);
		CHECK(page.record.page_in_chunk == p);
		CHECK(page.record.constant_group == 0u);
		CHECK(page.record.splat_count == expected_counts[p]);
		CHECK(page.source_index.size() == expected_counts[p]);
		// Page p holds exactly importance ranks [4096 p, 4096 p + count).
		uint32_t lo = UINT32_MAX;
		uint32_t hi = 0u;
		uint32_t inversions = 0u;
		for (uint32_t j = 0; j < page.source_index.size(); j++) {
			lo = MIN(lo, page.source_index[j]);
			hi = MAX(hi, page.source_index[j]);
			if (j > 0u && page.source_index[j] < page.source_index[j - 1u]) {
				inversions++;
			}
		}
		CHECK(lo == p * 4096u);
		CHECK(hi == p * 4096u + expected_counts[p] - 1u);
		CHECK(page.record.first_source == lo);
		CHECK(page.record.last_source == hi);
		// Inside the page the order is spatial, not the importance order.
		CHECK(inversions > page.source_index.size() / 4u);

		// W15: the stored digest is the page's, the decoded digest is that of its decoded splats.
		PageInfo info;
		CHECK(verify_page(page.bytes.ptr(), page.bytes.size(), info, reason) == OK);
		CHECK(TestGSQCodec::digest_equal(info.stored_digest, page.record.stored_digest));
		CHECK(page.record.stored_bytes == page.bytes.size());
		Fixture out;
		if (TestGSQCodec::decode_all(page.bytes, ctx, out, true, &reason) != OK) {
			FAIL("decode refused: ", reason.utf8().get_data());
			continue;
		}
		uint8_t digest[kDigestBytes];
		decoded_digest(out.gaussians.ptr(), out.sh_high.ptr(), out.gaussians.size(), 3, digest);
		CHECK(TestGSQCodec::digest_equal(digest, page.record.decoded_digest));
		// Slot j decodes the splat source_index[j] names.
		float worst = 0.0f;
		for (uint32_t j = 0; j < out.gaussians.size(); j++) {
			worst = MAX(worst, out.gaussians[j].position.distance_to(src.gaussians[page.source_index[j]].position));
		}
		CHECK(worst < 0.01f);
	}
	CHECK(builder.get_stats().pages == 3u);
	CHECK(builder.get_stats().splats == 10000u);
	CHECK(builder.get_stats().heterogeneous_chunks == 0u);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] page builder: inside a page the splats are in Morton order") {
	// An 8 x 8 x 8 integer grid at 64..71 (one float exponent, so integer steps are linear),
	// handed over in a scrambled importance order.
	Fixture src = make_fixture(512, 0, 8.0f, 3);
	LocalVector<uint32_t> scramble;
	scramble.resize(512);
	for (uint32_t i = 0; i < 512u; i++) {
		scramble[i] = (i * 389u + 77u) % 512u; // 389 is odd: a permutation of 0..511
	}
	for (uint32_t i = 0; i < 512u; i++) {
		const uint32_t cell = scramble[i];
		src.gaussians[i].position = Vector3(64.0f + float(cell & 7u), 64.0f + float((cell >> 3u) & 7u), 64.0f + float(cell >> 6u));
	}
	PageBuilder builder;
	String reason;
	CHECK(builder.configure(EncodeParams(), 0, reason) == OK);
	LocalVector<BuiltPage> pages;
	if (build(builder, src, 0, pages, reason) != OK || pages.size() != 1u) {
		FAIL("expected one page: ", reason.utf8().get_data());
		return;
	}
	// The expected order, computed independently: x in bit 3b, y in 3b + 1, z in 3b + 2.
	auto morton = [](uint32_t x, uint32_t y, uint32_t z) {
		uint32_t key = 0u;
		for (uint32_t b = 0; b < 3u; b++) {
			key |= ((x >> b) & 1u) << (3u * b) | ((y >> b) & 1u) << (3u * b + 1u) | ((z >> b) & 1u) << (3u * b + 2u);
		}
		return key;
	};
	DecodeContext ctx;
	Fixture out;
	if (TestGSQCodec::decode_all(pages[0].bytes, ctx, out, true, &reason) != OK || out.gaussians.size() != 512u) {
		FAIL("decode refused: ", reason.utf8().get_data());
		return;
	}
	uint32_t out_of_order = 0u;
	for (uint32_t j = 0; j < 512u; j++) {
		const Vector3 &p = out.gaussians[j].position;
		const uint32_t key = morton(uint32_t(std::lround(p.x - 64.0f)), uint32_t(std::lround(p.y - 64.0f)), uint32_t(std::lround(p.z - 64.0f)));
		// 512 distinct cells: Morton order puts cell key j in slot j.
		if (key != j) {
			out_of_order++;
		}
	}
	CHECK(out_of_order == 0u);
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] page builder: a painterly chunk is split into constant groups, never refused, and the split is counted") {
	// Painterly fixture: 3 palettes x 2 brush shapes x 2 stroke ages, per-splat surfel normals,
	// drawn at random along the importance order.
	Fixture src = make_fixture(12000, 1, 24.0f, 9);
	Rng rng(4);
	const Vector2 brushes[2] = { Vector2(1.0f, 0.25f), Vector2(0.5f, 0.5f) };
	const float ages[2] = { 0.0f, 0.5f };
	uint32_t group_size[12] = {};
	for (uint32_t i = 0; i < src.gaussians.size(); i++) {
		Gaussian &g = src.gaussians[i];
		const uint32_t palette = uint32_t(rng.next() % 3u);
		const uint32_t brush = uint32_t(rng.next() % 2u);
		const uint32_t age = uint32_t(rng.next() % 2u);
		g.painterly_meta = gaussian_pack_painterly_meta(uint16_t(palette + 1u), uint16_t(brush));
		g.brush_axes = brushes[brush];
		g.stroke_age = ages[age];
		g.normal = Vector3(rng.range(-1, 1), 1.0f, rng.range(-1, 1)).normalized();
		group_size[palette * 4u + brush * 2u + age]++;
	}
	uint32_t expected_pages = 0u;
	for (uint32_t size : group_size) {
		expected_pages += (size + kWriterPageSplats - 1u) / kWriterPageSplats;
	}

	PageBuilder builder;
	String reason;
	CHECK(builder.configure(EncodeParams(), 1, reason) == OK);
	LocalVector<BuiltPage> pages;
	if (build(builder, src, 2, pages, reason) != OK) {
		FAIL("painterly chunk refused: ", reason.utf8().get_data());
		return;
	}
	CHECK(pages.size() == expected_pages);
	CHECK(covers_once(pages, src.gaussians.size()));
	CHECK(builder.get_stats().constant_groups == 12u);
	CHECK(builder.get_stats().heterogeneous_chunks == 1u);
	DecodeContext ctx;
	uint32_t constants_broken = 0u;
	uint32_t group_order_broken = 0u;
	uint32_t page_order_broken = 0u;
	for (uint32_t p = 0; p < pages.size(); p++) {
		const BuiltPage &page = pages[p];
		if (p > 0u && page.record.first_source <= pages[p - 1u].record.first_source) {
			page_order_broken++;
		}
		// Within one constant group, a later page holds only less important splats.
		for (uint32_t q = 0; q < p; q++) {
			if (pages[q].record.constant_group == page.record.constant_group && pages[q].record.last_source >= page.record.first_source) {
				group_order_broken++;
			}
		}
		Fixture out;
		if (TestGSQCodec::decode_all(page.bytes, ctx, out, true, &reason) != OK) {
			FAIL("decode refused: ", reason.utf8().get_data());
			continue;
		}
		for (uint32_t j = 0; j < out.gaussians.size(); j++) {
			if (!same_constants(out.gaussians[j], src.gaussians[page.source_index[j]])) {
				constants_broken++;
			}
		}
	}
	CHECK(page_order_broken == 0u);
	CHECK(group_order_broken == 0u);
	CHECK(constants_broken == 0u);

	// Worst case, a stroke age per splat: still encodable, one page per splat, and counted.
	Fixture aged = make_fixture(40, 1, 4.0f, 10);
	for (uint32_t i = 0; i < aged.gaussians.size(); i++) {
		aged.gaussians[i].stroke_age = 0.01f * float(i);
	}
	LocalVector<BuiltPage> aged_pages;
	CHECK(build(builder, aged, 3, aged_pages, reason) == OK);
	CHECK(aged_pages.size() == 40u);
	CHECK(builder.get_stats().constant_groups == 12u + 40u);
	CHECK(builder.get_stats().heterogeneous_chunks == 2u);

	// What a page still cannot carry fails the chunk with its reason, and returns no page of it.
	Fixture bad = make_fixture(100, 1, 4.0f, 12);
	bad.gaussians[60].opacity = NAN;
	const uint32_t before = aged_pages.size();
	CHECK(build(builder, bad, 7, aged_pages, reason) != OK);
	CHECK(reason.contains("chunk 7"));
	CHECK(reason.contains("gsq_non_finite"));
	CHECK(aged_pages.size() == before);
	CHECK(builder.get_stats().chunks == 2u);
}

inline void flip_last_byte(LocalVector<uint8_t> &r_page) {
	r_page[r_page.size() - 1u] ^= 0x01u;
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] page builder: W12 refuses a page damaged after encoding, before it is used") {
	const Fixture src = make_fixture(5000, 2, 16.0f, 31);
	PageBuilder builder;
	String reason;
	CHECK(builder.configure(EncodeParams(), 2, reason) == OK);
	LocalVector<BuiltPage> pages;
	PageBuilder::post_encode_test_hook = flip_last_byte;
	const Error damaged = build(builder, src, 1, pages, reason);
	PageBuilder::post_encode_test_hook = nullptr;
	CHECK(damaged != OK);
	CHECK(reason.contains("w12_"));
	CHECK(pages.size() == 0u);
	CHECK(build(builder, src, 1, pages, reason) == OK);
	CHECK(pages.size() == 2u);

	// The writer's GSQ block: the D-12 values map onto the encoder; out-of-range fields are refused.
	gs_world_writer::CodecParams codec = gs_world_writer::gsq_v1_codec_params();
	EncodeParams mapped;
	CHECK(gs_world_writer::gsq_encode_params(codec, mapped, reason) == OK);
	CHECK(mapped.sh_bits[0] == 6u);
	CHECK(mapped.opacity_bits == 16u);
	CHECK(mapped.pos_step_m == 0.00025f);
	CHECK(mapped.zstd_level == 3);
	codec.opacity_bits = 4u;
	CHECK(gs_world_writer::gsq_encode_params(codec, mapped, reason) == ERR_INVALID_PARAMETER);
	CHECK(reason.contains("opacity_bits"));
	CHECK(builder.configure(EncodeParams(), 4, reason) == ERR_INVALID_PARAMETER);
}

} // namespace TestGSQPageBuilder
