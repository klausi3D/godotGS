#pragma once

// D2-3 phase 1: host-only runtime-red contract tests. modules/SCsub discovers
// tests/*.h; [WorldIO] selects the existing CPU lane. No external fixtures.
#include "test_macros.h"
#include "gs_gsq_bounds_test_helpers.h"

#include "../core/gaussian_splat_hlod_auto_tree.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gaussian_splat_world_paged.h"
#include "../io/gs_auto_tree_source.h"
#include "../io/gs_world_generator.h"
#include "../io/gsq/gsq_codec.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace TestHlodD2AutoTree {

#define D2_AUTO_NEED(condition) \
	do { \
		if (!(condition)) { \
			FAIL("D2 auto-tree precondition failed: ", #condition); \
			return; \
		} \
	} while (0)

// Only called on this test's newly created temporary directory, after assertions.
inline void remove_tree(const String &p_path) {
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
			remove_tree(child);
		} else {
			DirAccess::remove_absolute(child);
		}
	}
	dir->list_dir_end();
	dir.unref();
	DirAccess::remove_absolute(p_path);
}

inline bool directory_empty(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return false;
	}
	bool empty = true;
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		empty = empty && (name == "." || name == "..");
	}
	dir->list_dir_end();
	return empty;
}

inline bool directory_names(const String &p_path, PackedStringArray &r_names) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return false;
	}
	r_names.clear();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name != "." && name != "..") {
			r_names.push_back(name);
		}
	}
	dir->list_dir_end();
	r_names.sort();
	return true;
}

inline bool write_bytes(const String &p_path, const PackedByteArray &p_bytes) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_buffer(p_bytes.ptr(), p_bytes.size());
	return file->get_error() == OK;
}

inline Dictionary json_file(const String &p_path) {
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(p_path));
	return parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
}

inline bool hex_digest(const String &p_digest) {
	if (p_digest.length() != 64) {
		return false;
	}
	for (int i = 0; i < 64; ++i) {
		if (!((p_digest[i] >= '0' && p_digest[i] <= '9') || (p_digest[i] >= 'a' && p_digest[i] <= 'f'))) {
			return false;
		}
	}
	return true;
}

inline String payload_digest(const Gaussian *p_g, const Vector3 *p_sh, uint32_t p_count, uint32_t p_high) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(reinterpret_cast<const uint8_t *>(p_g), size_t(p_count) * sizeof(Gaussian));
	if (p_high) {
		hash.update(reinterpret_cast<const uint8_t *>(p_sh), size_t(p_count) * p_high * sizeof(Vector3));
	}
	uint8_t digest[32];
	hash.finish(digest);
	return String::hex_encode_buffer(digest, 32);
}

// Independent hash oracle: no auto-tree production helper participates.
inline String tree_sha(const Dictionary &p_manifest) {
	Dictionary params = Dictionary(p_manifest.get("build_params", Dictionary())).duplicate(true);
	params.erase("worker_count");
	// Godot JSON parses every number as double. Restore the documented schema
	// types before canonical serialization (16384 and 16384.0 hash differently).
	params["leaf_max"] = int64_t(params.get("leaf_max", 0));
	params["max_tile_splats"] = int64_t(params.get("max_tile_splats", 0));
	Dictionary interior = params.get("interior", Dictionary());
	for (const char *field : { "merge_grid", "node_max_splats", "max_eps_steps" }) {
		interior[field] = int64_t(interior.get(field, 0));
	}
	for (const char *field : { "reduction", "eps_growth" }) {
		interior[field] = double(interior.get(field, 0));
	}
	params["interior"] = interior;
	Dictionary key;
	key["source_fingerprint"] = p_manifest.get("source_fingerprint", "");
	key["build_params"] = params;
	Dictionary codec = Dictionary(p_manifest.get("codec", Dictionary())).duplicate(true);
	if (String(codec.get("codec", "")) == "gsq") {
		for (const char *field : { "codec_id", "opacity_bits", "zstd_level" }) {
			codec[field] = int64_t(codec.get(field, 0));
		}
		Array bits = codec.get("sh_bits", Array());
		for (int i = 0; i < bits.size(); ++i) {
			bits[i] = int64_t(bits[i]);
		}
		codec["sh_bits"] = bits;
		codec["pos_step_m"] = double(codec.get("pos_step_m", 0));
	}
	key["codec"] = codec;
	key["input_file_sha256"] = p_manifest.get("input_file_sha256", "");
	return JSON::stringify(key, "", true, true).sha256_text();
}

struct Fixture {
	String dir;
	String scratch;
	String input;
	gs_world_generator::Report generated;

	// p_near_plane selects the plain chunks that touch the x = 0 or z = 0 plane
	// instead: their splats sit close to a coordinate plane, where a leaf's
	// node-relative float32 position does not reconstruct bit-exactly (#1432).
	bool create(uint32_t p_total = 32768, uint32_t p_codec = gs_world_writer::kCodecGsqV1, int64_t p_seed_delta = 0, bool p_near_plane = false) {
		static uint32_t sequence = 0;
		dir = OS::get_singleton()->get_temp_path().path_join("gs_d2_auto_" + itos(OS::get_singleton()->get_ticks_usec()) + "_" + itos(sequence++));
		scratch = dir.path_join("scratch");
		input = dir.path_join("input.gsplatworld");
		if (DirAccess::make_dir_recursive_absolute(scratch) != OK) {
			return false;
		}
		gs_world_generator::Params params;
		if (gs_world_generator::params_for_tier("T-smoke", params) != OK) {
			return false;
		}
		params.total_splats = p_total;
		params.max_chunk_splats = 4096;
		params.seed += p_seed_delta;
		params.town_count = 0;
		params.foliage_count = 0;
		params.beacon_cells.clear();
		params.wall_length_m = 2; // Generator requires a corridor; selection excludes it.
		params.wall_density_per_m2 = 1;
		params.plain_y_jitter_q = 0;
		gs_world_generator::Plan plan;
		String reason;
		if (gs_world_generator::build_plan(params, plan, reason) != OK) {
			FAIL("Synthetic plan refused: ", reason.utf8().get_data());
			return false;
		}
		gs_world_generator::GenerateOptions options;
		options.codec_id = p_codec;
		// World is centred on zero. Keep nontrivial degree-3 SH and both signs,
		// but avoid tiny coordinates relative to V4 leaf-cell centres. A full
		// 262144-splat plan yields about 147k selected splats, several 64k tiles.
		for (uint32_t i = 0; i < plan.chunks.size(); ++i) {
			const auto &chunk = plan.chunks[i];
			if (p_near_plane) {
				if ((chunk.cell.x0 == 0 || chunk.cell.x1 == 0 || chunk.cell.z0 == 0 || chunk.cell.z1 == 0) &&
						chunk.class_counts[gs_world_generator::CLASS_PLAIN] == chunk.count) {
					options.selection.push_back(i);
				}
				continue;
			}
			if ((chunk.cell.x1 <= -32 || chunk.cell.x0 >= 32) &&
					(chunk.cell.z1 <= -32 || chunk.cell.z0 >= 32) &&
					chunk.class_counts[gs_world_generator::CLASS_PLAIN] == chunk.count) {
				options.selection.push_back(i);
			}
		}
		if (options.selection.is_empty()) {
			return false;
		}
		return gs_world_generator::generate_world(params, input, options, generated) == OK;
	}

	Dictionary options(uint32_t p_workers = 1) const {
		Dictionary limits;
		limits["max_ram_bytes"] = int64_t(2) << 30;
		limits["max_scratch_bytes"] = int64_t(1) << 30;
		limits["max_output_bytes"] = int64_t(1) << 30;
		limits["deadline_sec"] = 45;
		Dictionary out;
		out["scratch_dir"] = scratch;
		out["source_fingerprint"] = String("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
		out["worker_count"] = p_workers;
		out["max_tile_splats"] = 65536;
		out["codec"] = "none";
		out["limits"] = limits;
		return out;
	}

	~Fixture() { remove_tree(dir); }
};

struct Resident {
	gs_hlod::TiledBakeChunk payload;
	uint32_t high = 0;
	uint32_t first = 0;
	bool load(const Fixture &p_fixture) {
		ResourceFormatLoaderGaussianSplatWorld loader;
		Error error = FAILED;
		Ref<GaussianSplatWorld> world = loader.load_resident(p_fixture.input, &error);
		if (error != OK || world.is_null()) {
			return false;
		}
		Ref<GaussianData> data = world->get_gaussian_data();
		return data.is_valid() && data->capture_chunk_snapshot(0, uint32_t(p_fixture.generated.splat_count),
				payload.gaussians, payload.sh_high_order, first, high);
	}
	gs_hlod::BakeInput view() const {
		return { payload.gaussians.ptr(), payload.sh_high_order.ptr(), payload.gaussians.size(), high, first };
	}
};

inline void same_node(const GaussianSplatHlodNode &a, const GaussianSplatHlodNode &b) {
	CHECK(a.cell.e == b.cell.e);
	CHECK(a.cell.ix == b.cell.ix);
	CHECK(a.cell.iy == b.cell.iy);
	CHECK(a.cell.iz == b.cell.iz);
	CHECK(a.aabb_min == b.aabb_min);
	CHECK(a.aabb_max == b.aabb_max);
	CHECK(a.radius == b.radius);
	CHECK(a.geometric_error == b.geometric_error);
	CHECK(a.error_chosen == b.error_chosen);
	CHECK(a.error_rejected == b.error_rejected);
	CHECK(a.parent == b.parent);
	CHECK(a.first_child == b.first_child);
	CHECK(a.child_count == b.child_count);
	CHECK(a.height == b.height);
	CHECK(a.payload_first == b.payload_first);
	CHECK(a.payload_count == b.payload_count);
	CHECK(a.flags == b.flags);
	CHECK(a.overlap_footprint == b.overlap_footprint);
}

inline void success_report(const Dictionary &p_report) {
	CHECK(int(p_report.get("error", FAILED)) == OK);
	CHECK(String(p_report.get("reason", "missing")).is_empty());
	CHECK(int64_t(p_report.get("wall_usec", 0)) > 0);
	CHECK(int64_t(p_report.get("peak_reserved_bytes", 0)) > 0);
	// Process RSS is optional only when the OS cannot supply it, explicitly marked.
	D2_AUTO_NEED(p_report.has("process_peak_memory_available"));
	D2_AUTO_NEED(p_report.has("process_peak_memory_bytes"));
	if (bool(p_report["process_peak_memory_available"])) {
		CHECK(int64_t(p_report["process_peak_memory_bytes"]) > 0);
	} else {
		CHECK(int64_t(p_report["process_peak_memory_bytes"]) == 0);
	}
	CHECK(int64_t(p_report.get("nodes_verified", 0)) == int64_t(p_report.get("node_count", -1)));
	CHECK(int64_t(p_report.get("node_count", 0)) > 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] GSQ adapter preserves resident bytes and checks every page") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create());
	Resident resident;
	D2_AUTO_NEED(resident.load(fixture));
	GsqAutoTreeSource source;
	String reason;
	D2_AUTO_NEED(source.open(fixture.input, &reason) == OK);
	const auto info = source.get_info();
	D2_AUTO_NEED(info.chunk_count > 1);
	CHECK(info.splat_count == fixture.generated.splat_count);
	CHECK(info.sh_first_order_count == resident.first);
	CHECK(info.sh_high_order_count == resident.high);
	D2_AUTO_NEED(resident.high > 0);
	std::vector<uint8_t> seen(info.splat_count, 0);
	for (uint32_t i = 0; i < info.chunk_count; ++i) {
		gs_hlod::TiledBakeChunkInfo desc;
		gs_hlod::TiledBakeChunk chunk;
		D2_AUTO_NEED(source.get_chunk_info(i, desc, &reason) == OK);
		D2_AUTO_NEED(desc.splat_count > 0);
		D2_AUTO_NEED(uint64_t(desc.first_splat) + desc.splat_count <= resident.payload.gaussians.size());
		D2_AUTO_NEED(uint64_t(desc.first_splat) + desc.splat_count <= seen.size());
		D2_AUTO_NEED(source.read_chunk(i, chunk, &reason) == OK);
		D2_AUTO_NEED(chunk.gaussians.size() == desc.splat_count);
		D2_AUTO_NEED(chunk.sh_high_order.size() == uint64_t(desc.splat_count) * resident.high);
		CHECK(std::memcmp(chunk.gaussians.ptr(), resident.payload.gaussians.ptr() + desc.first_splat, desc.splat_count * sizeof(Gaussian)) == 0);
		CHECK(std::memcmp(chunk.sh_high_order.ptr(), resident.payload.sh_high_order.ptr() + uint64_t(desc.first_splat) * resident.high,
				uint64_t(desc.splat_count) * resident.high * sizeof(Vector3)) == 0);
		for (uint32_t j = 0; j < desc.splat_count; ++j) {
			CHECK(seen[desc.first_splat + j]++ == 0);
			const Vector3 pos = chunk.gaussians[j].position;
			for (int axis = 0; axis < 3; ++axis) {
				CHECK(pos[axis] >= desc.centre_bounds.position[axis]);
				CHECK(pos[axis] <= desc.centre_bounds.get_end()[axis]);
			}
		}
	}
	CHECK(std::count(seen.begin(), seen.end(), uint8_t(1)) == info.splat_count);
	const auto stats = source.get_stats();
	CHECK(stats.page_count == fixture.generated.page_count);
	CHECK(stats.page_count > 0);
	CHECK(stats.pages_decoded == stats.page_count);
	CHECK(stats.pages_decoded_digest_checked == stats.page_count);
	CHECK(stats.failures == 0);
	CHECK(stats.last_reason.is_empty());

	// Simultaneous reads of the same chunk must own independent decoder output.
	gs_hlod::TiledBakeChunk a, b;
	Error ea = FAILED, eb = FAILED;
	std::thread first([&]() { ea = source.read_chunk(0, a); });
	std::thread second([&]() { eb = source.read_chunk(0, b); });
	first.join();
	second.join();
	D2_AUTO_NEED(ea == OK && eb == OK);
	D2_AUTO_NEED(!a.gaussians.is_empty() && a.gaussians.size() == b.gaussians.size());
	D2_AUTO_NEED(!a.sh_high_order.is_empty() && a.sh_high_order.size() == b.sh_high_order.size());
	CHECK(a.gaussians.ptr() != b.gaussians.ptr());
	CHECK(a.sh_high_order.ptr() != b.sh_high_order.ptr());
	CHECK(std::memcmp(a.gaussians.ptr(), b.gaussians.ptr(), a.gaussians.size() * sizeof(Gaussian)) == 0);
	CHECK(std::memcmp(a.sh_high_order.ptr(), b.sh_high_order.ptr(), a.sh_high_order.size() * sizeof(Vector3)) == 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] GSQ adapter corruption forcing control fails closed") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create());
	const Array chunks = json_file(fixture.generated.manifest_path).get("chunks", Array());
	D2_AUTO_NEED(!chunks.is_empty());
	const Dictionary run = chunks[0];
	const int64_t offset = int64_t(run.get("pages_offset", -1)) + int64_t(run.get("pages_bytes", 0)) - 1;
	PackedByteArray bytes = FileAccess::get_file_as_bytes(fixture.input);
	D2_AUTO_NEED(offset >= 0 && offset < bytes.size());
	bytes.write[offset] ^= 0x40; // stored payload byte, not padding or directory
	const String corrupt = fixture.dir.path_join("corrupt.gsplatworld");
	D2_AUTO_NEED(write_bytes(corrupt, bytes));
	GsqAutoTreeSource source;
	String reason;
	D2_AUTO_NEED(source.open(corrupt, &reason) == OK); // directory is still valid
	bool refused = false;
	for (uint32_t i = 0; i < source.get_info().chunk_count; ++i) {
		gs_hlod::TiledBakeChunk chunk;
		const Error error = source.read_chunk(i, chunk, &reason);
		if (error != OK) {
			CHECK(error == ERR_FILE_CORRUPT);
			CHECK(reason.contains("gsq_digest_mismatch"));
			CHECK(chunk.gaussians.is_empty());
			CHECK(chunk.sh_high_order.is_empty());
			refused = true;
			break;
		}
	}
	CHECK(refused);
	CHECK(source.get_stats().failures > 0);
	CHECK(source.get_stats().last_reason.contains("gsq_digest_mismatch"));

	const String output = fixture.dir.path_join("refused.gsplatworld");
	PackedStringArray before, after;
	D2_AUTO_NEED(directory_names(fixture.dir, before));
	const Dictionary report = GaussianSplatHlodAutoTree::build(corrupt, output, fixture.options());
	CHECK(int(report.get("error", OK)) == ERR_FILE_CORRUPT);
	CHECK(String(report.get("reason", "")).contains("gsq_digest_mismatch"));
	CHECK_FALSE(FileAccess::exists(output));
	CHECK_FALSE(FileAccess::exists(gs_world_generator::manifest_path_for(output)));
	CHECK(directory_empty(fixture.scratch));
	D2_AUTO_NEED(directory_names(fixture.dir, after));
	CHECK(after == before); // includes unpublished output/manifest temp leaks
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] GSQ to codec-none V4 equals in-core merge and records verified metrics") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create(262144));
	D2_AUTO_NEED(fixture.generated.splat_count > 2 * 65536);
	Resident resident;
	D2_AUTO_NEED(resident.load(fixture));
	gs_hlod::BakeResult oracle;
	String reason;
	Error error = OK;
	D2_AUTO_NEED(gs_hlod::bake_world(resident.view(), gs_hlod::BakeParams(), oracle, &reason, &error));
	D2_AUTO_NEED(error == OK);
	D2_AUTO_NEED(gs_hlod_validate_tree(oracle.tree, &reason));
	D2_AUTO_NEED(oracle.leaf_gaussians.size() == oracle.tree.leaf_splat_count);
	D2_AUTO_NEED(oracle.leaf_sh_high_order.size() == uint64_t(oracle.tree.leaf_splat_count) * resident.high);
	D2_AUTO_NEED(oracle.tree.interior_gaussians.size() == oracle.tree.interior_splat_count);
	D2_AUTO_NEED(oracle.tree.interior_sh_high_order.size() == uint64_t(oracle.tree.interior_splat_count) * resident.high);
	// Fixture proof: V4 must be able to preserve these GSQ-decoded leaves exactly.
	for (const auto &node : oracle.tree.nodes) {
		if (!node.is_leaf()) {
			continue;
		}
		double center[3];
		oracle.tree.node_cell_center(node, center);
		for (uint32_t j = 0; j < node.payload_count; ++j) {
			const Vector3 pos = oracle.leaf_gaussians[node.payload_first + j].position;
			for (int axis = 0; axis < 3; ++axis) {
				D2_AUTO_NEED(float(double(float(double(pos[axis]) - center[axis])) + center[axis]) == pos[axis]);
			}
		}
	}
	const String output = fixture.dir.path_join("tree.gsplatworld");
	const Dictionary report = GaussianSplatHlodAutoTree::build(fixture.input, output, fixture.options());
	D2_AUTO_NEED(int(report.get("error", FAILED)) == OK);
	success_report(report);
	CHECK(int64_t(report.get("tiles_baked", 0)) > 1);
	CHECK(int64_t(report.get("stitch_levels", 0)) > 0);
	CHECK(directory_empty(fixture.scratch));
	const String manifest_path = gs_world_generator::manifest_path_for(output);
	CHECK(String(report.get("manifest_path", "")) == manifest_path);
	const Dictionary manifest = json_file(manifest_path);
	D2_AUTO_NEED(!manifest.is_empty());
	CHECK(String(manifest.get("schema", "")) == "godotgs-auto-tree-manifest/1");
	CHECK(String(Dictionary(manifest.get("codec", Dictionary())).get("codec", "")) == "none");
	CHECK(String(manifest.get("output_file_sha256", "")) == FileAccess::get_sha256(output));
	const Dictionary recorded_report = manifest.get("report", Dictionary());
	CHECK(recorded_report == Dictionary(JSON::parse_string(JSON::stringify(report, "", true, true))));

	Ref<GaussianSplatHlodPagedSource> paged;
	paged.instantiate();
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	D2_AUTO_NEED(paged->open(output, tree, metadata, reason) == OK);
	D2_AUTO_NEED(tree.nodes.size() == oracle.tree.nodes.size());
	D2_AUTO_NEED(paged->get_nodes().size() == int(tree.nodes.size()));
	CHECK(tree.leaf_splat_count == oracle.tree.leaf_splat_count);
	CHECK(tree.interior_splat_count == oracle.tree.interior_splat_count);
	CHECK(tree.sh_high_order_count == oracle.tree.sh_high_order_count);
	CHECK(tree.bake_rule_version == 1);
	CHECK(std::memcmp(tree.origin, oracle.tree.origin, sizeof(tree.origin)) == 0);
	CHECK(String(manifest.get("content_generation", "")) == String::num_uint64(paged->get_info().content_generation));
	const Array nodes = manifest.get("nodes", Array());
	D2_AUTO_NEED(nodes.size() == int(tree.nodes.size()));
	for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
		const auto &expected = oracle.tree.nodes[i];
		same_node(tree.nodes[i], expected);
		const uint64_t start = expected.is_leaf() ? expected.payload_first : expected.payload_first - oracle.tree.leaf_splat_count;
		const Gaussian *g = expected.is_leaf() ? oracle.leaf_gaussians.ptr() + start : oracle.tree.interior_gaussians.ptr() + start;
		const Vector3 *sh = expected.is_leaf() ? oracle.leaf_sh_high_order.ptr() + start * resident.high : oracle.tree.interior_sh_high_order.ptr() + start * resident.high;
		Vector<Gaussian> actual;
		Vector<Vector3> actual_sh;
		D2_AUTO_NEED(paged->read_node_payload(i, actual, actual_sh, reason) == OK);
		D2_AUTO_NEED(actual.size() == int(expected.payload_count));
		D2_AUTO_NEED(actual_sh.size() == int(expected.payload_count * resident.high));
		CHECK(std::memcmp(actual.ptr(), g, expected.payload_count * sizeof(Gaussian)) == 0);
		CHECK(std::memcmp(actual_sh.ptr(), sh, expected.payload_count * resident.high * sizeof(Vector3)) == 0);
		const Dictionary entry = nodes[i];
		CHECK(String(entry.get("decoded_sha256", "")) == payload_digest(g, sh, expected.payload_count, resident.high));
		const auto &record = paged->get_nodes()[i];
		CHECK(String(entry.get("stored_sha256", "")) == String::hex_encode_buffer(record.digest, 32));
		gs_world_paged::NodeMetrics metrics;
		D2_AUTO_NEED(gs_world_paged::compute_node_metrics(actual.ptr(), actual.size(), record.cell_center, record.is_leaf, metrics, reason));
		CHECK(metrics.footprint == record.metrics.footprint);
		CHECK(metrics.dc_encoding == record.metrics.dc_encoding);
		CHECK(metrics.center_exact == record.metrics.center_exact);
		CHECK(metrics.homogeneous_dc == record.metrics.homogeneous_dc);
	}
	const Dictionary totals = manifest.get("totals", Dictionary());
	CHECK(int64_t(totals.get("node_count", -1)) == tree.nodes.size());
	CHECK(int64_t(totals.get("leaf_splat_count", -1)) == tree.leaf_splat_count);
	CHECK(int64_t(totals.get("interior_splat_count", -1)) == tree.interior_splat_count);
	// Derive depth and eps distributions from the reopened table, independently.
	std::vector<std::vector<double>> eps;
	std::vector<uint32_t> depths(tree.nodes.size());
	for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
		const auto &node = tree.nodes[i];
		if (i > 0) {
			D2_AUTO_NEED(node.parent < i);
			depths[i] = depths[node.parent] + 1;
		}
		eps.resize(MAX(eps.size(), size_t(depths[i] + 1)));
		eps[depths[i]].push_back(node.geometric_error);
	}
	const Array levels = manifest.get("levels", Array());
	D2_AUTO_NEED(levels.size() == int(eps.size()));
	for (uint32_t depth = 0; depth < eps.size(); ++depth) {
		auto &values = eps[depth];
		D2_AUTO_NEED(!values.empty());
		std::sort(values.begin(), values.end());
		const Dictionary level = levels[depth];
		CHECK(int64_t(level.get("depth", -1)) == depth);
		CHECK(int64_t(level.get("node_count", -1)) == int64_t(values.size()));
		CHECK(double(level.get("eps_min", -1)) == values.front());
		CHECK(double(level.get("eps_max", -1)) == values.back());
		CHECK(double(level.get("eps_median", -1)) == (values[(values.size() - 1) / 2] + values[values.size() / 2]) / 2.0);
	}
}

// #1432: a codec-none leaf stores float(x - c) and the reader returns
// float(float(x - c) + c). Next to a coordinate plane that round trip is not the
// identity (x = 0.1f in a cell centred on 0.5 reads back 0.09999999f), yet it is
// idempotent, so the writer accepts the leaf. The reopen check must then expect
// exactly that reconstructed payload, not the pre-store absolute input.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Codec-none auto-tree reopens near-plane leaves whose positions do not round-trip exactly") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create(262144, gs_world_writer::kCodecGsqV1, 0, true));
	D2_AUTO_NEED(fixture.generated.splat_count > 0);
	Resident resident;
	D2_AUTO_NEED(resident.load(fixture));
	gs_hlod::BakeResult oracle;
	String reason;
	Error error = OK;
	D2_AUTO_NEED(gs_hlod::bake_world(resident.view(), gs_hlod::BakeParams(), oracle, &reason, &error));
	D2_AUTO_NEED(error == OK);
	D2_AUTO_NEED(oracle.leaf_gaussians.size() == oracle.tree.leaf_splat_count);
	D2_AUTO_NEED(oracle.leaf_sh_high_order.size() == uint64_t(oracle.tree.leaf_splat_count) * resident.high);
	D2_AUTO_NEED(oracle.tree.interior_gaussians.size() == oracle.tree.interior_splat_count);
	// Independent expectation: the store/reconstruct arithmetic spelled out here,
	// not the writer's or reader's helpers.
	LocalVector<Gaussian> expected_leaves = oracle.leaf_gaussians;
	uint64_t inexact = 0;
	for (const auto &node : oracle.tree.nodes) {
		if (!node.is_leaf()) {
			continue;
		}
		double center[3];
		oracle.tree.node_cell_center(node, center);
		for (uint32_t j = 0; j < node.payload_count; ++j) {
			Vector3 &pos = expected_leaves[node.payload_first + j].position;
			for (int axis = 0; axis < 3; ++axis) {
				const float stored = float(double(pos[axis]) - center[axis]);
				const float reconstructed = float(double(stored) + center[axis]);
				// The writer's acceptance rule: the reconstruction is idempotent.
				D2_AUTO_NEED(float(double(float(double(reconstructed) - center[axis])) + center[axis]) == reconstructed);
				inexact += reconstructed != pos[axis] ? 1u : 0u;
				pos[axis] = reconstructed;
			}
		}
	}
	// Fixture proof: without inexact coordinates this case tests nothing.
	D2_AUTO_NEED(inexact > 0);
	MESSAGE("near-plane leaf coordinates that do not round-trip exactly: ", inexact);

	const String output = fixture.dir.path_join("near_plane.gsplatworld");
	const Dictionary report = GaussianSplatHlodAutoTree::build(fixture.input, output, fixture.options());
	CHECK_MESSAGE(int(report.get("error", FAILED)) == OK, "auto-tree build refused: ", String(report.get("reason", "missing")).utf8().get_data());
	D2_AUTO_NEED(int(report.get("error", FAILED)) == OK);
	success_report(report);
	CHECK(directory_empty(fixture.scratch));
	const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(output));
	D2_AUTO_NEED(!manifest.is_empty());
	CHECK(String(Dictionary(manifest.get("codec", Dictionary())).get("codec", "")) == "none");
	CHECK(String(manifest.get("output_file_sha256", "")) == FileAccess::get_sha256(output));

	Ref<GaussianSplatHlodPagedSource> paged;
	paged.instantiate();
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	D2_AUTO_NEED(paged->open(output, tree, metadata, reason) == OK);
	D2_AUTO_NEED(tree.nodes.size() == oracle.tree.nodes.size());
	const Array nodes = manifest.get("nodes", Array());
	D2_AUTO_NEED(nodes.size() == int(tree.nodes.size()));
	for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
		const auto &expected = oracle.tree.nodes[i];
		same_node(tree.nodes[i], expected);
		const uint64_t start = expected.is_leaf() ? expected.payload_first : expected.payload_first - oracle.tree.leaf_splat_count;
		const Gaussian *g = expected.is_leaf() ? expected_leaves.ptr() + start : oracle.tree.interior_gaussians.ptr() + start;
		const Vector3 *sh = expected.is_leaf() ? oracle.leaf_sh_high_order.ptr() + start * resident.high : oracle.tree.interior_sh_high_order.ptr() + start * resident.high;
		Vector<Gaussian> actual;
		Vector<Vector3> actual_sh;
		D2_AUTO_NEED(paged->read_node_payload(i, actual, actual_sh, reason) == OK);
		D2_AUTO_NEED(actual.size() == int(expected.payload_count));
		D2_AUTO_NEED(actual_sh.size() == int(expected.payload_count * resident.high));
		CHECK(std::memcmp(actual.ptr(), g, expected.payload_count * sizeof(Gaussian)) == 0);
		CHECK(std::memcmp(actual_sh.ptr(), sh, expected.payload_count * resident.high * sizeof(Vector3)) == 0);
		const Dictionary entry = nodes[i];
		CHECK(String(entry.get("decoded_sha256", "")) == payload_digest(g, sh, expected.payload_count, resident.high));
		CHECK(String(entry.get("stored_sha256", "")) == String::hex_encode_buffer(paged->get_nodes()[i].digest, 32));
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Worker schedule preserves V4 bytes tree SHA and manifest content digests") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create(262144));
	const String one_path = fixture.dir.path_join("one.gsplatworld");
	const String many_path = fixture.dir.path_join("many.gsplatworld");
	const Dictionary one = GaussianSplatHlodAutoTree::build(fixture.input, one_path, fixture.options(1));
	D2_AUTO_NEED(int(one.get("error", FAILED)) == OK);
	const Dictionary many = GaussianSplatHlodAutoTree::build(fixture.input, many_path, fixture.options(4));
	D2_AUTO_NEED(int(many.get("error", FAILED)) == OK);
	CHECK(int64_t(one.get("tiles_baked", 0)) > 1);
	CHECK(int64_t(many.get("tiles_baked", 0)) > 1);
	const String hash = FileAccess::get_sha256(one_path);
	D2_AUTO_NEED(hex_digest(hash));
	CHECK(hash == FileAccess::get_sha256(many_path));
	const Dictionary a = json_file(gs_world_generator::manifest_path_for(one_path));
	const Dictionary b = json_file(gs_world_generator::manifest_path_for(many_path));
	D2_AUTO_NEED(hex_digest(a.get("tree_sha", "")));
	CHECK(a.get("tree_sha", "") == b.get("tree_sha", ""));
	CHECK(a.get("tree_sha", "") == one.get("tree_sha", "missing"));
	CHECK(b.get("tree_sha", "") == many.get("tree_sha", "missing"));
	CHECK(String(a["tree_sha"]) == tree_sha(a));
	CHECK(String(b["tree_sha"]) == tree_sha(b));
	for (const char *key : { "nodes", "totals", "levels", "content_generation", "output_file_sha256", "input_file_sha256" }) {
		D2_AUTO_NEED(a.has(key) && b.has(key));
		CHECK(a[key] == b[key]);
	}
	CHECK(int(Dictionary(a.get("build_params", Dictionary())).get("worker_count", 0)) == 1);
	CHECK(int(Dictionary(b.get("build_params", Dictionary())).get("worker_count", 0)) == 4);
	// Whole manifest bytes intentionally include timings/paths/worker_count;
	// deterministic content digests, not wall time, are the equality contract.
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Manifest hash pins sources frozen parameters and input bytes") {
	Fixture fixture, changed;
	D2_AUTO_NEED(fixture.create());
	D2_AUTO_NEED(changed.create(32768, gs_world_writer::kCodecGsqV1, 1));
	String original_sha;
	for (int variant = 0; variant < 4; ++variant) {
		Dictionary options = fixture.options();
		if (variant == 1) {
			options["source_fingerprint"] = String("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
		} else if (variant == 2) {
			options["max_tile_splats"] = 32768;
		}
		const Fixture &source = variant == 3 ? changed : fixture;
		const String path = fixture.dir.path_join("hash" + itos(variant) + ".gsplatworld");
		const Dictionary report = GaussianSplatHlodAutoTree::build(source.input, path, options);
		D2_AUTO_NEED(int(report.get("error", FAILED)) == OK);
		const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(path));
		const String sha = manifest.get("tree_sha", "");
		D2_AUTO_NEED(hex_digest(sha));
		CHECK(sha == tree_sha(manifest));
		CHECK(sha == String(report.get("tree_sha", "")));
		CHECK(manifest.get("source_fingerprint", "") == options["source_fingerprint"]);
		CHECK(String(manifest.get("input_file_sha256", "")) == FileAccess::get_sha256(source.input));
		CHECK(String(manifest.get("input_manifest_sha256", "")) == FileAccess::get_sha256(source.generated.manifest_path));
		CHECK(String(manifest.get("input_gen_sha", "")) == source.generated.gen_sha);
		CHECK(String(manifest.get("input_generator_params_sha", "")) == source.generated.params_sha);
		const Dictionary params = manifest.get("build_params", Dictionary());
		CHECK(params.size() == 4); // three content fields plus execution worker_count
		CHECK(int(params.get("leaf_max", 0)) == 16384);
		CHECK(int64_t(params.get("max_tile_splats", -1)) == int64_t(options["max_tile_splats"]));
		const Dictionary interior = params.get("interior", Dictionary());
		CHECK(interior.size() == 5);
		CHECK(int(interior.get("merge_grid", 0)) == 64);
		CHECK(double(interior.get("reduction", 0)) == 4.0);
		CHECK(int(interior.get("node_max_splats", 0)) == 16384);
		CHECK(double(interior.get("eps_growth", 0)) == 1.25);
		CHECK(int(interior.get("max_eps_steps", 0)) == 1024);
		if (variant == 0) {
			original_sha = sha;
		} else {
			CHECK(sha != original_sha);
		}
	}
	CHECK(fixture.generated.file_sha256 != changed.generated.file_sha256);
}

// Use Object::callp, not a direct static call, to catch missing binding/registration.
inline Dictionary bound_build(const String &p_input, const String &p_output, const Dictionary &p_options) {
	Ref<GaussianSplatHlodAutoTree> api;
	api.instantiate();
	const Variant input = p_input, output = p_output, options = p_options;
	const Variant *args[] = { &input, &output, &options };
	Callable::CallError error;
	const Variant result = api->callp("build", args, 3, error);
	if (error.error != Callable::CallError::CALL_OK || result.get_type() != Variant::DICTIONARY) {
		FAIL("GaussianSplatHlodAutoTree.build must be bound and return Dictionary");
		return {};
	}
	return result;
}

// D2-6 phase 1: runtime contracts through the existing bound API only.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Streaming auto-tree completes under a job RAM limit the retained builder refuses with identical bytes") {
	Fixture fixture;
	if (!fixture.create(1048576)) {
		FAIL("D2-6 streaming fixture generation failed");
		return;
	}
	// Approximately 589824 selected degree-3 splats, P = 144 + 12*12 = 288 B.
	// References below are at base 5359cce6220, relative to the module directory.
	// core/gaussian_splat_hlod_tiled_internal.h:62 node_bound ~= 36417;
	// core/gaussian_splat_hlod_tiled_bake.cpp:774-777 retains
	// 4*(N + ceil(N/3) + node_bound)*P + 4096*node_bound + 64*N + 256*chunks
	// ~= 1082 MiB. With the 128 MiB base (core/gaussian_splat_hlod_auto_tree.cpp:52)
	// this already exceeds 3*384 MiB, before workers or the later payload_reserved
	// charge (auto_tree.cpp:651-653). The default 2 GiB still admits that builder.
	// At T=16385 (>16384), OfflineJob has nodes=1057, interiors=6519:
	// [3*T + 3*6519 + 6*T + 10*16384]*288 + 128*T + 64*T + 2048*1057 + 1 MiB
	// = 96.939 MiB (core/gaussian_splat_hlod_job.h:104-131; reload term is smaller).
	// Add 3*4096*288 decode + 3*T*4 indices + 8 MiB stack = 108.501 MiB/worker
	// (tiled_bake.cpp:648, core/gaussian_splat_hlod_tiled_workers.cpp:257).
	// Writer fixed buffers = 16 MiB + 16*(16384*288) = 88 MiB (auto_tree.cpp:657-659).
	// Thus 128 + 108.501 + 88 + ~17 MiB source decoder = ~342 MiB, leaving ~42 MiB
	// for node tables/frontier/directory. Four workers alone with the base exceed
	// 384 MiB: admission must reduce concurrency, including decoder reservations
	// (io/gs_auto_tree_source.cpp:185-192). Stitch must also charge its actual
	// frontier, not the unconditional eight-full-child workspace (tiled_bake.cpp:805).
	// Output codec none avoids doubling these large debug builds for GSQ output;
	// GSQ input still exercises decode. Additional GSQ output verification/workspace
	// terms (auto_tree.cpp:663-685) are zero for none, not borrowed from the writer.
	constexpr int64_t ram_limit = int64_t(384) << 20;
	Dictionary options = fixture.options(1);
	options["max_tile_splats"] = 16385;
	Dictionary limits = options.get("limits", Dictionary());
	// Four times the existing 262144-plan fixture: allow four times its 45 s.
	limits["deadline_sec"] = 180;
	options["limits"] = limits;
	const String reference_path = fixture.dir.path_join("reference.gsplatworld");
	const Dictionary reference = bound_build(fixture.input, reference_path, options);
	if (int(reference.get("error", FAILED)) != OK) {
		const CharString reason = String(reference.get("reason", "missing")).utf8();
		FAIL("D2-6 unrestricted reference failed: ", doctest::String(reason.get_data()));
		return;
	}
	success_report(reference);
	CHECK(int64_t(reference.get("tiles_baked", 0)) >= 4);
	const String file_sha = FileAccess::get_sha256(reference_path);
	const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(reference_path));
	const String generation = manifest.get("content_generation", "");
	const String sha = manifest.get("tree_sha", "");
	const Array nodes = manifest.get("nodes", Array());
	if (!hex_digest(file_sha) || !hex_digest(sha) || generation.is_empty() || nodes.is_empty()) {
		FAIL("D2-6 reference must publish file/tree hashes, generation and node digests");
		return;
	}
	CHECK(String(manifest.get("output_file_sha256", "")) == file_sha);
	CHECK(sha == tree_sha(manifest));
	CHECK(sha == String(reference.get("tree_sha", "")));
	CHECK(int64_t(nodes.size()) == int64_t(reference.get("node_count", -1)));
	for (int i = 0; i < nodes.size(); ++i) {
		if (nodes[i].get_type() != Variant::DICTIONARY) {
			FAIL("D2-6 reference node must be a Dictionary");
			return;
		}
		const Dictionary node = nodes[i];
		if (!hex_digest(node.get("stored_sha256", "")) || !hex_digest(node.get("decoded_sha256", ""))) {
			FAIL("D2-6 reference node must contain both payload digests");
			return;
		}
	}
	options["worker_count"] = 4;
	limits["max_ram_bytes"] = ram_limit;
	options["limits"] = limits;
	const String limited_path = fixture.dir.path_join("limited.gsplatworld");
	const Dictionary limited = bound_build(fixture.input, limited_path, options);
	const CharString limited_reason = String(limited.get("reason", "missing")).utf8();
	MESSAGE("D2-6 streaming selected=", fixture.generated.splat_count, " limit=", ram_limit,
			" reference_peak=", int64_t(reference.get("peak_reserved_bytes", 0)),
			" limited_peak=", int64_t(limited.get("peak_reserved_bytes", 0)),
			" reason=", doctest::String(limited_reason.get_data()));
	if (int(limited.get("error", FAILED)) != OK) {
		// Master reaches this explicit failure with limit_ram; never index its absent manifest.
		FAIL("D2-6 one tile fits: streaming build must succeed after reducing worker admission");
		return;
	}
	success_report(limited);
	CHECK(int64_t(limited.get("peak_reserved_bytes", 0)) <= ram_limit);
	CHECK(int64_t(limited.get("tiles_baked", 0)) >= 4);
	CHECK(FileAccess::get_sha256(limited_path) == file_sha);
	const Dictionary streamed = json_file(gs_world_generator::manifest_path_for(limited_path));
	if (!streamed.has("nodes") || !streamed.has("content_generation") || !streamed.has("tree_sha")) {
		FAIL("D2-6 limited build must publish its identity manifest");
		return;
	}
	CHECK(streamed["nodes"] == Variant(nodes));
	CHECK(String(streamed["content_generation"]) == generation);
	CHECK(String(streamed["tree_sha"]) == sha);
	CHECK(String(limited.get("tree_sha", "")) == sha);
	CHECK(tree_sha(streamed) == sha);
	CHECK(String(streamed.get("output_file_sha256", "")) == file_sha);
	CHECK(directory_empty(fixture.scratch));
	// Fixture's destructor removes both outputs and input, including on every early return.
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Auto-tree peak reservation does not grow with splat count") {
	Fixture fixtures[2];
	int64_t peaks[2] = {};
	int64_t node_counts[2] = {};
	// Same world/seed, four times the density: approximately 147456 and 589824
	// selected splats. An extra xz subdivision restores the same maximum tile
	// occupancy (up to integer apportionment); both source chunk caps remain 4096.
	// Same T=16385, one worker and 2 GiB for BOTH runs: master must complete both
	// and fail the peak delta, not fail early from admission or a different limit.
	constexpr int64_t payload_bytes = sizeof(Gaussian) + 12 * sizeof(Vector3);
	// At base 5359cce6220, core/gaussian_splat_hlod_auto_tree.cpp:656-659 charges
	// 16384/node + writer [16384 + 64*(output.length + scratch.length + 256)]/node;
	// core/gaussian_splat_hlod_tiled_bake.cpp:776 adds 4096/node. Include 64/node
	// for the two 32-byte GSQ digest tables (auto_tree.cpp:677-678), even though
	// codec none uses zero. Round up to 96 KiB, with the path-dependent term
	// explicitly checked below so a longer temporary path cannot invalidate it.
	constexpr int64_t k_node = 96 * 1024;
	// Allow 4 KiB per actual extra chunk: covers the source directory's two Page
	// copies (io/gs_auto_tree_source.cpp:185) and tiled 256 B/chunk (:777), including
	// directory/path/manifest bookkeeping. The input sidecar expansion must remain
	// inside the fixed 128 MiB base (auto_tree.cpp:579), checked below. The resulting
	// slack must stay below one full tile payload (~4.5 MiB); no retained splat term.
	constexpr int64_t per_chunk_slack = 4 * 1024;
	for (int i = 0; i < 2; ++i) {
		Fixture &fixture = fixtures[i];
		if (!fixture.create(i == 0 ? 262144 : 1048576)) {
			FAIL("D2-6 scaling fixture generation failed");
			return;
		}
		const String output = fixture.dir.path_join("scaling.gsplatworld");
		const int64_t per_node = 16384 + 16384 + 4096 + 64 +
				64 * int64_t(output.length() + fixture.scratch.length() + 256);
		if (per_node > k_node) {
			FAIL("D2-6 temporary paths exceed the documented per-node allowance");
			return;
		}
		const int64_t manifest_bytes = FileAccess::get_file_as_bytes(fixture.generated.manifest_path).size();
		if (manifest_bytes <= 0 || manifest_bytes * 128 + (int64_t(4) << 20) > (int64_t(128) << 20)) {
			FAIL("D2-6 source manifest must fit the fixed base reservation");
			return;
		}
		Dictionary options = fixture.options(1);
		options["max_tile_splats"] = 16385;
		Dictionary limits = options.get("limits", Dictionary());
		limits["deadline_sec"] = 180;
		options["limits"] = limits;
		const Dictionary report = bound_build(fixture.input, output, options);
		if (int(report.get("error", FAILED)) != OK) {
			const CharString reason = String(report.get("reason", "missing")).utf8();
			FAIL("D2-6 scaling build must complete before measuring peaks: ", doctest::String(reason.get_data()));
			return;
		}
		success_report(report);
		peaks[i] = int64_t(report.get("peak_reserved_bytes", 0));
		node_counts[i] = int64_t(report.get("node_count", 0));
		if (peaks[i] <= 0 || node_counts[i] <= 0) {
			FAIL("D2-6 scaling build must report measured peaks and nonempty node tables");
			return;
		}
		CHECK(peaks[i] <= int64_t(limits.get("max_ram_bytes", 0)));
		CHECK(int64_t(report.get("tiles_baked", 0)) > 1);
		CHECK(directory_empty(fixture.scratch));
	}
	const int64_t selected_n = int64_t(fixtures[0].generated.splat_count);
	const int64_t selected_4n = int64_t(fixtures[1].generated.splat_count);
	if (selected_n <= 0 || selected_4n < 4 * selected_n - 4096 || selected_4n > 4 * selected_n + 4096 || node_counts[1] <= node_counts[0]) {
		FAIL("D2-6 scaling fixtures must increase selected splats about fourfold and grow the node table");
		return;
	}
	const int64_t extra_chunks = int64_t(fixtures[1].generated.chunk_count) - int64_t(fixtures[0].generated.chunk_count);
	CHECK(extra_chunks >= 0);
	const int64_t slack = extra_chunks * per_chunk_slack;
	CHECK(slack < 16385 * payload_bytes);
	const int64_t allowed_delta = (node_counts[1] - node_counts[0]) * k_node + slack;
	MESSAGE("D2-6 scaling N=", selected_n, " peak_N=", peaks[0], " nodes_N=", node_counts[0],
			" four_N=", selected_4n, " peak_4N=", peaks[1], " nodes_4N=", node_counts[1],
			" K_node=", k_node, " slack=", slack, " allowed_delta=", allowed_delta);
	// A full extra 3N payload alone must exceed the bound, before master's 4x
	// retained-capacity factor/interiors/metadata. Prevent a vacuously loose bound.
	CHECK(3 * selected_n * payload_bytes > allowed_delta);
	CHECK(peaks[1] - peaks[0] <= allowed_delta);
	// Both fixtures clean themselves on success and on explicit failure returns.
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Streaming auto-tree matches retained BFS writer for both codecs and worker schedules") {
	Fixture fixture;
	if (!fixture.create(262144)) {
		FAIL("D2-6 retained-reference fixture generation failed");
		return;
	}
	// Existing D2-3 multi-tile fixture. The reference takes the old retained
	// BakeResult -> BFS/bucket-0 append route; production emits completed tiles
	// before the stitch and remaps provisional handles only after assembly.
	// D2-2 independently pins the shared in-memory sink to in-core MERGE_ONLY.
	for (const char *codec : { "none", "gsq" }) {
		String first_sha;
		Dictionary first_manifest;
		for (uint32_t workers : { 1u, 4u }) {
			Dictionary options = fixture.options(workers);
			options["codec"] = codec;
			Dictionary limits = options.get("limits", Dictionary());
			// The GSQ route independently encodes/decodes and reopens every node.
			limits["deadline_sec"] = 90;
			options["limits"] = limits;
			const String suffix = String(codec) + "_" + itos(workers) + ".gsplatworld";
			const String reference_path = fixture.dir.path_join("retained_" + suffix);
			const String streamed_path = fixture.dir.path_join("streamed_" + suffix);
			const Dictionary reference = GaussianSplatHlodAutoTree::build_retained_for_test(fixture.input, reference_path, options);
			if (int(reference.get("error", FAILED)) != OK) {
				FAIL("D2-6 retained reference failed: ", codec, " workers=", workers,
						" reason=", String(reference.get("reason", "missing")).utf8().get_data());
				return;
			}
			const Dictionary streamed = bound_build(fixture.input, streamed_path, options);
			if (int(streamed.get("error", FAILED)) != OK) {
				FAIL("D2-6 streaming identity build failed: ", codec, " workers=", workers,
						" reason=", String(streamed.get("reason", "missing")).utf8().get_data());
				return;
			}
			success_report(reference);
			success_report(streamed);
			CHECK(int64_t(reference.get("tiles_baked", 0)) > 1);
			CHECK(int64_t(streamed.get("tiles_baked", 0)) > 1);
			const String reference_sha = FileAccess::get_sha256(reference_path);
			const Dictionary a = json_file(gs_world_generator::manifest_path_for(reference_path));
			const Dictionary b = json_file(gs_world_generator::manifest_path_for(streamed_path));
			if (!hex_digest(reference_sha) || !hex_digest(a.get("tree_sha", "")) ||
					String(a.get("content_generation", "")).is_empty() || Array(a.get("nodes", Array())).is_empty()) {
				FAIL("D2-6 retained reference must publish nonempty identities");
				return;
			}
			CHECK(FileAccess::get_sha256(streamed_path) == reference_sha);
			for (const char *key : { "content_generation", "tree_sha", "nodes", "output_file_sha256", "totals", "levels" }) {
				if (!a.has(key) || !b.has(key)) {
					FAIL("D2-6 identity manifest field missing: ", key);
					return;
				}
				CHECK(a[key] == b[key]);
				if (workers == 4) {
					if (!first_manifest.has(key)) {
						FAIL("D2-6 one-worker reference identity field missing");
						return;
					}
					CHECK(a[key] == first_manifest[key]);
				}
			}
			CHECK(String(a.get("output_file_sha256", "")) == reference_sha);
			CHECK(tree_sha(a) == String(a.get("tree_sha", "")));
			CHECK(tree_sha(b) == String(b.get("tree_sha", "")));
			CHECK(reference.get("tree_sha", "") == a.get("tree_sha", "missing"));
			CHECK(streamed.get("tree_sha", "") == b.get("tree_sha", "missing"));
			CHECK(int64_t(streamed.get("scratch_bytes_total", 0)) > 0);
			CHECK(int64_t(streamed.get("scratch_bytes_peak", 0)) > 0);
			if (workers == 1) {
				first_sha = reference_sha;
				first_manifest = a;
			} else {
				CHECK(reference_sha == first_sha);
			}
			CHECK(directory_empty(fixture.scratch));
		}
	}
}

// D2-3b: use only existing bindings and Dictionary fields so these contracts
// compile before GSQ output is enabled. Each GSQ build must succeed first; the
// current ERR_UNAVAILABLE refusal is a runtime failure, never a skipped case.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Bound GSQ auto-tree output records codec parameters in tree SHA") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create());
	String none_sha;
	for (const char *name : { "none", "gsq" }) {
		Dictionary options = fixture.options();
		options["codec"] = name;
		const String output = fixture.dir.path_join(String(name) + ".gsplatworld");
		const Dictionary report = bound_build(fixture.input, output, options);
		const CharString build_reason = String(report.get("reason", "")).utf8();
		INFO("codec=", doctest::String(name), " reason=", doctest::String(build_reason.get_data()));
		CHECK(String(report.get("reason", "")) != "v4_gsq_output_requires_bv_c4");
		D2_AUTO_NEED(int(report.get("error", FAILED)) == OK);
		success_report(report);
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(output);
		D2_AUTO_NEED(bytes.size() >= int64_t(gs_world_paged::kHeaderBytes));
		CHECK(decode_uint32(bytes.ptr() + 4) == 4u);
		CHECK(decode_uint32(bytes.ptr() + 60) == (String(name) == "gsq" ? 1u : 0u));
		const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(output));
		D2_AUTO_NEED(!manifest.is_empty());
		const Dictionary codec = manifest.get("codec", Dictionary());
		CHECK(String(codec.get("codec", "")) == name);
		if (String(name) == "gsq") {
			// Flat CodecParams block, using the generator's existing field names.
			const gs_world_writer::CodecParams params = gs_world_writer::gsq_v1_codec_params();
			CHECK(double(codec.get("codec_id", -1)) == double(params.codec_id));
			const Array bits = codec.get("sh_bits", Array());
			D2_AUTO_NEED(bits.size() == 3);
			for (int band = 0; band < 3; ++band) {
				CHECK(double(bits[band]) == double(params.sh_bits[band]));
			}
			CHECK(double(codec.get("opacity_bits", -1)) == double(params.opacity_bits));
			CHECK(double(codec.get("pos_step_m", -1)) == double(params.pos_step_m));
			CHECK(double(codec.get("zstd_level", -1)) == double(params.zstd_level));
		}
		const String sha = manifest.get("tree_sha", "");
		D2_AUTO_NEED(hex_digest(sha));
		CHECK(sha == tree_sha(manifest));
		CHECK(sha == String(report.get("tree_sha", "")));
		CHECK(String(manifest.get("input_file_sha256", "")) == FileAccess::get_sha256(fixture.input));
		if (String(name) == "none") {
			none_sha = sha;
		} else {
			CHECK(sha != none_sha);
		}
	}
	// The bound API currently exposes only the codec name, not individual
	// CodecParams overrides. No hypothetical option names are added here.
	CHECK(directory_empty(fixture.scratch));
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] GSQ auto-tree canonical node digests equal in-core encode decode and reopened payloads") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create(262144));
	D2_AUTO_NEED(fixture.generated.splat_count > 2 * 65536);
	D2_AUTO_NEED(fixture.generated.splat_count <= 1000000);
	Dictionary options = fixture.options();
	options["codec"] = "gsq";
	const String output = fixture.dir.path_join("gsq_oracle.gsplatworld");
	const Dictionary report = bound_build(fixture.input, output, options);
	const CharString build_reason = String(report.get("reason", "")).utf8();
	INFO("GSQ build reason=", doctest::String(build_reason.get_data()));
	D2_AUTO_NEED(int(report.get("error", FAILED)) == OK);
	success_report(report);
	CHECK(int64_t(report.get("tiles_baked", 0)) > 1);
	CHECK(int64_t(report.get("stitch_levels", 0)) > 0);

	Resident resident;
	D2_AUTO_NEED(resident.load(fixture));
	D2_AUTO_NEED(resident.first == 3);
	D2_AUTO_NEED(resident.high == gs_gsq::sh_high_order_for_degree(3));
	gs_hlod::BakeParams bake_params;
	bake_params.mode = gs_hlod::BakeParams::MERGE_ONLY;
	gs_hlod::BakeResult oracle;
	String reason;
	Error error = OK;
	D2_AUTO_NEED(gs_hlod::bake_world(resident.view(), bake_params, oracle, &reason, &error));
	D2_AUTO_NEED(error == OK);
	D2_AUTO_NEED(gs_hlod_validate_tree(oracle.tree, &reason));
	D2_AUTO_NEED(oracle.leaf_gaussians.size() == oracle.tree.leaf_splat_count);
	D2_AUTO_NEED(oracle.leaf_sh_high_order.size() == uint64_t(oracle.tree.leaf_splat_count) * resident.high);
	D2_AUTO_NEED(oracle.tree.interior_gaussians.size() == oracle.tree.interior_splat_count);
	D2_AUTO_NEED(oracle.tree.interior_sh_high_order.size() == uint64_t(oracle.tree.interior_splat_count) * resident.high);
	D2_AUTO_NEED(oracle.tree.interior_splat_count > 0);
	gs_gsq::EncodeParams encode_params;
	D2_AUTO_NEED(gs_world_writer::gsq_encode_params(gs_world_writer::gsq_v1_codec_params(), encode_params, reason) == OK);
	gs_gsq::DecodeContext decoder;

	Ref<GaussianSplatHlodPagedSource> paged;
	paged.instantiate();
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	D2_AUTO_NEED(paged->open(output, tree, metadata, reason) == OK);
	D2_AUTO_NEED(tree.nodes.size() == oracle.tree.nodes.size());
	D2_AUTO_NEED(paged->get_nodes().size() == int(tree.nodes.size()));
	CHECK(tree.bake_rule_version == 1);
	CHECK(tree.leaf_splat_count == oracle.tree.leaf_splat_count);
	CHECK(tree.interior_splat_count == oracle.tree.interior_splat_count);
	CHECK(tree.sh_high_order_count == resident.high);
	CHECK(std::memcmp(tree.origin, oracle.tree.origin, sizeof(tree.origin)) == 0);
	const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(output));
	CHECK(String(manifest.get("output_file_sha256", "")) == FileAccess::get_sha256(output));
	const Array nodes = manifest.get("nodes", Array());
	D2_AUTO_NEED(nodes.size() == int(tree.nodes.size()));
	bool lossy = false;
	std::vector<TestGSQBounds::Budget> bounds_budget(tree.nodes.size());
	for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
		INFO("node=", i);
		const auto &expected = oracle.tree.nodes[i];
		TestGSQBounds::check_unchanged_fields(tree.nodes[i], expected);
		D2_AUTO_NEED(expected.payload_count > 0);
		D2_AUTO_NEED(expected.is_leaf() || expected.payload_first >= oracle.tree.leaf_splat_count);
		const uint64_t start = expected.is_leaf() ? expected.payload_first : expected.payload_first - oracle.tree.leaf_splat_count;
		const auto &source_g = expected.is_leaf() ? oracle.leaf_gaussians : oracle.tree.interior_gaussians;
		const auto &source_sh = expected.is_leaf() ? oracle.leaf_sh_high_order : oracle.tree.interior_sh_high_order;
		D2_AUTO_NEED(start + expected.payload_count <= source_g.size());
		D2_AUTO_NEED((start + expected.payload_count) * resident.high <= source_sh.size());
		const Gaussian *g = source_g.ptr() + start;
		const Vector3 *sh = source_sh.ptr() + start * resident.high;
		// Independent oracle: encode the in-core payload exactly once, without
		// using WriterV4, the output bytes, or the manifest as encoder input.
		// V4 frames leaves relative to their node centre; interiors already use
		// that frame. Derive the centre from the in-core tree, not the reader.
		LocalVector<Gaussian> encoder_input;
		encoder_input.resize(expected.payload_count);
		std::memcpy(encoder_input.ptr(), g, expected.payload_count * sizeof(Gaussian));
		double center[3];
		oracle.tree.node_cell_center(expected, center);
		if (expected.is_leaf()) {
			for (Gaussian &splat : encoder_input) {
				for (int axis = 0; axis < 3; ++axis) {
					splat.position[axis] = real_t(double(splat.position[axis]) - center[axis]);
				}
			}
		}
		LocalVector<uint8_t> page;
		LocalVector<Gaussian> decoded;
		LocalVector<Vector3> decoded_sh;
		decoded.resize(expected.payload_count);
		decoded_sh.resize(expected.payload_count * resident.high);
		D2_AUTO_NEED(gs_gsq::encode_page(encoder_input.ptr(), sh, expected.payload_count, 3, encode_params, page, reason) == OK);
		gs_gsq::PageInfo page_info;
		D2_AUTO_NEED(gs_gsq::verify_page(page.ptr(), page.size(), page_info, reason) == OK);
		D2_AUTO_NEED(TestGSQBounds::quantisation_budget(g, expected.payload_count, center, expected.is_leaf(), page_info,
				gs_world_writer::gsq_v1_codec_params(), bounds_budget[i]));
		D2_AUTO_NEED(decoder.decode_range(page.ptr(), page.size(), 0, expected.payload_count, decoded.ptr(), decoded_sh.ptr(), reason) == OK);
		if (expected.is_leaf()) {
			for (Gaussian &splat : decoded) {
				for (int axis = 0; axis < 3; ++axis) {
					splat.position[axis] = real_t(double(splat.position[axis]) + center[axis]);
				}
			}
		}
		uint8_t expected_digest[32], source_digest[32], actual_digest[32];
		gs_gsq::decoded_digest(decoded.ptr(), decoded_sh.ptr(), expected.payload_count, 3, expected_digest);
		gs_gsq::decoded_digest(g, sh, expected.payload_count, 3, source_digest);
		lossy |= std::memcmp(source_digest, expected_digest, 32) != 0;
		Vector<Gaussian> actual;
		Vector<Vector3> actual_sh;
		D2_AUTO_NEED(paged->read_node_payload(i, actual, actual_sh, reason) == OK);
		D2_AUTO_NEED(actual.size() == int(expected.payload_count));
		D2_AUTO_NEED(actual_sh.size() == int(expected.payload_count * resident.high));
		gs_gsq::decoded_digest(actual.ptr(), actual_sh.ptr(), expected.payload_count, 3, actual_digest);
		const Dictionary entry = nodes[i];
		const String recorded = entry.get("decoded_sha256", "");
		CHECK(hex_digest(recorded));
		CHECK(recorded == String::hex_encode_buffer(expected_digest, 32));
		CHECK(recorded == String::hex_encode_buffer(actual_digest, 32));
		CHECK(std::memcmp(expected_digest, actual_digest, 32) == 0);
		CHECK(String(entry.get("stored_sha256", "")) == String::hex_encode_buffer(paged->get_nodes()[i].digest, 32));
		D2_AUTO_NEED(TestGSQBounds::check_decoded_support(*paged.ptr(), tree, i, actual, actual_sh));
	}
	TestGSQBounds::check_tree_bounds(oracle.tree, tree, bounds_budget);
	CHECK(lossy); // Reject a fixture for which bypassing GSQ would also pass.
	CHECK(directory_empty(fixture.scratch));
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Auto-tree GSQ output refuses old readers and resealed unknown codecs") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create());
	Dictionary options = fixture.options();
	options["codec"] = "gsq";
	const String output = fixture.dir.path_join("gsq_identity.gsplatworld");
	const Dictionary report = bound_build(fixture.input, output, options);
	const CharString build_reason = String(report.get("reason", "")).utf8();
	INFO("GSQ build reason=", doctest::String(build_reason.get_data()));
	D2_AUTO_NEED(int(report.get("error", FAILED)) == OK);
	PackedByteArray bytes = FileAccess::get_file_as_bytes(output);
	D2_AUTO_NEED(bytes.size() >= int64_t(gs_world_paged::kHeaderBytes));
	CHECK(decode_uint32(bytes.ptr() + 4) == 4u);
	CHECK(decode_uint32(bytes.ptr() + 60) == 1u);
	// Pre-BV-C4 V4 readers require this reserved word to be zero.
	const bool old_reader_accepts = decode_uint32(bytes.ptr() + 60) == 0u;
	CHECK_FALSE(old_reader_accepts);
	String reason;
	GaussianSplatHlodPagedSource good;
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	D2_AUTO_NEED(good.open(output, tree, metadata, reason) == OK);
	D2_AUTO_NEED(!tree.nodes.is_empty());
	// Only the header changes; the index pages and their digests remain valid.
	// Reseal its SHA just as in BV-C4 so corruption cannot mask codec refusal.
	encode_uint32(0xFFFFFFFFu, bytes.ptrw() + 60);
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(bytes.ptr(), gs_world_paged::kHeaderBytes - 32);
	hash.finish(bytes.ptrw() + gs_world_paged::kHeaderBytes - 32);
	const String corrupt = fixture.dir.path_join("unknown_codec.gsplatworld");
	D2_AUTO_NEED(write_bytes(corrupt, bytes));
	GaussianSplatHlodPagedSource refused;
	metadata["stale"] = true; // Failed open must clear populated outputs too.
	CHECK(refused.open(corrupt, tree, metadata, reason) == ERR_FILE_CORRUPT);
	CHECK(reason == "v4_unsupported_codec");
	CHECK_FALSE(refused.is_open());
	CHECK(refused.get_nodes().is_empty());
	CHECK(tree.nodes.is_empty());
	CHECK(metadata.is_empty());
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Bound auto-tree API refuses invalid options") {
	Fixture fixture, plain;
	D2_AUTO_NEED(fixture.create());
	D2_AUTO_NEED(plain.create(32768, gs_world_writer::kCodecNone));
	struct Bad {
		const char *key;
		Variant value;
		const char *reason;
	};
	const Bad cases[] = {
		{ "codec", "invented", "codec_invalid" },
		{ "scratch_dir", Variant(), "scratch_dir_required" },
		{ "source_fingerprint", Variant(), "source_fingerprint_required" },
		{ "source_fingerprint", "bad", "source_fingerprint_invalid" },
		{ "source_fingerprint", String("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"), "source_fingerprint_invalid" },
		{ "worker_count", 0, "worker_count_invalid" },
		{ "worker_count", 65, "worker_count_invalid" },
		{ "max_tile_splats", 16384, "max_tile_splats_invalid" },
		{ "typo_option", 1, "unknown_option" },
		{ "limits", Variant(), "limits_required" },
		{ "worker_count", "4", "option_type" },
		{ "worker_count", 4.0, "option_type" },
		{ "max_tile_splats", 65536.0, "option_type" },
		{ "source_fingerprint", 123, "option_type" },
		{ "scratch_dir", 123, "option_type" },
		{ "codec", 0, "option_type" },
		{ "limits", "unbounded", "option_type" },
	};
	const String output = fixture.dir.path_join("refused.gsplatworld");
	PackedStringArray before, after;
	D2_AUTO_NEED(directory_names(fixture.dir, before));
	for (const Bad &bad : cases) {
		Dictionary options = fixture.options();
		if (bad.value.get_type() == Variant::NIL) {
			options.erase(bad.key);
		} else {
			options[bad.key] = bad.value;
		}
		const Dictionary report = bound_build(fixture.input, output, options);
		CHECK(int(report.get("error", OK)) == ERR_INVALID_PARAMETER);
		CHECK(String(report.get("reason", "")).begins_with(bad.reason));
		CHECK_FALSE(FileAccess::exists(output));
		CHECK_FALSE(FileAccess::exists(gs_world_generator::manifest_path_for(output)));
		CHECK(directory_empty(fixture.scratch));
	}
	// Required limit members, nested unknown keys and numeric types are strict too.
	for (const char *key : { "max_ram_bytes", "max_scratch_bytes", "max_output_bytes", "deadline_sec", "typo_limit" }) {
		Dictionary options = fixture.options();
		Dictionary limits = options["limits"];
		if (String(key) == "typo_limit") {
			limits[key] = 1;
		} else {
			limits.erase(key);
		}
		const Dictionary report = bound_build(fixture.input, output, options);
		CHECK(int(report.get("error", OK)) == ERR_INVALID_PARAMETER);
		CHECK(String(report.get("reason", "")).begins_with(String(key) == "typo_limit" ? "unknown_limit" : "limits_required"));
	}
	Dictionary options = fixture.options();
	Dictionary limits = options["limits"];
	limits["max_ram_bytes"] = "2147483648";
	const Dictionary wrong_type = bound_build(fixture.input, output, options);
	CHECK(int(wrong_type.get("error", OK)) == ERR_INVALID_PARAMETER);
	CHECK(String(wrong_type.get("reason", "")).begins_with("option_type"));
	const Dictionary missing = bound_build(fixture.dir.path_join("missing.gsplatworld"), output, fixture.options());
	CHECK(int(missing.get("error", OK)) == ERR_FILE_NOT_FOUND);
	CHECK(String(missing.get("reason", "")) == "input_not_found");
	const Dictionary flat = bound_build(plain.input, output, fixture.options());
	CHECK(int(flat.get("error", OK)) == ERR_UNAVAILABLE);
	CHECK(String(flat.get("reason", "")) == "input_requires_plain_v3_gsq");
	CHECK_FALSE(FileAccess::exists(output));
	CHECK_FALSE(FileAccess::exists(gs_world_generator::manifest_path_for(output)));
	CHECK(directory_empty(fixture.scratch));
	D2_AUTO_NEED(directory_names(fixture.dir, after));
	CHECK(after == before);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] RAM refusal preserves destinations cleans scratch and permits legal retry") {
	Fixture fixture;
	D2_AUTO_NEED(fixture.create());
	const String output = fixture.dir.path_join("tree.gsplatworld");
	const String manifest = gs_world_generator::manifest_path_for(output);
	Dictionary options = fixture.options();
	Dictionary limits = options["limits"];
	limits["max_ram_bytes"] = 1;
	for (int existing = 0; existing < 3; ++existing) {
		const PackedByteArray sentinel = String("pre-existing destination: preserve every byte\n").to_utf8_buffer();
		if (existing) {
			D2_AUTO_NEED(write_bytes(output, sentinel));
		}
		if (existing == 2) {
			D2_AUTO_NEED(write_bytes(manifest, sentinel));
		}
		PackedStringArray before, after;
		D2_AUTO_NEED(directory_names(fixture.dir, before));
		const Dictionary report = bound_build(fixture.input, output, options);
		CHECK(int(report.get("error", OK)) == ERR_OUT_OF_MEMORY);
		CHECK(String(report.get("reason", "")).begins_with("limit_ram"));
		if (existing == 2) {
			CHECK(FileAccess::get_file_as_bytes(manifest) == sentinel);
		} else {
			CHECK_FALSE(FileAccess::exists(manifest));
		}
		CHECK(directory_empty(fixture.scratch));
		D2_AUTO_NEED(directory_names(fixture.dir, after));
		CHECK(after == before);
		if (existing) {
			CHECK(FileAccess::get_file_as_bytes(output) == sentinel);
		} else {
			CHECK_FALSE(FileAccess::exists(output));
		}
	}
	const Dictionary retry = bound_build(fixture.input, output, fixture.options());
	D2_AUTO_NEED(int(retry.get("error", FAILED)) == OK);
	success_report(retry);
	CHECK(FileAccess::exists(manifest));
	CHECK(directory_empty(fixture.scratch));
	Ref<GaussianSplatHlodPagedSource> paged;
	paged.instantiate();
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	String reason;
	CHECK(paged->open(output, tree, metadata, reason) == OK);
}

// D2-4: call the proposed API through the existing Variant binding machinery.
// This compiles before oracle_cut exists and fails with binding_missing at
// runtime, rather than breaking compilation or silently skipping the contract.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Bound CPU oracle cut and leaves-only control") {
	Ref<GaussianSplatHlodAutoTree> api;
	api.instantiate();
	if (!api->has_method("oracle_cut")) {
		FAIL("binding_missing: GaussianSplatHlodAutoTree.oracle_cut");
		return;
	}
	Fixture fixture;
	D2_AUTO_NEED(fixture.create(262144));
	const String output = fixture.dir.path_join("oracle.gsplatworld");
	Dictionary build_options = fixture.options();
	build_options["codec"] = "gsq";
	const Dictionary built = bound_build(fixture.input, output, build_options);
	D2_AUTO_NEED(int(built.get("error", FAILED)) == OK);
	Dictionary camera;
	camera["width"] = 1920;
	camera["height"] = 1080;
	camera["fov_deg"] = 107.5;
	camera["keep"] = "width";
	camera["near_m"] = 0.05;
	camera["far_m"] = 1000.0;
	camera["tau_pixels"] = 1000000.0; // Main cut stops coarse; leaves control must not.
	Array pose;
	for (double value : { 0.0, 100.0, 400.0, 0.0, -15.0 }) {
		pose.push_back(value); // probe_path convention: x, y, z, yaw, pitch.
	}
	Dictionary checkpoint;
	checkpoint["checkpoint"] = "test-view";
	checkpoint["pose"] = pose;
	Array poses;
	poses.push_back(checkpoint);
	// A tiny positive double is not necessarily a representable perspective in
	// real_t. The API must refuse it, not select with an identity frustum.
	Dictionary invalid_camera = camera.duplicate(true);
	invalid_camera["fov_deg"] = std::numeric_limits<double>::denorm_min();
	invalid_camera["drawn_budget_splats"] = 1000000;
	invalid_camera["leaves_only"] = false;
	const Variant invalid_path_arg = output, invalid_poses_arg = poses, invalid_options_arg = invalid_camera;
	const Variant *invalid_args[] = { &invalid_path_arg, &invalid_poses_arg, &invalid_options_arg };
	Callable::CallError invalid_call;
	const Variant invalid_value = api->callp("oracle_cut", invalid_args, 3, invalid_call);
	D2_AUTO_NEED(invalid_call.error == Callable::CallError::CALL_OK && invalid_value.get_type() == Variant::DICTIONARY);
	const Dictionary invalid_result = invalid_value;
	CHECK(int(invalid_result.get("error", OK)) != OK);
	uint64_t coarse_splats = 0;
	for (int mode = 0; mode < 3; ++mode) {
		Dictionary options = camera.duplicate(true);
		options["drawn_budget_splats"] = mode == 0 ? 1000000 : 1;
		options["leaves_only"] = mode == 2;
		const Variant path_arg = output, poses_arg = poses, options_arg = options;
		const Variant *args[] = { &path_arg, &poses_arg, &options_arg };
		Callable::CallError error;
		const Variant value = api->callp("oracle_cut", args, 3, error);
		D2_AUTO_NEED(error.error == Callable::CallError::CALL_OK);
		D2_AUTO_NEED(value.get_type() == Variant::DICTIONARY);
		const Dictionary report = value;
		D2_AUTO_NEED(report.has("poses") && report["poses"].get_type() == Variant::ARRAY);
		const Array rows = report["poses"];
		if (rows.size() != 1) {
			FAIL("oracle_cut must return exactly one row for the single test pose");
			return;
		}
		D2_AUTO_NEED(rows[0].get_type() == Variant::DICTIONARY);
		const Dictionary row = rows[0];
		for (const char *field : { "status", "selected_splats", "selected_count", "budget", "drawn_le_budget", "budget_bound" }) {
			D2_AUTO_NEED(row.has(field));
		}
		CHECK(int64_t(row["budget"]) == (mode == 0 ? 1000000 : 1));
		CHECK(row["budget_bound"].get_type() == Variant::BOOL);
		const uint64_t selected = uint64_t(int64_t(row["selected_splats"]));
		CHECK(bool(row["drawn_le_budget"]) == (selected <= uint64_t(int64_t(row["budget"]))));
		if (mode == 1) {
			CHECK(String(row["status"]) == "BOOTSTRAP_INFEASIBLE");
			CHECK(int64_t(row["selected_count"]) == 0);
		} else {
			CHECK(String(row["status"]) == "COMPLETE");
			CHECK(int64_t(row["selected_count"]) > 0);
			CHECK(selected > 0);
			if (mode == 0) {
				coarse_splats = selected;
			} else {
				// Private snapshot marks interiors EDIT_STALE; CutHistory expands
				// them with nonbinding capacities. tau=0 is INVALID in production.
				CHECK(String(report.get("mechanism", "")) == "stale_interiors_unbounded");
				CHECK(bool(row.get("only_leaves", false)));
				CHECK(selected > coarse_splats);
				CHECK(selected <= fixture.generated.splat_count);
				CHECK_FALSE(bool(row["drawn_le_budget"]));
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] RAM and consumed disk frontiers preserve both codec identities") {
	Fixture fixture;
	if (!fixture.create(65536)) {
		FAIL("frontier identity fixture generation failed");
		return;
	}
	for (const char *codec : { "none", "gsq" }) {
		Dictionary options = fixture.options(4);
		options["max_tile_splats"] = 16385;
		options["codec"] = codec;
		String ram_sha;
		Dictionary ram_manifest;
		for (bool disk : { false, true }) {
			GaussianSplatHlodAutoTree::TestHooks hooks;
			hooks.frontier_ram_bytes = disk ? 0 : uint64_t(128) << 20;
			const String output = fixture.dir.path_join(String(codec) + (disk ? "_disk.gsplatworld" : "_ram.gsplatworld"));
			const Dictionary report = GaussianSplatHlodAutoTree::build_streamed_for_test(fixture.input, output, options, hooks);
			if (int(report.get("error", FAILED)) != OK) {
				const CharString reason = String(report.get("reason", "missing")).utf8();
				FAIL("frontier identity build failed: ", doctest::String(reason.get_data()));
				return;
			}
			success_report(report);
			CHECK(int64_t(report.get("tiles_planned", 0)) > 1);
			CHECK(report.get("tiles_planned", -1) == report.get("tiles_baked", -2));
			CHECK(int64_t(report.get("scratch_spill_bytes_peak", 0)) > 0);
			CHECK(int64_t(report.get("scratch_combined_bytes_peak", 0)) <= int64_t(report.get("scratch_bytes_peak", 0)));
			const String sha = FileAccess::get_sha256(output);
			const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(output));
			if (!hex_digest(sha) || !manifest.has("nodes") || !manifest.has("tree_sha") || !manifest.has("content_generation")) {
				FAIL("frontier identity manifest missing");
				return;
			}
			if (!disk) {
				ram_sha = sha;
				ram_manifest = manifest;
				CHECK(int64_t(report.get("frontier_ram_bytes_peak", 0)) > 0);
				CHECK_FALSE(report.has("scratch_frontier_bytes_peak"));
				CHECK_FALSE(String(report.get("scratch_frontier_bytes_peak_unavailable_reason", "")).is_empty());
			} else {
				CHECK(sha == ram_sha);
				for (const char *key : { "nodes", "tree_sha", "content_generation" }) {
					if (!ram_manifest.has(key)) {
						FAIL("RAM reference manifest missing identity");
						return;
					}
					CHECK(manifest[key] == ram_manifest[key]);
				}
				const int64_t peak = report.get("scratch_frontier_bytes_peak", 0);
				CHECK(peak > 0);
				// A parent is emitted only after its children have been consumed.
				// Retaining every frontier until final assembly makes total == peak.
				CHECK(int64_t(report.get("scratch_frontier_bytes_total", 0)) > peak);
				CHECK_FALSE(report.has("frontier_ram_bytes_peak"));
			}
			CHECK(directory_empty(fixture.scratch));
		}
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Failed auto-tree retains measured scratch and planned tile progress") {
	Fixture fixture;
	if (!fixture.create(65536)) {
		FAIL("frontier failure fixture generation failed");
		return;
	}
	Dictionary options = fixture.options(1);
	options["max_tile_splats"] = 16385;
	options["codec"] = "gsq";
	for (bool disk : { false, true }) {
		GaussianSplatHlodAutoTree::TestHooks hooks;
		hooks.frontier_ram_bytes = disk ? 0 : uint64_t(64) << 20;
		hooks.fail_after_frontier = true;
		const String output = fixture.dir.path_join(disk ? "failed_disk.gsplatworld" : "failed_ram.gsplatworld");
		const Dictionary report = GaussianSplatHlodAutoTree::build_streamed_for_test(fixture.input, output, options, hooks);
		CHECK(int(report.get("error", OK)) == ERR_SKIP);
		CHECK(String(report.get("reason", "")) == "test_after_frontier");
		CHECK(int64_t(report.get("tiles_baked", 0)) == 1);
		CHECK(int64_t(report.get("tiles_planned", 0)) > 1);
		for (const char *key : { "scratch_bytes_peak", "scratch_bytes_total", "scratch_spill_bytes_peak",
				 "scratch_spill_bytes_total", "scratch_combined_bytes_peak" }) {
			CHECK(int64_t(report.get(key, 0)) > 0);
		}
		if (disk) {
			CHECK(int64_t(report.get("scratch_frontier_bytes_peak", 0)) > 0);
			CHECK(int64_t(report.get("scratch_frontier_bytes_total", 0)) > 0);
		} else {
			CHECK(int64_t(report.get("frontier_ram_bytes_peak", 0)) > 0);
			CHECK_FALSE(report.has("scratch_frontier_bytes_total"));
			CHECK_FALSE(String(report.get("scratch_frontier_bytes_total_unavailable_reason", "")).is_empty());
		}
		CHECK_FALSE(FileAccess::exists(output));
		CHECK(directory_empty(fixture.scratch));
	}
	const Dictionary missing = bound_build(fixture.dir.path_join("missing.gsplatworld"), fixture.dir.path_join("unused.gsplatworld"), options);
	CHECK(int(missing.get("error", OK)) == ERR_FILE_NOT_FOUND);
	CHECK_FALSE(missing.has("scratch_bytes_peak"));
	CHECK_FALSE(String(missing.get("scratch_bytes_peak_unavailable_reason", "")).is_empty());
}

#undef D2_AUTO_NEED

} // namespace TestHlodD2AutoTree
