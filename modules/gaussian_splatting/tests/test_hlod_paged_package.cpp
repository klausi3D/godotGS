extern "C" int test_hlod_paged_package_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gs_vector_alloc.h"
#include "../io/gaussian_splat_world_format.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gaussian_splat_world_paged.h"
#include "../renderer/hlod_node_pack.h"
#include "../renderer/hlod_node_streamer.h"
#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/time.h"

// ADR 14.4, package E1: the V4 paged package. CPU only. These tests drive the real writer, the real strict reader and
// the real node streamer jobs; the file bytes are corrupted by hand (and re-sealed where a hash would otherwise be the
// only thing that notices) so that every refusal is shown to come from the named rule.
namespace TestHlodPagedPackage {
// Neither REQUIRE nor CHECK aborts in this build (DOCTEST_CONFIG_NO_EXCEPTIONS): a failed requirement must leave the test
// before the next line dereferences what it was guarding.
#define GS_NEED(cond) 	do { 		if (!(cond)) { 			FAIL("requirement failed: ", #cond); 			return; 		} 	} while (0)

using namespace gs_world_paged;

static String temp_path(const String &p_prefix) {
	static int serial = 0;
	return OS::get_singleton()->get_temp_path().path_join(vformat("godotgs_v4_%s_%d_%d.gsplatworld", p_prefix, ++serial, int64_t(OS::get_singleton()->get_ticks_usec())));
}

// A baked world with several leaves (more than one 16,384 splat node). Dyadic positions and scales keep every cell
// centre reconstruction exact. `p_high_order` > 0 adds SH coefficients; `p_seed` changes the content, not the layout.
static Ref<GaussianSplatWorld> make_world(uint32_t p_count, uint32_t p_high_order, float p_seed = 0.0f) {
	Ref<GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(p_count);
	for (uint32_t i = 0; i < p_count; ++i) {
		Gaussian &g = gaussians[i];
		g.position = Vector3(float(i % 40) * 0.5f, float((i / 40) % 40) * 0.5f, float(i / 1600) * 0.5f);
		g.scale = Vector3(0.03125f, 0.0625f + float(i % 3) * 0.03125f, 0.125f);
		g.opacity = 0.5f + p_seed * 0.25f;
		g.sh_dc = Color(0.25f + float(i % 4) * 0.125f, 0.5f, 0.75f, 1.0f);
		g.sh_1[0] = Vector3(0.125f, float(i % 5) * 0.0625f, 0.0f);
	}
	LocalVector<Vector3> sh;
	sh.resize(size_t(p_count) * p_high_order);
	for (uint32_t i = 0; i < sh.size(); ++i) {
		sh[i] = Vector3(float(i % 7) * 0.0078125f, 0.0625f, -0.015625f);
	}
	data->set_gaussian_payload(gaussians, sh, p_high_order > 0 ? 3u : 0u, p_high_order, false, false);
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(data);
	Dictionary metadata;
	metadata["source"] = "paged_test";
	metadata["version"] = 7;
	world->set_metadata(metadata);
	return world;
}

struct Fixture {
	Ref<GaussianSplatWorld> world;
	String path;
	Ref<GaussianSplatHlodRuntimeData> resident; // The resident snapshot the paged one must match.
	bool ok = false;
};

static bool build_fixture(Fixture &r_fx, uint32_t p_count = 20000, uint32_t p_high_order = 0, float p_seed = 0.0f, const char *p_tag = "base") {
	r_fx.world = make_world(p_count, p_high_order, p_seed);
	if (r_fx.world->bake_hlod() != OK) {
		FAIL("synthetic world bake failed");
		return false;
	}
	if (r_fx.world->get_hlod_tree().nodes.size() < 2) {
		FAIL("the fixture must have more than one node");
		return false;
	}
	String reason;
	if (r_fx.world->capture_hlod_runtime_snapshot(r_fx.resident, reason) != OK) {
		FAIL("resident snapshot failed: ", reason.utf8().get_data());
		return false;
	}
	r_fx.path = temp_path(p_tag);
	if (r_fx.world->save_hlod_paged(r_fx.path) != OK) {
		FAIL("save_hlod_paged failed");
		return false;
	}
	r_fx.ok = true;
	return true;
}

static Error open_source(const String &p_path, Ref<GaussianSplatHlodPagedSource> &r_source, String &r_reason, GaussianSplatHlodTree *r_tree = nullptr) {
	r_source.instantiate();
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	const Error err = r_source->open(p_path, r_tree ? *r_tree : tree, metadata, r_reason);
	return err;
}

static Vector<uint8_t> read_all(const String &p_path) {
	return FileAccess::get_file_as_bytes(p_path);
}

static void write_all(const String &p_path, const Vector<uint8_t> &p_bytes) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	GS_NEED(f.is_valid());
	f->store_buffer(p_bytes.ptr(), p_bytes.size());
}

static uint32_t rd32(const Vector<uint8_t> &p_f, uint64_t p_at) { uint32_t v; memcpy(&v, p_f.ptr() + p_at, 4); return v; }
static uint64_t rd64(const Vector<uint8_t> &p_f, uint64_t p_at) { uint64_t v; memcpy(&v, p_f.ptr() + p_at, 8); return v; }
static void wr32(Vector<uint8_t> &p_f, uint64_t p_at, uint32_t p_v) { memcpy(p_f.ptrw() + p_at, &p_v, 4); }
static void wr64(Vector<uint8_t> &p_f, uint64_t p_at, uint64_t p_v) { memcpy(p_f.ptrw() + p_at, &p_v, 8); }
static void wrf64(Vector<uint8_t> &p_f, uint64_t p_at, double p_v) { memcpy(p_f.ptrw() + p_at, &p_v, 8); }

static void sha(const uint8_t *p_data, size_t p_size, uint8_t *r_out) {
	CryptoCore::SHA256Context ctx;
	ctx.start();
	if (p_size) { ctx.update(p_data, p_size); }
	ctx.finish(r_out);
}

// Recomputes the index page digests, the whole-index digest and the header digest, so a semantic corruption made on
// purpose is not caught by a hash first. Byte offsets are the documented V4 layout.
static void reseal(Vector<uint8_t> &r_f) {
	const uint32_t node_count = rd32(r_f, 40);
	const uint64_t pages = rd64(r_f, 120);
	Vector<uint8_t> digests;
	digests.resize(int64_t(pages * 32));
	for (uint64_t p = 0; p < pages; ++p) {
		const uint64_t base = rd64(r_f, 112) + p * kIndexPageBytes;
		const uint32_t in_page = rd32(r_f, base + 12);
		(void)node_count;
		sha(r_f.ptr() + base + kIndexPageHeaderBytes, size_t(in_page) * kIndexRecordBytes, r_f.ptrw() + base + 32);
		memcpy(digests.ptrw() + p * 32, r_f.ptr() + base + 32, 32);
	}
	sha(digests.ptr(), digests.size(), r_f.ptrw() + 176);
	sha(r_f.ptr(), kHeaderBytes - 32, r_f.ptrw() + kHeaderBytes - 32);
}

static uint64_t index_record_at(const Vector<uint8_t> &p_f, uint32_t p_node) {
	return rd64(p_f, 112) + (p_node / kRecordsPerIndexPage) * kIndexPageBytes + kIndexPageHeaderBytes + (p_node % kRecordsPerIndexPage) * kIndexRecordBytes;
}

// Re-seals one node's payload record digest in the index (after the test changed its bytes), then the rest.
static void reseal_record(Vector<uint8_t> &r_f, uint32_t p_node) {
	const uint64_t rec = index_record_at(r_f, p_node);
	const uint64_t offset = rd64(r_f, rec + 128);
	const uint64_t stored = rd64(r_f, rec + 136);
	sha(r_f.ptr() + offset, size_t(stored), r_f.ptrw() + rec + 192);
	reseal(r_f);
}

static bool same_gaussian(const Gaussian &p_a, const Gaussian &p_b) {
	if (p_a.position != p_b.position || p_a.scale != p_b.scale || p_a.opacity != p_b.opacity || p_a.rotation != p_b.rotation ||
			p_a.sh_dc != p_b.sh_dc || p_a.render_meta != p_b.render_meta || p_a.area != p_b.area) {
		return false;
	}
	for (int k = 0; k < 3; ++k) {
		if (p_a.sh_1[k] != p_b.sh_1[k]) { return false; }
	}
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO] a V4 package round-trips: identity, metrics and every node payload match the resident snapshot") {
	for (uint32_t high_order : { 0u, 12u }) {
		CAPTURE(high_order);
		Fixture fx;
		if (!build_fixture(fx, 20000, high_order, 0.0f, "rt")) { return; }
		{
			Ref<FileAccess> f = FileAccess::open(fx.path, FileAccess::READ);
			GS_NEED(f.is_valid());
			CHECK_EQ(f->get_32(), 0x57505347u);
			CHECK_EQ(f->get_32(), 4u);
		}
		Error load_error = ERR_BUG;
		ResourceFormatLoaderGaussianSplatWorld loader;
		Ref<GaussianSplatWorld> loaded = loader.load(fx.path, "", &load_error);
		GS_NEED(load_error == OK);
		GS_NEED(loaded.is_valid());
		CHECK(loaded->has_hlod_paged_source());
		CHECK(loaded->get_gaussian_data().is_null());
		CHECK_FALSE(loaded->has_chunk_payload_source());
		CHECK(loaded->has_renderable_payload());
		CHECK(loaded->get_payload_mode() == "hlod_paged_v4");
		CHECK(loaded->get_splat_count() == 20000u);
		CHECK(String(loaded->get_metadata()["source"]) == "paged_test");
		CHECK(int(loaded->get_metadata()["version"]) == 7);
		CHECK(loaded->get_hlod_tree().nodes.size() == fx.world->get_hlod_tree().nodes.size());
		CHECK(loaded->get_bounds().encloses(fx.world->get_bounds()));

		Ref<GaussianSplatHlodRuntimeData> paged;
		String reason;
		GS_NEED(loaded->capture_hlod_runtime_snapshot(paged, reason) == OK);
		GS_NEED(paged.is_valid());
		CHECK(paged->is_paged());
		CHECK(paged->is_current());
		CHECK(paged->parents.is_empty());
		CHECK(paged->sh_first_order_count() == fx.resident->sh_first_order_count());
		GS_NEED(paged->nodes.size() == fx.resident->nodes.size());
		// The index carries what the resident capture derives from a scan of the payload.
		for (int i = 0; i < paged->nodes.size(); ++i) {
			CAPTURE(i);
			const auto &a = paged->nodes[i];
			const auto &b = fx.resident->nodes[i];
			CHECK(a.footprint == b.footprint);
			CHECK(a.dc_encoding == b.dc_encoding);
			CHECK(a.homogeneous_dc == b.homogeneous_dc);
			CHECK(a.topology.payload_first == b.topology.payload_first);
			CHECK(a.topology.payload_count == b.topology.payload_count);
			CHECK(a.topology.radius == b.topology.radius);
			CHECK(a.bounds_center == b.bounds_center);
			CHECK(a.payload_center == b.payload_center);
		}
		// The CPU copy of the payload is gone: the paged snapshot charges only the node tables and a bounded
		// on-demand working set (independent of the splat count), and holds no payload vector.
		CHECK(paged->owned_cpu_bytes > 0);
		CHECK(paged->owned_cpu_bytes <= GaussianSplatHlodRuntimeData::CPU_BYTE_LIMIT);

		// Every node: on-demand read equals the resident payload, and packs to the same GPU bytes (raw and quantized).
		Ref<GaussianSplatHlodPagedSource> source = loaded->get_hlod_paged_source();
		for (int mode = 0; mode < 2; ++mode) {
			gs_hlod_pack::Params params;
			params.relative = true;
			params.quantized = mode == 1;
			params.position_bits = 16;
			params.scale_bits = 12;
			for (uint32_t n = 0; n < uint32_t(paged->nodes.size()); ++n) {
				CAPTURE(n);
				Vector<Gaussian> g;
				Vector<Vector3> sh;
				String why;
				GS_NEED(source->read_node_payload(n, g, sh, why) == OK);
				const auto &node = fx.resident->nodes[n].topology;
				GS_NEED(uint32_t(g.size()) == node.payload_count);
				CHECK(sh.size() == int64_t(node.payload_count) * high_order);
				if (mode == 0) {
					for (uint32_t i = 0; i < node.payload_count; ++i) {
						if (!same_gaussian(g[i], fx.resident->gaussian(node.payload_first + i))) {
							FAIL("node ", n, " splat ", i, " differs from the resident payload");
							return;
						}
					}
					for (uint32_t i = 0; i < sh.size(); ++i) {
						const Vector3 *expected = fx.resident->sh(node.payload_first + i / high_order);
						if (sh[i] != expected[i % high_order]) {
							FAIL("node ", n, " SH differs from the resident payload");
							return;
						}
					}
				}
				Vector<PackedGaussian> raw_a, raw_b;
				Vector<PackedGaussianQuantized> q_a, q_b;
				ChunkMetaGPU meta_a, meta_b;
				ChunkQuantizationGPU quant_a, quant_b;
				SHCompressionMetrics metrics_a, metrics_b;
				GS_NEED(gs_resize_or_fail(raw_a, node.payload_count, "t"));
				GS_NEED(gs_resize_or_fail(raw_b, node.payload_count, "t"));
				GS_NEED(gs_resize_or_fail(q_a, node.payload_count, "t"));
				GS_NEED(gs_resize_or_fail(q_b, node.payload_count, "t"));
				gs_hlod_pack::Output out_a;
				out_a.raw = raw_a.ptrw(); out_a.quantized = q_a.ptrw(); out_a.meta = &meta_a; out_a.quant = &quant_a; out_a.metrics = &metrics_a;
				gs_hlod_pack::Output out_b = out_a;
				out_b.raw = raw_b.ptrw(); out_b.quantized = q_b.ptrw(); out_b.meta = &meta_b; out_b.quant = &quant_b; out_b.metrics = &metrics_b;
				gs_hlod_pack::PayloadView view;
				view.gaussians = g.ptr();
				view.sh = sh.is_empty() ? nullptr : sh.ptr();
				GS_NEED(gs_hlod_pack::pack_node(*fx.resident.ptr(), n, params, n * 16384u, out_a));
				GS_NEED(gs_hlod_pack::pack_node(*paged.ptr(), n, params, n * 16384u, out_b, &view));
				CHECK(memcmp(&meta_a, &meta_b, sizeof(ChunkMetaGPU)) == 0);
				if (mode == 0) {
					CHECK(memcmp(raw_a.ptr(), raw_b.ptr(), size_t(raw_a.size()) * sizeof(PackedGaussian)) == 0);
				} else {
					CHECK(memcmp(&quant_a, &quant_b, sizeof(ChunkQuantizationGPU)) == 0);
					CHECK(memcmp(q_a.ptr(), q_b.ptr(), size_t(q_a.size()) * sizeof(PackedGaussianQuantized)) == 0);
				}
			}
		}
		// A paged snapshot refuses to be packed without a payload view (it has none).
		{
			gs_hlod_pack::Params params;
			ChunkMetaGPU meta;
			SHCompressionMetrics metrics;
			Vector<PackedGaussian> raw;
			GS_NEED(gs_resize_or_fail(raw, paged->nodes[0].topology.payload_count, "t"));
			gs_hlod_pack::Output out;
			out.raw = raw.ptrw(); out.meta = &meta; out.metrics = &metrics;
			CHECK_FALSE(gs_hlod_pack::pack_node(*paged.ptr(), 0, params, 0, out));
		}
		// Bounded reads: no read ever exceeds one record, whatever the world size.
		const GaussianSplatHlodPagedSource::Stats stats = source->get_stats();
		CHECK(stats.read_failures == 0);
		CHECK(stats.largest_read_bytes <= source->get_max_record_bytes());
		CHECK(source->get_max_record_bytes() <= 64u + uint64_t(gs_hlod::kMaxNodeSplats) * (sizeof(Gaussian) + high_order * sizeof(Vector3)));
		// Saving a paged world with the ordinary saver fails closed instead of writing an empty world.
		ResourceFormatSaverGaussianSplatWorld saver;
		ERR_PRINT_OFF;
		CHECK(saver.save(loaded, temp_path("paged_resave")) != OK);
		ERR_PRINT_ON;
		// ... and so does materializing one: V4 has no resident form.
		ERR_PRINT_OFF;
		Error resident_error = OK;
		Ref<GaussianSplatWorld> resident_load = loader.load_resident(fx.path, &resident_error);
		ERR_PRINT_ON;
		CHECK(resident_load.is_null());
		CHECK(resident_error == ERR_UNAVAILABLE);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO] v2/v3 files keep loading and V4 is a separate identity") {
	Fixture fx;
	if (!build_fixture(fx, 20000, 0, 0.0f, "legacy")) { return; }
	ResourceFormatSaverGaussianSplatWorld saver;
	const String v3 = temp_path("v3");
	GS_NEED(saver.save(fx.world, v3) == OK);
	{
		Ref<FileAccess> f = FileAccess::open(v3, FileAccess::READ);
		GS_NEED(f.is_valid());
		f->seek(4);
		CHECK_EQ(f->get_32(), 3u); // The default saver still writes v3.
	}
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> loaded = loader.load(v3, "", &err);
	GS_NEED(err == OK);
	GS_NEED(loaded.is_valid());
	CHECK_FALSE(loaded->has_hlod_paged_source());
	CHECK(loaded->has_hlod_tree());
	CHECK(loaded->get_hlod_tree().nodes.size() == fx.world->get_hlod_tree().nodes.size());
	// A v3 file is not a V4 package: the paged reader refuses it by identity.
	Ref<GaussianSplatHlodPagedSource> source;
	String reason;
	CHECK(open_source(v3, source, reason) == ERR_FILE_UNRECOGNIZED);
	CHECK(reason == "v4_wrong_version");
	// The strict v1-v3 header selector still refuses version 4: no reinterpretation of V4 as a legacy layout.
	gs_world_format::HeaderLayout layout;
	CHECK(gs_world_format::select_header(4u, gs_world_format::kFlagHasHlod, 1u << 20, layout) == ERR_FILE_CORRUPT);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO] the reader fails closed with a distinct reason for each corruption") {
	Fixture fx;
	if (!build_fixture(fx, 20000, 12, 0.0f, "corrupt")) { return; }
	const Vector<uint8_t> good = read_all(fx.path);
	GS_NEED(good.size() > int64_t(kHeaderBytes));
	const uint32_t node_count = rd32(good, 40);
	GS_NEED(node_count >= 2);

	struct Case {
		const char *name;
		const char *reason;
		Error error;
	};
	auto expect_open = [&](const char *p_name, const Vector<uint8_t> &p_bytes, const char *p_reason, Error p_error) {
		CAPTURE(String(p_name));
		const String path = temp_path("case");
		write_all(path, p_bytes);
		Ref<GaussianSplatHlodPagedSource> source;
		String reason;
		const Error err = open_source(path, source, reason);
		CHECK_MESSAGE(reason == String(p_reason), "got reason ", reason.utf8().get_data());
		CHECK(err == p_error);
		CHECK_FALSE(source->is_open());
		// The loader must not publish a world either.
		ERR_PRINT_OFF;
		Error load_error = OK;
		ResourceFormatLoaderGaussianSplatWorld loader;
		Ref<GaussianSplatWorld> world = loader.load(path, "", &load_error);
		ERR_PRINT_ON;
		CHECK(world.is_null());
		CHECK(load_error == p_error);
	};

	{ // Truncation, extension, tiny file.
		Vector<uint8_t> f = good;
		f.resize(f.size() - 1);
		expect_open("truncated by one byte", f, "v4_truncated_file", ERR_FILE_CORRUPT);
		f = good;
		f.resize(f.size() + 1);
		expect_open("one trailing byte", f, "v4_trailing_bytes", ERR_FILE_CORRUPT);
		f = good;
		f.resize(100);
		// 100 bytes cannot hold the version word dispatch's header page either: the loader sees a v1-sized file.
		const String path = temp_path("tiny");
		write_all(path, f);
		Ref<GaussianSplatHlodPagedSource> source;
		String reason;
		CHECK(open_source(path, source, reason) == ERR_FILE_CORRUPT);
		CHECK(reason == "v4_truncated_header");
	}
	{ // Identity.
		Vector<uint8_t> f = good;
		wr32(f, 0, 0x12345678u);
		expect_open("bad magic", f, "v4_bad_magic", ERR_FILE_UNRECOGNIZED);
		f = good;
		wr32(f, 4, 5u);
		const String path = temp_path("v5");
		write_all(path, f);
		Ref<GaussianSplatHlodPagedSource> source;
		String reason;
		CHECK(open_source(path, source, reason) == ERR_FILE_UNRECOGNIZED);
		CHECK(reason == "v4_wrong_version");
	}
	{ // Integrity: a flipped header / index / payload-independent byte, without re-sealing.
		Vector<uint8_t> f = good;
		f.ptrw()[36] ^= 0x01; // interior splat count, header digest not updated
		expect_open("header byte flipped", f, "v4_header_digest_mismatch", ERR_FILE_CORRUPT);
		f = good;
		f.ptrw()[index_record_at(good, 1) + 80] ^= 0x01; // a node's parent field
		expect_open("index record byte flipped", f, "v4_index_page_digest_mismatch", ERR_FILE_CORRUPT);
		f = good;
		f.ptrw()[rd64(good, 112) + 33] ^= 0x01; // the page's own stored digest
		expect_open("index page digest flipped", f, "v4_index_page_digest_mismatch", ERR_FILE_CORRUPT);
	}
	{ // Semantic checks that a matching hash would otherwise let through.
		Vector<uint8_t> f = good;
		wr32(f, index_record_at(f, 1) + 80, 77u); // parent of node 1 no longer names node 0
		reseal(f);
		const String path = temp_path("tree");
		write_all(path, f);
		Ref<GaussianSplatHlodPagedSource> source;
		String reason;
		CHECK(open_source(path, source, reason) == ERR_FILE_CORRUPT);
		CHECK_MESSAGE(reason.begins_with("v4_invalid_tree"), "got ", reason.utf8().get_data());

		f = good;
		wr64(f, index_record_at(f, 0) + 128, rd64(f, index_record_at(f, 0) + 128) + kPageBytes); // record misplaced
		reseal(f);
		expect_open("payload record offset moved", f, "v4_payload_record_misplaced", ERR_FILE_CORRUPT);

		f = good;
		wr64(f, index_record_at(f, 0) + 136, rd64(f, index_record_at(f, 0) + 136) + 1u); // size no longer count-derived
		reseal(f);
		expect_open("payload record size inflated", f, "v4_payload_record_size_inconsistent", ERR_FILE_CORRUPT);

		f = good;
		wr64(f, index_record_at(f, 0) + 120, 1u); // reserved word must be zero
		reseal(f);
		expect_open("reserved node field set", f, "v4_nonzero_reserved_node_record", ERR_FILE_CORRUPT);

		f = good;
		wrf64(f, index_record_at(f, 0) + 104, std::numeric_limits<double>::quiet_NaN());
		reseal(f);
		expect_open("non-finite footprint", f, "v4_node_metrics_invalid", ERR_FILE_CORRUPT);
	}
	{ // Counts and extents are range checked before anything is allocated or read.
		Vector<uint8_t> f = good;
		wr32(f, 40, 0xFFFFFFFFu);
		sha(f.ptr(), kHeaderBytes - 32, f.ptrw() + kHeaderBytes - 32);
		expect_open("node count overflow", f, "v4_header_field_out_of_range", ERR_FILE_CORRUPT);

		f = good;
		wr64(f, 136, UINT64_MAX - kPageBytes + 1u); // payload byte count that wraps when added to its offset
		sha(f.ptr(), kHeaderBytes - 32, f.ptrw() + kHeaderBytes - 32);
		expect_open("payload extent overflow", f, "v4_payload_region_overflow", ERR_FILE_CORRUPT);

		f = good;
		wr64(f, 160, rd64(f, 160) + kPageBytes); // declared length no longer the sum of the regions
		sha(f.ptr(), kHeaderBytes - 32, f.ptrw() + kHeaderBytes - 32);
		expect_open("declared length wrong", f, "v4_declared_length_inconsistent", ERR_FILE_CORRUPT);

		f = good;
		wr32(f, 28, 99u); // Gaussian struct layout version
		sha(f.ptr(), kHeaderBytes - 32, f.ptrw() + kHeaderBytes - 32);
		expect_open("layout mismatch", f, "v4_gaussian_layout_mismatch", ERR_FILE_UNRECOGNIZED);

		f = good;
		wr32(f, 52, 22u); // SH high-order count above the format maximum
		sha(f.ptr(), kHeaderBytes - 32, f.ptrw() + kHeaderBytes - 32);
		expect_open("high order out of range", f, "v4_header_field_out_of_range", ERR_FILE_CORRUPT);
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO] a damaged payload fails only its own node, at read time") {
	Fixture fx;
	if (!build_fixture(fx, 20000, 12, 0.0f, "payload")) { return; }
	const Vector<uint8_t> good = read_all(fx.path);
	const uint32_t node_count = rd32(good, 40);
	const uint32_t victim = node_count - 1;
	const uint64_t victim_offset = rd64(good, index_record_at(good, victim) + 128);

	auto read_victim_and_neighbour = [&](const Vector<uint8_t> &p_bytes, const char *p_reason) {
		const String path = temp_path("pl");
		write_all(path, p_bytes);
		Ref<GaussianSplatHlodPagedSource> source;
		String reason;
		// The index and topology are fine: the package opens (the payload is not read at open).
		GS_NEED(open_source(path, source, reason) == OK);
		Vector<Gaussian> g;
		Vector<Vector3> sh;
		String why;
		CHECK(source->read_node_payload(victim, g, sh, why) != OK);
		CHECK_MESSAGE(why.begins_with(p_reason), "got ", why.utf8().get_data());
		CHECK(g.is_empty());
		CHECK(sh.is_empty());
		CHECK(source->get_stats().read_failures >= 1);
		// Another node is unaffected.
		CHECK(source->read_node_payload(0, g, sh, why) == OK);
	};
	{
		Vector<uint8_t> f = good;
		f.ptrw()[victim_offset + kPayloadRecordHeaderBytes + 100] ^= 0x10; // body byte, digest untouched
		read_victim_and_neighbour(f, "v4_payload_digest_mismatch");
	}
	{
		Vector<uint8_t> f = good;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		memcpy(f.ptrw() + victim_offset + kPayloadRecordHeaderBytes, &nan, 4); // position.x = NaN
		reseal_record(f, victim);
		read_victim_and_neighbour(f, "v4_payload_invalid");
	}
	{
		Vector<uint8_t> f = good;
		wrf64(f, index_record_at(f, victim) + 104, 12345.0); // the index promises a different footprint
		reseal(f);
		read_victim_and_neighbour(f, "v4_payload_metrics_mismatch");
	}
	{
		Vector<uint8_t> f = good;
		wr32(f, victim_offset + 4, victim - 1); // record claims to be another node
		reseal_record(f, victim);
		read_victim_and_neighbour(f, "v4_payload_record_header_mismatch");
	}
	{ // The file is replaced under an open package: same length, different content, different generation.
		Fixture other;
		if (!build_fixture(other, 20000, 12, 0.5f, "payload_other")) { return; }
		const Vector<uint8_t> other_bytes = read_all(other.path);
		GS_NEED(other_bytes.size() == good.size());
		const String path = temp_path("swap");
		write_all(path, good);
		Ref<GaussianSplatHlodPagedSource> source;
		String reason;
		GS_NEED(open_source(path, source, reason) == OK);
		write_all(path, other_bytes);
		Vector<Gaussian> g;
		Vector<Vector3> sh;
		String why;
		CHECK(source->read_node_payload(0, g, sh, why) != OK);
		CHECK_MESSAGE(why == "v4_content_generation_mismatch", "got ", why.utf8().get_data());
		// ... and a different length is caught before any record is trusted.
		Vector<uint8_t> shorter = good;
		shorter.resize(shorter.size() - int64_t(kPageBytes));
		write_all(path, shorter);
		CHECK(source->read_node_payload(0, g, sh, why) != OK);
		CHECK_MESSAGE(why == "v4_file_changed_length", "got ", why.utf8().get_data());
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][Residency] the node streamer reads exactly the requested nodes from the file, bounded, and shuts down cleanly mid-read") {
	Fixture fx;
	if (!build_fixture(fx, 20000, 0, 0.0f, "stream")) { return; }
	Error load_error = ERR_BUG;
	ResourceFormatLoaderGaussianSplatWorld loader;
	Ref<GaussianSplatWorld> loaded = loader.load(fx.path, "", &load_error);
	GS_NEED(load_error == OK);
	Ref<GaussianSplatHlodRuntimeData> paged;
	String reason;
	GS_NEED(loaded->capture_hlod_runtime_snapshot(paged, reason) == OK);
	const uint32_t node_total = paged->nodes.size();

	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (uint32_t n = 0; n < node_total; ++n) {
		pages += (paged->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages;
	config.max_jobs_in_flight = 4;
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	GS_NEED(streamer->init(paged, config, 1, 1));
	streamer->begin_frame();

	// Demand: the root and its children. Without a device the uploads cannot complete (the nodes end up FAILED),
	// which is irrelevant here: what is measured is that the worker jobs read the file, once per requested node.
	LocalVector<gs_hlod_runtime::DesiredRecord> desired;
	{
		gs_hlod_runtime::DesiredRecord root;
		root.node = 0;
		desired.push_back(root);
	}
	const uint64_t root_bytes = paged->paged_source->get_nodes()[0].stored_bytes;
	for (int frame = 0; frame < 400 && streamer->get_telemetry().payload_reads_total < 1; ++frame) {
		streamer->update(nullptr, HlodNodeStreamer::Buffers(), desired.ptr(), desired.size(), nullptr, 0);
		OS::get_singleton()->delay_usec(2000);
	}
	const HlodNodeStreamer::Telemetry t = streamer->get_telemetry();
	CHECK(t.payload_reads_total >= 1);
	CHECK(t.payload_bytes_read_total == t.payload_reads_total * root_bytes);
	CHECK(t.payload_read_failures_total == 0);
	CHECK(t.payload_largest_read_bytes == root_bytes);
	// The working set (record + packed copy) is bounded by the configured read cap, not by the tree.
	CHECK(t.payload_in_flight_bytes_peak >= root_bytes);
	CHECK(t.payload_in_flight_bytes_peak <= config.max_inflight_read_bytes);

	// Shutdown with reads possibly in flight waits for them and leaves no job behind. A failed retry of the root
	// (no device here) may have read it again, but nothing but the root was ever demanded, so nothing else was read.
	streamer->shutdown();
	CHECK_FALSE(streamer->is_active());
	const GaussianSplatHlodPagedSource::Stats final_stats = paged->paged_source->get_stats();
	CHECK(final_stats.reads >= t.payload_reads_total);
	CHECK(final_stats.bytes_read == final_stats.reads * root_bytes);
	CHECK(final_stats.read_failures == 0);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][Residency] a failed read marks the node failed instead of publishing a partial payload") {
	Fixture fx;
	if (!build_fixture(fx, 20000, 0, 0.0f, "failread")) { return; }
	Vector<uint8_t> bytes = read_all(fx.path);
	const uint64_t root_offset = rd64(bytes, index_record_at(bytes, 0) + 128);
	bytes.ptrw()[root_offset + kPayloadRecordHeaderBytes + 40] ^= 0x01; // damage the root's payload
	const String path = temp_path("failread_bad");
	write_all(path, bytes);
	Error load_error = ERR_BUG;
	ResourceFormatLoaderGaussianSplatWorld loader;
	Ref<GaussianSplatWorld> loaded = loader.load(path, "", &load_error);
	GS_NEED(load_error == OK); // the index is intact; only a read can find the damage
	Ref<GaussianSplatHlodRuntimeData> paged;
	String reason;
	GS_NEED(loaded->capture_hlod_runtime_snapshot(paged, reason) == OK);
	HlodNodeStreamer::Config config;
	uint32_t pages = 0;
	for (int n = 0; n < paged->nodes.size(); ++n) {
		pages += (paged->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages;
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	GS_NEED(streamer->init(paged, config, 1, 1));
	streamer->begin_frame();
	gs_hlod_runtime::DesiredRecord root;
	root.node = 0;
	// The failure is settled into the residency model by a later update (mailbox drain -> fail_load).
	for (int frame = 0; frame < 400 && streamer->get_telemetry().failed_total == 0; ++frame) {
		streamer->update(nullptr, HlodNodeStreamer::Buffers(), &root, 1, nullptr, 0);
		OS::get_singleton()->delay_usec(2000);
	}
	const HlodNodeStreamer::Telemetry t = streamer->get_telemetry();
	CHECK(t.payload_read_failures_total >= 1);
	CHECK(t.payload_reads_total == 0);
	CHECK(t.uploaded_bytes_total == 0);
	CHECK(t.resident == 0);
	CHECK(t.failed_total >= 1);
	streamer->shutdown();
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][Residency] the in-flight read cap serializes reads: the CPU payload working set is the cap, not the demand") {
	Fixture fx;
	if (!build_fixture(fx, 20000, 12, 0.0f, "cap")) { return; }
	Error load_error = ERR_BUG;
	ResourceFormatLoaderGaussianSplatWorld loader;
	Ref<GaussianSplatWorld> loaded = loader.load(fx.path, "", &load_error);
	GS_NEED(load_error == OK);
	Ref<GaussianSplatHlodRuntimeData> paged;
	String reason;
	GS_NEED(loaded->capture_hlod_runtime_snapshot(paged, reason) == OK);
	// Demand the root and every child of the root: several records are wanted at once.
	LocalVector<gs_hlod_runtime::DesiredRecord> desired;
	LocalVector<uint32_t> wanted;
	wanted.push_back(0);
	for (uint32_t n = 1; n < uint32_t(paged->nodes.size()); ++n) {
		if (paged->nodes[n].topology.parent == 0) { wanted.push_back(n); }
	}
	GS_NEED(wanted.size() >= 3);
	for (uint32_t node : wanted) {
		gs_hlod_runtime::DesiredRecord record;
		record.node = node;
		desired.push_back(record);
	}
	uint32_t pages = 0;
	HlodNodeStreamer::Config config;
	for (int n = 0; n < paged->nodes.size(); ++n) {
		pages += (paged->nodes[n].topology.payload_count + config.page_splats - 1u) / config.page_splats;
	}
	config.atlas_pages = pages;
	config.max_inflight_read_bytes = 1; // Every read is larger: exactly one may run at a time.
	uint64_t largest_reservation = 0;
	for (uint32_t node : wanted) {
		largest_reservation = MAX(largest_reservation, paged->paged_source->get_nodes()[node].stored_bytes + uint64_t(paged->nodes[node].topology.payload_count) * sizeof(PackedGaussian));
	}
	Ref<HlodNodeStreamer> streamer;
	streamer.instantiate();
	GS_NEED(streamer->init(paged, config, 1, 1));
	streamer->begin_frame();
	uint32_t max_concurrent = 0;
	for (int frame = 0; frame < 600 && streamer->get_telemetry().payload_reads_total < wanted.size(); ++frame) {
		streamer->update(nullptr, HlodNodeStreamer::Buffers(), desired.ptr(), desired.size(), nullptr, 0);
		max_concurrent = MAX(max_concurrent, streamer->get_telemetry().payload_reads_in_flight);
		OS::get_singleton()->delay_usec(1000);
	}
	const HlodNodeStreamer::Telemetry t = streamer->get_telemetry();
	CHECK(t.payload_reads_total >= wanted.size()); // All of them were read, one after the other.
	CHECK(max_concurrent <= 1);
	CHECK(t.payload_in_flight_bytes_peak <= largest_reservation);
	streamer->shutdown();
}

} // namespace TestHlodPagedPackage
