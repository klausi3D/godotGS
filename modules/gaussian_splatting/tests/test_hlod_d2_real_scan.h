#pragma once

// Discovered by modules/SCsub's tests/*.h glob, like the other D2 headers.
// No environment: generated plain GSQ input, with >64k splats, always asserted.
// GS_D2_REAL_SCAN_PLY selects a read-only scan. Only vertex i % STRIDE == 0 is
// copied, without decoding the full scan, to an owned temporary PLY. No derived
// payload survives this case. GS_D2_REAL_SCAN_OUT is the reports-only JSON path.
// GS_D2_REAL_SCAN_SCRATCH selects a separate scratch root (OS temp by default).
#include "test_macros.h"
#include "gs_gsq_bounds_test_helpers.h"

#include "../core/gaussian_splat_hlod_auto_tree.h"
#include "../core/gaussian_splat_hlod_bake.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gs_world_generator.h"
#include "../io/gs_world_writer_v4.h"
#include "../io/ply_loader.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace TestHlodD2RealScan {

inline void remove_owned_tree(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name == "." || name == "..") {
			continue;
		}
		const String child = p_path.path_join(name);
		if (dir->current_is_dir()) {
			remove_owned_tree(child);
		} else {
			DirAccess::remove_absolute(child);
		}
	}
	dir->list_dir_end();
	dir.unref();
	DirAccess::remove_absolute(p_path);
}

struct Run {
	String dir;
	String report_path;
	Dictionary report;

	Run() {
		report_path = OS::get_singleton()->get_environment("GS_D2_REAL_SCAN_OUT");
		String parent = OS::get_singleton()->get_environment("GS_D2_REAL_SCAN_SCRATCH");
		if (parent.is_empty()) {
			parent = OS::get_singleton()->get_temp_path();
		}
		dir = parent.path_join("d2-real-scan-payload-" + itos(OS::get_singleton()->get_ticks_usec()));
		report["schema"] = "godotgs-d2-real-scan-identity/1";
		report["status"] = "FAIL";
		report["reason_code"] = "case_incomplete";
		report["first_divergence"] = Dictionary();
		report["negative_control"] = Dictionary();
	}

	bool fail(const String &p_reason) {
		report["status"] = "FAIL";
		report["reason_code"] = p_reason;
		return false;
	}

	~Run() {
		// Finish the report on every ordinary early return, including MISSING.
		if (!report_path.is_empty()) {
			Ref<FileAccess> out = FileAccess::open(report_path, FileAccess::WRITE);
			if (out.is_null()) {
				FAIL("cannot write D2 identity report");
			} else {
				out->store_string(JSON::stringify(report, "\t", true, true) + "\n");
				CHECK(out->get_error() == OK);
			}
		}
		remove_owned_tree(dir);
	}
};

struct Input {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh;
	uint32_t degree = 0;
	uint32_t first = 0;
	uint32_t high = 0;
	bool is_2d = false;
	AABB bounds;
	gs_hlod::BakeInput view() const { return { gaussians.ptr(), high ? sh.ptr() : nullptr, gaussians.size(), high, first }; }
};

inline String canonical_digest(const Gaussian *p_g, const Vector3 *p_sh, uint32_t p_count, uint32_t p_degree) {
	uint8_t bytes[32];
	gs_gsq::decoded_digest(p_g, p_sh, p_count, p_degree, bytes);
	return String::hex_encode_buffer(bytes, 32);
}

inline String raw_digest(const Gaussian *p_g, const Vector3 *p_sh, uint32_t p_count, uint32_t p_high) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(reinterpret_cast<const uint8_t *>(p_g), uint64_t(p_count) * sizeof(Gaussian));
	if (p_high) {
		hash.update(reinterpret_cast<const uint8_t *>(p_sh), uint64_t(p_count) * p_high * sizeof(Vector3));
	}
	uint8_t bytes[32];
	hash.finish(bytes);
	return String::hex_encode_buffer(bytes, 32);
}

inline Dictionary json_file(const String &p_path) {
	const Variant value = JSON::parse_string(FileAccess::get_file_as_string(p_path));
	return value.get_type() == Variant::DICTIONARY ? Dictionary(value) : Dictionary();
}

// This narrow test-only extractor preserves scalar property bytes and the exact
// original vertex order. Unsupported layouts fail explicitly; no data is dropped
// to make a subset fit. PLYLoader then supplies the production property decoder.
inline bool stride_ply(Run &r_run, const String &p_scan, const String &p_subset, uint32_t p_stride, uint32_t p_cap) {
	Ref<FileAccess> src = FileAccess::open(p_scan, FileAccess::READ);
	if (src.is_null()) {
		return r_run.fail("scan_open_failed");
	}
	r_run.report["input_file_size_bytes"] = int64_t(src->get_length());
	// FileAccess hashes in bounded slices, rather than materialising the 1.5GB scan.
	r_run.report["input_sha256"] = FileAccess::get_sha256(p_scan);
	if (String(r_run.report["input_sha256"]).length() != 64) {
		return r_run.fail("scan_sha256_unavailable");
	}
	PackedStringArray lines;
	uint64_t count = 0;
	uint64_t row_bytes = 0;
	int vertex_line = -1;
	bool vertex = false;
	bool binary = false;
	bool format_seen = false;
	bool ended = false;
	while (!src->eof_reached() && src->get_position() < (1u << 20)) {
		const String line = src->get_line().strip_edges();
		const PackedStringArray words = line.split(" ", false);
		if (lines.is_empty() && line != "ply") {
			return r_run.fail("ply_header_invalid");
		}
		if (words.size() >= 3 && words[0] == "format") {
			binary = words[1] == "binary_little_endian" || words[1] == "binary_big_endian";
			format_seen = binary || words[1] == "ascii";
		}
		if (words.size() == 3 && words[0] == "element") {
			if (!words[2].is_valid_int() || words[2].to_int() < 0) {
				return r_run.fail("ply_element_count_invalid");
			}
			vertex = words[1] == "vertex";
			if (vertex) {
				if (vertex_line >= 0) {
					return r_run.fail("ply_duplicate_vertex_element");
				}
				count = words[2].to_int();
				vertex_line = lines.size();
			} else if (words[2].to_int() != 0) {
				return r_run.fail("ply_non_vertex_elements_unsupported");
			}
		}
		if (vertex && !words.is_empty() && words[0] == "property") {
			if (words.size() != 3 || words[1] == "list") {
				return r_run.fail("ply_vertex_list_unsupported");
			}
			const String type = words[1];
			if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") {
				row_bytes += 1;
			} else if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") {
				row_bytes += 2;
			} else if (type == "int" || type == "uint" || type == "float" || type == "int32" || type == "uint32" || type == "float32") {
				row_bytes += 4;
			} else if (type == "double" || type == "float64") {
				row_bytes += 8;
			} else {
				return r_run.fail("ply_scalar_type_unsupported");
			}
		}
		lines.push_back(line);
		if (line == "end_header") {
			ended = true;
			break;
		}
	}
	if (!ended || !format_seen || vertex_line < 0 || !count || !row_bytes || count > UINT32_MAX) {
		return r_run.fail("ply_header_invalid");
	}
	const uint64_t selected = (count + p_stride - 1) / p_stride;
	r_run.report["input_vertex_count"] = int64_t(count);
	r_run.report["subset_count"] = int64_t(selected);
	if (selected > p_cap) {
		return r_run.fail("subset_exceeds_splat_cap");
	}
	const uint64_t offset = src->get_position();
	if (binary && (offset > src->get_length() || count > (src->get_length() - offset) / row_bytes)) {
		return r_run.fail("ply_vertex_data_truncated");
	}
	Ref<FileAccess> out = FileAccess::open(p_subset, FileAccess::WRITE);
	if (out.is_null()) {
		return r_run.fail("subset_open_failed");
	}
	for (int i = 0; i < lines.size(); ++i) {
		out->store_string((i == vertex_line ? "element vertex " + itos(selected) : lines[i]) + "\n");
	}
	LocalVector<uint8_t> row;
	if (binary) {
		row.resize(row_bytes);
		for (uint64_t i = 0; i < count; i += p_stride) {
			src->seek(offset + i * row_bytes);
			if (src->get_buffer(row.ptr(), row_bytes) != row_bytes) {
				return r_run.fail("ply_vertex_data_truncated");
			}
			out->store_buffer(row.ptr(), row_bytes);
		}
	} else {
		for (uint64_t i = 0; i < count; ++i) {
			if (src->eof_reached()) {
				return r_run.fail("ply_vertex_data_truncated");
			}
			const String line = src->get_line();
			if (i % p_stride == 0) {
				out->store_string(line + "\n");
			}
		}
	}
	return out->get_error() == OK || r_run.fail("subset_write_failed");
}

inline bool capture(const Ref<GaussianData> &p_data, Input &r_input) {
	if (p_data.is_null() || p_data->get_count() <= 0) {
		return false;
	}
	r_input.degree = p_data->get_sh_degree();
	r_input.is_2d = p_data->get_2d_mode();
	r_input.bounds = p_data->get_aabb();
	return p_data->capture_chunk_snapshot(0, p_data->get_count(), r_input.gaussians, r_input.sh, r_input.first, r_input.high);
}

inline bool write_input(const String &p_path, const Input &p_input, String &r_reason) {
	gs_world_writer::PlainSpec spec;
	spec.splat_count = p_input.gaussians.size();
	spec.sh_degree = p_input.degree;
	spec.sh_first_order = p_input.first;
	spec.sh_high_order = p_input.high;
	spec.is_2d = p_input.is_2d;
	spec.bounds = p_input.bounds;
	spec.chunk_count = (spec.splat_count + 4095) / 4096;
	spec.write_chunk_table = true;
	gs_world_writer::Writer writer;
	if (writer.begin_plain(p_path, spec, gs_world_writer::gsq_v1_codec_params(), gs_world_writer::SourceMode::REITERABLE,
			gs_world_writer::Limits::unbounded()) != OK) {
		r_reason = writer.get_error_message();
		return false;
	}
	for (uint32_t pass = writer.current_pass(); pass != gs_world_writer::Writer::kNoPass; pass = writer.current_pass()) {
		for (uint32_t first = 0; first < spec.splat_count; first += 4096) {
			const uint32_t count = MIN(4096u, spec.splat_count - first);
			gs_world_writer::ChunkDesc desc;
			desc.bounds = p_input.bounds;
			desc.center = desc.bounds.get_center();
			desc.radius = desc.bounds.size.length() * 0.5f;
			if (writer.append_chunk(desc, p_input.gaussians.ptr() + first, p_input.high ? p_input.sh.ptr() + uint64_t(first) * p_input.high : nullptr, count) != OK) {
				r_reason = writer.get_error_message();
				return false;
			}
		}
	}
	if (writer.finalize() != OK) {
		r_reason = writer.get_error_message();
		return false;
	}
	return true;
}

inline Dictionary bound_call(const String &p_method, const String &p_path, const Variant &p_second, const Dictionary &p_options) {
	Ref<GaussianSplatHlodAutoTree> api;
	api.instantiate();
	const Variant path = p_path, options = p_options;
	const Variant *args[] = { &path, &p_second, &options };
	Callable::CallError error;
	const Variant value = api->callp(p_method, args, 3, error);
	if (error.error != Callable::CallError::CALL_OK || value.get_type() != Variant::DICTIONARY) {
		return {};
	}
	return value;
}

inline Dictionary divergence(int64_t p_node, const String &p_field, const Variant &p_expected = Variant(), const Variant &p_actual = Variant()) {
	Dictionary out;
	out["node_index"] = p_node;
	out["field"] = p_field;
	out["expected"] = p_expected;
	out["actual"] = p_actual;
	return out;
}

inline void keep_first(Dictionary &r_first, const Dictionary &p_next) {
	if (!p_next.is_empty() && (r_first.is_empty() || int64_t(p_next.get("node_index", 0)) < int64_t(r_first.get("node_index", 0)))) {
		r_first = p_next;
	}
}

inline Dictionary node_difference(uint32_t p_index, const GaussianSplatHlodNode &a, const GaussianSplatHlodNode &b, bool p_bounds) {
#define D2_REAL_FIELD(field) \
	if (a.field != b.field) { \
		return divergence(p_index, #field, b.field, a.field); \
	}
	D2_REAL_FIELD(cell.e);
	D2_REAL_FIELD(cell.ix);
	D2_REAL_FIELD(cell.iy);
	D2_REAL_FIELD(cell.iz);
	D2_REAL_FIELD(geometric_error);
	D2_REAL_FIELD(error_chosen);
	D2_REAL_FIELD(error_rejected);
	D2_REAL_FIELD(parent);
	D2_REAL_FIELD(first_child);
	D2_REAL_FIELD(child_count);
	D2_REAL_FIELD(height);
	D2_REAL_FIELD(payload_first);
	D2_REAL_FIELD(payload_count);
	D2_REAL_FIELD(flags);
	D2_REAL_FIELD(overlap_footprint);
	if (p_bounds) {
		D2_REAL_FIELD(aabb_min);
		D2_REAL_FIELD(aabb_max);
		D2_REAL_FIELD(radius);
	}
#undef D2_REAL_FIELD
	return {};
}

inline bool write_in_core(const String &p_path, const String &p_scratch, const Input &p_input, const gs_hlod::BakeResult &p_bake,
		const gs_world_writer::CodecParams &p_codec, String &r_reason) {
	gs_world_writer::PagedV4Spec spec;
	spec.node_count = p_bake.tree.nodes.size();
	spec.leaf_splat_count = p_bake.tree.leaf_splat_count;
	spec.interior_splat_count = p_bake.tree.interior_splat_count;
	spec.sh_degree = p_input.degree;
	spec.sh_first_order = p_input.first;
	spec.sh_high_order = p_input.high;
	spec.is_2d = p_input.is_2d;
	spec.bounds = p_input.bounds;
	spec.bake_rule_version = p_bake.tree.bake_rule_version;
	std::memcpy(spec.origin, p_bake.tree.origin, sizeof(spec.origin));
	gs_world_writer::WriterV4 writer;
	if (writer.begin_paged(p_path, p_scratch, spec, p_codec, gs_world_writer::Limits::unbounded()) != OK) {
		r_reason = writer.get_report().full_reason;
		return false;
	}
	LocalVector<gs_world_writer::V4Node> nodes;
	LocalVector<uint32_t> depth;
	nodes.resize(spec.node_count);
	depth.resize(spec.node_count);
	for (uint32_t i = 0; i < spec.node_count; ++i) {
		const auto &node = p_bake.tree.nodes[i];
		if (i && node.parent >= i) {
			r_reason = "in_core_non_bfs_parent";
			return false;
		}
		depth[i] = i ? depth[node.parent] + 1 : 0;
		gs_world_writer::V4BodyDesc desc;
		desc.segment.depth = depth[i];
		desc.is_leaf = node.is_leaf();
		p_bake.tree.node_cell_center(node, desc.cell_center);
		const uint64_t start = node.is_leaf() ? node.payload_first : node.payload_first - spec.leaf_splat_count;
		const auto &g = node.is_leaf() ? p_bake.leaf_gaussians : p_bake.tree.interior_gaussians;
		const auto &sh = node.is_leaf() ? p_bake.leaf_sh_high_order : p_bake.tree.interior_sh_high_order;
		if (start + node.payload_count > g.size() || (start + node.payload_count) * p_input.high > sh.size()) {
			r_reason = "in_core_payload_range";
			return false;
		}
		nodes[i].topology = node;
		if (writer.append_node(desc, g.ptr() + start, p_input.high ? sh.ptr() + start * p_input.high : nullptr, node.payload_count, nodes[i].body) != OK) {
			r_reason = writer.get_report().full_reason;
			return false;
		}
	}
	if (writer.finalize(nodes) != OK) {
		r_reason = writer.get_report().full_reason;
		return false;
	}
	return true;
}

// Independent canonical encode/decode of one in-core node. The V4 leaf frame
// round trip also applies to codec none (#1432), even when it changes one ULP.
inline bool expected_digest(const gs_hlod::BakeResult &p_bake, uint32_t p_index, const Input &p_input, bool p_gsq, String &r_digest, String &r_reason) {
	if (p_index >= p_bake.tree.nodes.size()) {
		r_reason = "in_core_node_range";
		return false;
	}
	const auto &node = p_bake.tree.nodes[p_index];
	if (!node.is_leaf() && node.payload_first < p_bake.tree.leaf_splat_count) {
		r_reason = "in_core_interior_range";
		return false;
	}
	const uint64_t start = node.is_leaf() ? node.payload_first : node.payload_first - p_bake.tree.leaf_splat_count;
	const auto &g = node.is_leaf() ? p_bake.leaf_gaussians : p_bake.tree.interior_gaussians;
	const auto &sh = node.is_leaf() ? p_bake.leaf_sh_high_order : p_bake.tree.interior_sh_high_order;
	if (!node.payload_count || start + node.payload_count > g.size() || (start + node.payload_count) * p_input.high > sh.size()) {
		r_reason = "in_core_payload_range";
		return false;
	}
	LocalVector<Gaussian> relative;
	relative.resize(node.payload_count);
	std::memcpy(relative.ptr(), g.ptr() + start, uint64_t(node.payload_count) * sizeof(Gaussian));
	double center[3];
	p_bake.tree.node_cell_center(node, center);
	if (node.is_leaf()) {
		for (Gaussian &splat : relative) {
			for (int axis = 0; axis < 3; ++axis) {
				splat.position[axis] = real_t(double(splat.position[axis]) - center[axis]);
			}
		}
	}
	const Vector3 *source_sh = p_input.high ? sh.ptr() + start * p_input.high : nullptr;
	LocalVector<Vector3> decoded_sh;
	if (p_gsq) {
		gs_gsq::EncodeParams params;
		LocalVector<uint8_t> page;
		gs_gsq::DecodeContext decoder;
		LocalVector<Gaussian> decoded;
		decoded.resize(node.payload_count);
		decoded_sh.resize(uint64_t(node.payload_count) * p_input.high);
		if (gs_world_writer::gsq_encode_params(gs_world_writer::gsq_v1_codec_params(), params, r_reason) != OK ||
				gs_gsq::encode_page(relative.ptr(), source_sh, node.payload_count, p_input.degree, params, page, r_reason) != OK ||
				decoder.decode_range(page.ptr(), page.size(), 0, node.payload_count, decoded.ptr(), p_input.high ? decoded_sh.ptr() : nullptr, r_reason) != OK) {
			return false;
		}
		relative = std::move(decoded);
		source_sh = p_input.high ? decoded_sh.ptr() : nullptr;
	}
	if (node.is_leaf()) {
		for (Gaussian &splat : relative) {
			for (int axis = 0; axis < 3; ++axis) {
				splat.position[axis] = real_t(double(splat.position[axis]) + center[axis]);
			}
		}
	}
	r_digest = p_gsq ? canonical_digest(relative.ptr(), source_sh, node.payload_count, p_input.degree) :
			raw_digest(relative.ptr(), source_sh, node.payload_count, p_input.high);
	return true;
}

inline Dictionary compare(const GaussianSplatHlodTree &p_tree, const gs_hlod::BakeResult &p_bake, const Input &p_input,
		const Array &p_manifest_nodes, bool p_gsq, String &r_reason, Array *r_digests = nullptr) {
	if (p_tree.nodes.size() != p_bake.tree.nodes.size()) {
		return divergence(0, "node_count", p_bake.tree.nodes.size(), p_tree.nodes.size());
	}
	if (p_manifest_nodes.size() != int(p_tree.nodes.size())) {
		return divergence(0, "manifest_node_count", p_tree.nodes.size(), p_manifest_nodes.size());
	}
	Dictionary first;
	for (uint32_t i = 0; i < p_tree.nodes.size(); ++i) {
		const Dictionary fields = node_difference(i, p_tree.nodes[i], p_bake.tree.nodes[i], false);
		keep_first(first, fields);
		String digest;
		if (!expected_digest(p_bake, i, p_input, p_gsq, digest, r_reason)) {
			return divergence(i, "in_core_encode_decode", "success", r_reason);
		}
		const Dictionary entry = p_manifest_nodes[i];
		if (r_digests) {
			Dictionary recorded = entry.duplicate(true);
			recorded["node_index"] = i;
			recorded["in_core_decoded_sha256"] = digest;
			r_digests->push_back(recorded);
		}
		if (String(entry.get("decoded_sha256", "")) != digest) {
			keep_first(first, divergence(i, "decoded_sha256", digest, entry.get("decoded_sha256", "")));
		}
		if (!first.is_empty() && !r_digests) {
			return first;
		}
	}
	return first;
}

inline Dictionary tree_difference(const GaussianSplatHlodTree &a, const GaussianSplatHlodTree &b) {
#define D2_REAL_TREE_FIELD(field) \
	if (a.field != b.field) { \
		return divergence(0, #field, b.field, a.field); \
	}
	D2_REAL_TREE_FIELD(leaf_splat_count);
	D2_REAL_TREE_FIELD(interior_splat_count);
	D2_REAL_TREE_FIELD(sh_high_order_count);
	D2_REAL_TREE_FIELD(bake_rule_version);
#undef D2_REAL_TREE_FIELD
	for (int axis = 0; axis < 3; ++axis) {
		if (a.origin[axis] != b.origin[axis]) {
			return divergence(0, "origin[" + itos(axis) + "]", b.origin[axis], a.origin[axis]);
		}
	}
	return {};
}

inline Array checkpoints(const AABB &p_bounds) {
	Array poses;
	const Vector3 c = p_bounds.get_center();
	const double radius = MAX(1.0, double(p_bounds.size.length()));
	for (int i = 0; i < 8; ++i) {
		const double angle = Math::TAU * i / 8;
		Array pose;
		for (double value : { double(c.x) + radius * std::sin(angle), double(c.y), double(c.z) + radius * std::cos(angle), 45.0 * i, 0.0 }) {
			pose.push_back(value);
		}
		Dictionary row;
		row["checkpoint"] = "C" + itos(i);
		row["pose"] = pose;
		poses.push_back(row);
	}
	return poses;
}

inline bool run_case(Run &r_run) {
	const String scan = OS::get_singleton()->get_environment("GS_D2_REAL_SCAN_PLY");
	const bool full = OS::get_singleton()->has_environment("GS_D2_REAL_SCAN_PLY");
	r_run.report["mode"] = full ? "real_scan" : "self_contained";
	if (full && !FileAccess::exists(scan)) {
		r_run.report["status"] = "NOT_RUN";
		r_run.report["reason_code"] = "real_scan_missing";
		CHECK_FALSE(FileAccess::exists(scan));
		return true;
	}
	if (DirAccess::make_dir_recursive_absolute(r_run.dir.path_join("scratch")) != OK) {
		return r_run.fail("temporary_directory_failed");
	}
	uint32_t stride = 7;
	uint32_t cap = 1000000;
	for (const char *name : { "GS_D2_REAL_SCAN_STRIDE", "GS_D2_REAL_SCAN_MAX_SPLATS" }) {
		const String text = OS::get_singleton()->get_environment(name);
		if (!text.is_empty()) {
			if (!text.is_valid_int() || text.to_int() <= 0 || uint64_t(text.to_int()) > UINT32_MAX) {
				return r_run.fail("subset_parameter_invalid");
			}
			if (String(name) == "GS_D2_REAL_SCAN_STRIDE") {
				stride = text.to_int();
			} else {
				cap = text.to_int();
			}
		}
	}
	r_run.report["stride"] = full ? stride : 1;
	r_run.report["subset_rule"] = full ? "keep original PLY vertex i iff i % " + itos(stride) + " == 0 (zero based, no truncation)" : "generated 70001-splat grid, all vertices";
	Input input;
	if (full) {
		const String subset = r_run.dir.path_join("subset.ply");
		if (!stride_ply(r_run, scan, subset, stride, cap)) {
			return false;
		}
		Ref<PLYLoader> loader;
		loader.instantiate();
		if (loader->load_file(subset) != OK || !capture(loader->get_gaussian_data(), input)) {
			return r_run.fail("subset_ply_decode_failed");
		}
		if (input.gaussians.size() != uint64_t(int64_t(r_run.report["subset_count"]))) {
			return r_run.fail("subset_count_changed_by_loader");
		}
		r_run.report["ply_subset_sha256"] = canonical_digest(input.gaussians.ptr(), input.high ? input.sh.ptr() : nullptr, input.gaussians.size(), input.degree);
	} else {
		input.gaussians.resize(70001);
		for (uint32_t i = 0; i < input.gaussians.size(); ++i) {
			Gaussian &g = input.gaussians[i];
			g.position = Vector3(1.01f + (i % 64), 1.03f + ((i / 64) % 64), 1.07f + i / 4096);
			g.scale = Vector3(0.01f, 0.012f, 0.014f);
			g.area = 1.0f;
			g.opacity = 0.25f + float(i % 17) / 32;
			g.sh_dc = Color(float(i % 31) / 32, 0.25f, 0.5f, 1);
			g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		}
		r_run.report["input_vertex_count"] = input.gaussians.size();
		r_run.report["subset_count"] = input.gaussians.size();
	}
	if (input.gaussians.size() <= 65536 || input.degree > 3) {
		return r_run.fail("subset_must_exercise_multiple_tiles_and_supported_sh");
	}
	// A generous input header encloses the original and quantized splat support.
	Vector3 lo = input.gaussians[0].position, hi = lo;
	for (const Gaussian &g : input.gaussians) {
		const float pad = 1 + 4 * MAX(g.scale.x, MAX(g.scale.y, g.scale.z));
		for (int a = 0; a < 3; ++a) {
			lo[a] = MIN(lo[a], g.position[a] - pad);
			hi[a] = MAX(hi[a], g.position[a] + pad);
		}
	}
	input.bounds = AABB(lo, hi - lo);
	const String plain = r_run.dir.path_join("input.gsplatworld");
	String reason;
	if (!write_input(plain, input, reason)) {
		r_run.report["detail"] = reason;
		return r_run.fail("plain_gsq_input_write_failed");
	}
	if (!full) {
		Ref<FileAccess> file = FileAccess::open(plain, FileAccess::READ);
		if (file.is_null()) {
			return r_run.fail("plain_gsq_input_stat_failed");
		}
		r_run.report["input_file_size_bytes"] = int64_t(file->get_length());
		r_run.report["input_sha256"] = FileAccess::get_sha256(plain);
	}
	// Release PLY/synthetic decoded bytes before loading the exact GSQ stream.
	input.gaussians.clear();
	input.sh.clear();
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error error = FAILED;
	{
		Ref<GaussianSplatWorld> world = loader.load_resident(plain, &error);
		if (error != OK || world.is_null() || !capture(world->get_gaussian_data(), input)) {
			return r_run.fail("plain_gsq_input_decode_failed");
		}
		input.bounds = world->get_bounds();
	}
	r_run.report["subset_sha256"] = canonical_digest(input.gaussians.ptr(), input.high ? input.sh.ptr() : nullptr, input.gaussians.size(), input.degree);
	r_run.report["subset_digest_definition"] = "gs_gsq::decoded_digest of exactly the decoded plain GSQ input used by both bakes";
	r_run.report["sh_degree"] = input.degree;
	gs_hlod::BakeParams params;
	params.mode = gs_hlod::BakeParams::MERGE_ONLY;
	gs_hlod::BakeResult bake;
	if (!gs_hlod::bake_world(input.view(), params, bake, &reason, &error) || error != OK || !gs_hlod_validate_tree(bake.tree, &reason)) {
		r_run.report["detail"] = reason;
		return r_run.fail("in_core_bake_failed");
	}
	Array codecs;
	Dictionary main_difference;
	Array main_nodes;
	GaussianSplatHlodTree main_tree;
	for (const char *codec : { "gsq", "none" }) {
		Dictionary row;
		row["codec"] = codec;
		row["status"] = "FAIL";
		const bool gsq = String(codec) == "gsq";
		const String output = r_run.dir.path_join(String(codec) + ".gsplatworld");
		const String reference = r_run.dir.path_join(String(codec) + "-in-core.gsplatworld");
		Dictionary limits;
		limits["max_ram_bytes"] = int64_t(4) << 30;
		limits["max_scratch_bytes"] = int64_t(16) << 30;
		limits["max_output_bytes"] = int64_t(8) << 30;
		limits["deadline_sec"] = 2400;
		Dictionary options;
		options["scratch_dir"] = r_run.dir.path_join("scratch");
		options["source_fingerprint"] = r_run.report["subset_sha256"];
		options["worker_count"] = 1;
		options["max_tile_splats"] = 65536;
		options["codec"] = codec;
		options["limits"] = limits;
		const Dictionary built = bound_call("build", plain, output, options);
		row["build"] = built;
		codecs.push_back(row);
		r_run.report["codecs"] = codecs;
		if (int(built.get("error", FAILED)) != OK || int64_t(built.get("tiles_planned", 0)) <= 1 || int64_t(built.get("tiles_baked", 0)) <= 1) {
			return r_run.fail("bound_build_failed_or_not_tiled");
		}
		CHECK(int64_t(built["tiles_planned"]) > 1);
		CHECK(int64_t(built["tiles_baked"]) > 1);
		if (gsq) {
			r_run.report["tiles_planned"] = built["tiles_planned"];
			r_run.report["tiles_baked"] = built["tiles_baked"];
		}
		Ref<GaussianSplatHlodPagedSource> source;
		source.instantiate();
		GaussianSplatHlodTree tree;
		Dictionary metadata;
		if (source->open(output, tree, metadata, reason) != OK) {
			return r_run.fail("auto_tree_reopen_failed");
		}
		const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(output));
		const Array nodes = manifest.get("nodes", Array());
		row["nodes"] = nodes;
		row["node_count"] = tree.nodes.size();
		row["leaf_splat_count"] = tree.leaf_splat_count;
		row["interior_splat_count"] = tree.interior_splat_count;
		row["tree_sha"] = manifest.get("tree_sha", "");
		row["file_sha256"] = FileAccess::get_sha256(output);
		Array independent_digests;
		Dictionary difference = compare(tree, bake, input, nodes, gsq, reason, &independent_digests);
		if (independent_digests.size() == nodes.size()) {
			row["nodes"] = independent_digests;
		}
		keep_first(difference, tree_difference(tree, bake.tree));
		if (difference.is_empty() && String(manifest.get("output_file_sha256", "")) != String(row["file_sha256"])) {
			difference = divergence(0, "manifest_file_sha256", row["file_sha256"], manifest.get("output_file_sha256", ""));
		}
		r_run.report["first_divergence"] = difference;
		if (!write_in_core(reference, r_run.dir.path_join("scratch"), input, bake,
				gsq ? gs_world_writer::gsq_v1_codec_params() : gs_world_writer::CodecParams(), reason)) {
			r_run.report["detail"] = reason;
			return r_run.fail("independent_in_core_v4_write_failed");
		}
		row["in_core_file_sha256"] = FileAccess::get_sha256(reference);
		Ref<GaussianSplatHlodPagedSource> expected_source;
		expected_source.instantiate();
		GaussianSplatHlodTree expected_tree;
		if (expected_source->open(reference, expected_tree, metadata, reason) != OK) {
			return r_run.fail("independent_in_core_v4_reopen_failed");
		}
		if (expected_tree.nodes.size() != tree.nodes.size() || nodes.size() != int(tree.nodes.size()) || source->get_nodes().size() != int(tree.nodes.size()) || expected_source->get_nodes().size() != int(tree.nodes.size())) {
			if (difference.is_empty()) {
				difference = divergence(0, "independent_in_core_node_count", expected_tree.nodes.size(), tree.nodes.size());
			}
			row["first_divergence"] = difference;
			codecs[codecs.size() - 1] = row;
			r_run.report["codecs"] = codecs;
			r_run.report["first_divergence"] = difference;
			return r_run.fail("identity_mismatch");
		}
		uint32_t leaves = 0;
		uint32_t expected_leaves = 0;
		for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
			leaves += tree.nodes[i].is_leaf();
			expected_leaves += expected_tree.nodes[i].is_leaf();
			keep_first(difference, node_difference(i, tree.nodes[i], expected_tree.nodes[i], true));
			Vector<Gaussian> actual;
			Vector<Vector3> actual_sh;
			if (source->read_node_payload(i, actual, actual_sh, reason) != OK || actual.size() != int(tree.nodes[i].payload_count) || actual_sh.size() != int(uint64_t(actual.size()) * input.high)) {
				keep_first(difference, divergence(i, "reopened_payload_read_or_count", "valid node payload", reason));
				r_run.report["first_divergence"] = main_difference.is_empty() ? difference : main_difference;
				return r_run.fail("auto_tree_node_decode_failed");
			}
			const String digest = gsq ? canonical_digest(actual.ptr(), input.high ? actual_sh.ptr() : nullptr, actual.size(), input.degree) :
					raw_digest(actual.ptr(), input.high ? actual_sh.ptr() : nullptr, actual.size(), input.high);
			if (nodes.size() != int(tree.nodes.size())) {
				return r_run.fail("manifest_node_count_invalid");
			}
			const Dictionary entry = nodes[i];
			if (digest != String(entry.get("decoded_sha256", ""))) {
				keep_first(difference, divergence(i, "reopened_decoded_sha256", entry.get("decoded_sha256", ""), digest));
			}
			const String stored = String::hex_encode_buffer(source->get_nodes()[i].digest, 32);
			if (stored != String(entry.get("stored_sha256", ""))) {
				keep_first(difference, divergence(i, "stored_sha256", entry.get("stored_sha256", ""), stored));
			}
			const String in_core_stored = String::hex_encode_buffer(expected_source->get_nodes()[i].digest, 32);
			if (stored != in_core_stored) {
				keep_first(difference, divergence(i, "stored_sha256_vs_in_core", in_core_stored, stored));
			}
			CHECK(actual.size() > 0);
		}
		// Both paths use the same spec/bounds and empty metadata. WriterV4 assigns
		// final records in BFS order (io/gs_world_writer_v4.cpp:1459-1503), independent
		// of scratch layout. The source fingerprint is manifest-only
		// (core/gaussian_splat_hlod_auto_tree.cpp:1502-1508), so file bytes must agree.
		// Preserve a concrete per-node divergence before diagnosing the whole file.
		if (difference.is_empty() && row["file_sha256"] != row["in_core_file_sha256"]) {
			difference = divergence(0, "file_sha256_vs_in_core", row["in_core_file_sha256"], row["file_sha256"]);
		}
		row["leaf_node_count"] = leaves;
		row["interior_node_count"] = tree.nodes.size() - leaves;
		row["in_core_leaf_node_count"] = expected_leaves;
		row["in_core_interior_node_count"] = expected_tree.nodes.size() - expected_leaves;
		Array origin;
		for (int a = 0; a < 3; ++a) {
			origin.push_back(tree.origin[a]);
		}
		row["origin"] = origin;
		if (leaves != expected_leaves) {
			keep_first(difference, divergence(0, "leaf_node_count", expected_leaves, leaves));
		}
		r_run.report["first_divergence"] = difference;
		Dictionary camera;
		camera["width"] = 1920;
		camera["height"] = 1080;
		camera["fov_deg"] = 107.5;
		camera["keep"] = "width";
		camera["near_m"] = 0.05;
		camera["far_m"] = MAX(1000.0, double(input.bounds.size.length()) * 8);
		camera["tau_pixels"] = 2.0;
		camera["drawn_budget_splats"] = int64_t(input.gaussians.size()) * 2;
		camera["leaves_only"] = false;
		row["oracle_camera"] = camera;
		const Array poses = checkpoints(input.bounds);
		const Dictionary cut = bound_call("oracle_cut", output, poses, camera);
		const Dictionary reference_cut = bound_call("oracle_cut", reference, poses, camera);
		row["oracle_cut"] = cut;
		row["in_core_oracle_cut"] = reference_cut;
		if (int(cut.get("error", FAILED)) != OK || int(reference_cut.get("error", FAILED)) != OK ||
				Array(cut.get("poses", Array())).size() != 8 || Array(reference_cut.get("poses", Array())).size() != 8) {
			return r_run.fail("oracle_cut_not_executed");
		}
		const Array cut_rows = cut["poses"];
		uint64_t selected = 0;
		for (int i = 0; i < cut_rows.size(); ++i) {
			const Dictionary cut_row = cut_rows[i];
			const String status = cut_row.get("status", "");
			if (status != "COMPLETE" && status != "EMPTY") {
				return r_run.fail("oracle_cut_invalid_status");
			}
			selected += uint64_t(int64_t(cut_row.get("selected_count", 0)));
		}
		if (!selected) {
			return r_run.fail("oracle_cut_no_selected_nodes");
		}
		if (difference.is_empty() && cut != reference_cut) {
			difference = divergence(0, "oracle_cut_C0_C7", reference_cut, cut);
		}
		r_run.report["first_divergence"] = difference;
		// A separate bound build exercises independent execution and publication.
		const String repeat = r_run.dir.path_join(String(codec) + "-repeat.gsplatworld");
		const Dictionary repeated = bound_call("build", plain, repeat, options);
		const Dictionary repeat_manifest = json_file(gs_world_generator::manifest_path_for(repeat));
		row["repeat_tree_sha"] = repeat_manifest.get("tree_sha", "");
		row["repeat_file_sha256"] = FileAccess::get_sha256(repeat);
		if (int(repeated.get("error", FAILED)) != OK) {
			return r_run.fail("independent_repeat_build_failed");
		}
		if (String(row["tree_sha"]).length() != 64 || row["tree_sha"] != row["repeat_tree_sha"]) {
			keep_first(difference, divergence(0, "independent_build_tree_sha", row["tree_sha"], row["repeat_tree_sha"]));
		}
		if (row["file_sha256"] != row["repeat_file_sha256"]) {
			keep_first(difference, divergence(0, "independent_build_file_sha256", row["file_sha256"], row["repeat_file_sha256"]));
		}
		row["first_divergence"] = difference;
		row["status"] = difference.is_empty() ? "PASS" : "FAIL";
		row["checks"] = "raw in-core unchanged fields; independently serialized in-core every node field/bounds; manifest stored and decoded SHA; independent per-node encode/decode; counts/origin; independent repeat tree/file SHA; CPU C0..C7 on both packages";
		codecs[codecs.size() - 1] = row;
		r_run.report["codecs"] = codecs;
		if (main_difference.is_empty() && !difference.is_empty()) {
			main_difference = difference;
		}
		if (gsq) {
			main_nodes = nodes;
			main_tree = std::move(tree);
		}
	}
	// Release the unperturbed BakeResult before the negative bake. Perturb one
	// component of exactly one input splat by 1m (above GSQ's quantization), never
	// the auto-tree input or output. The same comparison must identify a node.
	bake = gs_hlod::BakeResult();
	const real_t before = input.gaussians[0].position.x;
	input.gaussians[0].position.x = before + 1.0f;
	if (input.gaussians[0].position.x == before) {
		input.gaussians[0].position.x = std::nextafter(before, std::numeric_limits<real_t>::infinity());
	}
	gs_hlod::BakeResult perturbed;
	if (!gs_hlod::bake_world(input.view(), params, perturbed, &reason, &error) || error != OK) {
		return r_run.fail("negative_control_bake_failed");
	}
	const Dictionary control_difference = compare(main_tree, perturbed, input, main_nodes, true, reason);
	if (String(control_difference.get("field", "")) == "in_core_encode_decode") {
		r_run.report["detail"] = reason;
		return r_run.fail("negative_control_comparison_error");
	}
	Dictionary control;
	control["mutation"] = "decoded input splat 0 position.x += 1 metre; next representable float if addition rounds away";
	control["detected"] = !control_difference.is_empty();
	control["first_divergence"] = control_difference;
	r_run.report["negative_control"] = control;
	r_run.report["first_divergence"] = main_difference;
	CHECK_FALSE(control_difference.is_empty());
	if (control_difference.is_empty() || int64_t(control_difference.get("node_index", -1)) < 0 || String(control_difference.get("field", "")).is_empty()) {
		return r_run.fail("negative_control_not_detected");
	}
	if (!main_difference.is_empty()) {
		return r_run.fail("identity_mismatch");
	}
	r_run.report["status"] = "PASS";
	r_run.report["reason_code"] = "identity_verified";
	return true;
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Real-scan auto-tree equals in-core merge with negative control") {
	Run run;
	const bool ok = run_case(run);
	INFO("D2 identity reason=", doctest::String(String(run.report.get("reason_code", "missing")).utf8().get_data()));
	INFO("first_divergence=", doctest::String(JSON::stringify(run.report.get("first_divergence", Dictionary())).utf8().get_data()));
	CHECK(ok);
}

} // namespace TestHlodD2RealScan
