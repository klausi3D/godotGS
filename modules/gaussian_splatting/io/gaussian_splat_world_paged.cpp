#include "gaussian_splat_world_paged.h"

#include "../core/gaussian_splat_hlod_merge.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gs_vector_alloc.h"
#include "gs_atomic_file_writer.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gs_world_paged {

namespace {

// Little-endian cursors over a fixed buffer. The module only builds for little-endian hosts (the v3 format
// already memcpy()s Gaussian structs), so memcpy of scalars is the on-disk encoding.
struct Out {
	uint8_t *base;
	size_t at = 0;
	explicit Out(uint8_t *p_base) :
			base(p_base) {}
	void u32(uint32_t v) { memcpy(base + at, &v, 4); at += 4; }
	void i32(int32_t v) { memcpy(base + at, &v, 4); at += 4; }
	void u64(uint64_t v) { memcpy(base + at, &v, 8); at += 8; }
	void i64(int64_t v) { memcpy(base + at, &v, 8); at += 8; }
	void f32(float v) { memcpy(base + at, &v, 4); at += 4; }
	void f64(double v) { memcpy(base + at, &v, 8); at += 8; }
	void bytes(const uint8_t *p, size_t n) { memcpy(base + at, p, n); at += n; }
	void skip_to(size_t p_offset) { at = p_offset; }
};
struct In {
	const uint8_t *base;
	size_t at = 0;
	explicit In(const uint8_t *p_base) :
			base(p_base) {}
	uint32_t u32() { uint32_t v; memcpy(&v, base + at, 4); at += 4; return v; }
	int32_t i32() { int32_t v; memcpy(&v, base + at, 4); at += 4; return v; }
	uint64_t u64() { uint64_t v; memcpy(&v, base + at, 8); at += 8; return v; }
	int64_t i64() { int64_t v; memcpy(&v, base + at, 8); at += 8; return v; }
	float f32() { float v; memcpy(&v, base + at, 4); at += 4; return v; }
	double f64() { double v; memcpy(&v, base + at, 8); at += 8; return v; }
	void bytes(uint8_t *p, size_t n) { memcpy(p, base + at, n); at += n; }
	void skip_to(size_t p_offset) { at = p_offset; }
};

bool fits_within(uint64_t p_offset, uint64_t p_size, uint64_t p_file_len) {
	return p_offset <= p_file_len && p_size <= p_file_len - p_offset;
}

bool mul_u64(uint64_t a, uint64_t b, uint64_t &r) {
	if (a != 0u && b > UINT64_MAX / a) {
		return false;
	}
	r = a * b;
	return true;
}

bool add_u64(uint64_t a, uint64_t b, uint64_t &r) {
	if (b > UINT64_MAX - a) {
		return false;
	}
	r = a + b;
	return true;
}

void sha256(const uint8_t *p_data, size_t p_size, uint8_t r_digest[32]) {
	CryptoCore::SHA256Context ctx;
	ctx.start();
	if (p_size != 0u) {
		ctx.update(p_data, p_size);
	}
	ctx.finish(r_digest);
}

bool all_zero(const uint8_t *p, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		if (p[i] != 0u) {
			return false;
		}
	}
	return true;
}

// stored bytes of one payload record, with every multiplication checked.
bool record_stored_bytes(uint32_t p_count, uint32_t p_high_order, uint64_t &r_gaussian, uint64_t &r_sh, uint64_t &r_total) {
	uint64_t sh_count;
	if (!mul_u64(p_count, sizeof(Gaussian), r_gaussian) || !mul_u64(p_count, p_high_order, sh_count) ||
			!mul_u64(sh_count, sizeof(Vector3), r_sh)) {
		return false;
	}
	uint64_t partial;
	return add_u64(kPayloadRecordHeaderBytes, r_gaussian, partial) && add_u64(partial, r_sh, r_total);
}

} // namespace

bool compute_node_metrics(const Gaussian *p_gaussians, uint32_t p_count, const double p_cell_center[3],
		bool p_is_leaf, NodeMetrics &r_metrics, String &r_reason) {
	r_metrics = NodeMetrics();
	float payload_center[3];
	for (int a = 0; a < 3; ++a) {
		payload_center[a] = float(p_cell_center[a]);
	}
	for (uint32_t j = 0; j < p_count; ++j) {
		const Gaussian &g = p_gaussians[j];
		// Exactly the snapshot's reconstruction rule: moving the centre to float32 must not move a position.
		for (int a = 0; a < 3; ++a) {
			const float canonical = p_is_leaf ? float(g.position[a]) : float(p_cell_center[a] + double(g.position[a]));
			const float relative = p_is_leaf ? float(double(g.position[a]) - p_cell_center[a]) : float(g.position[a]);
			const float reconstructed = float(payload_center[a] + relative);
			if (!Math::is_finite(canonical) || !Math::is_finite(relative) || reconstructed != canonical) {
				r_metrics.center_exact = false;
			}
		}
		double scales[3] = { double(g.scale.x), double(g.scale.y), double(g.scale.z) };
		for (double scale : scales) {
			if (!Math::is_finite(scale) || scale < 0.0) {
				r_reason = "v4_footprint_scale_invalid";
				return false;
			}
		}
		std::sort(scales, scales + 3);
		r_metrics.footprint += 36.0 * scales[1] * scales[2];
		if (!Math::is_finite(r_metrics.footprint)) {
			r_reason = "v4_footprint_overflow";
			return false;
		}
		const uint32_t dc = uint32_t(gaussian_get_dc_encoding(g.render_meta));
		if (j == 0u) {
			r_metrics.dc_encoding = dc;
		} else if (dc != r_metrics.dc_encoding) {
			r_metrics.homogeneous_dc = false;
		}
	}
	return true;
}

bool validate_node_payload(const double p_cell_center[3], uint32_t p_sh_high_order_count, const GaussianSplatHlodNode &p_node,
		const Gaussian *p_gaussians, const Vector3 *p_sh, String &r_reason) {
	const double *center = p_cell_center;
	for (uint32_t i = 0; i < p_node.payload_count; i++) {
		const Gaussian &g = p_gaussians[i];
		const real_t fields[] = { g.position.x, g.position.y, g.position.z, g.opacity, g.scale.x, g.scale.y, g.scale.z,
			g.area, g.rotation.x, g.rotation.y, g.rotation.z, g.rotation.w, g.sh_dc.r, g.sh_dc.g, g.sh_dc.b, g.sh_dc.a,
			g.normal.x, g.normal.y, g.normal.z, g.stroke_age, g.brush_axes.x, g.brush_axes.y };
		for (real_t field : fields) {
			if (!Math::is_finite(field)) {
				r_reason = "non-finite Gaussian field in HLOD payload";
				return false;
			}
		}
		for (const Vector3 &v : g.sh_1) {
			if (!Math::is_finite(v.x) || !Math::is_finite(v.y) || !Math::is_finite(v.z)) {
				r_reason = "non-finite first-order SH in HLOD payload";
				return false;
			}
		}
		for (uint32_t k = 0; k < p_sh_high_order_count; k++) {
			const Vector3 &v = p_sh[uint64_t(i) * p_sh_high_order_count + k];
			if (!Math::is_finite(v.x) || !Math::is_finite(v.y) || !Math::is_finite(v.z)) {
				r_reason = "non-finite high-order SH in HLOD payload";
				return false;
			}
		}
		double cov[6];
		gs_hlod::splat_covariance(g, cov);
		double distance_squared = 0.0;
		double tolerance_squared = 0.0;
		const int diagonal[] = { 0, 3, 5 };
		for (int a = 0; a < 3; a++) {
			const double position = g.position[a];
			const double absolute = center[a] + position;
			if (!Math::is_finite(absolute) || std::abs(absolute) > std::numeric_limits<real_t>::max()) {
				r_reason = "HLOD payload position overflows runtime coordinates";
				return false;
			}
			// Leaf serialization rounds (absolute_position - cell_center) to real_t.
			// Permit only that half-ULP plus double arithmetic rounding, not a world-scale epsilon.
			const real_t lower = std::nextafter(g.position[a], real_t(-INFINITY));
			const real_t upper = std::nextafter(g.position[a], real_t(INFINITY));
			const double spacing = MAX(Math::is_finite(lower) ? position - double(lower) : 0.0,
					Math::is_finite(upper) ? double(upper) - position : 0.0);
			const double tolerance = 0.5 * spacing +
					4.0 * std::numeric_limits<double>::epsilon() * (std::abs(center[a]) + std::abs(position) +
							std::abs(double(p_node.aabb_min[a])) + std::abs(double(p_node.aabb_max[a])));
			const double half_extent = 3.0 * std::sqrt(MAX(cov[diagonal[a]], 0.0));
			if (!Math::is_finite(half_extent) || position - half_extent < double(p_node.aabb_min[a]) - tolerance ||
					position + half_extent > double(p_node.aabb_max[a]) + tolerance) {
				r_reason = "HLOD node AABB does not contain its payload's three-sigma support";
				return false;
			}
			const double mid = 0.5 * (double(p_node.aabb_min[a]) + double(p_node.aabb_max[a]));
			const double delta = position - mid;
			distance_squared += delta * delta;
			tolerance_squared += tolerance * tolerance;
		}
		const double support_radius = 3.0 * MAX(std::abs(double(g.scale.x)), MAX(std::abs(double(g.scale.y)), std::abs(double(g.scale.z))));
		if (std::sqrt(distance_squared) + support_radius > double(p_node.radius) + std::sqrt(tolerance_squared)) {
			r_reason = "HLOD node sphere does not contain its payload's three-sigma support";
			return false;
		}
	}
	return true;
}

} // namespace gs_world_paged

using namespace gs_world_paged;

namespace {

void write_node_topology(Out &o, const GaussianSplatHlodNode &n) {
	o.i32(n.cell.e);
	o.u32(n.child_count);
	o.u32(n.height);
	o.u32(n.flags);
	o.i64(n.cell.ix);
	o.i64(n.cell.iy);
	o.i64(n.cell.iz);
	for (int a = 0; a < 3; ++a) { o.f32(float(n.aabb_min[a])); }
	for (int a = 0; a < 3; ++a) { o.f32(float(n.aabb_max[a])); }
	o.f32(n.radius);
	o.f32(n.geometric_error);
	o.f32(n.error_chosen);
	o.f32(n.error_rejected);
	o.u32(n.parent);
	o.u32(n.first_child);
	o.u64(n.payload_first);
	o.u32(n.payload_count);
	o.f32(n.overlap_footprint);
}

void read_node_topology(In &in, GaussianSplatHlodNode &n) {
	n.cell.e = in.i32();
	n.child_count = in.u32();
	n.height = in.u32();
	n.flags = in.u32();
	n.cell.ix = in.i64();
	n.cell.iy = in.i64();
	n.cell.iz = in.i64();
	for (int a = 0; a < 3; ++a) { n.aabb_min[a] = in.f32(); }
	for (int a = 0; a < 3; ++a) { n.aabb_max[a] = in.f32(); }
	n.radius = in.f32();
	n.geometric_error = in.f32();
	n.error_chosen = in.f32();
	n.error_rejected = in.f32();
	n.parent = in.u32();
	n.first_child = in.u32();
	n.payload_first = in.u64();
	n.payload_count = in.u32();
	n.overlap_footprint = in.f32();
}

struct HeaderFields {
	uint32_t flags = 0u;
	uint32_t leaf = 0u;
	uint32_t interior = 0u;
	uint32_t node_count = 0u;
	uint32_t sh_degree = 0u;
	uint32_t sh_first_order = 0u;
	uint32_t sh_high_order = 0u;
	uint32_t bake_rule = 0u;
	double origin[3] = { 0.0, 0.0, 0.0 };
	Vector3 bounds_pos;
	Vector3 bounds_size;
	uint64_t index_offset = 0u;
	uint64_t index_page_count = 0u;
	uint64_t payload_offset = 0u;
	uint64_t payload_bytes = 0u;
	uint64_t metadata_offset = 0u;
	uint64_t metadata_size = 0u;
	uint64_t file_bytes = 0u;
	uint64_t content_generation = 0u;
	uint8_t index_digest[32] = {};
	uint8_t metadata_digest[32] = {};
};

void write_header(uint8_t *p_page, const HeaderFields &h) {
	memset(p_page, 0, kHeaderBytes);
	Out o(p_page);
	o.u32(kMagic);
	o.u32(kVersion);
	o.u32(h.flags);
	o.u32(uint32_t(kHeaderBytes));
	o.u32(uint32_t(kPageBytes));
	o.u32(uint32_t(kIndexPageBytes));
	o.u32(uint32_t(kIndexRecordBytes));
	o.u32(GAUSSIAN_STRUCT_LAYOUT_VERSION);
	o.u32(h.leaf);
	o.u32(h.interior);
	o.u32(h.node_count);
	o.u32(h.sh_degree);
	o.u32(h.sh_first_order);
	o.u32(h.sh_high_order);
	o.u32(h.bake_rule);
	o.u32(0u);
	for (int a = 0; a < 3; ++a) { o.f64(h.origin[a]); }
	for (int a = 0; a < 3; ++a) { o.f32(float(h.bounds_pos[a])); }
	for (int a = 0; a < 3; ++a) { o.f32(float(h.bounds_size[a])); }
	o.u64(h.index_offset);
	o.u64(h.index_page_count);
	o.u64(h.payload_offset);
	o.u64(h.payload_bytes);
	o.u64(h.metadata_offset);
	o.u64(h.metadata_size);
	o.u64(h.file_bytes);
	o.u64(h.content_generation);
	o.bytes(h.index_digest, 32);
	o.bytes(h.metadata_digest, 32);
	sha256(p_page, kHeaderBytes - 32, p_page + kHeaderBytes - 32);
}

// Fills `r_h` from a verified-digest header page. Distinct reasons per violated rule.
Error parse_header(const uint8_t *p_page, uint64_t p_file_len, HeaderFields &r_h, String &r_reason) {
	In in(p_page);
	const uint32_t magic = in.u32();
	if (magic != kMagic) {
		r_reason = "v4_bad_magic";
		return ERR_FILE_UNRECOGNIZED;
	}
	const uint32_t version = in.u32();
	if (version != kVersion) {
		r_reason = "v4_wrong_version";
		return ERR_FILE_UNRECOGNIZED;
	}
	uint8_t expect[32];
	sha256(p_page, kHeaderBytes - 32, expect);
	if (memcmp(expect, p_page + kHeaderBytes - 32, 32) != 0) {
		r_reason = "v4_header_digest_mismatch";
		return ERR_FILE_CORRUPT;
	}
	r_h.flags = in.u32();
	if ((r_h.flags & ~kKnownFlags) != 0u) {
		r_reason = "v4_unknown_header_flags";
		return ERR_FILE_CORRUPT;
	}
	if (in.u32() != kHeaderBytes || in.u32() != kPageBytes || in.u32() != kIndexPageBytes || in.u32() != kIndexRecordBytes) {
		r_reason = "v4_unexpected_page_geometry";
		return ERR_FILE_CORRUPT;
	}
	const uint32_t layout = in.u32();
	if (layout != GAUSSIAN_STRUCT_LAYOUT_VERSION) {
		r_reason = "v4_gaussian_layout_mismatch";
		return ERR_FILE_UNRECOGNIZED;
	}
	r_h.leaf = in.u32();
	r_h.interior = in.u32();
	r_h.node_count = in.u32();
	r_h.sh_degree = in.u32();
	r_h.sh_first_order = in.u32();
	r_h.sh_high_order = in.u32();
	r_h.bake_rule = in.u32();
	if (in.u32() != 0u) {
		r_reason = "v4_nonzero_reserved_header";
		return ERR_FILE_CORRUPT;
	}
	for (int a = 0; a < 3; ++a) { r_h.origin[a] = in.f64(); }
	for (int a = 0; a < 3; ++a) { r_h.bounds_pos[a] = in.f32(); }
	for (int a = 0; a < 3; ++a) { r_h.bounds_size[a] = in.f32(); }
	r_h.index_offset = in.u64();
	r_h.index_page_count = in.u64();
	r_h.payload_offset = in.u64();
	r_h.payload_bytes = in.u64();
	r_h.metadata_offset = in.u64();
	r_h.metadata_size = in.u64();
	r_h.file_bytes = in.u64();
	r_h.content_generation = in.u64();
	in.bytes(r_h.index_digest, 32);
	in.bytes(r_h.metadata_digest, 32);
	if (!all_zero(p_page + in.at, kHeaderBytes - 32 - in.at)) {
		r_reason = "v4_nonzero_reserved_header";
		return ERR_FILE_CORRUPT;
	}
	for (int a = 0; a < 3; ++a) {
		if (!Math::is_finite(r_h.origin[a]) || !Math::is_finite(r_h.bounds_pos[a]) || !Math::is_finite(r_h.bounds_size[a]) ||
				r_h.bounds_size[a] < 0.0f) {
			r_reason = "v4_nonfinite_header_geometry";
			return ERR_FILE_CORRUPT;
		}
	}
	if (r_h.bake_rule == 0u || r_h.node_count == 0u || r_h.node_count > kMaxNodes ||
			r_h.sh_degree > 4u || r_h.sh_first_order > 3u || r_h.sh_high_order > kMaxHighOrder) {
		r_reason = "v4_header_field_out_of_range";
		return ERR_FILE_CORRUPT;
	}
	const uint64_t total = uint64_t(r_h.leaf) + r_h.interior;
	if (r_h.leaf == 0u || total > uint64_t(UINT32_MAX) || uint64_t(r_h.node_count) > total) {
		r_reason = "v4_splat_counts_inconsistent";
		return ERR_FILE_CORRUPT;
	}
	// Region geometry: header | index | payload | optional metadata, back to back, page aligned.
	uint64_t pages = (uint64_t(r_h.node_count) + kRecordsPerIndexPage - 1u) / kRecordsPerIndexPage;
	if (r_h.index_offset != kHeaderBytes || r_h.index_page_count != pages) {
		r_reason = "v4_index_region_inconsistent";
		return ERR_FILE_CORRUPT;
	}
	uint64_t index_bytes;
	uint64_t index_end;
	if (!mul_u64(pages, kIndexPageBytes, index_bytes) || !add_u64(kHeaderBytes, index_bytes, index_end) ||
			r_h.payload_offset != index_end || (r_h.payload_bytes % kPageBytes) != 0u || r_h.payload_bytes == 0u) {
		r_reason = "v4_payload_region_inconsistent";
		return ERR_FILE_CORRUPT;
	}
	uint64_t payload_end;
	if (!add_u64(r_h.payload_offset, r_h.payload_bytes, payload_end)) {
		r_reason = "v4_payload_region_overflow";
		return ERR_FILE_CORRUPT;
	}
	if (((r_h.flags & kFlagHasMetadata) != 0u) != (r_h.metadata_size != 0u) || r_h.metadata_size > kMaxMetadataBytes) {
		r_reason = "v4_metadata_region_inconsistent";
		return ERR_FILE_CORRUPT;
	}
	uint64_t expected_file;
	if (r_h.metadata_size != 0u) {
		if (r_h.metadata_offset != payload_end || !add_u64(payload_end, r_h.metadata_size, expected_file)) {
			r_reason = "v4_metadata_region_inconsistent";
			return ERR_FILE_CORRUPT;
		}
	} else {
		if (r_h.metadata_offset != 0u) {
			r_reason = "v4_metadata_region_inconsistent";
			return ERR_FILE_CORRUPT;
		}
		expected_file = payload_end;
	}
	if (r_h.file_bytes != expected_file) {
		r_reason = "v4_declared_length_inconsistent";
		return ERR_FILE_CORRUPT;
	}
	if (p_file_len < r_h.file_bytes) {
		r_reason = "v4_truncated_file";
		return ERR_FILE_CORRUPT;
	}
	if (p_file_len > r_h.file_bytes) {
		r_reason = "v4_trailing_bytes";
		return ERR_FILE_CORRUPT;
	}
	return OK;
}

bool probe_allocation(uint64_t p_bytes) {
	if (p_bytes == 0u) {
		return true;
	}
	void *probe = memalloc(p_bytes);
	if (probe == nullptr) {
		return false;
	}
	memfree(probe);
	return true;
}

Error read_exact(const Ref<FileAccess> &p_file, uint8_t *p_dst, uint64_t p_size) {
	if (p_size == 0u) {
		return OK;
	}
	if (p_file->get_buffer(p_dst, p_size) != p_size) {
		return p_file->get_error() != OK && p_file->get_error() != ERR_FILE_EOF ? p_file->get_error() : ERR_FILE_EOF;
	}
	return OK;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Reader

Error GaussianSplatHlodPagedSource::open(const String &p_path, GaussianSplatHlodTree &r_tree, Dictionary &r_metadata, String &r_reason) {
	opened = false;
	nodes.clear();
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		r_reason = "v4_cannot_open";
		return open_error != OK ? open_error : ERR_FILE_CANT_OPEN;
	}
	const uint64_t file_len = file->get_length();
	if (file_len < kHeaderBytes) {
		r_reason = "v4_truncated_header";
		return ERR_FILE_CORRUPT;
	}
	uint8_t header_page[kHeaderBytes];
	Error err = read_exact(file, header_page, kHeaderBytes);
	if (err != OK) {
		r_reason = "v4_header_read_failed";
		return err;
	}
	HeaderFields h;
	err = parse_header(header_page, file_len, h, r_reason);
	if (err != OK) {
		return err;
	}

	// Tree + records. Allocation is bounded by the file length: the index region was just proven to fit it.
	GaussianSplatHlodTree tree;
	for (int a = 0; a < 3; ++a) { tree.origin[a] = h.origin[a]; }
	tree.bake_rule_version = h.bake_rule;
	tree.leaf_splat_count = h.leaf;
	tree.interior_splat_count = h.interior;
	tree.sh_high_order_count = h.sh_high_order;
	if (!probe_allocation(uint64_t(h.node_count) * (sizeof(GaussianSplatHlodNode) + sizeof(NodeRecord)))) {
		r_reason = "v4_cannot_allocate_node_table";
		return ERR_OUT_OF_MEMORY;
	}
	tree.nodes.resize(h.node_count);
	Vector<NodeRecord> records;
	if (!gs_resize_or_fail(records, int64_t(h.node_count), "V4 node records")) {
		r_reason = "v4_cannot_allocate_node_table";
		return ERR_OUT_OF_MEMORY;
	}

	Vector<uint8_t> page;
	if (!gs_resize_or_fail(page, int64_t(kIndexPageBytes), "V4 index page")) {
		r_reason = "v4_cannot_allocate_index_page";
		return ERR_OUT_OF_MEMORY;
	}
	LocalVector<uint8_t> page_digests;
	page_digests.resize(h.index_page_count * 32u);
	file->seek(h.index_offset);
	for (uint64_t pi = 0; pi < h.index_page_count; ++pi) {
		err = read_exact(file, page.ptrw(), kIndexPageBytes);
		if (err != OK) {
			r_reason = "v4_index_page_read_failed";
			return err;
		}
		const uint8_t *p = page.ptr();
		In head(p);
		if (head.u32() != kIndexPageMagic || uint64_t(head.u32()) != pi) {
			r_reason = "v4_index_page_identity_mismatch";
			return ERR_FILE_CORRUPT;
		}
		const uint32_t first_node = head.u32();
		const uint32_t in_page = head.u32();
		const uint64_t expect_first = pi * kRecordsPerIndexPage;
		const uint64_t expect_count = MIN<uint64_t>(kRecordsPerIndexPage, uint64_t(h.node_count) - expect_first);
		if (first_node != expect_first || in_page != expect_count || !all_zero(p + 16, 16)) {
			r_reason = "v4_index_page_identity_mismatch";
			return ERR_FILE_CORRUPT;
		}
		uint8_t digest[32];
		sha256(p + kIndexPageHeaderBytes, size_t(in_page) * kIndexRecordBytes, digest);
		if (memcmp(digest, p + 32, 32) != 0) {
			r_reason = "v4_index_page_digest_mismatch";
			return ERR_FILE_CORRUPT;
		}
		memcpy(page_digests.ptr() + pi * 32u, digest, 32);
		// The unused tail of the last page must stay zero.
		if (!all_zero(p + kIndexPageHeaderBytes + size_t(in_page) * kIndexRecordBytes,
					size_t(kIndexPageBytes - kIndexPageHeaderBytes - size_t(in_page) * kIndexRecordBytes))) {
			r_reason = "v4_index_page_tail_not_zero";
			return ERR_FILE_CORRUPT;
		}
		for (uint32_t k = 0; k < in_page; ++k) {
			const uint32_t node = first_node + k;
			In rec(p + kIndexPageHeaderBytes + size_t(k) * kIndexRecordBytes);
			NodeRecord &r = records.write[node];
			read_node_topology(rec, tree.nodes[node]);
			r.metrics.footprint = rec.f64();
			r.metrics.dc_encoding = rec.u32();
			const uint32_t node_flags = rec.u32();
			const uint64_t reserved0 = rec.u64();
			r.offset = rec.u64();
			r.stored_bytes = rec.u64();
			r.extent_bytes = rec.u64();
			rec.skip_to(192);
			rec.bytes(r.digest, 32);
			const uint8_t *base = p + kIndexPageHeaderBytes + size_t(k) * kIndexRecordBytes;
			if (reserved0 != 0u || !all_zero(base + 152, 40) || !all_zero(base + 224, 32) || (node_flags & ~kNodeKnownFlags) != 0u) {
				r_reason = "v4_nonzero_reserved_node_record";
				return ERR_FILE_CORRUPT;
			}
			r.metrics.homogeneous_dc = (node_flags & kNodeHomogeneousDc) != 0u;
			r.metrics.center_exact = (node_flags & kNodeCenterExact) != 0u;
			if (r.metrics.dc_encoding > 1u || !Math::is_finite(r.metrics.footprint) || r.metrics.footprint < 0.0) {
				r_reason = "v4_node_metrics_invalid";
				return ERR_FILE_CORRUPT;
			}
		}
	}
	uint8_t index_digest_actual[32];
	sha256(page_digests.ptr(), page_digests.size(), index_digest_actual);
	if (memcmp(index_digest_actual, h.index_digest, 32) != 0) {
		r_reason = "v4_index_digest_mismatch";
		return ERR_FILE_CORRUPT;
	}

	// Topology: the same validator the v2/v3 loader runs, over the node table alone.
	{
		String tree_reason;
		Error tree_error = OK;
		if (!gs_hlod_validate_tree(tree, &tree_reason, &tree_error)) {
			r_reason = "v4_invalid_tree: " + tree_reason;
			return tree_error == ERR_OUT_OF_MEMORY ? ERR_OUT_OF_MEMORY : ERR_FILE_CORRUPT;
		}
	}

	// Per-record geometry: strictly sequential, page-aligned, exactly sized.
	uint64_t cursor = h.payload_offset;
	uint64_t max_record = 0u;
	for (uint32_t i = 0; i < h.node_count; ++i) {
		NodeRecord &r = records.write[i];
		r.topology = tree.nodes[i];
		r.is_leaf = r.topology.child_count == 0u;
		tree.node_cell_center(r.topology, r.cell_center);
		uint64_t g_bytes, s_bytes, stored;
		if (r.topology.payload_count == 0u || r.topology.payload_count > gs_hlod::kMaxNodeSplats ||
				!record_stored_bytes(r.topology.payload_count, h.sh_high_order, g_bytes, s_bytes, stored) ||
				r.stored_bytes != stored || r.extent_bytes != align_up_page(stored)) {
			r_reason = "v4_payload_record_size_inconsistent";
			return ERR_FILE_CORRUPT;
		}
		if (r.offset != cursor) {
			r_reason = "v4_payload_record_misplaced";
			return ERR_FILE_CORRUPT;
		}
		if (!add_u64(cursor, r.extent_bytes, cursor)) {
			r_reason = "v4_payload_region_overflow";
			return ERR_FILE_CORRUPT;
		}
		// A node's payload lies wholly in the leaf range or wholly in the interior range.
		if (r.is_leaf ? r.topology.payload_first + r.topology.payload_count > h.leaf : r.topology.payload_first < h.leaf) {
			r_reason = "v4_payload_range_inconsistent";
			return ERR_FILE_CORRUPT;
		}
		max_record = MAX(max_record, stored);
	}
	if (cursor != h.payload_offset + h.payload_bytes) {
		r_reason = "v4_payload_region_inconsistent";
		return ERR_FILE_CORRUPT;
	}

	Dictionary metadata;
	if (h.metadata_size != 0u) {
		if (h.metadata_size >= uint64_t(INT32_MAX)) {
			r_reason = "v4_metadata_region_inconsistent";
			return ERR_FILE_CORRUPT;
		}
		Vector<uint8_t> bytes;
		if (!gs_resize_or_fail(bytes, int64_t(h.metadata_size), "V4 metadata")) {
			r_reason = "v4_cannot_allocate_metadata";
			return ERR_OUT_OF_MEMORY;
		}
		file->seek(h.metadata_offset);
		err = read_exact(file, bytes.ptrw(), h.metadata_size);
		if (err != OK) {
			r_reason = "v4_metadata_read_failed";
			return err;
		}
		uint8_t digest[32];
		sha256(bytes.ptr(), size_t(h.metadata_size), digest);
		if (memcmp(digest, h.metadata_digest, 32) != 0) {
			r_reason = "v4_metadata_digest_mismatch";
			return ERR_FILE_CORRUPT;
		}
		String text;
		JSON json;
		if (memchr(bytes.ptr(), 0, bytes.size()) != nullptr ||
				text.append_utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()) != OK ||
				json.parse(text) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
			r_reason = "v4_metadata_not_a_json_dictionary";
			return ERR_FILE_CORRUPT;
		}
		metadata = json.get_data();
	} else if (!all_zero(h.metadata_digest, 32)) {
		r_reason = "v4_metadata_region_inconsistent";
		return ERR_FILE_CORRUPT;
	}

	// Commit.
	info = Info();
	info.leaf_splat_count = h.leaf;
	info.interior_splat_count = h.interior;
	info.node_count = h.node_count;
	info.sh_degree = h.sh_degree;
	info.sh_first_order = h.sh_first_order;
	info.sh_high_order = h.sh_high_order;
	info.is_2d = (h.flags & kFlagIs2D) != 0u;
	info.bounds = AABB(h.bounds_pos, h.bounds_size);
	info.file_bytes = h.file_bytes;
	info.index_bytes = h.index_page_count * kIndexPageBytes;
	info.payload_bytes = h.payload_bytes;
	info.content_generation = h.content_generation;
	info.bake_rule_version = h.bake_rule;
	memcpy(index_digest, h.index_digest, 32);
	record_high_order = h.sh_high_order;
	max_record_bytes = max_record;
	nodes = records;
	path = p_path;
	stat_reads = 0u;
	stat_bytes = 0u;
	stat_failures = 0u;
	stat_largest = 0u;
	r_tree = std::move(tree);
	r_metadata = metadata;
	opened = true;
	r_reason = String();
	return OK;
}

void GaussianSplatHlodPagedSource::get_index_digest(uint8_t r_digest[32]) const {
	memcpy(r_digest, index_digest, 32);
}

GaussianSplatHlodPagedSource::Stats GaussianSplatHlodPagedSource::get_stats() const {
	Stats s;
	s.reads = stat_reads.load();
	s.bytes_read = stat_bytes.load();
	s.read_failures = stat_failures.load();
	s.largest_read_bytes = stat_largest.load();
	return s;
}

Error GaussianSplatHlodPagedSource::read_node_payload(uint32_t p_node, Vector<Gaussian> &r_gaussians, Vector<Vector3> &r_sh, String &r_reason) const {
	r_gaussians.clear();
	r_sh.clear();
	auto fail = [&](Error p_error, const char *p_why) {
		r_reason = p_why;
		r_gaussians.clear();
		r_sh.clear();
		stat_failures.fetch_add(1u);
		return p_error;
	};
	if (!opened || p_node >= uint32_t(nodes.size())) {
		return fail(ERR_INVALID_PARAMETER, "v4_node_out_of_range");
	}
	const NodeRecord &rec = nodes[p_node];
	const uint32_t count = rec.topology.payload_count;
	uint64_t g_bytes, s_bytes, stored;
	if (!record_stored_bytes(count, record_high_order, g_bytes, s_bytes, stored) || stored != rec.stored_bytes) {
		return fail(ERR_FILE_CORRUPT, "v4_payload_record_size_inconsistent");
	}
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		return fail(open_error != OK ? open_error : ERR_FILE_CANT_OPEN, "v4_cannot_reopen");
	}
	// A replacement on disk must never mix versions: length first, then the record stamp and digest below.
	if (file->get_length() != info.file_bytes) {
		return fail(ERR_FILE_CORRUPT, "v4_file_changed_length");
	}
	file->seek(rec.offset);
	uint8_t head[kPayloadRecordHeaderBytes];
	Error err = read_exact(file, head, sizeof(head));
	if (err != OK) {
		return fail(err, "v4_payload_header_read_failed");
	}
	{
		In in(head);
		const uint32_t magic = in.u32();
		const uint32_t node = in.u32();
		const uint32_t rcount = in.u32();
		const uint32_t high = in.u32();
		const uint64_t gb = in.u64();
		const uint64_t sb = in.u64();
		const uint64_t gen = in.u64();
		const uint64_t first = in.u64();
		const uint32_t layout = in.u32();
		if (magic != kRecordMagic || node != p_node || rcount != count || high != record_high_order || gb != g_bytes ||
				sb != s_bytes || first != rec.topology.payload_first || layout != GAUSSIAN_STRUCT_LAYOUT_VERSION ||
				!all_zero(head + in.at, sizeof(head) - in.at)) {
			return fail(ERR_FILE_CORRUPT, "v4_payload_record_header_mismatch");
		}
		if (gen != info.content_generation) {
			return fail(ERR_FILE_CORRUPT, "v4_content_generation_mismatch");
		}
	}
	if (!gs_resize_or_fail(r_gaussians, int64_t(count), "V4 node gaussians") ||
			!gs_resize_or_fail(r_sh, int64_t(uint64_t(count) * record_high_order), "V4 node SH")) {
		r_gaussians.clear();
		r_sh.clear();
		return fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_payload");
	}
	CryptoCore::SHA256Context ctx;
	ctx.start();
	ctx.update(head, sizeof(head));
	err = read_exact(file, reinterpret_cast<uint8_t *>(r_gaussians.ptrw()), g_bytes);
	if (err != OK) {
		return fail(err, "v4_payload_truncated");
	}
	ctx.update(reinterpret_cast<const uint8_t *>(r_gaussians.ptr()), size_t(g_bytes));
	if (s_bytes != 0u) {
		err = read_exact(file, reinterpret_cast<uint8_t *>(r_sh.ptrw()), s_bytes);
		if (err != OK) {
			return fail(err, "v4_payload_truncated");
		}
		ctx.update(reinterpret_cast<const uint8_t *>(r_sh.ptr()), size_t(s_bytes));
	}
	uint8_t digest[32];
	ctx.finish(digest);
	stat_reads.fetch_add(1u);
	stat_bytes.fetch_add(stored);
	uint64_t seen = stat_largest.load();
	while (stored > seen && !stat_largest.compare_exchange_weak(seen, stored)) {
	}
	if (memcmp(digest, rec.digest, 32) != 0) {
		return fail(ERR_FILE_CORRUPT, "v4_payload_digest_mismatch");
	}
	// Semantic validation on the node-relative form, then the same relative -> absolute step the v3 resident
	// load applies to a leaf (so the runtime sees identical floats), then the metrics the index promised.
	String why;
	if (!validate_node_payload(rec.cell_center, record_high_order, rec.topology, r_gaussians.ptr(), r_sh.ptr(), why)) {
		r_reason = "v4_payload_invalid: " + why;
		r_gaussians.clear();
		r_sh.clear();
		stat_failures.fetch_add(1u);
		return ERR_FILE_CORRUPT;
	}
	if (rec.is_leaf) {
		Gaussian *g = r_gaussians.ptrw();
		for (uint32_t i = 0; i < count; ++i) {
			const Vector3 &p = g[i].position;
			g[i].position = Vector3(real_t(double(p.x) + rec.cell_center[0]), real_t(double(p.y) + rec.cell_center[1]),
					real_t(double(p.z) + rec.cell_center[2]));
		}
	}
	NodeMetrics metrics;
	if (!compute_node_metrics(r_gaussians.ptr(), count, rec.cell_center, rec.is_leaf, metrics, why)) {
		return fail(ERR_FILE_CORRUPT, "v4_payload_metrics_invalid");
	}
	if (metrics.footprint != rec.metrics.footprint || metrics.dc_encoding != rec.metrics.dc_encoding ||
			metrics.homogeneous_dc != rec.metrics.homogeneous_dc || metrics.center_exact != rec.metrics.center_exact) {
		return fail(ERR_FILE_CORRUPT, "v4_payload_metrics_mismatch");
	}
	r_reason = String();
	return OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Writer

Error GaussianSplatHlodPagedSource::write_package(const GaussianSplatWorld &p_world, const String &p_path, String &r_reason) {
	Ref<GaussianSplatHlodRuntimeData> snap;
	{
		const Error capture = p_world.capture_hlod_runtime_snapshot(snap, r_reason);
		if (capture != OK) {
			r_reason = "v4_snapshot: " + r_reason;
			return capture;
		}
	}
	if (snap.is_null() || snap->is_paged()) {
		r_reason = "v4_source_is_already_paged";
		return ERR_INVALID_PARAMETER;
	}
	const GaussianSplatHlodTree &tree = p_world.get_hlod_tree();
	const uint32_t node_count = uint32_t(snap->nodes.size());
	if (!snap->is_current() || uint32_t(tree.nodes.size()) != node_count || !tree.instances.is_empty() || !tree.top_level_nodes.is_empty()) {
		r_reason = "v4_world_changed_or_instanced";
		return ERR_BUSY;
	}
	const uint32_t high = snap->high_order_count;
	if (high > kMaxHighOrder || snap->leaf_count == 0u) {
		r_reason = "v4_payload_shape_unsupported";
		return ERR_INVALID_DATA;
	}

	HeaderFields h;
	h.flags = snap->mode_2d ? kFlagIs2D : 0u;
	h.leaf = snap->leaf_count;
	h.interior = snap->parent_count;
	h.node_count = node_count;
	h.sh_degree = snap->sh_degree;
	h.sh_first_order = snap->sh_first_order_count();
	h.sh_high_order = high;
	h.bake_rule = tree.bake_rule_version;
	for (int a = 0; a < 3; ++a) { h.origin[a] = tree.origin[a]; }
	const AABB bounds = p_world.get_bounds();
	h.bounds_pos = bounds.position;
	h.bounds_size = bounds.size;
	h.index_page_count = (uint64_t(node_count) + kRecordsPerIndexPage - 1u) / kRecordsPerIndexPage;
	h.index_offset = kHeaderBytes;
	h.payload_offset = kHeaderBytes + h.index_page_count * kIndexPageBytes;

	// Metadata: only what the reader will accept.
	String metadata_json;
	Vector<uint8_t> metadata_bytes;
	const Dictionary metadata = p_world.get_metadata();
	if (!metadata.is_empty()) {
		metadata_json = JSON::stringify(metadata);
		const CharString utf8 = metadata_json.utf8();
		JSON check;
		if (utf8.length() == 0 || uint64_t(utf8.length()) > kMaxMetadataBytes || check.parse(metadata_json) != OK ||
				check.get_data().get_type() != Variant::DICTIONARY) {
			r_reason = "v4_metadata_not_representable";
			return ERR_INVALID_DATA;
		}
		if (!gs_resize_or_fail(metadata_bytes, int64_t(utf8.length()), "V4 metadata")) {
			r_reason = "v4_cannot_allocate_metadata";
			return ERR_OUT_OF_MEMORY;
		}
		memcpy(metadata_bytes.ptrw(), utf8.get_data(), size_t(utf8.length()));
		h.flags |= kFlagHasMetadata;
		h.metadata_size = uint64_t(utf8.length());
		sha256(metadata_bytes.ptr(), size_t(h.metadata_size), h.metadata_digest);
	}

	// Per node: node-relative payload (the v3 on-disk form), metrics from the runtime convention.
	Vector<NodeRecord> records;
	if (!gs_resize_or_fail(records, int64_t(node_count), "V4 writer records")) {
		r_reason = "v4_cannot_allocate_node_table";
		return ERR_OUT_OF_MEMORY;
	}
	uint64_t cursor = h.payload_offset;
	for (uint32_t i = 0; i < node_count; ++i) {
		NodeRecord &r = records.write[i];
		r.topology = tree.nodes[i];
		r.is_leaf = r.topology.child_count == 0u;
		tree.node_cell_center(r.topology, r.cell_center);
		uint64_t g_bytes, s_bytes;
		if (!record_stored_bytes(r.topology.payload_count, high, g_bytes, s_bytes, r.stored_bytes)) {
			r_reason = "v4_payload_record_size_inconsistent";
			return ERR_INVALID_DATA;
		}
		r.extent_bytes = align_up_page(r.stored_bytes);
		r.offset = cursor;
		cursor += r.extent_bytes;
	}
	h.payload_bytes = cursor - h.payload_offset;
	h.metadata_offset = h.metadata_size != 0u ? cursor : 0u;
	h.file_bytes = cursor + h.metadata_size;

	LocalVector<Gaussian> rel;
	LocalVector<Vector3> sh_buf;
	auto build_node = [&](uint32_t i, uint8_t *r_head) -> bool {
		const NodeRecord &r = records[i];
		const uint32_t count = r.topology.payload_count;
		rel.resize(count);
		sh_buf.resize(size_t(count) * high);
		for (uint32_t j = 0; j < count; ++j) {
			const uint64_t index = r.topology.payload_first + j;
			Gaussian g = snap->gaussian(index);
			if (r.is_leaf) {
				g.position = Vector3(real_t(double(g.position.x) - r.cell_center[0]), real_t(double(g.position.y) - r.cell_center[1]),
						real_t(double(g.position.z) - r.cell_center[2]));
			}
			rel[j] = g;
			const Vector3 *sh = snap->sh(index);
			for (uint32_t k = 0; k < high; ++k) {
				sh_buf[size_t(j) * high + k] = sh[k];
			}
		}
		memset(r_head, 0, kPayloadRecordHeaderBytes);
		Out o(r_head);
		uint64_t g_bytes, s_bytes, total;
		record_stored_bytes(count, high, g_bytes, s_bytes, total);
		o.u32(kRecordMagic);
		o.u32(i);
		o.u32(count);
		o.u32(high);
		o.u64(g_bytes);
		o.u64(s_bytes);
		o.u64(h.content_generation);
		o.u64(r.topology.payload_first);
		o.u32(GAUSSIAN_STRUCT_LAYOUT_VERSION);
		return true;
	};

	// Pass 1: content generation = hash of the identity fields and every node's body digest. Known before any
	// record header (which stamps it) is written, so the stamp is covered by the record digest.
	{
		CryptoCore::SHA256Context ctx;
		ctx.start();
		uint8_t head[kPayloadRecordHeaderBytes];
		HeaderFields identity = h;
		uint8_t page[kHeaderBytes];
		write_header(page, identity);
		ctx.update(page, kHeaderBytes - 32);
		for (uint32_t i = 0; i < node_count; ++i) {
			build_node(i, head);
			// Metrics of exactly what the reader will hand the runtime (the v3 relative -> absolute round trip),
			// cross-checked against the snapshot the resident route would have used.
			NodeRecord &r = records.write[i];
			LocalVector<Gaussian> runtime_form = rel;
			if (r.is_leaf) {
				for (uint32_t j = 0; j < runtime_form.size(); ++j) {
					const Vector3 &q = runtime_form[j].position;
					runtime_form[j].position = Vector3(real_t(double(q.x) + r.cell_center[0]), real_t(double(q.y) + r.cell_center[1]),
							real_t(double(q.z) + r.cell_center[2]));
				}
			}
			String metric_reason;
			if (!compute_node_metrics(runtime_form.ptr(), r.topology.payload_count, r.cell_center, r.is_leaf, r.metrics, metric_reason)) {
				r_reason = metric_reason;
				return ERR_INVALID_DATA;
			}
			if (!r.metrics.center_exact) {
				r_reason = "v4_center_reconstruction_not_exact";
				return ERR_UNAVAILABLE;
			}
			const GaussianSplatHlodRuntimeData::Node &runtime_node = snap->nodes[i];
			if (r.metrics.footprint != runtime_node.footprint || r.metrics.dc_encoding != runtime_node.dc_encoding ||
					r.metrics.homogeneous_dc != runtime_node.homogeneous_dc) {
				r_reason = "v4_metrics_disagree_with_snapshot";
				return ERR_INVALID_DATA;
			}
			uint8_t body[32];
			CryptoCore::SHA256Context body_ctx;
			body_ctx.start();
			body_ctx.update(reinterpret_cast<const uint8_t *>(rel.ptr()), size_t(rel.size()) * sizeof(Gaussian));
			if (!sh_buf.is_empty()) {
				body_ctx.update(reinterpret_cast<const uint8_t *>(sh_buf.ptr()), size_t(sh_buf.size()) * sizeof(Vector3));
			}
			body_ctx.finish(body);
			ctx.update(body, 32);
		}
		uint8_t all[32];
		ctx.finish(all);
		memcpy(&h.content_generation, all, 8);
	}

	return gs_atomic_file_write(p_path, [&](const Ref<FileAccess> &file) -> Error {
		Vector<uint8_t> zero;
		if (!gs_resize_or_fail(zero, int64_t(kIndexPageBytes), "V4 writer zero page")) {
			r_reason = "v4_cannot_allocate_index_page";
			return ERR_OUT_OF_MEMORY;
		}
		memset(zero.ptrw(), 0, kIndexPageBytes);
		file->store_buffer(zero.ptr(), kHeaderBytes);
		for (uint64_t p = 0; p < h.index_page_count; ++p) {
			file->store_buffer(zero.ptr(), kIndexPageBytes);
		}
		uint8_t head[kPayloadRecordHeaderBytes];
		for (uint32_t i = 0; i < node_count; ++i) {
			NodeRecord &r = records.write[i];
			build_node(i, head);
			CryptoCore::SHA256Context ctx;
			ctx.start();
			ctx.update(head, sizeof(head));
			ctx.update(reinterpret_cast<const uint8_t *>(rel.ptr()), size_t(rel.size()) * sizeof(Gaussian));
			if (!sh_buf.is_empty()) {
				ctx.update(reinterpret_cast<const uint8_t *>(sh_buf.ptr()), size_t(sh_buf.size()) * sizeof(Vector3));
			}
			ctx.finish(r.digest);
			file->store_buffer(head, sizeof(head));
			file->store_buffer(reinterpret_cast<const uint8_t *>(rel.ptr()), size_t(rel.size()) * sizeof(Gaussian));
			if (!sh_buf.is_empty()) {
				file->store_buffer(reinterpret_cast<const uint8_t *>(sh_buf.ptr()), size_t(sh_buf.size()) * sizeof(Vector3));
			}
			const uint64_t pad = r.extent_bytes - r.stored_bytes;
			if (pad != 0u) {
				file->store_buffer(zero.ptr(), pad);
			}
			if (file->get_error() != OK) {
				r_reason = "v4_write_failed";
				return ERR_FILE_CANT_WRITE;
			}
		}
		if (h.metadata_size != 0u) {
			file->store_buffer(metadata_bytes.ptr(), size_t(h.metadata_size));
		}
		// Index pages (second pass over the already written range), then the header.
		LocalVector<uint8_t> page_digests;
		page_digests.resize(h.index_page_count * 32u);
		Vector<uint8_t> page;
		if (!gs_resize_or_fail(page, int64_t(kIndexPageBytes), "V4 writer index page")) {
			r_reason = "v4_cannot_allocate_index_page";
			return ERR_OUT_OF_MEMORY;
		}
		for (uint64_t pi = 0; pi < h.index_page_count; ++pi) {
			uint8_t *p = page.ptrw();
			memset(p, 0, kIndexPageBytes);
			const uint64_t first = pi * kRecordsPerIndexPage;
			const uint32_t in_page = uint32_t(MIN<uint64_t>(kRecordsPerIndexPage, uint64_t(node_count) - first));
			for (uint32_t k = 0; k < in_page; ++k) {
				const NodeRecord &r = records[uint32_t(first) + k];
				Out o(p + kIndexPageHeaderBytes + size_t(k) * kIndexRecordBytes);
				write_node_topology(o, r.topology);
				o.f64(r.metrics.footprint);
				o.u32(r.metrics.dc_encoding);
				o.u32((r.metrics.homogeneous_dc ? kNodeHomogeneousDc : 0u) | (r.metrics.center_exact ? kNodeCenterExact : 0u));
				o.u64(0u);
				o.u64(r.offset);
				o.u64(r.stored_bytes);
				o.u64(r.extent_bytes);
				o.skip_to(192);
				o.bytes(r.digest, 32);
			}
			Out head_out(p);
			head_out.u32(kIndexPageMagic);
			head_out.u32(uint32_t(pi));
			head_out.u32(uint32_t(first));
			head_out.u32(in_page);
			sha256(p + kIndexPageHeaderBytes, size_t(in_page) * kIndexRecordBytes, p + 32);
			memcpy(page_digests.ptr() + pi * 32u, p + 32, 32);
			file->seek(h.index_offset + pi * kIndexPageBytes);
			file->store_buffer(p, kIndexPageBytes);
		}
		sha256(page_digests.ptr(), page_digests.size(), h.index_digest);
		uint8_t header_page[kHeaderBytes];
		write_header(header_page, h);
		file->seek(0);
		file->store_buffer(header_page, kHeaderBytes);
		if (file->get_error() != OK) {
			r_reason = "v4_write_failed";
			return ERR_FILE_CANT_WRITE;
		}
		return OK;
	});
}
