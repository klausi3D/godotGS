/**************************************************************************/
/*  test_ply_sh_degree_stride.h                                           */
/**************************************************************************/

#pragma once

// #1546: PLYLoader read every f_rest_* block with the degree-3 channel stride
// (15), whatever the number of f_rest_* properties the file declares. A PLY
// stores f_rest channel-major: for degree d there are T = (d+1)^2 - 1 terms per
// channel, R in f_rest_[0..T), G in f_rest_[T..2T), B in f_rest_[2T..3T). With
// stride 15 a degree-1 or degree-2 file lost its G and B coefficients, the
// leftover R/G/B values landed in the wrong bands, the reported degree was too
// high, and the load still succeeded.
//
// Every value below is analytic and distinct per (splat, channel, term), so a
// wrong stride, a channel transposition or a misreported band count cannot
// reproduce the expected payload by accident. The cases cover degrees 0-4 in
// ASCII and binary PLY, the .gsplatcache form (a cache hit and a stale
// pre-fix cache), the GaussianSplatAsset import path, inconsistent f_rest
// blocks that must be refused, and the view-dependent colour on the GPU.

#include "gs_test_pump.h"
#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/ply_loader.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/quantization_config.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

#include <cmath>

namespace TestPLYSHDegreeStride {

static constexpr float SH_C0 = 0.28209479177387814f;
static constexpr int SPLATS = 3;

static int _terms_for_degree(int p_degree) {
	return (p_degree + 1) * (p_degree + 1) - 1;
}

// Analytic coefficient for splat s, channel c (0=R,1=G,2=B), term t (1-based).
// Integers are exact in fp32 and in the ASCII form.
static float _coeff(int p_splat, int p_channel, int p_term) {
	return float(1000 * (p_splat + 1) + 100 * (p_channel + 1) + p_term);
}

static float _f_dc(int p_splat, int p_channel) {
	return 0.125f * float(p_channel + 1) - 0.0625f * float(p_splat);
}

// A PLY in the canonical Inria property order. p_rest_names are the f_rest
// property names in header order; p_rest_value(splat, k) is the value of the
// k-th of them.
template <typename F>
static bool _write_ply(const String &p_path, bool p_binary, int p_count, const Vector<String> &p_rest_names,
		F p_rest_value, const Vector3 *p_positions = nullptr, float p_log_scale = -2.0f, float p_opacity_logit = 2.0f) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	String header = "ply\n";
	header += p_binary ? "format binary_little_endian 1.0\n" : "format ascii 1.0\n";
	header += vformat("element vertex %d\n", p_count);
	header += "property float x\nproperty float y\nproperty float z\n";
	header += "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n";
	for (const String &name : p_rest_names) {
		header += "property float " + name + "\n";
	}
	header += "property float opacity\n";
	header += "property float scale_0\nproperty float scale_1\nproperty float scale_2\n";
	header += "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n";
	header += "end_header\n";
	f->store_string(header);
	for (int i = 0; i < p_count; i++) {
		Vector<float> row;
		const Vector3 position = p_positions ? p_positions[i] : Vector3(float(i), 0.5f * float(i), -1.0f);
		row.push_back(position.x);
		row.push_back(position.y);
		row.push_back(position.z);
		for (int c = 0; c < 3; c++) {
			row.push_back(p_positions ? 0.0f : _f_dc(i, c));
		}
		for (int k = 0; k < p_rest_names.size(); k++) {
			row.push_back(p_rest_value(i, k));
		}
		row.push_back(p_opacity_logit);
		for (int c = 0; c < 3; c++) {
			row.push_back(p_log_scale);
		}
		row.push_back(1.0f);
		row.push_back(0.0f);
		row.push_back(0.0f);
		row.push_back(0.0f);
		if (p_binary) {
			for (float v : row) {
				f->store_float(v);
			}
		} else {
			String line;
			for (int k = 0; k < row.size(); k++) {
				if (k > 0) {
					line += " ";
				}
				line += String::num(row[k], 9);
			}
			f->store_string(line + "\n");
		}
	}
	return f->get_error() == OK;
}

static Vector<String> _rest_names(int p_count) {
	Vector<String> names;
	for (int i = 0; i < p_count; i++) {
		names.push_back(vformat("f_rest_%d", i));
	}
	return names;
}

// A well-formed degree-d PLY with the analytic channel-major block.
static bool _write_degree_ply(const String &p_path, int p_degree, bool p_binary) {
	const int terms = _terms_for_degree(p_degree);
	return _write_ply(p_path, p_binary, SPLATS, _rest_names(3 * terms), [terms](int p_splat, int p_k) {
		const int channel = p_k / terms;
		const int term = p_k % terms + 1;
		return _coeff(p_splat, channel, term);
	});
}

static String _fixture_path(const String &p_tag) {
	const uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	const String base = OS::get_singleton() ? OS::get_singleton()->get_temp_path() : String(".");
	return base.path_join("godotgs_ply_sh_stride_" + p_tag + "_" + itos(int64_t(ticks)) + ".ply");
}

static String _cache_path(const String &p_path) {
	return p_path.get_basename() + ".gsplatcache";
}

static void _remove_with_cache(const String &p_path) {
	DirAccess::remove_absolute(p_path);
	DirAccess::remove_absolute(_cache_path(p_path));
}

// Every DC and rest coefficient, the band counts and the reported degree.
static void _check_payload(const Ref<::GaussianData> &p_data, int p_degree, const String &p_label) {
	if (p_data.is_null()) {
		FAIL(p_label, ": no GaussianData");
		return;
	}
	if (p_data->get_count() != SPLATS) {
		FAIL(p_label, ": splat count ", p_data->get_count(), " != ", SPLATS);
		return;
	}
	const int terms = _terms_for_degree(p_degree);
	const uint32_t expected_first = p_degree >= 1 ? 3u : 0u;
	const uint32_t expected_high = p_degree >= 2 ? uint32_t(terms - 3) : 0u;
	CHECK_MESSAGE(p_data->get_sh_degree() == uint32_t(p_degree),
			vformat("%s: reported SH degree %d, file is degree %d", p_label, p_data->get_sh_degree(), p_degree));
	CHECK_MESSAGE(p_data->get_sh_first_order_count() == expected_first,
			vformat("%s: first-order count %d, expected %d", p_label, p_data->get_sh_first_order_count(), expected_first));
	CHECK_MESSAGE(p_data->get_sh_high_order_count() == expected_high,
			vformat("%s: high-order count %d, expected %d", p_label, p_data->get_sh_high_order_count(), expected_high));
	for (int i = 0; i < SPLATS; i++) {
		const PackedFloat32Array sh = p_data->get_spherical_harmonics(i);
		if (sh.size() != 3 * (terms + 1)) {
			FAIL(p_label, ": splat ", i, " carries ", sh.size(), " SH floats, expected ", 3 * (terms + 1));
			return;
		}
		for (int c = 0; c < 3; c++) {
			const float expected_dc = SH_C0 * _f_dc(i, c);
			CHECK_MESSAGE(Math::abs(sh[c] - expected_dc) <= 1e-6f,
					vformat("%s: splat %d DC channel %d got %f expected %f", p_label, i, c, sh[c], expected_dc));
		}
		for (int term = 1; term <= terms; term++) {
			for (int c = 0; c < 3; c++) {
				const float got = sh[term * 3 + c];
				const float expected = _coeff(i, c, term);
				CHECK_MESSAGE(got == expected,
						vformat("%s: splat %d term %d channel %d got %f expected %f", p_label, i, term, c, got, expected));
			}
		}
	}
}

static Ref<::GaussianData> _load(const String &p_path, Error &r_err, bool *r_cache_hit = nullptr) {
	PLYLoader loader;
	r_err = loader.load_file(p_path);
	if (r_cache_hit) {
		*r_cache_hit = bool(loader.get_load_statistics().get("cache_hit", false));
	}
	if (r_err != OK) {
		return Ref<::GaussianData>();
	}
	return loader.get_gaussian_data();
}

} // namespace TestPLYSHDegreeStride

TEST_CASE("[GaussianSplatting][PLY] f_rest stride follows the declared SH degree 0-4, ASCII and binary (#1546)") {
	using namespace TestPLYSHDegreeStride;
	for (int degree = 0; degree <= 4; degree++) {
		for (const bool binary : { false, true }) {
			const String label = vformat("degree %d %s", degree, binary ? "binary" : "ascii");
			const String path = _fixture_path(vformat("d%d_%s", degree, binary ? "bin" : "ascii"));
			if (!_write_degree_ply(path, degree, binary)) {
				FAIL(label, ": could not write the fixture");
				continue;
			}
			Error err = OK;
			Ref<::GaussianData> data = _load(path, err);
			CHECK_MESSAGE(err == OK, vformat("%s: load_file returned %d", label, err));
			_check_payload(data, degree, label);
			_remove_with_cache(path);
		}
	}
}

TEST_CASE("[GaussianSplatting][PLY] .gsplatcache round-trips every SH degree and a stale pre-#1546 cache is not served") {
	using namespace TestPLYSHDegreeStride;
	for (int degree = 0; degree <= 4; degree++) {
		const String label = vformat("degree %d cache", degree);
		const String path = _fixture_path(vformat("cache_d%d", degree));
		if (!_write_degree_ply(path, degree, true)) {
			FAIL(label, ": could not write the fixture");
			continue;
		}
		// First load parses and writes the cache; the second must be a cache hit
		// carrying the same payload.
		Error err = OK;
		bool hit = true;
		Ref<::GaussianData> parsed = _load(path, err, &hit);
		CHECK_MESSAGE(err == OK, vformat("%s: first load returned %d", label, err));
		CHECK_FALSE_MESSAGE(hit, vformat("%s: premise: the first load must parse, not hit a cache", label));
		_check_payload(parsed, degree, label + " (parsed)");
		if (!FileAccess::exists(_cache_path(path))) {
			FAIL(label, ": the loader wrote no .gsplatcache, so the cache form is untested");
			_remove_with_cache(path);
			continue;
		}
		hit = false;
		Ref<::GaussianData> cached = _load(path, err, &hit);
		CHECK_MESSAGE(err == OK, vformat("%s: second load returned %d", label, err));
		CHECK_MESSAGE(hit, vformat("%s: premise: the second load must be served from the cache", label));
		_check_payload(cached, degree, label + " (cache hit)");
		_remove_with_cache(path);
	}

	// A cache the pre-fix loader wrote (cache_version 5) for the same source,
	// size and mtime holds the misread payload. It must be refused and the PLY
	// re-parsed; serving it would keep the wrong colours after the fix.
	const String path = _fixture_path("stale_v5");
	REQUIRE(_write_degree_ply(path, 1, true));
	{
		LocalVector<Gaussian> wrong;
		wrong.resize(SPLATS);
		for (int i = 0; i < SPLATS; i++) {
			wrong[i] = Gaussian{};
			wrong[i].position = Vector3(float(i), 0.5f * float(i), -1.0f);
			wrong[i].scale = Vector3(0.1f, 0.1f, 0.1f);
			wrong[i].rotation = Quaternion();
			wrong[i].opacity = 0.5f;
			// What the stride-15 reader produced: R band-1 only, G and B empty.
			for (int t = 0; t < 3; t++) {
				wrong[i].sh_1[t] = Vector3(_coeff(i, 0, t + 1), 0.0f, 0.0f);
			}
		}
		Ref<::GaussianData> stale_data;
		stale_data.instantiate();
		stale_data->set_gaussians(wrong);
		Ref<GaussianSplatWorld> world;
		world.instantiate();
		world->set_gaussian_data(stale_data);
		world->set_bounds(stale_data->get_aabb());
		Dictionary metadata;
		metadata[StringName("cache_source_path")] = path;
		metadata[StringName("cache_source_size")] = int64_t(FileAccess::get_size(path));
		metadata[StringName("cache_source_mtime")] = int64_t(FileAccess::get_modified_time(path));
		metadata[StringName("cache_version")] = 5;
		metadata[StringName("gs_ply_semantics")] = Dictionary();
		world->set_metadata(metadata);
		ResourceFormatSaverGaussianSplatWorld saver;
		REQUIRE_EQ(saver.save(world, _cache_path(path)), OK);
	}
	Error err = OK;
	bool hit = true;
	Ref<::GaussianData> reloaded = _load(path, err, &hit);
	CHECK_MESSAGE(err == OK, vformat("stale cache: load returned %d", err));
	CHECK_FALSE_MESSAGE(hit, "a cache_version 5 .gsplatcache (written by the stride-15 loader) was served");
	_check_payload(reloaded, 1, "stale v5 cache");
	_remove_with_cache(path);
}

TEST_CASE("[GaussianSplatting][PLY] GaussianSplatAsset import keeps per-channel SH for degrees 1 and 2 (#1546)") {
	using namespace TestPLYSHDegreeStride;
	for (int degree = 1; degree <= 2; degree++) {
		const String label = vformat("asset degree %d", degree);
		const String path = _fixture_path(vformat("asset_d%d", degree));
		if (!_write_degree_ply(path, degree, true)) {
			FAIL(label, ": could not write the fixture");
			continue;
		}
		Ref<GaussianSplatAsset> asset;
		asset.instantiate();
		const Error err = asset->load_from_file(path);
		if (err != OK) {
			FAIL(label, ": load_from_file returned ", err);
			_remove_with_cache(path);
			continue;
		}
		const int terms = _terms_for_degree(degree);
		const uint32_t first_terms = asset->get_sh_first_order_terms();
		const uint32_t high_terms = asset->get_sh_high_order_terms();
		CHECK_MESSAGE(first_terms == 3u, vformat("%s: first-order terms %d, expected 3", label, first_terms));
		CHECK_MESSAGE(high_terms == uint32_t(terms - 3), vformat("%s: high-order terms %d, expected %d", label, high_terms, terms - 3));
		const PackedFloat32Array first = asset->get_sh_first_order_coefficients();
		const PackedFloat32Array high = asset->get_sh_high_order_coefficients();
		if (first_terms != 3u || high_terms != uint32_t(terms - 3) ||
				first.size() != SPLATS * 3 * 3 || high.size() != SPLATS * (terms - 3) * 3) {
			FAIL(label, ": SH lane sizes do not describe a degree-", degree, " payload (first ", first.size(), ", high ", high.size(), ")");
			_remove_with_cache(path);
			continue;
		}
		for (int i = 0; i < SPLATS; i++) {
			for (int term = 1; term <= terms; term++) {
				for (int c = 0; c < 3; c++) {
					const float got = term <= 3 ? first[(i * 3 + (term - 1)) * 3 + c]
												: high[(i * (terms - 3) + (term - 4)) * 3 + c];
					CHECK_MESSAGE(got == _coeff(i, c, term),
							vformat("%s: splat %d term %d channel %d got %f expected %f", label, i, term, c, got, _coeff(i, c, term)));
				}
			}
		}
		_remove_with_cache(path);
	}
}

TEST_CASE("[GaussianSplatting][PLY] an f_rest block that is no whole SH degree is refused, not truncated (#1546)") {
	using namespace TestPLYSHDegreeStride;
	struct Malformed {
		const char *label;
		Vector<String> names;
	};
	Vector<Malformed> cases;
	cases.push_back({ "10 f_rest (not 3 * ((d+1)^2 - 1))", _rest_names(10) });
	cases.push_back({ "3 f_rest (partial band 1)", _rest_names(3) });
	{
		Vector<String> gap = _rest_names(10);
		gap.remove_at(4); // f_rest_0..3, 5..9: nine names, but not contiguous
		cases.push_back({ "nine f_rest with a gap", gap });
	}
	cases.push_back({ "93 f_rest (degree 5, unsupported)", _rest_names(93) });
	{
		Vector<String> named = _rest_names(9);
		named.set(8, "f_rest_x");
		cases.push_back({ "a non-numeric f_rest suffix", named });
	}
	for (const Malformed &m : cases) {
		for (const bool binary : { false, true }) {
			const String path = _fixture_path("malformed");
			const int count = m.names.size();
			REQUIRE(_write_ply(path, binary, SPLATS, m.names, [](int p_splat, int p_k) { return float(p_splat * 100 + p_k); }));
			Error err = OK;
			Ref<::GaussianData> data = _load(path, err);
			CHECK_MESSAGE(err != OK, vformat("%s (%s, %d names): load_file succeeded on an f_rest block that is no whole SH degree",
											 m.label, binary ? "binary" : "ascii", count));
			_remove_with_cache(path);
		}
	}
}

namespace TestPLYSHDegreeStride {

// Independent real-SH reference (associated Legendre recurrence with the
// Condon-Shortley phase), indexed l*l + l + m like the Inria term order.
static double _real_sh(int l, int m, const Vector3 &dir) {
	const int order = m < 0 ? -m : m;
	const double z = dir.z;
	double pmm = 1.0;
	for (int k = 1; k <= order; k++) {
		pmm *= -(2 * k - 1) * std::sqrt(MAX(0.0, 1.0 - z * z));
	}
	double plm = pmm;
	if (l > order) {
		double previous = pmm;
		plm = (2 * order + 1) * z * pmm;
		for (int degree = order + 2; degree <= l; degree++) {
			const double next = ((2 * degree - 1) * z * plm - (degree + order - 1) * previous) / (degree - order);
			previous = plm;
			plm = next;
		}
	}
	double ratio = 1.0;
	for (int k = l - order + 1; k <= l + order; k++) {
		ratio /= k;
	}
	double result = std::sqrt((2 * l + 1) * ratio / (4.0 * Math::PI)) * plm;
	if (m != 0) {
		const double phi = std::atan2(double(dir.y), double(dir.x));
		result *= std::sqrt(2.0) * (m < 0 ? std::sin(order * phi) : std::cos(order * phi));
	}
	return result;
}

static double _basis(int p_term, const Vector3 &p_dir) {
	int l = 0;
	while ((l + 1) * (l + 1) <= p_term) {
		l++;
	}
	return _real_sh(l, p_term - l * l - l, p_dir);
}

// GPU fixture coefficients: each channel is driven by a different term, so a
// stride error moves colour into the wrong channel or drops it.
static float _gpu_coeff(int p_degree, int p_channel, int p_term) {
	if (p_term == 1 + p_channel) {
		return 0.15f; // band 1: R term 1, G term 2, B term 3
	}
	if (p_degree >= 2 && p_term == 4 + 2 * p_channel) {
		return 0.2f; // band 2: R term 4, G term 6, B term 8
	}
	return 0.0f;
}

static RID _create_readback_texture(RenderingDevice *p_rd, const Vector2i &p_size) {
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	format.width = p_size.x;
	format.height = p_size.y;
	format.depth = 1;
	format.array_layers = 1;
	format.mipmaps = 1;
	format.samples = RD::TEXTURE_SAMPLES_1;
	format.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	return p_rd->texture_create(format, RD::TextureView());
}

// Mean RGB over the 3x3 pixels around the alpha-coverage centroid.
static bool _sample_centroid_rgb(const Vector<uint8_t> &p_pixels, const Vector2i &p_size, Vector3 &r_rgb, String &r_why) {
	if (p_pixels.size() != p_size.x * p_size.y * 4) {
		r_why = vformat("readback returned %d bytes, expected %d", p_pixels.size(), p_size.x * p_size.y * 4);
		return false;
	}
	double sum_w = 0.0, sum_x = 0.0, sum_y = 0.0;
	for (int y = 0; y < p_size.y; y++) {
		for (int x = 0; x < p_size.x; x++) {
			const double a = p_pixels[(y * p_size.x + x) * 4 + 3] / 255.0;
			if (a > 0.5) {
				sum_w += a;
				sum_x += a * x;
				sum_y += a * y;
			}
		}
	}
	if (sum_w < 9.0) {
		r_why = vformat("the splat covers too few opaque pixels (alpha mass %f)", sum_w);
		return false;
	}
	const int cx = int(Math::round(sum_x / sum_w));
	const int cy = int(Math::round(sum_y / sum_w));
	Vector3 rgb;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int x = CLAMP(cx + dx, 0, p_size.x - 1);
			const int y = CLAMP(cy + dy, 0, p_size.y - 1);
			for (int c = 0; c < 3; c++) {
				rgb[c] += p_pixels[(y * p_size.x + x) * 4 + c] / 255.0f;
			}
		}
	}
	r_rgb = rgb / 9.0f;
	return true;
}

// Loads a one-splat PLY of p_degree at p_position (coefficients scaled by
// p_scale) through PLYLoader, renders it from the origin looking at it and
// returns the centroid colour.
static bool _render_ply_rgb(RenderingDevice *p_rd, int p_degree, const Vector3 &p_position, float p_scale,
		bool p_expect_quantized, Vector3 &r_rgb, String &r_why) {
	const String path = _fixture_path(vformat("gpu_d%d", p_degree));
	const int terms = _terms_for_degree(p_degree);
	const bool written = _write_ply(path, true, 1, _rest_names(3 * terms), [&](int, int p_k) {
		return p_scale * _gpu_coeff(p_degree, p_k / terms, p_k % terms + 1);
	}, &p_position, std::log(0.6f), std::log(0.995f / 0.005f));
	if (!written) {
		r_why = "could not write the fixture";
		return false;
	}
	Error err = OK;
	Ref<::GaussianData> data = _load(path, err);
	_remove_with_cache(path);
	if (data.is_null()) {
		r_why = vformat("PLYLoader refused the fixture (error %d)", err);
		return false;
	}
	// Checked, not a precondition: the colour comparison below must still run
	// when the degree is wrong, so a stride defect shows up as wrong colour too.
	CHECK_MESSAGE(data->get_sh_degree() == uint32_t(p_degree),
			vformat("loaded SH degree %d, file is degree %d", data->get_sh_degree(), p_degree));
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(p_rd);
	if (!renderer.is_valid()) {
		r_why = "renderer did not instantiate";
		return false;
	}
	renderer->initialize();
	renderer->set_painterly_enabled(false);
	renderer->set_max_splats(1);
	if (renderer->set_gaussian_data(data) != OK) {
		r_why = "set_gaussian_data failed";
		return false;
	}
	const Vector2i resolution(96, 96);
	Projection projection;
	projection.set_perspective(60.0f, 1.0f, 0.1f, 50.0f);
	// render_for_view takes the view (world-to-camera) matrix.
	const Transform3D view = Transform3D().looking_at(p_position, Vector3(0, 1, 0)).affine_inverse();
	const TestGaussianSplatting::GSPumpOutcome outcome = TestGaussianSplatting::gs_pump_until([&]() {
		return renderer->render_for_view(view, projection, RID(), resolution);
	}, 10000000, 1);
	if (!outcome.ready()) {
		r_why = "render_for_view never reported a rendered frame: " + outcome.describe();
		return false;
	}
	const bool quantized = bool(renderer->get_render_stats().get("raster_feature_quantized_storage", false));
	if (quantized != p_expect_quantized) {
		r_why = vformat("premise: raster_feature_quantized_storage is %s, expected %s",
				quantized ? "true" : "false", p_expect_quantized ? "true" : "false");
		return false;
	}
	RID target = _create_readback_texture(p_rd, resolution);
	if (!target.is_valid()) {
		r_why = "readback texture creation failed";
		return false;
	}
	const bool copied = renderer->copy_final_texture_to_target(target, resolution);
	Vector<uint8_t> pixels;
	if (copied) {
		pixels = p_rd->texture_get_data(target, 0);
	}
	p_rd->free(target);
	renderer.unref();
	if (!copied) {
		r_why = "copy_final_texture_to_target failed";
		return false;
	}
	return _sample_centroid_rgb(pixels, resolution, r_rgb, r_why);
}

static void _check_view_dependent_colour(RenderingDevice *p_rd, int p_degree, bool p_quantized) {
	const Vector3 directions[2] = {
		Vector3(0.3f, 0.4f, -0.8660254f).normalized(),
		Vector3(-0.5f, 0.2f, -0.8426150f).normalized(),
	};
	Vector3 measured_delta[2];
	for (int v = 0; v < 2; v++) {
		const Vector3 position = directions[v] * 3.0f;
		const String label = vformat("degree %d %s view %d", p_degree, p_quantized ? "quantized" : "unquantized", v);
		Vector3 baseline, rgb;
		String why;
		if (!_render_ply_rgb(p_rd, p_degree, position, 0.0f, p_quantized, baseline, why)) {
			FAIL(label, " zero-SH render: ", why);
			return;
		}
		for (int c = 0; c < 3; c++) {
			if (!(baseline[c] > 0.4f && baseline[c] < 0.6f)) {
				FAIL(label, ": zero-SH splat is not mid grey in channel ", c, " (", baseline[c], ")");
				return;
			}
		}
		if (!_render_ply_rgb(p_rd, p_degree, position, 1.0f, p_quantized, rgb, why)) {
			FAIL(label, " SH render: ", why);
			return;
		}
		const Vector3 dir = position.normalized();
		for (int c = 0; c < 3; c++) {
			double expected = 0.0;
			for (int term = 1; term <= _terms_for_degree(p_degree); term++) {
				expected += double(_gpu_coeff(p_degree, c, term)) * _basis(term, dir);
			}
			const float delta = rgb[c] - baseline[c];
			measured_delta[v][c] = delta;
			MESSAGE(vformat("%s channel %d: colour delta %+f, reference %+f", label, c, delta, expected));
			CHECK_MESSAGE(Math::abs(delta - float(expected)) < 0.02f,
					vformat("%s channel %d: colour delta %+f != reference %+f", label, c, delta, expected));
		}
	}
	// The premise of "view-dependent": the two views must differ visibly.
	CHECK_MESSAGE((measured_delta[0] - measured_delta[1]).length() > 0.05f,
			vformat("degree %d: the colour did not change between the two views", p_degree));
}

} // namespace TestPLYSHDegreeStride

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] PLY-loaded degree-1 and degree-2 SH render the reference view-dependent colour per channel (#1546)") {
	if (RenderingServer::get_singleton() == nullptr) {
		FAIL("RenderingServer unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return;
	}
	GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
	if (manager == nullptr) {
		FAIL("GaussianSplatManager unavailable - module registration always provides one");
		return;
	}
	RenderingDevice *rd = manager->get_primary_rendering_device();
	if (rd == nullptr) {
		FAIL("RenderingDevice unavailable in a [RequiresGPU] case");
		return;
	}
	const QuantizationConfig saved_quantization = g_quantization_config;
	for (const bool quantized : { false, true }) {
		g_quantization_config.per_chunk_quantization = quantized;
		TestPLYSHDegreeStride::_check_view_dependent_colour(rd, 1, quantized);
		TestPLYSHDegreeStride::_check_view_dependent_colour(rd, 2, quantized);
	}
	g_quantization_config = saved_quantization;
}
