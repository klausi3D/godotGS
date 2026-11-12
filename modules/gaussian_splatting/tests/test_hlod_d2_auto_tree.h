#pragma once

// D2-3 phase 1: host-only runtime-red contract tests. modules/SCsub discovers
// tests/*.h; [WorldIO] selects the existing CPU lane. No external fixtures.
#include "test_macros.h"

#include "../core/gaussian_splat_hlod_auto_tree.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gaussian_splat_world_paged.h"
#include "../io/gs_auto_tree_source.h"
#include "../io/gs_world_generator.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

#include <algorithm>
#include <cstring>
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
	key["codec"] = p_manifest.get("codec", Dictionary());
	key["input_file_sha256"] = p_manifest.get("input_file_sha256", "");
	return JSON::stringify(key, "", true, true).sha256_text();
}

struct Fixture {
	String dir;
	String scratch;
	String input;
	gs_world_generator::Report generated;

	bool create(uint32_t p_total = 32768, uint32_t p_codec = gs_world_writer::kCodecGsqV1, int64_t p_seed_delta = 0) {
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
		CHECK(params.get("max_tile_splats", 0) == options["max_tile_splats"]);
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

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] Bound auto-tree API refuses invalid options and unavailable GSQ output") {
	Fixture fixture, plain;
	D2_AUTO_NEED(fixture.create());
	D2_AUTO_NEED(plain.create(32768, gs_world_writer::kCodecNone));
	struct Bad {
		const char *key;
		Variant value;
		const char *reason;
	};
	const Bad cases[] = {
		{ "codec", "gsq", "v4_gsq_output_requires_bv_c4" },
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
		CHECK(int(report.get("error", OK)) == (String(bad.key) == "codec" && bad.value == Variant("gsq") ? ERR_UNAVAILABLE : ERR_INVALID_PARAMETER));
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

#undef D2_AUTO_NEED

} // namespace TestHlodD2AutoTree
