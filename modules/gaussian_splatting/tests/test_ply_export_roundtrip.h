/**************************************************************************/
/*  test_ply_export_roundtrip.h                                           */
/**************************************************************************/

#pragma once

// #1170: GaussianData::save_to_file / GaussianSplatAsset::save_to_file used to
// write a DC-only PLY: no f_rest_* columns, so every view-dependent SH band was
// dropped and a degree-3 asset round-tripped to flat colour. These cases import a
// synthetic degree-3 PLY, export it and re-import the export with PLYLoader, and
// compare every coefficient against the source file's own values.

#include "test_macros.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../io/ply_loader.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"

namespace TestPLYExportRoundtrip {

static constexpr float SH_C0 = 0.28209479177387814f;
static constexpr int SPLATS = 5;
static constexpr int REST_PER_CHANNEL = 15;
static constexpr int REST_COMPONENTS = 45;

struct SourceSplat {
	float position[3];
	float f_dc[3];
	float f_rest[REST_COMPONENTS]; // channel-major, exactly as stored in the file
	float opacity_logit;
	float log_scale[3];
	float rot_wxyz[4];
};

// Deterministic, sign-mixed, non-trivial values: every coefficient is distinct so
// a channel/term transposition in the writer cannot round-trip by accident.
static float _value(int p_splat, int p_slot) {
	const float magnitude = 0.05f + 0.013f * float((p_splat * 53 + p_slot * 17) % 41);
	return ((p_splat + p_slot) % 3 == 0) ? -magnitude : magnitude;
}

static SourceSplat _make_source_splat(int p_index) {
	SourceSplat s;
	for (int c = 0; c < 3; c++) {
		s.position[c] = float(p_index) * 0.75f + float(c) * 0.25f;
		s.f_dc[c] = _value(p_index, 100 + c) * 4.0f;
		s.log_scale[c] = -3.0f + 0.1f * float(c + p_index);
	}
	for (int i = 0; i < REST_COMPONENTS; i++) {
		s.f_rest[i] = _value(p_index, i);
	}
	s.opacity_logit = 0.3f * float(p_index) - 0.5f;
	s.rot_wxyz[0] = 1.0f;
	s.rot_wxyz[1] = 0.0f;
	s.rot_wxyz[2] = 0.0f;
	s.rot_wxyz[3] = 0.0f;
	return s;
}

static String _fixture_path(const String &p_tag) {
	const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	const String base = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : String(".");
	return base.path_join("godotgs_ply_export_" + p_tag + "_" + itos(int64_t(ticks)) + ".ply");
}

static void _remove_with_cache(const String &p_path) {
	DirAccess::remove_absolute(p_path);
	// PLYLoader writes a sibling .gsplatcache on load.
	DirAccess::remove_absolute(p_path.get_basename() + ".gsplatcache");
}

// Canonical Inria degree-3 binary PLY (x y z nx ny nz f_dc f_rest_0..44 opacity scale rot).
static bool _write_degree3_ply(const String &p_path, const SourceSplat *p_splats, int p_count) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	String header = "ply\nformat binary_little_endian 1.0\n";
	header += vformat("element vertex %d\n", p_count);
	header += "property float x\nproperty float y\nproperty float z\n";
	header += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n";
	for (int i = 0; i < REST_COMPONENTS; i++) {
		header += vformat("property float f_rest_%d\n", i);
	}
	header += "property float opacity\n";
	header += "property float scale_0\nproperty float scale_1\nproperty float scale_2\n";
	header += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
	header += "end_header\n";
	f->store_string(header);
	for (int i = 0; i < p_count; i++) {
		const SourceSplat &s = p_splats[i];
		for (int c = 0; c < 3; c++) {
			f->store_float(s.position[c]);
		}
		for (int c = 0; c < 3; c++) {
			f->store_float(s.f_dc[c]);
		}
		for (int r = 0; r < REST_COMPONENTS; r++) {
			f->store_float(s.f_rest[r]);
		}
		f->store_float(s.opacity_logit);
		for (int c = 0; c < 3; c++) {
			f->store_float(s.log_scale[c]);
		}
		for (int c = 0; c < 4; c++) {
			f->store_float(s.rot_wxyz[c]);
		}
	}
	return f->get_error() == OK;
}

static PackedStringArray _read_header_property_names(const String &p_path) {
	PackedStringArray names;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return names;
	}
	for (int guard = 0; guard < 256 && !f->eof_reached(); guard++) {
		const String line = f->get_line().strip_edges();
		if (line == "end_header") {
			break;
		}
		if (line.begins_with("property ")) {
			names.push_back(line.get_slicec(' ', 2));
		}
	}
	return names;
}

static Ref<::GaussianData> _load_with_ply_loader(const String &p_path) {
	PLYLoader loader;
	if (loader.load_file(p_path) != OK) {
		return Ref<::GaussianData>();
	}
	return loader.get_gaussian_data();
}

// Coefficient for SH term t (1..15) of channel c, as the source file stores it.
static float _source_rest(const SourceSplat &p_splat, int p_term, int p_channel) {
	return p_splat.f_rest[(p_term - 1) + p_channel * REST_PER_CHANNEL];
}

// Assert every DC and band-1..3 coefficient of `p_data` equals the source file.
static void _check_matches_source(const Ref<::GaussianData> &p_data, const SourceSplat *p_splats, int p_count) {
	// CHECK + early return (not REQUIRE): a mismatch here is the defect under
	// test, and it must fail this case without terminating the whole lane.
	if (p_data.is_null()) {
		FAIL("PLYLoader returned no GaussianData");
		return;
	}
	if (p_data->get_count() != p_count) {
		FAIL("splat count ", p_data->get_count(), " != ", p_count);
		return;
	}
	CHECK_EQ(p_data->get_sh_first_order_count(), 3u);
	CHECK_EQ(p_data->get_sh_high_order_count(), 12u);
	for (int i = 0; i < p_count; i++) {
		const PackedFloat32Array sh = p_data->get_spherical_harmonics(i);
		if (sh.size() != 16 * 3) {
			FAIL("splat ", i, " carries ", sh.size(), " SH floats, expected 48 (degree 3)");
			return;
		}
		for (int c = 0; c < 3; c++) {
			// sh[0..2] is sh_dc = SH_C0 * f_dc; fp32 tolerance relative to magnitude.
			const float expected_dc = SH_C0 * p_splats[i].f_dc[c];
			CHECK_MESSAGE(Math::abs(sh[c] - expected_dc) <= 1e-6f + 1e-5f * Math::abs(expected_dc),
					vformat("splat %d DC channel %d: got %f expected %f", i, c, sh[c], expected_dc));
		}
		for (int term = 1; term < 16; term++) {
			for (int c = 0; c < 3; c++) {
				const float got = sh[term * 3 + c];
				const float expected = _source_rest(p_splats[i], term, c);
				CHECK_MESSAGE(Math::abs(got - expected) <= 1e-6f,
						vformat("splat %d SH term %d channel %d: got %f expected %f", i, term, c, got, expected));
			}
		}
	}
}

} // namespace TestPLYExportRoundtrip

TEST_CASE("[GaussianSplatting][PLY] save_to_file round-trips every degree-3 SH coefficient through PLYLoader (#1170)") {
	using namespace TestPLYExportRoundtrip;
	SourceSplat splats[SPLATS];
	for (int i = 0; i < SPLATS; i++) {
		splats[i] = _make_source_splat(i);
	}
	const String source_path = _fixture_path("src");
	const String export_path = _fixture_path("out");
	REQUIRE(_write_degree3_ply(source_path, splats, SPLATS));

	Ref<::GaussianData> imported = _load_with_ply_loader(source_path);
	if (imported.is_null()) {
		FAIL("PLYLoader could not import the synthetic degree-3 source");
		return;
	}
	// Precondition: the import itself carries the full degree-3 payload, so a
	// failure below is the writer's, not the loader's.
	_check_matches_source(imported, splats, SPLATS);

	REQUIRE_EQ(imported->save_to_file(export_path), OK);

	const PackedStringArray names = _read_header_property_names(export_path);
	for (int i = 0; i < REST_COMPONENTS; i++) {
		CHECK_MESSAGE(names.has(vformat("f_rest_%d", i)), vformat("export lacks f_rest_%d", i));
	}
	// Default export is the canonical Inria layout: no GodotGS-only columns.
	CHECK_FALSE(names.has("palette_id"));
	CHECK_FALSE(names.has("brush_override_id"));
	CHECK_FALSE(names.has("brush_axis_u"));
	CHECK_FALSE(names.has("brush_axis_v"));
	CHECK_FALSE(names.has("stroke_age"));

	Ref<::GaussianData> reloaded = _load_with_ply_loader(export_path);
	_check_matches_source(reloaded, splats, SPLATS);

	_remove_with_cache(source_path);
	_remove_with_cache(export_path);
}

TEST_CASE("[GaussianSplatting][PLY] GaussianSplatAsset::save_to_file keeps degree-3 SH and the linear DC term (#1170)") {
	using namespace TestPLYExportRoundtrip;
	SourceSplat splats[SPLATS];
	for (int i = 0; i < SPLATS; i++) {
		splats[i] = _make_source_splat(i);
	}
	const String source_path = _fixture_path("asset_src");
	const String export_path = _fixture_path("asset_out");
	REQUIRE(_write_degree3_ply(source_path, splats, SPLATS));

	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	REQUIRE_EQ(asset->load_from_file(source_path), OK);
	REQUIRE_EQ(asset->save_to_file(export_path), OK);

	Ref<::GaussianData> reloaded = _load_with_ply_loader(export_path);
	_check_matches_source(reloaded, splats, SPLATS);

	_remove_with_cache(source_path);
	_remove_with_cache(export_path);
}

TEST_CASE("[GaussianSplatting][PLY] save_to_file writes painterly columns only when asked (#1170)") {
	using namespace TestPLYExportRoundtrip;
	Ref<::GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(2);
	for (uint32_t i = 0; i < gaussians.size(); i++) {
		Gaussian &g = gaussians[i];
		g.position = Vector3(float(i), 0.0f, 0.0f);
		g.scale = Vector3(0.1f, 0.1f, 0.1f);
		g.opacity = 0.5f;
		g.area = 1.0f;
		g.sh_dc = Color(0.1f, 0.2f, 0.3f, 1.0f);
		g.brush_axes = Vector2(0.25f, 0.75f);
		g.stroke_age = 2.5f;
		g.painterly_meta = gaussian_pack_painterly_meta(uint16_t(7 + i), uint16_t(40 + i));
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
	}
	data->set_gaussians(gaussians);

	const String canonical_path = _fixture_path("canonical");
	const String painterly_path = _fixture_path("painterly");
	REQUIRE_EQ(data->save_to_file(canonical_path), OK);
	REQUIRE_EQ(data->save_to_file(painterly_path, true), OK);

	const PackedStringArray canonical = _read_header_property_names(canonical_path);
	CHECK_FALSE(canonical.has("palette_id"));
	CHECK_FALSE(canonical.has("stroke_age"));
	// DC-only payload: no SH rest columns are invented.
	CHECK_FALSE(canonical.has("f_rest_0"));

	const PackedStringArray painterly = _read_header_property_names(painterly_path);
	CHECK(painterly.has("palette_id"));
	CHECK(painterly.has("brush_override_id"));
	CHECK(painterly.has("brush_axis_u"));
	CHECK(painterly.has("brush_axis_v"));
	CHECK(painterly.has("stroke_age"));

	Ref<::GaussianData> reloaded = _load_with_ply_loader(painterly_path);
	if (reloaded.is_null() || reloaded->get_count() != 2) {
		FAIL("painterly export did not reload as 2 splats");
		return;
	}
	for (int i = 0; i < 2; i++) {
		const Gaussian g = reloaded->get_gaussian(i);
		CHECK_EQ(gaussian_get_palette_id(g.painterly_meta), uint16_t(7 + i));
		CHECK_EQ(gaussian_get_brush_override_id(g.painterly_meta), uint16_t(40 + i));
		CHECK(g.brush_axes.is_equal_approx(Vector2(0.25f, 0.75f)));
		CHECK(Math::is_equal_approx(g.stroke_age, 2.5f));
	}

	_remove_with_cache(canonical_path);
	_remove_with_cache(painterly_path);
}
