#pragma once

// BV-C2 (docs/architecture/adr-beyond-vram-worlds.md, section 10 row BV-C2, sections 5.3 and
// 5.4, risks K3 and K9): GSQ v1 page codec ratio and decode/pack throughput probe. CPU only.
//
// It measures, per data set, the stored bytes per splat after zstd (page headers included),
// the fixed-width bytes per splat the same pages would take with no zstd gain, and the
// single-thread (one core, one DecodeContext) throughput of decode (with and without the page
// SHA-256 check) and of the pack into today's PackedGaussian. Real scans and generator subsets
// are reported separately (ADR section 5.3, synthetic-content caveat).
//
// It measures and reports; it gates nothing (thresholds are BV-09). Absence is never a
// number: a scan that is not there is MISSING with no fields, and a page the codec refuses
// makes its data set INVALID with the gsq_* reason.
//
// Modes:
// - default (every module test run): a 8,192-splat T-smoke prefix, one repetition. Proves the
//   probe runs end to end and that absence is reported as MISSING.
// - full: GS_GSQ_PROBE_OUT=<file.json> selects T0 whole, a Tier A stride sample and a
//   hash-noise SH control, three repetitions, and writes the JSON report there.
//   GS_GSQ_PROBE_SCANS=<a.ply>;<b.ply> adds real scans. The lane entry point is
//   tests/runtime/flythrough/run_codec_probe.py; timings are only meaningful in the optimized
//   producer build of beyond_vram_flythrough.yml.

#include "test_macros.h"

#include "../io/gs_world_generator.h"
#include "../io/gsq/gsq_codec.h"
#include "../io/ply_loader.h"
#include "../renderer/gaussian_gpu_layout.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace TestGSQCodecProbe {

using namespace gs_gsq;
namespace gen = gs_world_generator;

// Plain v3 uncompressed SH3 bytes per splat (ADR section 2, [D]).
constexpr double kPlainV3Sh3BytesPerSplat = 292.0;
constexpr uint32_t kSmokeSplats = 8192u;
constexpr uint32_t kFullReps = 3u;

struct Scratch {
	LocalVector<uint8_t> page;
	LocalVector<Gaussian> decoded;
	LocalVector<Vector3> decoded_sh;
	Vector<PackedGaussian> packed;
};

struct DataSet {
	String name;
	String kind; // synthetic | synthetic_control | real_scan
	String source;
	String status = "OK"; // OK | MISSING | INVALID
	String reason;
	uint32_t reps = 1;
	uint32_t sh_degree = 0;
	uint64_t pages = 0;
	uint64_t splats = 0;
	uint64_t stored_bytes = 0;
	uint64_t fixed_bytes = 0; // fixed-width planes, no header
	uint64_t pages_21bit = 0;
	uint64_t pages_normals = 0;
	uint64_t encode_usec = 0;
	LocalVector<uint64_t> decode_usec; // per repetition, page SHA-256 checked
	LocalVector<uint64_t> decode_nv_usec; // per repetition, no page SHA-256
	LocalVector<uint64_t> pack_usec; // per repetition

	void begin(uint32_t p_reps) {
		reps = p_reps;
		decode_usec.resize(p_reps);
		decode_nv_usec.resize(p_reps);
		pack_usec.resize(p_reps);
		for (uint32_t r = 0; r < p_reps; r++) {
			decode_usec[r] = decode_nv_usec[r] = pack_usec[r] = 0;
		}
	}
	void fail(const String &p_status, const String &p_reason) {
		if (status == "OK") {
			status = p_status;
			reason = p_reason;
		}
	}
};

inline uint64_t now_usec() {
	return OS::get_singleton()->get_ticks_usec();
}

// Bytes per splat of the sub-block planes (gsq_codec.h layout), without zstd.
inline uint32_t fixed_width_bytes(const PageInfo &p_info) {
	const uint32_t position = p_info.position_bits == 21u ? 9u : 6u;
	const uint32_t sh = p_info.sh_degree == 0u ? 0u : 3u * ((p_info.sh_degree + 1u) * (p_info.sh_degree + 1u) - 1u);
	return position + 4u /* scale */ + 4u /* rotation */ + 2u /* opacity */ + 4u /* DC */ + sh +
			(p_info.has_normals ? 6u : 0u);
}

// Encodes one page, then decodes and packs it p_set.reps times on this thread.
inline bool probe_page(const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count, uint32_t p_degree,
		DataSet &r_set, DecodeContext &r_ctx, Scratch &r_scratch) {
	const EncodeParams params;
	String reason;
	const uint64_t e0 = now_usec();
	if (encode_page(p_gaussians, p_sh_high, p_count, p_degree, params, r_scratch.page, reason) != OK) {
		r_set.fail("INVALID", "encode: " + reason);
		return false;
	}
	r_set.encode_usec += now_usec() - e0;

	PageInfo info;
	if (parse_page_header(r_scratch.page.ptr(), r_scratch.page.size(), info, reason) != OK) {
		r_set.fail("INVALID", "header: " + reason);
		return false;
	}
	r_set.pages++;
	r_set.splats += p_count;
	r_set.stored_bytes += r_scratch.page.size();
	r_set.fixed_bytes += uint64_t(fixed_width_bytes(info)) * p_count;
	r_set.pages_21bit += info.position_bits == 21u ? 1u : 0u;
	r_set.pages_normals += info.has_normals ? 1u : 0u;

	const uint32_t high_order = sh_high_order_for_degree(p_degree);
	r_scratch.decoded.resize(p_count);
	r_scratch.decoded_sh.resize(p_count * high_order);
	Vector3 *sh_out = high_order ? r_scratch.decoded_sh.ptr() : nullptr;
	const uint32_t first_order = p_degree >= 1u ? 3u : 0u;
	for (uint32_t r = 0; r < r_set.reps; r++) {
		for (int verify = 1; verify >= 0; verify--) {
			const uint64_t d0 = now_usec();
			if (r_ctx.decode_range(r_scratch.page.ptr(), r_scratch.page.size(), 0, p_count, r_scratch.decoded.ptr(), sh_out,
						reason, verify == 1) != OK) {
				r_set.fail("INVALID", "decode: " + reason);
				return false;
			}
			(verify ? r_set.decode_usec : r_set.decode_nv_usec)[r] += now_usec() - d0;
		}
		SHCompressionMetrics metrics;
		const uint64_t p0 = now_usec();
		if (!pack_gaussians_range(r_scratch.decoded, 0, p_count, r_scratch.packed, metrics, sh_out, first_order, high_order) ||
				uint32_t(r_scratch.packed.size()) != p_count) {
			r_set.fail("INVALID", "pack_gaussians_range failed");
			return false;
		}
		r_set.pack_usec[r] += now_usec() - p0;
	}
	return true;
}

// Generator chunks [0, chunks) with the given stride; p_splat_cap > 0 keeps only that prefix
// of the first chunk (smoke mode).
inline void probe_generator(DataSet &r_set, const gen::Params &p_params, uint32_t p_stride, uint32_t p_splat_cap,
		uint32_t p_reps) {
	r_set.begin(p_reps);
	gen::Plan plan;
	String err;
	if (gen::build_plan(p_params, plan, err) != OK) {
		r_set.fail("INVALID", "build_plan: " + err);
		return;
	}
	r_set.sh_degree = uint32_t(plan.params.sh_degree);
	DecodeContext ctx;
	Scratch scratch;
	gen::ChunkBuffers buf;
	for (uint32_t c = 0; c < plan.chunks.size(); c += p_stride) {
		gen::generate_chunk(plan, c, buf);
		uint32_t count = buf.gaussians.size();
		if (p_splat_cap > 0u && count > p_splat_cap) {
			count = p_splat_cap;
		}
		if (count == 0u) {
			continue;
		}
		if (!probe_page(buf.gaussians.ptr(), buf.sh_high.is_empty() ? nullptr : buf.sh_high.ptr(), count,
					r_set.sh_degree, r_set, ctx, scratch)) {
			return;
		}
		if (p_splat_cap > 0u) {
			break;
		}
	}
	if (r_set.splats == 0u) {
		r_set.fail("INVALID", "no splats probed");
	}
}

inline uint64_t morton_spread(uint64_t p_v) {
	p_v &= 0x1fffffull;
	p_v = (p_v | p_v << 32u) & 0x1f00000000ffffull;
	p_v = (p_v | p_v << 16u) & 0x1f0000ff0000ffull;
	p_v = (p_v | p_v << 8u) & 0x100f00f00f00f00full;
	p_v = (p_v | p_v << 4u) & 0x10c30c30c30c30c3ull;
	p_v = (p_v | p_v << 2u) & 0x1249249249249249ull;
	return p_v;
}

// A real scan: loaded with PLYLoader, Morton-ordered, then split into full pages, which is how
// a spatial bake groups splats (one page per chunk of at most kMaxPageSplats).
inline void probe_scan(DataSet &r_set, const String &p_path, uint32_t p_reps) {
	r_set.begin(p_reps);
	r_set.source = p_path;
	if (p_path.is_empty() || !FileAccess::exists(p_path)) {
		r_set.fail("MISSING", "scan file not found on this machine: " + p_path);
		return;
	}
	Ref<PLYLoader> loader;
	loader.instantiate();
	const Error load_err = loader->load_file(p_path);
	Ref<GaussianData> data = loader->get_gaussian_data();
	if (load_err != OK || data.is_null() || data->get_count() <= 0) {
		r_set.fail("INVALID", vformat("PLYLoader::load_file failed (error %d)", int(load_err)));
		return;
	}
	const uint32_t count = uint32_t(data->get_count());
	const uint32_t degree = data->get_sh_degree();
	const uint32_t high_order = sh_high_order_for_degree(degree);
	r_set.sh_degree = degree;
	if (degree > kMaxShDegree || data->get_sh_high_order_count() != high_order ||
			(high_order > 0u && data->get_sh_high_order_coefficients_ptr() == nullptr)) {
		r_set.fail("INVALID", vformat("SH layout: degree %d with %d high-order vectors per splat", degree,
									  data->get_sh_high_order_count()));
		return;
	}
	const Gaussian *src = data->get_gaussians();
	const Vector3 *src_sh = data->get_sh_high_order_coefficients_ptr();
	const AABB box = data->get_aabb();
	std::vector<std::pair<uint64_t, uint32_t>> order(count);
	for (uint32_t i = 0; i < count; i++) {
		uint64_t key = 0;
		for (int axis = 0; axis < 3; axis++) {
			const float extent = box.size[axis] > 0.0f ? box.size[axis] : 1.0f;
			const float t = CLAMP((src[i].position[axis] - box.position[axis]) / extent, 0.0f, 1.0f);
			key |= morton_spread(uint64_t(t * 2097151.0f)) << axis;
		}
		order[i] = { key, i };
	}
	std::sort(order.begin(), order.end());

	DecodeContext ctx;
	Scratch scratch;
	LocalVector<Gaussian> page;
	LocalVector<Vector3> page_sh;
	for (uint32_t first = 0; first < count; first += kMaxPageSplats) {
		const uint32_t n = MIN(kMaxPageSplats, count - first);
		page.resize(n);
		page_sh.resize(n * high_order);
		for (uint32_t i = 0; i < n; i++) {
			const uint32_t s = order[first + i].second;
			page[i] = src[s];
			if (high_order) {
				memcpy(page_sh.ptr() + uint64_t(i) * high_order, src_sh + uint64_t(s) * high_order, sizeof(Vector3) * high_order);
			}
		}
		if (!probe_page(page.ptr(), high_order ? page_sh.ptr() : nullptr, n, degree, r_set, ctx, scratch)) {
			return;
		}
	}
}

inline double median_usec(const LocalVector<uint64_t> &p_values) {
	std::vector<uint64_t> v(p_values.ptr(), p_values.ptr() + p_values.size());
	std::sort(v.begin(), v.end());
	return v.empty() ? 0.0 : double(v[v.size() / 2u]);
}

inline double rate(double p_amount, double p_usec) {
	return p_usec > 0.0 ? p_amount / p_usec : 0.0; // amount per microsecond = millions per second
}

// Only an OK data set carries numbers; MISSING and INVALID carry status and reason only.
inline Dictionary to_dict(const DataSet &p_set) {
	Dictionary d;
	d["name"] = p_set.name;
	d["kind"] = p_set.kind;
	d["source"] = p_set.source;
	d["status"] = p_set.status;
	if (p_set.status != "OK") {
		d["reason"] = p_set.reason;
		return d;
	}
	const double splats = double(p_set.splats);
	const double stored = double(p_set.stored_bytes);
	const double fixed_with_headers = double(p_set.fixed_bytes) + double(p_set.pages) * kPageHeaderBytes;
	const double bps = stored / splats;
	d["sh_degree"] = p_set.sh_degree;
	d["pages"] = p_set.pages;
	d["splats"] = p_set.splats;
	d["pages_21bit_positions"] = p_set.pages_21bit;
	d["pages_per_splat_normals"] = p_set.pages_normals;
	d["stored_bytes"] = p_set.stored_bytes;
	d["bytes_per_splat"] = bps;
	d["fixed_width_bytes_per_splat"] = double(p_set.fixed_bytes) / splats;
	d["zstd_ratio_vs_fixed_width"] = fixed_with_headers / stored;
	if (p_set.sh_degree == 3u) {
		d["ratio_vs_plain_v3_sh3"] = kPlainV3Sh3BytesPerSplat / bps;
	}
	d["reps"] = p_set.reps;
	d["encode_msplats_per_s_one_core"] = rate(splats, double(p_set.encode_usec));
	const double dec = median_usec(p_set.decode_usec);
	const double dec_nv = median_usec(p_set.decode_nv_usec);
	const double pack = median_usec(p_set.pack_usec);
	d["decode_verified_stored_mb_per_s_one_core"] = rate(stored, dec);
	d["decode_verified_msplats_per_s_one_core"] = rate(splats, dec);
	d["decode_unverified_stored_mb_per_s_one_core"] = rate(stored, dec_nv);
	d["decode_unverified_msplats_per_s_one_core"] = rate(splats, dec_nv);
	d["pack_msplats_per_s_one_core"] = rate(splats, pack);
	d["pack_output_mb_per_s_one_core"] = rate(splats * double(sizeof(PackedGaussian)), pack);
	d["decode_verified_plus_pack_msplats_per_s_one_core"] = rate(splats, dec + pack);
	return d;
}

// Splat-weighted aggregate of the OK data sets of one kind; MISSING when there is none.
inline Dictionary aggregate(const LocalVector<DataSet> &p_sets, const String &p_kind) {
	Dictionary d;
	uint64_t splats = 0, stored = 0, fixed = 0, pages = 0;
	Array names;
	for (const DataSet &s : p_sets) {
		if (s.kind != p_kind || s.status != "OK") {
			continue;
		}
		splats += s.splats;
		stored += s.stored_bytes;
		fixed += s.fixed_bytes;
		pages += s.pages;
		names.push_back(s.name);
	}
	if (splats == 0u) {
		d["status"] = "MISSING";
		d["reason"] = "no " + p_kind + " data set was measured";
		return d;
	}
	d["status"] = "OK";
	d["data_sets"] = names;
	d["splats"] = splats;
	d["bytes_per_splat"] = double(stored) / double(splats);
	d["fixed_width_bytes_per_splat"] = double(fixed) / double(splats);
	d["zstd_ratio_vs_fixed_width"] = (double(fixed) + double(pages) * kPageHeaderBytes) / double(stored);
	return d;
}

inline Dictionary report(const LocalVector<DataSet> &p_sets, bool p_full) {
	const EncodeParams params;
	Dictionary r;
	r["schema"] = "gsq_codec_probe/1";
	r["mode"] = p_full ? "full" : "smoke";
	r["threads"] = 1;
	r["codec"] = vformat("GSQ v%d: sh bits %d/%d/%d, opacity %d bits, zstd level %d, %d splats per sub-block", kCodecId,
			params.sh_bits[0], params.sh_bits[1], params.sh_bits[2], params.opacity_bits, params.zstd_level, kSubBlockSplats);
	r["zstd_library_version"] = zstd_library_version();
	r["packed_gaussian_bytes"] = uint32_t(sizeof(PackedGaussian));
	r["units"] = "MB = 1e6 bytes; rates are medians over reps of the summed per-page single-thread wall time";
	Array sets;
	for (const DataSet &s : p_sets) {
		sets.push_back(to_dict(s));
	}
	r["data_sets"] = sets;
	r["synthetic"] = aggregate(p_sets, "synthetic");
	r["real_scan"] = aggregate(p_sets, "real_scan");
	return r;
}

inline void add_scans(LocalVector<DataSet> &r_sets, const String &p_list, uint32_t p_reps) {
	const PackedStringArray paths = p_list.split(";", false);
	for (const String &raw : paths) {
		const String path = raw.strip_edges();
		if (path.is_empty()) {
			continue;
		}
		DataSet s;
		s.name = "scan:" + path.get_file();
		s.kind = "real_scan";
		probe_scan(s, path, p_reps);
		r_sets.push_back(s);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] BV-C2 codec probe: ratio and decode/pack throughput") {
	OS *os = OS::get_singleton();
	const bool full = os->has_environment("GS_GSQ_PROBE_OUT") && !os->get_environment("GS_GSQ_PROBE_OUT").is_empty();
	const uint32_t reps = full ? kFullReps : 1u;
	LocalVector<DataSet> sets;

	if (full) {
		gen::Params t0;
		gen::Params a;
		REQUIRE(gen::params_for_tier("T0", t0) == OK);
		REQUIRE(gen::params_for_tier("A", a) == OK);
		gen::Params noise = t0;
		noise.sh_field_amp_q8 = 0; // hash-noise SH: the compressibility floor of the SH planes

		DataSet s0;
		s0.name = "T0 (whole tier)";
		s0.kind = "synthetic";
		s0.source = "gs_world_generator tier T0, every chunk";
		probe_generator(s0, t0, 1u, 0u, reps);
		sets.push_back(s0);

		DataSet sa;
		sa.name = "A (every 32nd chunk in Morton order)";
		sa.kind = "synthetic";
		sa.source = "gs_world_generator tier A, chunk stride 32";
		probe_generator(sa, a, 32u, 0u, reps);
		sets.push_back(sa);

		DataSet sn;
		sn.name = "T0 hash-noise SH control (every 4th chunk)";
		sn.kind = "synthetic_control";
		sn.source = "gs_world_generator tier T0, sh_field_amp_q8 = 0, chunk stride 4";
		probe_generator(sn, noise, 4u, 0u, reps);
		sets.push_back(sn);

		add_scans(sets, os->get_environment("GS_GSQ_PROBE_SCANS"), reps);
	} else {
		gen::Params smoke;
		REQUIRE(gen::params_for_tier("T-smoke", smoke) == OK);
		DataSet s;
		s.name = "T-smoke prefix";
		s.kind = "synthetic";
		s.source = "gs_world_generator tier T-smoke, chunk 0, first 8192 splats";
		probe_generator(s, smoke, 1u, kSmokeSplats, reps);
		sets.push_back(s);
	}

	const Dictionary r = report(sets, full);
	for (const DataSet &s : sets) {
		MESSAGE(s.name.utf8().get_data(), ": ", s.status.utf8().get_data(), " ", s.reason.utf8().get_data());
		if (s.kind != "real_scan") {
			// Generator content is always available: a synthetic data set that is not OK is a
			// probe or codec defect, never an absence.
			CHECK_MESSAGE(s.status == "OK", s.reason.utf8().get_data());
		}
	}
	const Dictionary synthetic = r["synthetic"];
	CHECK(String(synthetic["status"]) == "OK");
	if (String(synthetic["status"]) == "OK") {
		const double bps = synthetic["bytes_per_splat"];
		const double fixed = synthetic["fixed_width_bytes_per_splat"];
		MESSAGE("synthetic bytes/splat ", bps, " (fixed width ", fixed, ")");
		CHECK(bps > 0.0);
		CHECK(fixed >= 6.0 + 14.0); // positions + scale, rotation, opacity and DC at the least
	}

	if (full) {
		const String out = os->get_environment("GS_GSQ_PROBE_OUT");
		Ref<FileAccess> f = FileAccess::open(out, FileAccess::WRITE);
		if (f.is_null()) {
			FAIL("cannot write the probe report to ", out.utf8().get_data());
			return;
		}
		f->store_string(JSON::stringify(r, "  ", false));
		f->close();
		MESSAGE("BV-C2 probe report written to ", out.utf8().get_data());
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][GSQ] BV-C2 codec probe: an absent scan is MISSING with no numbers") {
	LocalVector<DataSet> sets;
	add_scans(sets, "/nonexistent/gsq-probe/absent_scan.ply; ", 1u);
	if (sets.size() != 1u) {
		FAIL("expected one scan data set, got ", sets.size());
		return;
	}
	CHECK(sets[0].status == "MISSING");
	const Dictionary d = to_dict(sets[0]);
	CHECK(String(d["status"]) == "MISSING");
	CHECK_FALSE(d.has("bytes_per_splat"));
	CHECK_FALSE(d.has("decode_verified_msplats_per_s_one_core"));

	const Dictionary r = report(sets, true);
	const Dictionary real = r["real_scan"];
	CHECK(String(real["status"]) == "MISSING");
	CHECK_FALSE(real.has("bytes_per_splat"));
}

} // namespace TestGSQCodecProbe
