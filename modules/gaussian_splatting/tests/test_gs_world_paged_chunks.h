#pragma once

// Plain v3 + flag bit 7, paged GSQ chunks (BV-C5a; docs/architecture/adr-beyond-vram-worlds.md
// D-12 "Containers", §4.1, §4.4 W11/W12/W15/W16, §6.2). CPU only; no GPU.
//
// Round trip: the writer with codec GSQ v1 writes each chunk as its run of PageBuilder pages, and
// the plain loader reads, verifies and decodes them. The loaded splats are exactly the decode of
// the builder's pages in page order (so importance order across pages survives), and every field
// stays inside its D-12 error bound against its source splat. Both source modes write the same
// bytes. A damaged page, a damaged directory entry and a truncated file are refused visibly.
//
// Old readers: a reader built before bit 7 refuses the file (its select_header() is frozen below
// exactly as it was on master before BV-C5a), a reader that ignores bit 7 cannot read the pages
// as raw splats, and this reader refuses the next unknown flag and an unknown codec id the same
// way. Plain worlds without bit 7 stay byte-identical: the [WorldWriter] identity matrix in
// test_gs_world_writer.h compares codec "none" output with ResourceSaver::save byte for byte.

#include "test_macros.h"

#include "test_gs_world_writer.h"
#include "test_gsq_codec.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_format.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gs_world_writer.h"
#include "../io/gsq/gsq_page_builder.h"

#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"

#include <cmath>
#include <cstring>

namespace TestGSWorldPagedChunks {

using gs_world_writer::ChunkDesc;
using gs_world_writer::CodecParams;
using gs_world_writer::Limits;
using gs_world_writer::PlainSpec;
using gs_world_writer::SourceMode;
using gs_world_writer::Writer;
using TestGSQCodec::Fixture;
using TestGSWorldWriter::ww_dir;
using TestGSWorldWriter::ww_first_diff;
using TestGSWorldWriter::ww_list;
using TestGSWorldWriter::ww_read;
using TestGSWorldWriter::ww_remove_dir;
using TestGSWorldWriter::ww_write_raw;

// The header check of every plain reader before BV-C5a, verbatim from
// io/gaussian_splat_world_format.h at ad6edfc15d1 (bits 0-6 known, everything else refused).
inline Error pre_c5a_select_header(uint32_t p_version, uint32_t p_flags) {
	constexpr uint32_t plain_flags = (1u << 6u) - 1u;
	constexpr uint32_t has_hlod_flag = 1u << 6u;
	if (p_version != 1u && p_version != 2u && p_version != 3u) {
		return ERR_FILE_CORRUPT;
	}
	const bool has_hlod = (p_flags & has_hlod_flag) != 0u;
	if ((p_flags & ~(plain_flags | has_hlod_flag)) != 0u) {
		return has_hlod ? ERR_FILE_CORRUPT : ERR_FILE_UNRECOGNIZED;
	}
	return OK;
}

struct PCWorld {
	Fixture src; // chunk after chunk, each in its importance order
	LocalVector<uint32_t> counts;
	LocalVector<ChunkDesc> descs;
	PlainSpec spec;
};

inline AABB pc_bounds(const Gaussian *p_g, uint32_t p_count) {
	AABB box(p_g[0].position, Vector3());
	for (uint32_t i = 1; i < p_count; i++) {
		box.expand_to(p_g[i].position);
	}
	return box;
}

// Three chunks: 9000 splats (pages of 4096 + 4096 + 808), an empty chunk, 5000 (4096 + 904).
inline PCWorld pc_make(uint32_t p_degree) {
	PCWorld w;
	w.counts.push_back(9000u);
	w.counts.push_back(0u);
	w.counts.push_back(5000u);
	w.src = TestGSQCodec::make_fixture(14000u, p_degree, 16.0f, 70u + p_degree);
	uint32_t base = 0;
	for (uint32_t n : w.counts) {
		ChunkDesc d;
		if (n > 0u) {
			d.bounds = pc_bounds(w.src.gaussians.ptr() + base, n);
			d.center = d.bounds.get_center();
			d.radius = float(d.bounds.size.length() * 0.5f);
		}
		w.descs.push_back(d);
		base += n;
	}
	w.spec.splat_count = 14000u;
	w.spec.sh_degree = p_degree;
	w.spec.sh_first_order = p_degree > 0u ? 3u : 0u;
	w.spec.sh_high_order = gs_gsq::sh_high_order_for_degree(p_degree);
	w.spec.bounds = pc_bounds(w.src.gaussians.ptr(), 14000u);
	w.spec.chunk_count = w.counts.size();
	w.spec.write_chunk_table = true;
	Dictionary metadata;
	metadata["name"] = "paged";
	w.spec.metadata = metadata;
	return w;
}

inline Error pc_write(const PCWorld &p_w, SourceMode p_mode, const String &p_path, uint32_t *r_pages = nullptr, String *r_message = nullptr) {
	Writer writer;
	Error err = writer.begin_plain(p_path, p_w.spec, gs_world_writer::gsq_v1_codec_params(), p_mode, Limits::unbounded());
	if (err == OK && writer.get_pass_count() != 1u) {
		err = ERR_BUG; // the SH travels inside the pages: one pass in every mode
	}
	uint32_t base = 0;
	for (uint32_t c = 0; err == OK && c < p_w.counts.size(); c++) {
		const uint32_t n = p_w.counts[c];
		const uint32_t high = p_w.spec.sh_high_order;
		err = writer.append_chunk(p_w.descs[c], p_w.src.gaussians.ptr() + base, high > 0u ? p_w.src.sh_high.ptr() + uint64_t(base) * high : nullptr, n);
		base += n;
	}
	if (err == OK) {
		err = writer.finalize();
	}
	if (r_pages) {
		*r_pages = writer.get_page_count();
	}
	if (r_message) {
		*r_message = writer.get_error_message();
	}
	return err;
}

inline uint32_t pc_u32(const PackedByteArray &p_bytes, int64_t p_offset) {
	uint32_t v = 0;
	if (p_offset + 4 <= p_bytes.size()) {
		memcpy(&v, p_bytes.ptr() + p_offset, sizeof(v));
	}
	return v;
}

inline void pc_put_u32(PackedByteArray &r_bytes, int64_t p_offset, uint32_t p_value) {
	memcpy(r_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
}

// Loads p_path resident; returns the error and the world (null on refusal).
inline Ref<GaussianSplatWorld> pc_load(const String &p_path, Error &r_err) {
	ResourceFormatLoaderGaussianSplatWorld loader;
	r_err = ERR_BUG;
	return loader.load_resident(p_path, &r_err);
}

} // namespace TestGSWorldPagedChunks

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] paged GSQ chunks (bit 7): writer -> reader round trip in page order, within the D-12 bounds; W16 codec block") {
	using namespace TestGSWorldPagedChunks;
	const String dir = ww_dir("paged_rt");
	for (uint32_t degree : { 0u, 3u }) {
		INFO("SH degree ", degree);
		const PCWorld w = pc_make(degree);
		const String path = dir.path_join("world.gsplatworld");
		uint32_t pages = 0;
		String message;
		const Error write_err = pc_write(w, SourceMode::SINGLE_PASS, path, &pages, &message);
		if (write_err != OK) {
			FAIL("paged write refused: ", message.utf8().get_data());
			continue;
		}
		CHECK(pages == 5u);
		const PackedByteArray bytes = ww_read(path);

		// W7: REITERABLE writes the same bytes (the SH is inside the pages, one pass).
		const String path_r = dir.path_join("world_r.gsplatworld");
		CHECK(pc_write(w, SourceMode::REITERABLE, path_r) == OK);
		CHECK(ww_first_diff(ww_read(path_r), bytes) == -1);
		DirAccess::remove_absolute(path_r);

		// Header: plain v3, bit 7 with its chunk table, no SH section and no index lists.
		const uint32_t flags = pc_u32(bytes, 8);
		CHECK(pc_u32(bytes, 4) == gs_world_format::kVersion);
		CHECK((flags & gs_world_format::kFlagPagedChunks) != 0u);
		CHECK((flags & gs_world_format::kFlagHasChunks) != 0u);
		CHECK(((flags & gs_world_format::kFlagHasHighSh) != 0u) == (degree >= 2u));
		CHECK(TestGSWorldWriter::ww_u64_at(bytes, 56) == gs_world_format::kPagedPageAlignment);
		CHECK(TestGSWorldWriter::ww_u64_at(bytes, 64) == 0u);
		CHECK(TestGSWorldWriter::ww_u64_at(bytes, 80) == 0u);
		// W16: the codec block in the paged extension is the one the writer was given.
		const gs_world_writer::CodecParams codec = gs_world_writer::gsq_v1_codec_params();
		const int64_t ext = int64_t(gs_world_format::kPrefixBytes);
		CHECK(pc_u32(bytes, ext + 0) == codec.codec_id);
		CHECK(bytes[ext + 4] == codec.sh_bits[0]);
		CHECK(bytes[ext + 5] == codec.sh_bits[1]);
		CHECK(bytes[ext + 6] == codec.sh_bits[2]);
		CHECK(bytes[ext + 7] == codec.opacity_bits);
		float step = 0.0f;
		memcpy(&step, bytes.ptr() + ext + 8, sizeof(step));
		CHECK(step == codec.pos_step_m);
		CHECK(int32_t(pc_u32(bytes, ext + 12)) == codec.zstd_level);
		CHECK(pc_u32(bytes, ext + 16) == 5u);

		// The expected payload: the builder's pages of each chunk, decoded, in page order.
		gs_gsq::PageBuilder builder;
		String reason;
		if (builder.configure(gs_gsq::EncodeParams(), degree, reason) != OK) {
			FAIL("configure refused");
			return;
		}
		const uint32_t high = w.spec.sh_high_order;
		Fixture expected;
		Fixture source; // the source splat behind each expected slot
		expected.degree = degree;
		source.degree = degree;
		gs_gsq::DecodeContext ctx;
		uint32_t base = 0;
		uint32_t page_index = 0;
		for (uint32_t c = 0; c < w.counts.size(); c++) {
			LocalVector<gs_gsq::BuiltPage> built;
			const Vector3 *sh = high > 0u ? w.src.sh_high.ptr() + uint64_t(base) * high : nullptr;
			if (builder.build_chunk(c, w.src.gaussians.ptr() + base, sh, w.counts[c], built, reason) != OK) {
				FAIL("build refused");
				return;
			}
			for (const gs_gsq::BuiltPage &page : built) {
				Fixture out;
				if (TestGSQCodec::decode_all(page.bytes, ctx, out, true, &reason) != OK) {
					FAIL("decode refused");
					return;
				}
				// The page table carries this page's offset, size and both digests (W15).
				const int64_t entry = int64_t(TestGSWorldWriter::ww_u64_at(bytes, ext + 24)) + int64_t(page_index) * 112;
				const uint64_t offset = TestGSWorldWriter::ww_u64_at(bytes, entry);
				CHECK(offset % gs_world_format::kPagedPageAlignment == 0u);
				CHECK(pc_u32(bytes, entry + 8) == page.bytes.size());
				CHECK(memcmp(bytes.ptr() + offset, page.bytes.ptr(), page.bytes.size()) == 0);
				CHECK(memcmp(bytes.ptr() + entry + 16, page.record.stored_digest, gs_gsq::kDigestBytes) == 0);
				CHECK(memcmp(bytes.ptr() + entry + 48, page.record.decoded_digest, gs_gsq::kDigestBytes) == 0);
				CHECK(pc_u32(bytes, entry + 104) == c);
				for (uint32_t j = 0; j < out.gaussians.size(); j++) {
					expected.gaussians.push_back(out.gaussians[j]);
					const uint32_t s = base + page.source_index[j];
					source.gaussians.push_back(w.src.gaussians[s]);
					for (uint32_t k = 0; k < high; k++) {
						expected.sh_high.push_back(out.sh_high[j * high + k]);
						source.sh_high.push_back(w.src.sh_high[s * high + k]);
					}
				}
				page_index++;
			}
			base += w.counts[c];
		}
		if (expected.gaussians.size() != 14000u) {
			FAIL("expected payload incomplete");
			return;
		}

		Error err = OK;
		Ref<GaussianSplatWorld> world = pc_load(path, err);
		CHECK(err == OK);
		if (world.is_null() || world->get_gaussian_data().is_null()) {
			FAIL("paged world did not load");
			continue;
		}
		CHECK(world->get_splat_count() == 14000u);
		CHECK(world->get_sh_degree() == degree);
		CHECK(String(world->get_metadata().get("name", Variant())) == "paged");
		// Chunks: the descriptors as written; each chunk is the contiguous run of its pages.
		const Vector<StaticChunk> &chunks = world->get_static_chunks();
		if (chunks.size() != 3) {
			FAIL("expected 3 chunks, got ", chunks.size());
			return;
		}
		uint32_t first = 0;
		for (int c = 0; c < 3; c++) {
			CHECK(chunks[c].bounds.is_equal_approx(w.descs[c].bounds));
			CHECK(chunks[c].radius == w.descs[c].radius);
			CHECK(uint32_t(chunks[c].indices.size()) == w.counts[c]);
			if (!chunks[c].indices.is_empty()) {
				CHECK(chunks[c].indices[0] == first);
				CHECK(chunks[c].indices[chunks[c].indices.size() - 1] == first + w.counts[c] - 1u);
			}
			first += w.counts[c];
		}

		Fixture loaded;
		loaded.degree = degree;
		const Ref<GaussianData> data = world->get_gaussian_data();
		const LocalVector<Gaussian> &storage = data->get_gaussian_storage();
		if (storage.size() != 14000u) {
			FAIL("loaded payload has ", storage.size(), " splats");
			return;
		}
		loaded.gaussians = storage;
		if (high > 0u) {
			const Vector3 *coeffs = data->get_sh_high_order_coefficients_ptr();
			if (coeffs == nullptr) {
				FAIL("loaded payload has no high-order SH");
				return;
			}
			loaded.sh_high.resize(14000u * high);
			memcpy(loaded.sh_high.ptr(), coeffs, sizeof(Vector3) * 14000u * high);
		}
		// The reader returns exactly the builder's decoded pages, in page order.
		uint8_t want[gs_gsq::kDigestBytes];
		uint8_t got[gs_gsq::kDigestBytes];
		gs_gsq::decoded_digest(expected.gaussians.ptr(), expected.sh_high.ptr(), 14000u, degree, want);
		gs_gsq::decoded_digest(loaded.gaussians.ptr(), loaded.sh_high.ptr(), 14000u, degree, got);
		CHECK(TestGSQCodec::digest_equal(want, got));

		// Field by field against the source splat, within the D-12 bounds (16 m pages: 16-bit positions).
		const TestGSQCodec::Errors e = TestGSQCodec::measure(source, loaded);
		const float x_bound = 16.0f / 65535.0f * 0.51f + 1.6e-5f;
		CHECK(e.position[0] <= x_bound);
		CHECK(e.position[1] <= 10.0f / 65535.0f * 0.51f + 1e-6f);
		CHECK(e.position[2] <= x_bound);
		CHECK(e.log_scale[0] <= (std::log2(0.5f) - std::log2(0.01f)) / 1023.0f * 0.52f);
		CHECK(e.log_scale[2] <= (std::log2(0.02f) - std::log2(0.001f)) / 1023.0f * 0.52f);
		CHECK(e.opacity <= 0.51f / 65535.0f);
		CHECK(e.dc[0] <= 3.0f / 2047.0f * 0.51f);
		CHECK(e.dc[2] <= 3.0f / 1023.0f * 0.51f);
		if (degree == 3u) {
			CHECK(e.sh[0] <= 0.4f / 31.0f * 0.51f);
			CHECK(e.sh[1] <= 0.1f / 15.0f * 0.51f);
			CHECK(e.sh[2] <= 0.1f / 15.0f * 0.51f);
		}
		CHECK(e.rotation_cos >= 0.999997f);
		CHECK(e.zero_sh_broken == 0u);
		CHECK(e.constants_broken == 0u);
		world.unref();

		// Damage is refused, never repaired or skipped.
		const uint64_t page1 = TestGSWorldWriter::ww_u64_at(bytes, int64_t(TestGSWorldWriter::ww_u64_at(bytes, ext + 24)) + 112);
		struct Damage {
			const char *name;
			int64_t offset;
		};
		const Damage damages[] = {
			{ "a byte inside page 1", int64_t(page1) + 300 },
			{ "page 1's decoded digest in the page table", int64_t(TestGSWorldWriter::ww_u64_at(bytes, ext + 24)) + 112 + 48 },
			{ "chunk 2's splat count", int64_t(TestGSWorldWriter::ww_u64_at(bytes, 72)) + 2 * 56 + 48 },
		};
		for (const Damage &d : damages) {
			INFO(d.name);
			PackedByteArray bad = bytes;
			bad.ptrw()[d.offset] ^= 0x01u;
			REQUIRE(ww_write_raw(path, bad));
			Ref<GaussianSplatWorld> refused = pc_load(path, err);
			CHECK(refused.is_null());
			CHECK(err == ERR_FILE_CORRUPT);
		}
		PackedByteArray truncated = bytes;
		truncated.resize(bytes.size() - 1);
		REQUIRE(ww_write_raw(path, truncated));
		CHECK(pc_load(path, err).is_null());
		CHECK(err == ERR_FILE_CORRUPT);
		DirAccess::remove_absolute(path);
	}
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] paged GSQ chunks (bit 7): an old reader refuses the file; unknown flags and codecs stay refused") {
	using namespace TestGSWorldPagedChunks;
	const String dir = ww_dir("paged_old");
	const String path = dir.path_join("world.gsplatworld");
	const PCWorld w = pc_make(1u);
	REQUIRE(pc_write(w, SourceMode::REITERABLE, path) == OK);
	const PackedByteArray bytes = ww_read(path);
	const uint32_t version = pc_u32(bytes, 4);
	const uint32_t flags = pc_u32(bytes, 8);
	REQUIRE((flags & gs_world_format::kFlagPagedChunks) != 0u);

	// A reader built before BV-C5a refuses it at the header, visibly (ERR_FILE_UNRECOGNIZED).
	CHECK(pre_c5a_select_header(version, flags) == ERR_FILE_UNRECOGNIZED);
	// ... while this build accepts it and loads it (so the refusal above is about bit 7).
	gs_world_format::HeaderLayout layout;
	CHECK(gs_world_format::select_header(version, flags, uint64_t(bytes.size()), layout) == OK);
	CHECK(layout.has_paged_chunks);
	CHECK(layout.header_bytes == gs_world_format::kPrefixBytes + gs_world_format::kPagedExtensionBytes);
	Error err = OK;
	CHECK(pc_load(path, err).is_valid());
	CHECK(err == OK);

	struct Patch {
		const char *name;
		int64_t offset;
		uint32_t value;
		Error expected;
	};
	const Patch patches[] = {
		// The next unknown flag is refused exactly as bit 7 was before this package.
		{ "unknown flag bit 8", 8, flags | (1u << 8u), ERR_FILE_UNRECOGNIZED },
		// An unknown codec id in the paged extension is refused, not guessed.
		{ "codec id 2", int64_t(gs_world_format::kPrefixBytes), 2u, ERR_FILE_UNRECOGNIZED },
		// A reader that ignored bit 7 would see no splat section of the declared size: refused.
		{ "bit 7 cleared", 8, flags & ~gs_world_format::kFlagPagedChunks, ERR_FILE_CORRUPT },
		// Bit 7 never combines with the whole-section gzip or HLOD, and needs its chunk table.
		{ "bit 7 with bit 4", 8, flags | gs_world_format::kFlagCompressed, ERR_FILE_CORRUPT },
		{ "bit 7 with HLOD", 8, flags | gs_world_format::kFlagHasHlod, ERR_FILE_CORRUPT },
		{ "bit 7 without chunks", 8, flags & ~gs_world_format::kFlagHasChunks, ERR_FILE_CORRUPT },
		{ "codec block: opacity bits 4", int64_t(gs_world_format::kPrefixBytes) + 4, 0x04050506u, ERR_FILE_CORRUPT },
	};
	for (const Patch &p : patches) {
		INFO(p.name);
		PackedByteArray patched = bytes;
		pc_put_u32(patched, p.offset, p.value);
		REQUIRE(ww_write_raw(path, patched));
		Ref<GaussianSplatWorld> refused = pc_load(path, err);
		CHECK(refused.is_null());
		CHECK(err == p.expected);
	}
	DirAccess::remove_absolute(path);
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] paged GSQ chunks (bit 7): what a page cannot carry fails the write visibly and publishes nothing") {
	using namespace TestGSWorldPagedChunks;
	const String dir = ww_dir("paged_refuse");
	const String path = dir.path_join("world.gsplatworld");
	PCWorld w = pc_make(0u);
	w.src.gaussians[9500].position.x = NAN;
	String message;
	CHECK(pc_write(w, SourceMode::SINGLE_PASS, path, nullptr, &message) != OK);
	INFO(message);
	CHECK(message.contains("chunk 2"));
	CHECK(message.contains("gsq_"));
	CHECK(ww_list(dir).size() == 0);

	// The chunk table is the page directory: GSQ without it is refused at begin_plain (W11).
	PCWorld no_table = pc_make(0u);
	no_table.spec.write_chunk_table = false;
	Writer writer;
	CHECK(writer.begin_plain(path, no_table.spec, gs_world_writer::gsq_v1_codec_params(), SourceMode::SINGLE_PASS, Limits::unbounded()) == ERR_INVALID_PARAMETER);
	CHECK(writer.get_error_message().contains("write_chunk_table"));
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] paged GSQ chunks (bit 7): output and RAM caps hold while writing (BV-03b), publishing nothing") {
	using namespace TestGSWorldPagedChunks;
	const String dir = ww_dir("paged_limits");
	const String path = dir.path_join("world.gsplatworld");
	const PCWorld w = pc_make(1u);
	const uint32_t high = w.spec.sh_high_order;

	// GSQ output is unknown at begin; preflight sizes the disk need by the cap, and the cap
	// is enforced per slice as the pages land.
	Limits out = Limits::unbounded();
	out.max_output_bytes = 16384u;
	Writer ow;
	REQUIRE(ow.begin_plain(path, w.spec, gs_world_writer::gsq_v1_codec_params(), SourceMode::SINGLE_PASS, out) == OK);
	CHECK(ow.get_report().output_estimate_bytes == 16384u);
	CHECK(ow.append_chunk(w.descs[0], w.src.gaussians.ptr(), w.src.sh_high.ptr(), w.counts[0]) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(ow.get_report().reason == "limit_output");
	CHECK(ow.get_report().output_bytes <= 16384u);
	CHECK(ww_list(dir).size() == 0);

	// RAM: exactly the preflight estimate passes begin; holding chunk 0's pages exceeds it.
	Writer probe;
	REQUIRE(probe.begin_plain(path, w.spec, gs_world_writer::gsq_v1_codec_params(), SourceMode::SINGLE_PASS, Limits::unbounded()) == OK);
	const uint64_t ram_estimate = probe.get_report().ram_estimate_bytes;
	probe.abort();
	CHECK(probe.get_report().reason == "aborted");
	Limits ram = Limits::unbounded();
	ram.max_ram_bytes = ram_estimate;
	Writer rw;
	REQUIRE(rw.begin_plain(path, w.spec, gs_world_writer::gsq_v1_codec_params(), SourceMode::SINGLE_PASS, ram) == OK);
	CHECK(rw.append_chunk(w.descs[0], w.src.gaussians.ptr(), high > 0u ? w.src.sh_high.ptr() : nullptr, w.counts[0]) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(rw.get_report().reason == "limit_ram");
	CHECK(rw.get_report().ram_peak_bytes > ram_estimate);
	CHECK(ww_list(dir).size() == 0);

	// Unbounded, the same world commits and the Report counts its pages.
	uint32_t pages = 0;
	Writer cw;
	Error err = cw.begin_plain(path, w.spec, gs_world_writer::gsq_v1_codec_params(), SourceMode::SINGLE_PASS, Limits::unbounded());
	uint32_t base = 0;
	for (uint32_t c = 0; err == OK && c < w.counts.size(); c++) {
		err = cw.append_chunk(w.descs[c], w.src.gaussians.ptr() + base, high > 0u ? w.src.sh_high.ptr() + uint64_t(base) * high : nullptr, w.counts[c]);
		base += w.counts[c];
	}
	if (err == OK) {
		err = cw.finalize();
	}
	pages = cw.get_page_count();
	CHECK(err == OK);
	CHECK(cw.get_report().committed);
	CHECK(cw.get_report().page_count == pages);
	CHECK(pages == 5u); // 4096 + 4096 + 808, then 4096 + 904
	CHECK(cw.get_report().output_bytes == uint64_t(ww_read(path).size()));
	CHECK(cw.get_report().splats_appended == 14000u);
	ww_remove_dir(dir);
}
