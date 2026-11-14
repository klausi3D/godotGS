#pragma once

// Shared paged-GSQ producer, without test registrations. Both header tests and
// force-link anchored test translation units use the same production writer.
#include "gs_gsq_codec_test_fixture.h"
#include "../io/gs_world_writer.h"

namespace TestGSWorldPagedChunks {

using gs_world_writer::ChunkDesc;
using gs_world_writer::CodecParams;
using gs_world_writer::Limits;
using gs_world_writer::PlainSpec;
using gs_world_writer::SourceMode;
using gs_world_writer::Writer;
using TestGSQCodec::Fixture;

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

} // namespace TestGSWorldPagedChunks
