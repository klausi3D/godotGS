#pragma once

// #1172: every format that persists the raw `Gaussian` struct records the struct
// layout it was written with, and every reader refuses a payload recorded under a
// different layout. Before the fix nothing recorded the layout, so a same-size
// field change (e.g. swapping painterly_meta and render_meta) would misdecode
// every existing file with no check firing.
//
// Covered here: `.gsplatworld` (v2 header word, streamable and resident and
// compressed paths, plus the v1 legacy route), the `.gsplatcache` PLY sidecar
// (which is a .gsplatworld written by PLYLoader::write_cache), and the GSF scene
// header word that guards the GAUSSIAN_DATA chunk.

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/ply_loader.h"
#include "../persistence/gaussian_scene_serializer.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace TestGaussianLayoutFingerprint {

// Byte offsets of the .gsplatworld v2 header fields this file inspects (see the
// version history at kWorldVersion in io/gaussian_splat_world_io.cpp).
static constexpr uint64_t WORLD_VERSION_OFFSET = 4u;
static constexpr uint64_t WORLD_GAUSSIAN_OFFSET_FIELD = 56u;
static constexpr uint64_t WORLD_LAYOUT_WORD_OFFSET = 104u;
static constexpr uint64_t WORLD_V2_HEADER_BYTES = 112u;
// GSF: the HEAD chunk payload follows a 16-byte chunk header; the layout word is
// the u16 at payload byte 58 (see _pack_scene_header).
static constexpr uint64_t GSF_LAYOUT_WORD_OFFSET = 16u + 58u;

static String fixture_path(const String &p_prefix, const String &p_suffix) {
	const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	const String base_temp = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : ".";
	const String dir = base_temp.path_join("godotgs_layout_fingerprint_fixtures");
	DirAccess::make_dir_recursive_absolute(dir);
	return dir.path_join(p_prefix + "_" + itos(ticks) + p_suffix);
}

static void remove_fixture(const String &p_path) {
	DirAccess::remove_absolute(p_path);
}

// Two splats whose fields are all distinct, so a misplaced word is visible.
static Ref<GaussianData> make_data() {
	Vector<Gaussian> gaussians;
	for (int i = 0; i < 2; i++) {
		Gaussian g;
		g.position = Vector3(1.0f + i, 2.0f, 3.0f);
		g.opacity = 0.5f;
		g.scale = Vector3(0.1f, 0.2f, 0.3f);
		g.area = 1.0f;
		g.rotation = Quaternion();
		g.sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
		g.sh_1[0] = Vector3(0.01f, 0.02f, 0.03f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.brush_axes = Vector2(1.0f, 1.0f);
		g.painterly_meta = gaussian_pack_painterly_meta(uint16_t(17 + i), 300);
		gaussians.push_back(g);
	}
	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);
	return data;
}

static Ref<GaussianSplatWorld> make_world(const Ref<GaussianData> &p_data) {
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(p_data);
	world->set_bounds(p_data->get_aabb());
	return world;
}

static uint32_t read_u32_at(const String &p_path, uint64_t p_offset) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null() || f->get_length() < p_offset + 4u) {
		return 0xFFFFFFFFu;
	}
	f->seek(p_offset);
	return f->get_32();
}

static uint64_t read_u64_at(const String &p_path, uint64_t p_offset) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null() || f->get_length() < p_offset + 8u) {
		return UINT64_MAX;
	}
	f->seek(p_offset);
	return f->get_64();
}

static uint16_t read_u16_at(const String &p_path, uint64_t p_offset) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null() || f->get_length() < p_offset + 2u) {
		return 0xFFFFu;
	}
	f->seek(p_offset);
	return f->get_16();
}

static bool write_u32_at(const String &p_path, uint64_t p_offset, uint32_t p_value) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ_WRITE);
	if (f.is_null() || f->get_length() < p_offset + 4u) {
		return false;
	}
	f->seek(p_offset);
	f->store_32(p_value);
	return true;
}

static bool write_u16_at(const String &p_path, uint64_t p_offset, uint16_t p_value) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ_WRITE);
	if (f.is_null() || f->get_length() < p_offset + 2u) {
		return false;
	}
	f->seek(p_offset);
	f->store_16(p_value);
	return true;
}

static void check_same_splats(const Ref<GaussianData> &p_got, const Ref<GaussianData> &p_expected) {
	if (!(p_got.is_valid())) {
		FAIL("precondition failed: p_got.is_valid()");
		return;
	}
	if (p_got->get_count() != p_expected->get_count()) {
		FAIL("splat count changed across the round-trip");
		return;
	}
	for (int i = 0; i < p_expected->get_count(); i++) {
		const Gaussian got = p_got->get_gaussian(i);
		const Gaussian want = p_expected->get_gaussian(i);
		CHECK(got.position.is_equal_approx(want.position));
		CHECK(got.sh_dc.is_equal_approx(want.sh_dc));
		CHECK_EQ(got.painterly_meta, want.painterly_meta);
		CHECK_EQ(got.render_meta, want.render_meta);
	}
}

// Asserts that BOTH .gsplatworld load entry points refuse p_path with the layout
// error (ERR_FILE_UNRECOGNIZED; structural corruption is ERR_FILE_CORRUPT).
static void check_world_refused_for_layout(const String &p_path) {
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = OK;
	Ref<Resource> res = loader.load(p_path, "", &err);
	CHECK_FALSE(res.is_valid());
	CHECK_EQ(err, ERR_FILE_UNRECOGNIZED);

	Error resident_err = OK;
	Ref<GaussianSplatWorld> resident = loader.load_resident(p_path, &resident_err);
	CHECK_FALSE(resident.is_valid());
	CHECK_EQ(resident_err, ERR_FILE_UNRECOGNIZED);
}

} // namespace TestGaussianLayoutFingerprint

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld records the Gaussian struct layout and loads it back (#1172)") {
	using namespace TestGaussianLayoutFingerprint;
	CHECK(GAUSSIAN_STRUCT_LAYOUT_VERSION != 0u);
	CHECK_EQ(gaussian_resolve_persisted_layout_version(0u), GAUSSIAN_STRUCT_LAYOUT_VERSION_UNRECORDED);
	CHECK_EQ(gaussian_resolve_persisted_layout_version(7u), 7u);

	Ref<GaussianData> data = make_data();
	const String path = fixture_path("world_layout_ok", ".gsplatworld");
	ResourceFormatSaverGaussianSplatWorld saver;
	if (!(saver.save(make_world(data), path) == OK)) {
		FAIL("precondition failed: saver.save(make_world(data), path) == OK");
		return;
	}

	CHECK_EQ(read_u32_at(path, WORLD_VERSION_OFFSET), 2u);
	CHECK_EQ(read_u32_at(path, WORLD_LAYOUT_WORD_OFFSET), GAUSSIAN_STRUCT_LAYOUT_VERSION);
	CHECK_EQ(read_u64_at(path, WORLD_GAUSSIAN_OFFSET_FIELD), WORLD_V2_HEADER_BYTES);

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<Resource> streamed_res = loader.load(path, "", &err);
	Ref<GaussianSplatWorld> streamed = streamed_res;
	CHECK_EQ(err, OK);
	if (!(streamed.is_valid())) {
		FAIL("precondition failed: streamed.is_valid()");
		return;
	}
	CHECK_EQ(streamed->get_splat_count(), 2);

	Error resident_err = ERR_BUG;
	Ref<GaussianSplatWorld> resident = loader.load_resident(path, &resident_err);
	CHECK_EQ(resident_err, OK);
	if (!(resident.is_valid())) {
		FAIL("precondition failed: resident.is_valid()");
		return;
	}
	check_same_splats(resident->get_gaussian_data(), data);

	remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][MalformedCorpus] gsplatworld refuses a payload recorded under another Gaussian layout (#1172)") {
	using namespace TestGaussianLayoutFingerprint;
	Ref<GaussianData> data = make_data();
	ResourceFormatSaverGaussianSplatWorld saver;

	// Streamable (generic save) and explicit resident-uncompressed.
	const String path = fixture_path("world_layout_mismatch", ".gsplatworld");
	if (!(saver.save(make_world(data), path) == OK)) {
		FAIL("precondition failed: saver.save(make_world(data), path) == OK");
		return;
	}
	if (!(write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u))) {
		FAIL("precondition failed: write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u)");
		return;
	}
	check_world_refused_for_layout(path);

	// A v2 header must record a layout: 0 is not the legacy route (that is v1).
	if (!(write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, 0u))) {
		FAIL("precondition failed: write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, 0u)");
		return;
	}
	check_world_refused_for_layout(path);

	// Sanity: restoring the word makes the same file load, so the word is the only defect.
	if (!(write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION))) {
		FAIL("precondition failed: write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION)");
		return;
	}
	{
		ResourceFormatLoaderGaussianSplatWorld loader;
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
		CHECK_EQ(err, OK);
		if (!(resident.is_valid())) {
			FAIL("precondition failed: resident.is_valid()");
			return;
		}
		check_same_splats(resident->get_gaussian_data(), data);
	}
	remove_fixture(path);

	// Compressed resident export carries the same word.
	const String compressed_path = fixture_path("world_layout_mismatch_compressed", ".gsplatworld");
	if (!(saver.save_resident_compressed(make_world(data), compressed_path) == OK)) {
		FAIL("precondition failed: saver.save_resident_compressed(make_world(data), compressed_path) == OK");
		return;
	}
	CHECK_EQ(read_u32_at(compressed_path, WORLD_LAYOUT_WORD_OFFSET), GAUSSIAN_STRUCT_LAYOUT_VERSION);
	if (!(write_u32_at(compressed_path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u))) {
		FAIL("precondition failed: write_u32_at(compressed_path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u)");
		return;
	}
	check_world_refused_for_layout(compressed_path);
	remove_fixture(compressed_path);
}

TEST_CASE("[GaussianSplatting][WorldIO] gsplatworld v1 files without a layout word keep loading (#1172)") {
	using namespace TestGaussianLayoutFingerprint;
	// A hand-written v1 world: 104-byte header, no layout word, one splat. v1
	// resolves to GAUSSIAN_STRUCT_LAYOUT_VERSION_UNRECORDED, which is today's layout.
	Gaussian g;
	g.position = Vector3(4.0f, 5.0f, 6.0f);
	g.opacity = 0.75f;
	g.scale = Vector3(0.5f, 0.5f, 0.5f);
	g.rotation = Quaternion();
	g.sh_dc = Color(0.1f, 0.2f, 0.3f, 1.0f);
	g.painterly_meta = gaussian_pack_painterly_meta(23, 5);

	const String path = fixture_path("world_v1_legacy", ".gsplatworld");
	{
		Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
		if (!(f.is_valid())) {
			FAIL("precondition failed: f.is_valid()");
			return;
		}
		f->store_32(0x57505347u); // 'GSPW'
		f->store_32(1u); // v1
		f->store_32(0u); // flags
		f->store_32(1u); // splat_count
		f->store_32(0u); // sh_degree
		f->store_32(0u); // sh_first_order
		f->store_32(0u); // sh_high_order
		for (int i = 0; i < 6; i++) {
			f->store_float(0.0f); // bounds position + size
		}
		f->store_32(0u); // chunk_count
		f->store_64(104u); // gaussian_offset: v1 header size
		for (int i = 0; i < 5; i++) {
			f->store_64(0u); // sh, chunk table, indices, metadata offsets; metadata size
		}
		if (f->get_position() != 104u) {
			FAIL("hand-written v1 header is not 104 bytes");
			return;
		}
		f->store_buffer(reinterpret_cast<const uint8_t *>(&g), sizeof(Gaussian));
	}

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
	CHECK_EQ(err, OK);
	if (!(resident.is_valid())) {
		FAIL("precondition failed: resident.is_valid()");
		return;
	}
	Ref<GaussianData> loaded = resident->get_gaussian_data();
	if (!(loaded.is_valid())) {
		FAIL("precondition failed: loaded.is_valid()");
		return;
	}
	if (loaded->get_count() != 1) {
		FAIL("v1 world should hold one splat");
		return;
	}
	const Gaussian got = loaded->get_gaussian(0);
	CHECK(got.position.is_equal_approx(g.position));
	CHECK_EQ(got.painterly_meta, g.painterly_meta);

	remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][PLY] gsplatcache records the Gaussian layout and a mismatched cache is re-parsed (#1172)") {
	using namespace TestGaussianLayoutFingerprint;
	const String ply_path = fixture_path("cache_layout", ".ply");
	{
		Ref<FileAccess> f = FileAccess::open(ply_path, FileAccess::WRITE);
		if (!(f.is_valid())) {
			FAIL("precondition failed: f.is_valid()");
			return;
		}
		f->store_string("ply\nformat binary_little_endian 1.0\nelement vertex 2\n"
						"property float x\nproperty float y\nproperty float z\n"
						"property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
						"property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
						"property float opacity\n"
						"property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
						"end_header\n");
		const float v0[14] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0 };
		const float v1[14] = { 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0 };
		f->store_buffer(reinterpret_cast<const uint8_t *>(v0), sizeof(v0));
		f->store_buffer(reinterpret_cast<const uint8_t *>(v1), sizeof(v1));
	}
	const String cache_path = ply_path.get_basename() + ".gsplatcache";

	{
		PLYLoader loader;
		if (!(loader.load_file(ply_path) == OK)) {
			FAIL("precondition failed: loader.load_file(ply_path) == OK");
			return;
		}
	}
	// The default project setting enables the cache; without one there is nothing
	// to check, so its absence fails rather than skips.
	if (!FileAccess::exists(cache_path)) {
		FAIL("PLYLoader should have written a .gsplatcache");
		remove_fixture(ply_path);
		return;
	}
	CHECK_EQ(read_u32_at(cache_path, WORLD_VERSION_OFFSET), 2u);
	CHECK_EQ(read_u32_at(cache_path, WORLD_LAYOUT_WORD_OFFSET), GAUSSIAN_STRUCT_LAYOUT_VERSION);

	// Sanity: the untouched cache is a hit, so the miss below is caused by the word.
	{
		PLYLoader loader;
		if (!(loader.load_file(ply_path) == OK)) {
			FAIL("precondition failed: loader.load_file(ply_path) == OK");
			return;
		}
		CHECK_EQ(loader.get_splat_count(), 2);
		const Dictionary stats = loader.get_load_statistics();
		if (!(stats.has("cache_hit"))) {
			FAIL("precondition failed: stats.has(\"cache_hit\")");
			return;
		}
		CHECK(bool(stats["cache_hit"]));
	}

	if (!(write_u32_at(cache_path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u))) {
		FAIL("precondition failed: write_u32_at(cache_path, WORLD_LAYOUT_WORD_OFFSET, GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u)");
		return;
	}
	check_world_refused_for_layout(cache_path);
	{
		PLYLoader loader;
		CHECK(loader.load_file(ply_path) == OK);
		CHECK_EQ(loader.get_splat_count(), 2);
		const Dictionary stats = loader.get_load_statistics();
		if (!(stats.has("cache_hit"))) {
			FAIL("precondition failed: stats.has(\"cache_hit\")");
			return;
		}
		CHECK_FALSE_MESSAGE(bool(stats["cache_hit"]), "A cache recorded under another Gaussian layout must not be reused");
	}
	// The re-parse rewrote the cache under this build's layout.
	CHECK_EQ(read_u32_at(cache_path, WORLD_LAYOUT_WORD_OFFSET), GAUSSIAN_STRUCT_LAYOUT_VERSION);

	remove_fixture(ply_path);
	remove_fixture(cache_path);
}

TEST_CASE("[GaussianSplatting][Persistence] GSF records the Gaussian struct layout in its header (#1172)") {
	using namespace TestGaussianLayoutFingerprint;
	Ref<GaussianData> data = make_data();
	const String path = fixture_path("gsf_layout_ok", ".gsf");

	GaussianSplatting::GaussianSceneSerializer serializer;
	serializer.set_enable_checksum(false); // So the tests below can patch the header in place.
	if (!(serializer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK)) {
		FAIL("precondition failed: serializer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK");
		return;
	}
	CHECK_EQ(read_u16_at(path, GSF_LAYOUT_WORD_OFFSET), uint16_t(GAUSSIAN_STRUCT_LAYOUT_VERSION));

	Ref<GaussianData> loaded;
	loaded.instantiate();
	CHECK_EQ(serializer.load_scene(path, loaded.ptr(), nullptr, nullptr), OK);
	check_same_splats(loaded, data);

	// Files written before #1172 hold 0 in this slot (it was `_reserved_v2`) and
	// resolve to the legacy layout, which is today's: they keep loading.
	if (!(write_u16_at(path, GSF_LAYOUT_WORD_OFFSET, 0u))) {
		FAIL("precondition failed: write_u16_at(path, GSF_LAYOUT_WORD_OFFSET, 0u)");
		return;
	}
	Ref<GaussianData> legacy_loaded;
	legacy_loaded.instantiate();
	CHECK_EQ(serializer.load_scene(path, legacy_loaded.ptr(), nullptr, nullptr), OK);
	check_same_splats(legacy_loaded, data);

	remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][Persistence][MalformedCorpus] GSF refuses a GAUSSIAN_DATA chunk recorded under another Gaussian layout (#1172)") {
	using namespace TestGaussianLayoutFingerprint;
	Ref<GaussianData> data = make_data();
	const String path = fixture_path("gsf_layout_mismatch", ".gsf");

	GaussianSplatting::GaussianSceneSerializer serializer;
	serializer.set_enable_checksum(false);
	if (!(serializer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK)) {
		FAIL("precondition failed: serializer.save_scene(path, data.ptr(), nullptr, Dictionary()) == OK");
		return;
	}
	if (!(write_u16_at(path, GSF_LAYOUT_WORD_OFFSET, uint16_t(GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u)))) {
		FAIL("precondition failed: write_u16_at(path, GSF_LAYOUT_WORD_OFFSET, uint16_t(GAUSSIAN_STRUCT_LAYOUT_VERSION + 1u))");
		return;
	}

	// The target already holds splats; a refused load must leave them untouched.
	Ref<GaussianData> target = make_data();
	CHECK_EQ(serializer.load_scene(path, target.ptr(), nullptr, nullptr), ERR_FILE_UNRECOGNIZED);
	check_same_splats(target, data);
	CHECK_EQ(serializer.validate_file(path), ERR_FILE_UNRECOGNIZED);

	// Sanity: restoring the word makes the same file load.
	if (!(write_u16_at(path, GSF_LAYOUT_WORD_OFFSET, uint16_t(GAUSSIAN_STRUCT_LAYOUT_VERSION)))) {
		FAIL("precondition failed: write_u16_at(path, GSF_LAYOUT_WORD_OFFSET, uint16_t(GAUSSIAN_STRUCT_LAYOUT_VERSION))");
		return;
	}
	Ref<GaussianData> loaded;
	loaded.instantiate();
	CHECK_EQ(serializer.load_scene(path, loaded.ptr(), nullptr, nullptr), OK);
	check_same_splats(loaded, data);

	remove_fixture(path);
}
