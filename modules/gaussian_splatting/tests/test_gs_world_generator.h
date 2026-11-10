#pragma once

// Synthetic world generator (BV-05; docs/architecture/adr-beyond-vram-worlds.md §6.1-§6.4,
// §4.1 sidecar manifest, W8). CPU only; no GPU.
//
// What these cases prove:
//   - the Tier A plan carries the ADR §6.2 content table exactly (every class and the
//     total), every chunk is <= 65,536 splats, chunks tile the world in Morton order, and
//     the expected file size is the ADR's 54.8 GiB; T0 and A-mini totals are exact too;
//   - generator_params_sha is pinned per tier and changes with every parameter (the key
//     list is derived from the parameter dictionary, never typed by hand);
//   - no parameter is dead: changing any one changes the generated content or is refused
//     by validation, and the refused set is pinned;
//   - byte determinism: the same parameters give the same world and manifest bytes, a
//     different seed gives different ones, and threaded and sequential chunk generation
//     agree; golden digests pin the content across platforms;
//   - the content table matches the output: splats re-classified from the loaded world
//     reproduce every chunk's per-class counts, and every splat lies in its chunk's cell;
//   - SH is spatially correlated (near pairs r >= 0.9, far pairs |r| <= 0.15) and the
//     hash-noise control (field off) is not (near |r| <= 0.15), so the check can fail;
//   - verify_world() accepts the generated world and names the chunk, section or field
//     a single corrupted byte, a truncation or an edited manifest breaks;
//   - a T-ref selection reproduces the parent's chunks byte for byte and is bounded;
//   - GSQ v1 (BV-C7): the compressed world is bit 7, deterministic, at most a quarter of the
//     flat size, has another gen_sha than the flat world, carries the codec block, loads
//     resident, and verify_world() names the chunk whose page run a flipped byte breaks and a
//     manifest whose codec is not the file's.

#include "test_macros.h"

#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_format.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gs_world_generator.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

#include <cmath>
#include <cstring>

namespace TestGSWorldGenerator {

namespace gen = gs_world_generator;

inline String wg_dir(const String &p_suffix) {
	static uint32_t s_seq = 0;
	const String dir = OS::get_singleton()->get_temp_path().path_join(
			"gs_wg_" + p_suffix + "_" + itos(OS::get_singleton()->get_ticks_usec()) + "_" + itos(s_seq++));
	DirAccess::make_dir_recursive_absolute(dir);
	return dir;
}

inline void wg_remove_dir(const String &p_dir) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
		if (name != "." && name != "..") {
			DirAccess::remove_absolute(p_dir.path_join(name));
		}
	}
	da->list_dir_end();
	DirAccess::remove_absolute(p_dir);
}

inline PackedByteArray wg_read(const String &p_path) {
	PackedByteArray out;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return out;
	}
	out.resize(int64_t(f->get_length()));
	if (!out.is_empty()) {
		f->get_buffer(out.ptrw(), out.size());
	}
	return out;
}

inline bool wg_write(const String &p_path, const PackedByteArray &p_bytes) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	f->store_buffer(p_bytes.ptr(), p_bytes.size());
	return f->get_error() == OK;
}

// A small world built from T-smoke: every class present, 65,536 splats, and a chunk bound
// low enough that town cells subdivide.
inline gen::Params wg_tiny() {
	gen::Params base;
	gen::params_for_tier("T-smoke", base);
	Dictionary over;
	Dictionary world;
	world["total_splats"] = 65536;
	world["max_chunk_splats"] = 4096;
	over["world"] = world;
	Dictionary town;
	town["density_per_m2"] = 10;
	over["town"] = town;
	Dictionary wall;
	wall["density_per_m2"] = 5;
	over["wall"] = wall;
	Dictionary foliage;
	foliage["density_per_m2"] = 5;
	over["foliage"] = foliage;
	Dictionary beacon;
	beacon["grid"] = 16;
	over["beacon"] = beacon;
	gen::Params out;
	String err;
	if (gen::apply_overrides(base, over, out, err) != OK) {
		MESSAGE("wg_tiny overrides failed: ", err);
	}
	out.tier = "test-tiny";
	return out;
}

inline Dictionary wg_manifest(const String &p_world) {
	const Variant v = JSON::parse_string(FileAccess::get_file_as_string(gen::manifest_path_for(p_world)));
	return v.get_type() == Variant::DICTIONARY ? Dictionary(v) : Dictionary();
}

inline bool wg_problems_contain(const Dictionary &p_report, const String &p_needle) {
	const Array problems = p_report.get("problems", Array());
	for (int i = 0; i < problems.size(); i++) {
		if (String(problems[i]).contains(p_needle)) {
			return true;
		}
	}
	return false;
}

// Leaf keys of a parameter dictionary as paths ("world.total_splats", "beacon.cells[3]").
inline void wg_leaf_paths(const Variant &p_v, const String &p_prefix, LocalVector<String> &r_paths) {
	if (p_v.get_type() == Variant::DICTIONARY) {
		const Dictionary d = p_v;
		for (const Variant &k : d.keys()) {
			wg_leaf_paths(d[k], p_prefix.is_empty() ? String(k) : p_prefix + "." + String(k), r_paths);
		}
	} else if (p_v.get_type() == Variant::ARRAY) {
		const Array a = p_v;
		for (int i = 0; i < a.size(); i++) {
			wg_leaf_paths(a[i], p_prefix + "[" + itos(i) + "]", r_paths);
		}
	} else {
		r_paths.push_back(p_prefix);
	}
}

// Returns a deep copy of p_root with the leaf at p_path replaced via p_fn(old value).
template <typename F>
inline Variant wg_mutate(const Variant &p_root, const String &p_path, F p_fn) {
	const int dot = p_path.find(".");
	const int bracket = p_path.find("[");
	if (p_root.get_type() == Variant::DICTIONARY) {
		Dictionary d = Dictionary(p_root).duplicate(true);
		const int cut = (dot < 0) ? (bracket < 0 ? p_path.length() : bracket) : (bracket < 0 ? dot : MIN(dot, bracket));
		const String key = p_path.substr(0, cut);
		String rest = p_path.substr(cut);
		if (rest.begins_with(".")) {
			rest = rest.substr(1);
		}
		d[key] = rest.is_empty() ? p_fn(d[key]) : wg_mutate(d[key], rest, p_fn);
		return d;
	}
	if (p_root.get_type() == Variant::ARRAY) {
		Array a = Array(p_root).duplicate(true);
		const int close = p_path.find("]");
		const int idx = p_path.substr(1, close - 1).to_int();
		String rest = p_path.substr(close + 1);
		if (rest.begins_with(".")) {
			rest = rest.substr(1);
		}
		a[idx] = rest.is_empty() ? p_fn(a[idx]) : wg_mutate(a[idx], rest, p_fn);
		return a;
	}
	return p_fn(p_root);
}

// The chunk index of every splat, and the class each splat's position puts it in, from
// the plan's features alone (an independent re-derivation of the content table).
inline int wg_classify(const gen::Plan &p_plan, const Vector3 &p_pos) {
	const float beacon_base = float(p_plan.params.beacon_base_y_q) / 4096.0f;
	for (const gen::Feature &f : p_plan.features) {
		const gen::IRect &r = f.footprint;
		const bool in_xz = p_pos.x >= float(r.x0) && p_pos.x < float(r.x1) && p_pos.z >= float(r.z0) && p_pos.z < float(r.z1);
		switch (f.cls) {
			case gen::CLASS_BEACON:
				if (p_pos.z == float(f.beacon_center_z_m) && p_pos.x >= float(r.x0) && p_pos.x <= float(r.x1) && p_pos.y >= beacon_base) {
					return gen::CLASS_BEACON;
				}
				break;
			case gen::CLASS_WALL:
				if (p_pos.z == float(f.plane_z_m) && p_pos.x >= float(r.x0) && p_pos.x < float(r.x1)) {
					return gen::CLASS_WALL;
				}
				break;
			default:
				if (in_xz) {
					return f.cls;
				}
				break;
		}
	}
	return gen::CLASS_PLAIN;
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] Tier A plan carries the ADR 6.2 content table exactly; T0 and A-mini totals are exact") {
	gen::Params a;
	REQUIRE(gen::params_for_tier("A", a) == OK);
	gen::Plan plan;
	String err;
	const Error perr = gen::build_plan(a, plan, err);
	if (perr != OK) {
		FAIL("Tier A plan failed: ", err);
		return;
	}
	// ADR §6.2, written out: towns 6 x 40 x 40 m at 800/m2, 2 walls x 1,800 m x 4 m at
	// 300/m2, 64 foliage clusters x 30 x 30 m at 200/m2, 4 beacons x 64 tiles x 4,096, and
	// the plain as the remainder over 4,105,504 m2.
	CHECK(plan.class_totals[gen::CLASS_TOWN] == 7680000u);
	CHECK(plan.class_totals[gen::CLASS_WALL] == 4320000u);
	CHECK(plan.class_totals[gen::CLASS_FOLIAGE] == 11520000u);
	CHECK(plan.class_totals[gen::CLASS_BEACON] == 1048576u);
	CHECK(plan.class_totals[gen::CLASS_PLAIN] == 176758016u);
	CHECK(plan.plain_area == 4105504);
	CHECK(plan.total_splats == 201326592u);
	CHECK(plan.class_instances[gen::CLASS_TOWN] == 6u);
	CHECK(plan.class_instances[gen::CLASS_WALL] == 2u);
	CHECK(plan.class_instances[gen::CLASS_FOLIAGE] == 64u);
	CHECK(plan.class_instances[gen::CLASS_BEACON] == 256u);
	CHECK(plan.world.x0 == -1024);
	CHECK(plan.world.x1 == 1024);
	CHECK(plan.sh_first_order == 3u);
	CHECK(plan.sh_high_order == 12u);
	// 292 B per splat (144 B gaussian + 144 B SH3 + 4 B index) plus header and chunk table:
	// the ADR's 54.8 GiB.
	const uint64_t bytes = gen::expected_file_bytes(plan, LocalVector<uint32_t>());
	CHECK(bytes == 112u + 201326592ull * 292u + uint64_t(plan.chunks.size()) * 56u);
	CHECK(double(bytes) / double(uint64_t(1) << 30u) == doctest::Approx(54.75).epsilon(0.001));

	// Chunks: <= 65,536 splats, non-empty, Morton-ordered, origin-aligned power-of-two cells
	// from 32 m down to 8 m, tiling the world exactly.
	uint64_t sum = 0;
	int64_t area = 0;
	bool ok_bound = true, ok_order = true, ok_cells = true;
	uint32_t subdivided = 0;
	for (uint32_t c = 0; c < plan.chunks.size(); c++) {
		const gen::ChunkPlan &cp = plan.chunks[c];
		ok_bound = ok_bound && cp.count >= 1u && cp.count <= 65536u;
		ok_order = ok_order && (c == 0u || plan.chunks[c - 1u].morton < cp.morton);
		const int64_t w = cp.cell.width();
		ok_cells = ok_cells && w == cp.cell.depth() && (w == 32 || w == 16 || w == 8) && cp.cell.x0 % w == 0 && cp.cell.z0 % w == 0;
		subdivided += w < 32 ? 1u : 0u;
		sum += cp.count;
		area += cp.cell.area();
	}
	CHECK(ok_bound);
	CHECK(ok_order);
	CHECK(ok_cells);
	CHECK(sum == 201326592u);
	CHECK(area == 2048 * 2048);
	CHECK(plan.max_chunk_count <= 65536u);
	// "The exact count is computed by the generator and recorded in the manifest" (§6.2);
	// pinned so a layout change is a reviewed change.
	CHECK(plan.chunks.size() == 4939u);
	CHECK(subdivided > 0u);
	MESSAGE("Tier A: ", plan.chunks.size(), " chunks (", subdivided, " subdivided), largest ", plan.max_chunk_count, " splats");

	// Beacons carry unique, decodable ids.
	HashSet<uint64_t> codes;
	for (const gen::Feature &f : plan.features) {
		if (f.cls == gen::CLASS_BEACON) {
			CHECK(((f.beacon_code >> 8u) & 0xFFFFu) == f.beacon_id);
			CHECK(((f.beacon_code >> 24u) & 0xFFFFu) == (~f.beacon_id & 0xFFFFu));
			codes.insert(f.beacon_code);
		}
	}
	CHECK(codes.size() == 256u);

	for (const char *tier : { "T0", "A-mini", "T-smoke" }) {
		gen::Params p;
		REQUIRE(gen::params_for_tier(tier, p) == OK);
		gen::Plan tp;
		const Error terr = gen::build_plan(p, tp, err);
		CHECK_MESSAGE(terr == OK, tier, ": ", err);
		CHECK(tp.total_splats == uint64_t(p.total_splats));
		CHECK(tp.max_chunk_count <= 65536u);
	}
	gen::Params t0;
	gen::params_for_tier("T0", t0);
	CHECK(t0.total_splats == 4194304);
	CHECK(t0.tiles_x * t0.tile_size_m == 512);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] generator_params_sha is pinned per tier and changes with every parameter") {
	// Pinned: a change here means the content definition changed (bump kGeneratorVersion
	// and re-prime the world cache, ADR §8.4).
	struct Pin {
		const char *tier;
		const char *sha;
	};
	const Pin pins[] = {
		{ "A", "404e9cc0104b2cb7ebeb7fa374d8888ced9a387e25e51c2b5fafd704577bd759" },
		{ "T0", "87a0952abb0a9404a833e6323d2967f84b8b91090da3b6d8cad959a83439d05b" },
		{ "A-mini", "8b64dec307b2161df95edebf8eed7afcd5dcea9c010ae0ca8e13c116b17259bd" },
	};
	for (const Pin &pin : pins) {
		gen::Params p;
		REQUIRE(gen::params_for_tier(pin.tier, p) == OK);
		CHECK_MESSAGE(gen::params_sha(p) == String(pin.sha), pin.tier, " -> ", gen::params_sha(p));
		// Stable through a dictionary round trip, including the JSON text of a manifest
		// (which turns every integer into a float).
		gen::Params back;
		String err;
		const Dictionary parsed = JSON::parse_string(JSON::stringify(gen::params_to_dict(p)));
		REQUIRE(gen::params_from_dict(parsed, back, err) == OK);
		CHECK(gen::params_sha(back) == gen::params_sha(p));
	}

	gen::Params a;
	gen::params_for_tier("A", a);
	const Dictionary base = gen::params_to_dict(a);
	const String base_sha = gen::params_sha(a);
	LocalVector<String> paths;
	wg_leaf_paths(base, String(), paths);
	CHECK(paths.size() > 70u);
	uint32_t changed = 0;
	for (const String &path : paths) {
		const Variant mutated = wg_mutate(base, path, [](const Variant &v) -> Variant {
			if (v.get_type() == Variant::STRING) {
				return String(v) + "x";
			}
			return int64_t(v) + 1;
		});
		const String sha = gen::sha256_hex_of_canonical(mutated);
		if (sha != base_sha) {
			changed++;
		} else {
			FAIL_CHECK("generator_params_sha ignores ", path);
		}
	}
	CHECK(changed == paths.size());
	CHECK(gen::sha256_hex_of_canonical(base) == base_sha);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] no parameter is dead: each changes the content or is refused (pinned set)") {
	const gen::Params tiny = wg_tiny();
	String base_digest;
	String err;
	REQUIRE(gen::compute_content_digest(tiny, base_digest, err) == OK);
	const Dictionary base = gen::params_to_dict(tiny);
	LocalVector<String> paths;
	wg_leaf_paths(base, String(), paths);

	// Policy (explicit on purpose): labels that are not content, the mutation for keys
	// where +1 is not a valid value, and the keys whose every single-key change is refused.
	HashSet<String> labels;
	labels.insert("schema");
	labels.insert("tier");
	HashMap<String, int64_t> value;
	value["world.tile_size_m"] = 512;
	value["world.base_cell_m"] = 16;
	value["world.min_cell_m"] = 32;
	value["world.max_chunk_splats"] = 2048;
	value["town.style.opacity_max_q8"] = 254;
	value["wall.style.opacity_max_q8"] = 254;
	value["beacon.cells[1]"] = 1; // row 3 would sit next to the corridor
	value["beacon.cells[3]"] = 1;
	value["town.size_m"] = 48;
	value["town.block_m"] = 4;
	value["town.building_m"] = 4;
	value["wall.length_m"] = 202;
	value["wall.footprint_m"] = 8;
	value["wall.separation_m"] = 12;
	value["beacon.grid"] = 32;
	value["beacon.style.sigma_min_q"] = 255;
	value["beacon.style.opacity_min_q8"] = 254;
	value["beacon.style.opacity_max_q8"] = 254;
	value["sh.degree"] = 2;
	HashSet<String> expect_refused;
	expect_refused.insert("world.min_cell_m"); // a 32 m minimum cell cannot hold the town at 4,096 per chunk
	expect_refused.insert("town.block_m"); // the block must divide min_cell_m and exceed building_m
	expect_refused.insert("beacon.style.opacity_max_q8"); // max >= min = 255
	expect_refused.insert("beacon.style.color_var_q8"); // beacons are flat-coded: frozen at 0

	// placement.margin_m acts only through rejected placement attempts, and the tiny
	// world's one tile leaves the town no room to move; it is proven on Tier A's plan.
	HashSet<String> plan_checked;
	plan_checked.insert("placement.margin_m");
	{
		gen::Params a;
		gen::params_for_tier("A", a);
		gen::Params a2 = a;
		a2.margin_m += 1;
		gen::Plan pa, pb;
		REQUIRE(gen::build_plan(a, pa, err) == OK);
		REQUIRE(gen::build_plan(a2, pb, err) == OK);
		bool moved = false;
		for (uint32_t i = 0; i < pa.features.size() && i < pb.features.size(); i++) {
			const gen::IRect &x = pa.features[i].footprint;
			const gen::IRect &y = pb.features[i].footprint;
			moved = moved || x.x0 != y.x0 || x.z0 != y.z0;
		}
		CHECK_MESSAGE(moved, "placement.margin_m + 1 moves no Tier A feature");
	}

	uint32_t live = 0;
	for (const String &path : paths) {
		if (labels.has(path) || plan_checked.has(path)) {
			continue;
		}
		const Variant mutated = wg_mutate(base, path, [&](const Variant &v) -> Variant {
			return value.has(path) ? Variant(value[path]) : Variant(int64_t(v) + 1);
		});
		gen::Params p;
		Error e = gen::params_from_dict(mutated, p, err);
		String digest;
		if (e == OK) {
			e = gen::compute_content_digest(p, digest, err);
		}
		if (expect_refused.has(path)) {
			CHECK_MESSAGE(e != OK, path, " was expected to be refused");
			continue;
		}
		if (e != OK) {
			FAIL_CHECK(path, " refused: ", err);
			continue;
		}
		if (digest == base_digest) {
			FAIL_CHECK("dead parameter: ", path, " does not change the content");
			continue;
		}
		live++;
	}
	CHECK(live + expect_refused.size() + labels.size() + plan_checked.size() == paths.size());
	MESSAGE(live, " parameters change the content; ", expect_refused.size(), " are refused when changed alone");
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] byte determinism: same params, same bytes; another seed, other bytes; golden digests") {
	const gen::Params tiny = wg_tiny();
	const String d1 = wg_dir("det1");
	const String d2 = wg_dir("det2");
	const String d3 = wg_dir("det3");
	const String w1 = d1.path_join("world.gsplatworld");
	const String w2 = d2.path_join("world.gsplatworld");
	const String w3 = d3.path_join("world.gsplatworld");
	gen::GenerateOptions opt;
	gen::Report r1, r2, r3;
	REQUIRE(gen::generate_world(tiny, w1, opt, r1) == OK);
	REQUIRE(gen::generate_world(tiny, w2, opt, r2) == OK);
	gen::Params other = tiny;
	other.seed += 1;
	REQUIRE(gen::generate_world(other, w3, opt, r3) == OK);

	const PackedByteArray b1 = wg_read(w1);
	const PackedByteArray b2 = wg_read(w2);
	const PackedByteArray b3 = wg_read(w3);
	CHECK(b1.size() > 0);
	CHECK(b1 == b2);
	CHECK(r1.file_sha256 == r2.file_sha256);
	CHECK(wg_read(gen::manifest_path_for(w1)) == wg_read(gen::manifest_path_for(w2)));
	CHECK(b1 != b3);
	CHECK(r1.file_sha256 != r3.file_sha256);
	CHECK(r1.params_sha != r3.params_sha);
	CHECK(wg_read(gen::manifest_path_for(w1)) != wg_read(gen::manifest_path_for(w3)));
	CHECK(r1.splat_count == 65536u);
	CHECK(uint64_t(b1.size()) == r1.file_bytes);

	// Threaded and sequential chunk generation produce the same bytes.
	gen::Plan plan;
	String err;
	REQUIRE(gen::build_plan(tiny, plan, err) == OK);
	bool same = true;
	for (uint32_t c = 0; c < plan.chunks.size(); c++) {
		gen::ChunkBuffers a, b;
		gen::generate_chunk(plan, c, a, true);
		gen::generate_chunk(plan, c, b, false);
		same = same && a.gaussians.size() == b.gaussians.size() &&
				memcmp(a.gaussians.ptr(), b.gaussians.ptr(), sizeof(Gaussian) * a.gaussians.size()) == 0 &&
				memcmp(a.sh_high.ptr(), b.sh_high.ptr(), sizeof(Vector3) * a.sh_high.size()) == 0 &&
				a.desc.radius == b.desc.radius;
	}
	CHECK(same);

	// Golden digests: the generated bytes are integer / exact-dyadic arithmetic, so these
	// hold on every platform and compiler. A change means the content changed: bump
	// gs_world_generator::kGeneratorVersion.
	String smoke_digest;
	gen::Params smoke;
	gen::params_for_tier("T-smoke", smoke);
	REQUIRE(gen::compute_content_digest(smoke, smoke_digest, err) == OK);
	String tiny_digest;
	REQUIRE(gen::compute_content_digest(tiny, tiny_digest, err) == OK);
	CHECK_MESSAGE(smoke_digest == "c694677fb8604f6eca889aefca980e3a0b60be43acf5ba2c95ce058294c3c236", "T-smoke content digest ", smoke_digest);
	CHECK_MESSAGE(tiny_digest == "c480c573a500e3681717796f2a4ac7414f240d887782fe734237866cbe1fe382", "tiny content digest ", tiny_digest);
	CHECK_MESSAGE(r1.file_sha256 == "7c3fbb02ec97a50329173a2220939cb15f8dc1ee1fb2a44546d4d2ddeb0991c9", "tiny file sha256 ", r1.file_sha256);

	wg_remove_dir(d1);
	wg_remove_dir(d2);
	wg_remove_dir(d3);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] the content table matches the generated output; every splat lies in its cell") {
	const gen::Params tiny = wg_tiny();
	const String dir = wg_dir("content");
	const String path = dir.path_join("world.gsplatworld");
	gen::GenerateOptions opt;
	gen::Report rep;
	REQUIRE(gen::generate_world(tiny, path, opt, rep) == OK);
	gen::Plan plan;
	String err;
	REQUIRE(gen::build_plan(tiny, plan, err) == OK);

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error lerr = FAILED;
	Ref<GaussianSplatWorld> world = loader.load_resident(path, &lerr);
	REQUIRE(lerr == OK);
	if (world.is_null() || world->get_gaussian_data().is_null()) {
		FAIL("the generated world did not load resident");
		return;
	}
	const Vector<StaticChunk> &chunks = world->get_static_chunks();
	if (uint32_t(chunks.size()) != plan.chunks.size()) {
		FAIL("loaded ", chunks.size(), " chunks, the plan has ", plan.chunks.size());
		return;
	}
	CHECK(world->get_splat_count() == plan.total_splats);
	CHECK(world->get_sh_degree() == 3u);
	const LocalVector<Gaussian> &g = world->get_gaussian_data()->get_gaussian_storage();
	if (uint64_t(g.size()) != plan.total_splats) {
		FAIL("loaded ", g.size(), " gaussians");
		return;
	}
	const Dictionary manifest = wg_manifest(path);
	const Array mchunks = manifest.get("chunks", Array());
	if (uint32_t(mchunks.size()) != plan.chunks.size()) {
		FAIL("the manifest lists ", mchunks.size(), " chunks, the plan has ", plan.chunks.size());
		return;
	}
	bool counts_ok = true, inside_ok = true, manifest_ok = true;
	uint64_t per_class[gen::CLASS_COUNT] = {};
	for (uint32_t c = 0; c < plan.chunks.size(); c++) {
		const gen::ChunkPlan &cp = plan.chunks[c];
		uint32_t got[gen::CLASS_COUNT] = {};
		const Vector<uint32_t> &idx = chunks[c].indices;
		counts_ok = counts_ok && uint32_t(idx.size()) == cp.count;
		for (int i = 0; i < idx.size(); i++) {
			const Vector3 &p = g[idx[i]].position;
			got[wg_classify(plan, p)]++;
			inside_ok = inside_ok && p.x >= float(cp.cell.x0) && p.x < float(cp.cell.x1) && p.z >= float(cp.cell.z0) && p.z < float(cp.cell.z1);
			inside_ok = inside_ok && chunks[c].bounds.has_point(p);
		}
		const Array mcc = Dictionary(mchunks[c]).get("class_counts", Array());
		for (int k = 0; k < gen::CLASS_COUNT; k++) {
			counts_ok = counts_ok && got[k] == cp.class_counts[k];
			manifest_ok = manifest_ok && int64_t(mcc[k]) == int64_t(cp.class_counts[k]);
			per_class[k] += got[k];
		}
	}
	CHECK(counts_ok);
	CHECK(inside_ok);
	CHECK(manifest_ok);
	const Dictionary table = manifest.get("content_table", Dictionary());
	for (int k = 0; k < gen::CLASS_COUNT; k++) {
		CHECK(per_class[k] == plan.class_totals[k]);
		CHECK(int64_t(Dictionary(table[gen::class_name(gen::ContentClass(k))])["splats"]) == int64_t(per_class[k]));
	}
	CHECK(int64_t(table["total"]) == 65536);
	// Every class is present, so the classifier was exercised on each.
	for (int k = 0; k < gen::CLASS_COUNT; k++) {
		CHECK(per_class[k] > 0u);
	}
	world.unref();
	wg_remove_dir(dir);
}

namespace {

struct Corr {
	double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
	uint64_t n = 0;
	void add(double a, double b) {
		sx += a;
		sy += b;
		sxx += a * a;
		syy += b * b;
		sxy += a * b;
		n++;
	}
	double r() const {
		const double cov = sxy / n - (sx / n) * (sy / n);
		const double va = sxx / n - (sx / n) * (sx / n);
		const double vb = syy / n - (sy / n) * (sy / n);
		return (va <= 0 || vb <= 0) ? 0.0 : cov / std::sqrt(va * vb);
	}
};

struct PlainSample {
	Vector3 pos;
	float c1; // band 1: sh_1[0].x
	float c3; // band 3: the last high-order coefficient's z
};

// Near pairs: plain splats sharing a 0.5 m bin. Far pairs: bins 96 m (6 correlation
// lengths) apart in x.
inline void wg_sh_correlation(const gen::Params &p_params, double &r_near1, double &r_near3, double &r_far1, uint64_t &r_pairs) {
	gen::Plan plan;
	String err;
	if (gen::build_plan(p_params, plan, err) != OK) {
		r_near1 = r_near3 = r_far1 = NAN;
		return;
	}
	HashMap<int64_t, LocalVector<PlainSample>> bins;
	gen::ChunkBuffers buf;
	for (uint32_t c = 0; c < plan.chunks.size(); c++) {
		gen::generate_chunk(plan, c, buf);
		for (uint32_t i = 0; i < buf.gaussians.size(); i++) {
			const Gaussian &g = buf.gaussians[i];
			if (g.position.y > 0.2f || g.normal.y != 1.0f) {
				continue; // keep ground splats only
			}
			const int64_t bx = int64_t(std::floor(g.position.x * 2.0f)) + 4096;
			const int64_t bz = int64_t(std::floor(g.position.z * 2.0f)) + 4096;
			const PlainSample s{ g.position, g.sh_1[0].x, buf.sh_high[uint64_t(i) * 12u + 11u].z };
			bins[bx * 16384 + bz].push_back(s);
		}
	}
	Corr near1, near3, far1;
	for (const KeyValue<int64_t, LocalVector<PlainSample>> &kv : bins) {
		const LocalVector<PlainSample> &v = kv.value;
		for (uint32_t i = 0; i + 1u < v.size(); i += 2u) {
			near1.add(v[i].c1, v[i + 1u].c1);
			near3.add(v[i].c3, v[i + 1u].c3);
		}
		const int64_t far_key = kv.key + int64_t(192) * 16384;
		if (bins.has(far_key) && !v.is_empty()) {
			far1.add(v[0].c1, bins[far_key][0].c1);
		}
	}
	r_near1 = near1.r();
	r_near3 = near3.r();
	r_far1 = far1.r();
	r_pairs = MIN(near1.n, far1.n);
}

} // namespace

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] far from the origin every splat still lies inside its own cell (Tier A edge chunks)") {
	// Away from the origin the float grid is coarser than the 2^-24 m fixed point, so a
	// round-to-nearest narrowing can lift a splat onto its cell's upper edge, i.e. into the
	// neighbouring chunk. Tier A's last chunks in Morton order sit at the +x/+z corner.
	gen::Params a;
	REQUIRE(gen::params_for_tier("A", a) == OK);
	gen::Plan plan;
	String err;
	REQUIRE(gen::build_plan(a, plan, err) == OK);
	uint64_t checked = 0, outside = 0;
	gen::ChunkBuffers buf;
	for (uint32_t k = 0; k < 6u; k++) {
		const uint32_t c = plan.chunks.size() - 1u - k;
		const gen::ChunkPlan &cp = plan.chunks[c];
		gen::generate_chunk(plan, c, buf);
		for (const Gaussian &g : buf.gaussians) {
			const Vector3 &p = g.position;
			outside += (p.x >= float(cp.cell.x0) && p.x < float(cp.cell.x1) && p.z >= float(cp.cell.z0) && p.z < float(cp.cell.z1)) ? 0u : 1u;
			checked++;
		}
	}
	CHECK(checked > 100000u);
	CHECK_MESSAGE(outside == 0u, outside, " of ", checked, " splats lie outside their cell");
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] SH is spatially correlated; the hash-noise control is not") {
	gen::Params smoke;
	REQUIRE(gen::params_for_tier("T-smoke", smoke) == OK);
	double n1 = 0, n3 = 0, f1 = 0;
	uint64_t pairs = 0;
	wg_sh_correlation(smoke, n1, n3, f1, pairs);
	MESSAGE("correlated: near band1 r=", n1, " near band3 r=", n3, " far band1 r=", f1, " (", pairs, " pairs)");
	CHECK(pairs > 1000u);
	CHECK(n1 >= 0.9);
	CHECK(n3 >= 0.9);
	CHECK(std::abs(f1) <= 0.15);

	// Negative control: the correlated field switched off leaves only per-splat jitter.
	gen::Params noise = smoke;
	noise.sh_field_amp_q8 = 0;
	double c1 = 0, c3 = 0, cf = 0;
	uint64_t cpairs = 0;
	wg_sh_correlation(noise, c1, c3, cf, cpairs);
	MESSAGE("hash-noise control: near band1 r=", c1, " near band3 r=", c3);
	CHECK(cpairs > 1000u);
	CHECK(std::abs(c1) <= 0.15);
	CHECK(std::abs(c3) <= 0.15);

	// View dependence: every band carries signal.
	gen::Plan plan;
	String err;
	REQUIRE(gen::build_plan(smoke, plan, err) == OK);
	gen::ChunkBuffers buf;
	gen::generate_chunk(plan, 0, buf);
	double e1 = 0, e2 = 0, e3 = 0;
	for (uint32_t i = 0; i < buf.gaussians.size(); i++) {
		e1 += std::abs(buf.gaussians[i].sh_1[0].x) + std::abs(buf.gaussians[i].sh_1[2].z);
		e2 += std::abs(buf.sh_high[uint64_t(i) * 12u + 0u].y);
		e3 += std::abs(buf.sh_high[uint64_t(i) * 12u + 11u].x);
	}
	CHECK(e1 > 0.0);
	CHECK(e2 > 0.0);
	CHECK(e3 > 0.0);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] verify_world accepts the world and names what a corruption breaks") {
	const gen::Params tiny = wg_tiny();
	const String dir = wg_dir("verify");
	const String path = dir.path_join("world.gsplatworld");
	gen::GenerateOptions opt;
	gen::Report rep;
	REQUIRE(gen::generate_world(tiny, path, opt, rep) == OK);
	Dictionary v;
	CHECK(gen::verify_world(path, v) == OK);
	CHECK(bool(v["ok"]));

	const PackedByteArray pristine = wg_read(path);
	const Dictionary manifest = wg_manifest(path);
	const Array chunks = manifest["chunks"];
	if (chunks.size() <= 6) {
		FAIL("the verify world needs more than 6 chunks, got ", chunks.size());
		return;
	}

	// One flipped byte in chunk 3's gaussian bytes.
	{
		PackedByteArray b = pristine;
		const int64_t off = int64_t(Dictionary(chunks[3])["gaussian_offset"]) + 17;
		b.write[off] ^= 0x40u;
		REQUIRE(wg_write(path, b));
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "chunk_digest_mismatch: chunk 3 gaussian_sha256"));
		CHECK_FALSE(wg_problems_contain(v, "chunk 2 "));
		CHECK(wg_problems_contain(v, "file_sha256_mismatch"));
	}
	// One flipped byte in chunk 5's SH bytes.
	{
		PackedByteArray b = pristine;
		const int64_t off = int64_t(Dictionary(chunks[5])["sh_offset"]) + 5;
		b.write[off] ^= 0x01u;
		REQUIRE(wg_write(path, b));
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "chunk_digest_mismatch: chunk 5 sh_sha256"));
		CHECK_FALSE(wg_problems_contain(v, "gaussian_sha256"));
	}
	// One flipped byte in chunk 6's chunk-table radius.
	{
		PackedByteArray b = pristine;
		const int64_t table = int64_t(Dictionary(chunks[0])["sh_offset"]) +
				int64_t(Dictionary(manifest["world"])["splat_count"]) * 12 * 12;
		b.write[table + 6 * 56 + 36] ^= 0x01u;
		REQUIRE(wg_write(path, b));
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "chunk_record_mismatch: chunk 6"));
		CHECK_FALSE(wg_problems_contain(v, "chunk_digest_mismatch"));
	}
	// Truncation.
	{
		PackedByteArray b = pristine;
		b.resize(b.size() - 1);
		REQUIRE(wg_write(path, b));
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "file_bytes_mismatch"));
	}
	REQUIRE(wg_write(path, pristine));
	CHECK(gen::verify_world(path, v) == OK);
	// An edited manifest: a parameter changed without its sha, and a chunk count changed.
	{
		const String mpath = gen::manifest_path_for(path);
		const String text = FileAccess::get_file_as_string(mpath);
		const String seed_key = vformat("\"seed\":%d", tiny.seed);
		REQUIRE(text.contains(seed_key));
		Ref<FileAccess> f = FileAccess::open(mpath, FileAccess::WRITE);
		f->store_string(text.replace(seed_key, vformat("\"seed\":%d", tiny.seed + 1)));
		f.unref();
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "generator_params_sha_mismatch"));
		f = FileAccess::open(mpath, FileAccess::WRITE);
		f->store_string(text);
		f.unref();
		CHECK(gen::verify_world(path, v) == OK);
	}
	wg_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] a T-ref selection reproduces the parent's chunks byte for byte and is bounded") {
	const gen::Params tiny = wg_tiny();
	const String dir = wg_dir("tref");
	const String parent = dir.path_join("world.gsplatworld");
	const String ref = dir.path_join("ref.gsplatworld");
	gen::GenerateOptions opt;
	gen::Report rep;
	REQUIRE(gen::generate_world(tiny, parent, opt, rep) == OK);
	gen::Plan plan;
	String err;
	REQUIRE(gen::build_plan(tiny, plan, err) == OK);

	gen::GenerateOptions sel;
	uint64_t expect_splats = 0;
	for (uint32_t c = 0; c < plan.chunks.size(); c++) {
		if (plan.chunks[c].class_counts[gen::CLASS_TOWN] > 0u || c == 0u || c + 1u == plan.chunks.size()) {
			sel.selection.push_back(c);
			expect_splats += plan.chunks[c].count;
		}
	}
	REQUIRE(sel.selection.size() > 2u);
	gen::Report rref;
	REQUIRE(gen::generate_world(tiny, ref, sel, rref) == OK);
	CHECK(rref.splat_count == expect_splats);
	CHECK(rref.chunk_count == sel.selection.size());
	CHECK(rref.params_sha == rep.params_sha);
	Dictionary v;
	CHECK(gen::verify_world(ref, v) == OK);

	const Dictionary pm = wg_manifest(parent);
	const Dictionary rm = wg_manifest(ref);
	const Array pchunks = pm["chunks"];
	const Array rchunks = rm["chunks"];
	if (uint32_t(rchunks.size()) != sel.selection.size() || uint32_t(pchunks.size()) != plan.chunks.size()) {
		FAIL("the T-ref manifest lists ", rchunks.size(), " chunks, the selection has ", sel.selection.size());
		return;
	}
	bool same = true;
	for (uint32_t i = 0; i < sel.selection.size(); i++) {
		const Dictionary a = rchunks[i];
		const Dictionary b = pchunks[sel.selection[i]];
		same = same && int64_t(a["plan_index"]) == int64_t(sel.selection[i]);
		for (const char *key : { "gaussian_sha256", "sh_sha256", "count", "radius" }) {
			same = same && a[key] == b[key];
		}
		same = same && Array(a["aabb"]) == Array(b["aabb"]);
	}
	CHECK(same);
	CHECK(Dictionary(rm["selection"]).has("selection_sha"));

	// Bounded (ADR §6.1: <= 12M per checkpoint) and well-formed.
	gen::GenerateOptions too_big = sel;
	too_big.max_selection_splats = expect_splats - 1u;
	gen::Report rbig;
	CHECK(gen::generate_world(tiny, dir.path_join("big.gsplatworld"), too_big, rbig) != OK);
	CHECK(rbig.message.contains("T-ref bound"));
	CHECK_FALSE(FileAccess::exists(dir.path_join("big.gsplatworld")));
	gen::GenerateOptions unsorted;
	unsorted.selection.push_back(3);
	unsorted.selection.push_back(1);
	gen::Report runs;
	CHECK(gen::generate_world(tiny, dir.path_join("bad.gsplatworld"), unsorted, runs) != OK);
	CHECK_FALSE(FileAccess::exists(dir.path_join("bad.gsplatworld")));
	wg_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] refusals: res:// paths, malformed parameters, invalid layouts; no stale manifest survives") {
	const gen::Params tiny = wg_tiny();
	gen::GenerateOptions opt;
	gen::Report rep;
	CHECK(gen::generate_world(tiny, "res://world.gsplatworld", opt, rep) == ERR_INVALID_PARAMETER);
	CHECK(rep.message.contains("res://"));
	const String dir = wg_dir("refuse");
	CHECK(gen::generate_world(tiny, dir.path_join("world.ply"), opt, rep) == ERR_INVALID_PARAMETER);

	String err;
	gen::Params p;
	Dictionary d = gen::params_to_dict(tiny);
	d["extra"] = 1;
	CHECK(gen::params_from_dict(d, p, err) != OK);
	CHECK(err.contains("unknown parameter 'extra'"));
	d = gen::params_to_dict(tiny);
	Dictionary world = Dictionary(d["world"]).duplicate();
	world.erase("total_splats");
	d["world"] = world;
	CHECK(gen::params_from_dict(d, p, err) != OK);
	CHECK(err.contains("missing parameter 'world.total_splats'"));
	d = gen::params_to_dict(tiny);
	d["seed"] = 1.5;
	CHECK(gen::params_from_dict(d, p, err) != OK);
	Dictionary over;
	over["nope"] = 1;
	CHECK(gen::apply_overrides(tiny, over, p, err) != OK);

	gen::Params bad = tiny;
	bad.max_chunk_splats = 65537;
	CHECK(gen::validate_params(bad, err) != OK);
	bad = tiny;
	bad.total_splats = 1000; // smaller than the features
	gen::Plan plan;
	CHECK(gen::build_plan(bad, plan, err) != OK);
	bad = tiny;
	bad.town_count = 40; // cannot be placed without overlap
	CHECK(gen::build_plan(bad, plan, err) != OK);
	CHECK(err.contains("could not place"));

	// A stale manifest is removed before generation starts, so a failed run leaves none.
	const String path = dir.path_join("world.gsplatworld");
	Ref<FileAccess> f = FileAccess::open(gen::manifest_path_for(path), FileAccess::WRITE);
	f->store_string("{\"stale\":true}");
	f.unref();
	REQUIRE(gen::generate_world(tiny, path, opt, rep) == OK);
	CHECK_FALSE(FileAccess::get_file_as_string(gen::manifest_path_for(path)).contains("stale"));
	wg_remove_dir(dir);
}

// BV-C7: the same world as GSQ v1 (plain v3 + bit 7). The codec block enters gen_sha, the
// manifest carries the codec block and per-chunk page runs, the engine loads the world, and
// verify_world() names the chunk whose page run a flipped byte breaks.
TEST_CASE("[GaussianSplatting][WorldIO][WorldGenerator] GSQ v1 world: codec in manifest and gen_sha, loads, verify names a corrupted page run") {
	const gen::Params tiny = wg_tiny();
	const String dir = wg_dir("gsq");
	const String flat_path = dir.path_join("flat.gsplatworld");
	const String path = dir.path_join("world.gsplatworld");
	gen::GenerateOptions flat_opt;
	gen::Report flat_rep;
	REQUIRE(gen::generate_world(tiny, flat_path, flat_opt, flat_rep) == OK);
	gen::GenerateOptions opt;
	opt.codec_id = gs_world_writer::kCodecGsqV1;
	gen::Report rep;
	REQUIRE(gen::generate_world(tiny, path, opt, rep) == OK);

	CHECK(rep.codec_id == gs_world_writer::kCodecGsqV1);
	CHECK(rep.params_sha == flat_rep.params_sha);
	CHECK_MESSAGE(rep.gen_sha != flat_rep.gen_sha, "the codec block must separate the two encodings' cache keys");
	CHECK(rep.splat_count == flat_rep.splat_count);
	CHECK(rep.page_count > 0u);
	CHECK_MESSAGE(rep.file_bytes * 4u < flat_rep.file_bytes, "GSQ ", rep.file_bytes, " B vs flat ", flat_rep.file_bytes, " B");

	const PackedByteArray pristine = wg_read(path);
	if (pristine.size() <= 12) {
		FAIL("the GSQ world could not be read back");
		return;
	}
	uint32_t flags = 0;
	memcpy(&flags, pristine.ptr() + 8, 4);
	CHECK((flags & gs_world_format::kFlagPagedChunks) != 0u);

	const Dictionary manifest = wg_manifest(path);
	const Dictionary codec = Dictionary(manifest.get("writer", Dictionary())).get("codec", Dictionary());
	CHECK(int64_t(codec.get("codec_id", -1)) == int64_t(gs_world_writer::kCodecGsqV1));
	const Dictionary block = manifest.get("codec", Dictionary());
	CHECK(int64_t(block.get("page_count", -1)) == int64_t(rep.page_count));
	CHECK(int64_t(block.get("pages_stored_bytes", -1)) == int64_t(rep.pages_stored_bytes));
	CHECK(String(block.get("decoded_digest_set_sha256", "")).length() == 64);
	CHECK_FALSE(wg_manifest(flat_path).has("codec"));
	const Array chunks = manifest.get("chunks", Array());
	if (chunks.size() <= 4) {
		FAIL("the GSQ world needs more than 4 chunks, got ", chunks.size());
		return;
	}
	CHECK(Dictionary(chunks[3]).has("pages_sha256"));
	CHECK_FALSE(Dictionary(chunks[3]).has("gaussian_offset"));

	// Byte determinism holds for the compressed world too.
	gen::Report again;
	REQUIRE(gen::generate_world(tiny, dir.path_join("again.gsplatworld"), opt, again) == OK);
	CHECK(again.file_sha256 == rep.file_sha256);

	// The engine reads what the generator wrote: every page decodes, counts match.
	{
		ResourceFormatLoaderGaussianSplatWorld loader;
		Error lerr = FAILED;
		Ref<GaussianSplatWorld> world = loader.load_resident(path, &lerr);
		CHECK(lerr == OK);
		if (world.is_null()) {
			FAIL("the GSQ world did not load resident");
			return;
		}
		CHECK(uint64_t(world->get_splat_count()) == rep.splat_count);
		CHECK(world->get_static_chunks().size() == chunks.size());
	}

	Dictionary v;
	CHECK(gen::verify_world(path, v) == OK);
	CHECK(bool(v["ok"]));
	// One flipped byte inside chunk 3's page run.
	{
		PackedByteArray b = pristine;
		// The last byte of the run is the last byte of its last page (never padding).
		const int64_t off = int64_t(Dictionary(chunks[3])["pages_offset"]) + int64_t(Dictionary(chunks[3])["pages_bytes"]) - 1;
		b.write[off] ^= 0x40u;
		REQUIRE(wg_write(path, b));
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "chunk_digest_mismatch: chunk 3 pages_sha256"));
		CHECK(wg_problems_contain(v, "page_invalid: chunk 3 "));
		CHECK(wg_problems_contain(v, "file_sha256_mismatch"));
		CHECK_FALSE(wg_problems_contain(v, "chunk 2 "));
	}
	// A manifest that claims the flat codec for a paged file.
	{
		REQUIRE(wg_write(path, pristine));
		const String mpath = gen::manifest_path_for(path);
		const String text = FileAccess::get_file_as_string(mpath);
		REQUIRE(text.contains("\"codec_id\":1"));
		Ref<FileAccess> f = FileAccess::open(mpath, FileAccess::WRITE);
		f->store_string(text.replace("\"codec_id\":1", "\"codec_id\":0"));
		f.unref();
		CHECK(gen::verify_world(path, v) != OK);
		CHECK(wg_problems_contain(v, "codec_mismatch"));
	}
	wg_remove_dir(dir);
}

} // namespace TestGSWorldGenerator
