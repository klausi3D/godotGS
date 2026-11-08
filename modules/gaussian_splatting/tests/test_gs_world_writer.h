#pragma once

// Streaming world writer, PLAIN_V3 core (BV-03a; docs/architecture/adr-beyond-vram-worlds.md
// §4.2-§4.5). CPU only; no GPU.
//
// §4.5 identity: the writer's output equals ResourceSaver::save of the same world, byte for
// byte, for seeds {1, 2, 3} x SH {0, 1, 3} x chunks on/off x metadata on/off, in both source
// modes. Negative controls show the comparison and the writer's checks are live: a 1-ULP
// chunk radius and a single flipped payload byte must each break identity at the field they
// touch; metadata serialized in a different key order must break it inside the metadata
// section (the saver sorts keys, so the writer must too: a dictionary built in a different
// insertion order still matches); and skipping pass 2 must be refused.

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_world.h"
#include "../core/streaming_chunk_payload_source.h"
#include "../io/gaussian_splat_world_format.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gs_atomic_file_transaction.h"
#include "../io/gs_world_writer.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"

#include <cmath>
#include <cstring>

namespace TestGSWorldWriter {

using gs_world_writer::ChunkDesc;
using gs_world_writer::CodecParams;
using gs_world_writer::Limits;
using gs_world_writer::PlainSpec;
using gs_world_writer::SourceMode;
using gs_world_writer::Writer;

// A fresh, empty directory per test so leftover temp/scratch files are observable.
inline String ww_dir(const String &p_suffix) {
	static uint32_t s_seq = 0;
	const String dir = OS::get_singleton()->get_temp_path().path_join(
			"gs_ww_" + p_suffix + "_" + itos(OS::get_singleton()->get_ticks_usec()) + "_" + itos(s_seq++));
	DirAccess::make_dir_recursive_absolute(dir);
	return dir;
}

inline PackedStringArray ww_list(const String &p_dir) {
	PackedStringArray out;
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return out;
	}
	da->list_dir_begin();
	for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
		if (name != "." && name != "..") {
			out.push_back(name);
		}
	}
	da->list_dir_end();
	out.sort();
	return out;
}

inline void ww_remove_dir(const String &p_dir) {
	const PackedStringArray names = ww_list(p_dir);
	for (const String &name : names) {
		DirAccess::remove_absolute(p_dir.path_join(name));
	}
	DirAccess::remove_absolute(p_dir);
}

inline PackedByteArray ww_read(const String &p_path) {
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

inline bool ww_write_raw(const String &p_path, const PackedByteArray &p_bytes) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	f->store_buffer(p_bytes.ptr(), p_bytes.size());
	return f->get_error() == OK;
}

// First differing byte, or -1 when equal (a size difference counts at the shorter end).
inline int64_t ww_first_diff(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	const int64_t n = MIN(p_a.size(), p_b.size());
	for (int64_t i = 0; i < n; i++) {
		if (p_a[i] != p_b[i]) {
			return i;
		}
	}
	return p_a.size() == p_b.size() ? -1 : n;
}

inline uint64_t ww_u64_at(const PackedByteArray &p_bytes, int64_t p_offset) {
	uint64_t v = 0;
	if (p_offset + 8 <= p_bytes.size()) {
		memcpy(&v, p_bytes.ptr() + p_offset, sizeof(v));
	}
	return v;
}

struct WWRng {
	uint64_t s;
	explicit WWRng(uint64_t p_seed) :
			s(p_seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
	uint32_t next() {
		s ^= s << 13u;
		s ^= s >> 7u;
		s ^= s << 17u;
		return uint32_t(s >> 11u);
	}
	float unit() { return float(next() & 0xFFFFFFu) / float(0x1000000u); }
	float range(float p_lo, float p_hi) { return p_lo + (p_hi - p_lo) * unit(); }
};

enum class ShCase : uint32_t {
	SH0 = 0,
	SH1 = 1,
	SH3 = 3,
};

struct WWFixture {
	Ref<GaussianData> data;
	Ref<GaussianSplatWorld> world;
	LocalVector<Gaussian> gaussians; // exactly what GaussianData stores (and the saver writes)
	LocalVector<Vector3> sh_high;
	LocalVector<uint32_t> counts; // append units, in order
	LocalVector<ChunkDesc> descs;
	Dictionary metadata;
	PlainSpec spec;
};

inline Dictionary ww_metadata(uint32_t p_seed, bool p_reversed) {
	// Same keys and values; only the insertion order differs when reversed.
	Dictionary nested;
	nested["generator"] = "bv03a";
	nested["seed"] = int64_t(p_seed);
	Array values;
	values.push_back(1.5);
	values.push_back("two");
	Dictionary d;
	if (!p_reversed) {
		d["name"] = vformat("world_%d", p_seed);
		d["lod_levels"] = int64_t(p_seed + 1);
		d["nested"] = nested;
		d["values"] = values;
	} else {
		d["values"] = values;
		d["nested"] = nested;
		d["lod_levels"] = int64_t(p_seed + 1);
		d["name"] = vformat("world_%d", p_seed);
	}
	return d;
}

// Deterministic world for (seed, SH, chunks, metadata). The chunk partition always exists
// (it frames the writer's append units); it becomes the world's chunk table only when
// p_chunks is set. One chunk is empty on purpose.
inline WWFixture ww_make(uint32_t p_seed, ShCase p_sh, bool p_chunks, bool p_metadata) {
	WWFixture fx;
	WWRng rng(p_seed);
	// > 1 MiB of gaussians and (for SH3) of SH, so payload sections span several slices.
	const uint32_t count = 7000u + p_seed * 733u;
	const uint32_t first_order = p_sh == ShCase::SH0 ? 0u : 3u;
	const uint32_t high_order = p_sh == ShCase::SH3 ? 12u : 0u;

	LocalVector<Gaussian> gaussians;
	gaussians.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		Gaussian g;
		g.position = Vector3(rng.range(-50.0f, 50.0f), rng.range(-5.0f, 5.0f), rng.range(-50.0f, 50.0f));
		g.opacity = rng.unit();
		g.scale = Vector3(rng.range(0.01f, 0.5f), rng.range(0.01f, 0.5f), rng.range(0.01f, 0.5f));
		g.rotation = Quaternion(rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f), 1.0f).normalized();
		g.sh_dc = Color(rng.unit(), rng.unit(), rng.unit(), 1.0f);
		for (uint32_t k = 0; k < first_order; k++) {
			g.sh_1[k] = Vector3(rng.range(-0.3f, 0.3f), rng.range(-0.3f, 0.3f), rng.range(-0.3f, 0.3f));
		}
		g.painterly_meta = rng.next() & 0xFFFFu;
		gaussians[i] = g;
	}
	LocalVector<Vector3> sh;
	sh.resize(count * high_order);
	for (uint32_t i = 0; i < sh.size(); i++) {
		sh[i] = Vector3(rng.range(-0.2f, 0.2f), rng.range(-0.2f, 0.2f), rng.range(-0.2f, 0.2f));
	}

	fx.data.instantiate();
	fx.data->set_gaussian_payload(gaussians, sh, first_order, high_order, false, false);
	fx.gaussians = fx.data->get_gaussian_storage();
	if (high_order > 0u) {
		const Vector3 *stored = fx.data->get_sh_high_order_coefficients_ptr();
		fx.sh_high.resize(count * high_order);
		memcpy(fx.sh_high.ptr(), stored, sizeof(Vector3) * fx.sh_high.size());
	}

	// Partition into 5 + seed contiguous chunks; chunk 2 is empty.
	const uint32_t chunk_count = 5u + p_seed;
	uint32_t cursor = 0;
	Vector<StaticChunk> chunks;
	for (uint32_t c = 0; c < chunk_count; c++) {
		uint32_t n = 0;
		if (c == chunk_count - 1u) {
			n = count - cursor;
		} else if (c != 2u) {
			n = MIN(count - cursor, 200u + (rng.next() % (count / chunk_count)));
		}
		StaticChunk chunk;
		AABB box;
		for (uint32_t i = 0; i < n; i++) {
			if (i == 0) {
				box = AABB(fx.gaussians[cursor].position, Vector3());
			} else {
				box.expand_to(fx.gaussians[cursor + i].position);
			}
		}
		chunk.bounds = box;
		chunk.center = box.get_center();
		chunk.radius = box.size.length() * 0.5f + 0.0001f * float(c);
		chunk.indices.resize(n);
		for (uint32_t i = 0; i < n; i++) {
			chunk.indices.write[i] = cursor + i;
		}
		chunks.push_back(chunk);
		ChunkDesc desc;
		desc.bounds = chunk.bounds;
		desc.center = chunk.center;
		desc.radius = chunk.radius;
		fx.descs.push_back(desc);
		fx.counts.push_back(n);
		cursor += n;
	}

	fx.world.instantiate();
	fx.world->set_gaussian_data(fx.data);
	fx.world->set_bounds(fx.data->get_aabb());
	if (p_chunks) {
		fx.world->set_static_chunks(chunks);
	}
	if (p_metadata) {
		fx.metadata = ww_metadata(p_seed, false);
		fx.world->set_metadata(fx.metadata);
	}

	fx.spec.splat_count = count;
	fx.spec.sh_degree = fx.data->get_sh_degree();
	fx.spec.sh_first_order = fx.data->get_sh_first_order_count();
	fx.spec.sh_high_order = fx.data->get_sh_high_order_count();
	fx.spec.is_2d = fx.data->get_2d_mode();
	fx.spec.bounds = fx.world->get_bounds();
	fx.spec.chunk_count = chunk_count;
	fx.spec.write_chunk_table = p_chunks;
	fx.spec.metadata = fx.metadata;
	return fx;
}

enum class WWMutation {
	NONE,
	RADIUS_ULP, // chunk 1's radius one ULP up
	METADATA_ORDER, // same metadata, built in reversed insertion order
	SINGLE_BYTE, // one byte of one splat flipped
	SKIP_PASS_2, // REITERABLE: finalize right after pass 1
};

// Drives the writer the way a producer does: pass by pass, chunk by chunk.
inline Error ww_write(const WWFixture &p_fx, SourceMode p_mode, const String &p_path, WWMutation p_mutation = WWMutation::NONE,
		uint32_t p_seed = 0, String *r_message = nullptr) {
	PlainSpec spec = p_fx.spec;
	LocalVector<ChunkDesc> descs = p_fx.descs;
	LocalVector<Gaussian> gaussians = p_fx.gaussians;
	if (p_mutation == WWMutation::RADIUS_ULP) {
		descs[1].radius = std::nextafter(descs[1].radius, INFINITY);
	} else if (p_mutation == WWMutation::METADATA_ORDER) {
		spec.metadata = ww_metadata(p_seed, true);
	} else if (p_mutation == WWMutation::SINGLE_BYTE) {
		reinterpret_cast<uint8_t *>(gaussians.ptr())[uint64_t(gaussians.size() / 2u) * sizeof(Gaussian) + 13u] ^= 0x01u;
	}

	Writer w;
	Error err = w.begin_plain(p_path, spec, CodecParams(), p_mode, Limits::unbounded());
	for (uint32_t pass = w.current_pass(); err == OK && pass != Writer::kNoPass; pass = w.current_pass()) {
		uint32_t base = 0;
		for (uint32_t c = 0; c < p_fx.counts.size() && err == OK; c++) {
			const uint32_t n = p_fx.counts[c];
			const Vector3 *sh = p_fx.sh_high.is_empty() ? nullptr : p_fx.sh_high.ptr() + uint64_t(base) * spec.sh_high_order;
			err = w.append_chunk(descs[c], gaussians.ptr() + base, sh, n);
			base += n;
		}
		if (p_mutation == WWMutation::SKIP_PASS_2 && pass == 1u) {
			break;
		}
	}
	if (err == OK) {
		err = w.finalize();
	}
	if (r_message) {
		*r_message = w.get_error_message();
	}
	return err;
}

// Section bounds of a plain v3 file, from its header.
struct WWSections {
	uint64_t gaussian_offset = 0;
	uint64_t sh_offset = 0;
	uint64_t chunk_table_offset = 0;
	uint64_t indices_offset = 0;
	uint64_t metadata_offset = 0;
};

inline WWSections ww_sections(const PackedByteArray &p_bytes) {
	WWSections s;
	s.gaussian_offset = ww_u64_at(p_bytes, 56);
	s.sh_offset = ww_u64_at(p_bytes, 64);
	s.chunk_table_offset = ww_u64_at(p_bytes, 72);
	s.indices_offset = ww_u64_at(p_bytes, 80);
	s.metadata_offset = ww_u64_at(p_bytes, 88);
	return s;
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] PLAIN_V3 output equals ResourceSaver::save across the identity matrix, in both modes") {
	const String dir = ww_dir("identity");
	uint32_t combos = 0;
	uint32_t identical = 0;
	for (uint32_t seed = 1; seed <= 3; seed++) {
		for (ShCase sh : { ShCase::SH0, ShCase::SH1, ShCase::SH3 }) {
			for (bool chunks : { false, true }) {
				for (bool metadata : { false, true }) {
					const WWFixture fx = ww_make(seed, sh, chunks, metadata);
					const String ref_path = dir.path_join("ref.gsplatworld");
					REQUIRE(ResourceSaver::save(fx.world, ref_path) == OK);
					const PackedByteArray ref = ww_read(ref_path);
					REQUIRE(ref.size() > int64_t(gs_world_format::kPrefixBytes));
					for (SourceMode mode : { SourceMode::REITERABLE, SourceMode::SINGLE_PASS }) {
						const String out_path = dir.path_join("out.gsplatworld");
						String message;
						const Error err = ww_write(fx, mode, out_path, WWMutation::NONE, seed, &message);
						INFO("seed=", seed, " sh=", uint32_t(sh), " chunks=", chunks, " metadata=", metadata,
								" mode=", mode == SourceMode::REITERABLE ? "REITERABLE" : "SINGLE_PASS", " ", message);
						CHECK(err == OK);
						const PackedByteArray out = ww_read(out_path);
						CHECK(out.size() == ref.size());
						const int64_t diff = ww_first_diff(out, ref);
						CHECK_MESSAGE(diff == -1, "first differing byte at ", diff);
						combos++;
						identical += (err == OK && diff == -1) ? 1u : 0u;
						DirAccess::remove_absolute(out_path);
					}
					DirAccess::remove_absolute(ref_path);
				}
			}
		}
	}
	CHECK(combos == 72u);
	CHECK(identical == 72u);
	// No temp or scratch survives any of the 72 writes.
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] writer output loads, streams and round-trips through the plain loader") {
	const String dir = ww_dir("roundtrip");
	const WWFixture fx = ww_make(2, ShCase::SH3, true, true);
	const String path = dir.path_join("world.gsplatworld");
	REQUIRE(ww_write(fx, SourceMode::SINGLE_PASS, path) == OK);

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = FAILED;
	Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
	REQUIRE(err == OK);
	if (resident.is_null()) {
		FAIL("load_resident returned no world");
		return;
	}
	CHECK(resident->get_splat_count() == fx.spec.splat_count);
	CHECK(resident->get_static_chunks().size() == int(fx.spec.chunk_count));
	const Dictionary loaded_metadata = resident->get_metadata();
	CHECK(loaded_metadata.size() == fx.metadata.size());
	CHECK(String(loaded_metadata.get("name", Variant())) == "world_2");
	Ref<GaussianData> data = resident->get_gaussian_data();
	if (data.is_null()) {
		FAIL("resident world has no GaussianData");
		return;
	}
	CHECK(memcmp(data->get_gaussian_storage().ptr(), fx.gaussians.ptr(), sizeof(Gaussian) * fx.gaussians.size()) == 0);
	CHECK(memcmp(data->get_sh_high_order_coefficients_ptr(), fx.sh_high.ptr(), sizeof(Vector3) * fx.sh_high.size()) == 0);

	Ref<GaussianSplatWorld> streamed = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (streamed.is_null()) {
		FAIL("streamed load returned no world");
		return;
	}
	Ref<ChunkPayloadSource> source = streamed->get_chunk_payload_source();
	if (source.is_null()) {
		FAIL("streamed world has no payload source");
		return;
	}
	LocalVector<Gaussian> snap;
	LocalVector<Vector3> snap_sh;
	uint32_t first = 0;
	uint32_t high = 0;
	const uint32_t last_start = fx.spec.splat_count - 64u;
	REQUIRE(source->capture_chunk_snapshot(last_start, 64u, snap, snap_sh, first, high));
	CHECK(memcmp(snap.ptr(), fx.gaussians.ptr() + last_start, sizeof(Gaussian) * 64u) == 0);
	CHECK(memcmp(snap_sh.ptr(), fx.sh_high.ptr() + uint64_t(last_start) * 12u, sizeof(Vector3) * 64u * 12u) == 0);
	source.unref();
	streamed.unref();
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] negative controls: 1-ULP radius, metadata key order and a single byte break identity where they should") {
	const String dir = ww_dir("negative");
	const uint32_t seed = 1;
	const WWFixture fx = ww_make(seed, ShCase::SH3, true, true);
	const String ref_path = dir.path_join("ref.gsplatworld");
	REQUIRE(ResourceSaver::save(fx.world, ref_path) == OK);
	const PackedByteArray ref = ww_read(ref_path);
	const WWSections sec = ww_sections(ref);
	REQUIRE(sec.metadata_offset > sec.indices_offset);

	for (SourceMode mode : { SourceMode::REITERABLE, SourceMode::SINGLE_PASS }) {
		INFO("mode=", mode == SourceMode::REITERABLE ? "REITERABLE" : "SINGLE_PASS");
		const String out_path = dir.path_join("out.gsplatworld");

		// Control 0: unmutated equals (so the controls below are not vacuous).
		REQUIRE(ww_write(fx, mode, out_path) == OK);
		CHECK(ww_first_diff(ww_read(out_path), ref) == -1);

		// 1-ULP radius on chunk 1: identity breaks inside chunk record 1's radius field.
		REQUIRE(ww_write(fx, mode, out_path, WWMutation::RADIUS_ULP, seed) == OK);
		const int64_t radius_diff = ww_first_diff(ww_read(out_path), ref);
		CHECK(radius_diff >= int64_t(sec.chunk_table_offset + 56u + 36u));
		CHECK(radius_diff < int64_t(sec.chunk_table_offset + 56u + 40u));

		// Metadata key order. The saver serializes with sorted keys, so the same
		// dictionary built in reversed insertion order must still be identical...
		REQUIRE(ww_write(fx, mode, out_path, WWMutation::METADATA_ORDER, seed) == OK);
		CHECK(ww_first_diff(ww_read(out_path), ref) == -1);
		// ...while the same keys serialized in a different order must break identity
		// inside the metadata section: the comparison sees key order, so a writer that
		// did not sort exactly like the saver would fail the matrix above. (The fixture
		// inserts keys unsorted, so insertion-order serialization really differs.)
		{
			PackedByteArray unsorted = ww_read(out_path);
			const PackedByteArray json = JSON::stringify(fx.metadata, "", false, false).to_utf8_buffer();
			REQUIRE(int64_t(sec.metadata_offset) + json.size() == unsorted.size()); // same keys, same length
			memcpy(unsorted.ptrw() + sec.metadata_offset, json.ptr(), size_t(json.size()));
			const int64_t meta_diff = ww_first_diff(unsorted, ref);
			CHECK(meta_diff >= int64_t(sec.metadata_offset));
		}

		// One flipped byte in the middle splat: identity breaks at exactly that byte.
		REQUIRE(ww_write(fx, mode, out_path, WWMutation::SINGLE_BYTE, seed) == OK);
		const int64_t expected_byte = int64_t(sec.gaussian_offset + uint64_t(fx.gaussians.size() / 2u) * sizeof(Gaussian) + 13u);
		CHECK(ww_first_diff(ww_read(out_path), ref) == expected_byte);
		DirAccess::remove_absolute(out_path);
	}
	DirAccess::remove_absolute(ref_path);
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] negative control: skipping pass 2 is refused and publishes nothing") {
	const String dir = ww_dir("skip_pass2");
	const WWFixture fx = ww_make(3, ShCase::SH3, true, false);
	const String path = dir.path_join("world.gsplatworld");
	String message;
	const Error err = ww_write(fx, SourceMode::REITERABLE, path, WWMutation::SKIP_PASS_2, 3, &message);
	CHECK(err == ERR_INVALID_DATA);
	CHECK(message.contains("count_mismatch"));
	CHECK_FALSE(FileAccess::exists(path));
	CHECK(ww_list(dir).size() == 0);

	// A world without high-order SH has a single pass even when REITERABLE, so the
	// same early finalize is legitimate there (the control is specific to pass 2).
	const WWFixture no_sh = ww_make(3, ShCase::SH1, true, false);
	CHECK(ww_write(no_sh, SourceMode::REITERABLE, path, WWMutation::SKIP_PASS_2, 3) == OK);
	CHECK(FileAccess::exists(path));
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] declared counts are exact (W4) and nothing reaches the final path before finalize (W1)") {
	const String dir = ww_dir("w4");
	const WWFixture fx = ww_make(1, ShCase::SH3, true, true);
	const String path = dir.path_join("world.gsplatworld");
	const PackedByteArray prior = String("prior good world").to_utf8_buffer();

	auto expect_refused = [&](Writer &w, Error p_err, const char *p_reason) {
		CHECK(p_err != OK);
		CHECK(w.get_state() == Writer::State::FAILED);
		CHECK_MESSAGE(w.get_error_message().contains(p_reason), w.get_error_message());
		CHECK(w.current_pass() == Writer::kNoPass);
		// The pre-existing final file is byte-intact and the directory holds nothing else.
		CHECK(ww_read(path) == prior);
		const PackedStringArray names = ww_list(dir);
		CHECK(names.size() == 1);
	};

	for (SourceMode mode : { SourceMode::REITERABLE, SourceMode::SINGLE_PASS }) {
		INFO("mode=", mode == SourceMode::REITERABLE ? "REITERABLE" : "SINGLE_PASS");
		REQUIRE(ww_write_raw(path, prior));

		{ // a pass that ends short of splat_count
			Writer w;
			REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
			Error err = OK;
			uint32_t base = 0;
			for (uint32_t c = 0; c < fx.counts.size() && err == OK; c++) {
				const uint32_t n = c + 1u == fx.counts.size() ? fx.counts[c] - 1u : fx.counts[c];
				err = w.append_chunk(fx.descs[c], fx.gaussians.ptr() + base, fx.sh_high.ptr() + uint64_t(base) * 12u, n);
				base += fx.counts[c];
			}
			CHECK(err == ERR_INVALID_DATA);
			expect_refused(w, err, "count_mismatch");
		}
		{ // a chunk past splat_count
			Writer w;
			REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
			LocalVector<Gaussian> big;
			big.resize(fx.spec.splat_count + 1u);
			LocalVector<Vector3> big_sh;
			big_sh.resize(big.size() * 12u);
			const Error err = w.append_chunk(fx.descs[0], big.ptr(), big_sh.ptr(), big.size());
			CHECK(err == ERR_INVALID_DATA);
			expect_refused(w, err, "count_mismatch");
		}
		{ // a chunk past the declared chunk_count (the only pass already completed)
			PlainSpec one = fx.spec;
			one.chunk_count = 1;
			one.write_chunk_table = false;
			one.sh_high_order = 0;
			Writer w;
			REQUIRE(w.begin_plain(path, one, CodecParams(), mode, Limits::unbounded()) == OK);
			REQUIRE(w.append_chunk(fx.descs[0], fx.gaussians.ptr(), nullptr, one.splat_count) == OK);
			CHECK(w.current_pass() == Writer::kNoPass);
			const Error err = w.append_chunk(fx.descs[0], fx.gaussians.ptr(), nullptr, 0);
			CHECK(err == ERR_INVALID_DATA);
			expect_refused(w, err, "count_mismatch");
		}
		{ // finalize mid-pass
			Writer w;
			REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
			REQUIRE(w.append_chunk(fx.descs[0], fx.gaussians.ptr(), fx.sh_high.ptr(), fx.counts[0]) == OK);
			// W1: the final path still holds the prior file while the writer is open.
			CHECK(ww_read(path) == prior);
			CHECK(ww_list(dir).size() == (mode == SourceMode::SINGLE_PASS ? 3 : 2)); // + temp (+ SH scratch)
			const Error err = w.finalize();
			CHECK(err == ERR_INVALID_DATA);
			expect_refused(w, err, "count_mismatch");
		}
		{ // a null payload pointer
			Writer w;
			REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
			const Error err = w.append_chunk(fx.descs[0], nullptr, fx.sh_high.ptr(), fx.counts[0]);
			CHECK(err == ERR_INVALID_PARAMETER);
			expect_refused(w, err, "invalid_chunk");
		}
		{ // abort and the destructor
			{
				Writer w;
				REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
				REQUIRE(w.append_chunk(fx.descs[0], fx.gaussians.ptr(), fx.sh_high.ptr(), fx.counts[0]) == OK);
				w.abort();
				CHECK(w.get_state() == Writer::State::ABORTED);
				CHECK(ww_read(path) == prior);
				CHECK(ww_list(dir).size() == 1);
			}
			{
				Writer w;
				REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
				REQUIRE(w.append_chunk(fx.descs[0], fx.gaussians.ptr(), fx.sh_high.ptr(), fx.counts[0]) == OK);
			}
			CHECK(ww_read(path) == prior);
			CHECK(ww_list(dir).size() == 1);
		}
	}
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] a REITERABLE pass 2 that diverges from pass 1 is refused") {
	const String dir = ww_dir("diverge");
	const WWFixture with_table = ww_make(2, ShCase::SH3, true, false);
	// Without a chunk table the per-chunk count is the only replay check, so the count
	// divergence is also driven there.
	const WWFixture without_table = ww_make(2, ShCase::SH3, false, false);
	const String path = dir.path_join("world.gsplatworld");

	auto run = [&](const WWFixture &fx, bool p_change_count) {
		Writer w;
		REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), SourceMode::REITERABLE, Limits::unbounded()) == OK);
		Error err = OK;
		for (uint32_t pass = w.current_pass(); err == OK && pass != Writer::kNoPass; pass = w.current_pass()) {
			uint32_t base = 0;
			for (uint32_t c = 0; c < fx.counts.size() && err == OK; c++) {
				ChunkDesc desc = fx.descs[c];
				uint32_t n = fx.counts[c];
				if (pass == 2u && c == 3u) {
					if (p_change_count) {
						n -= 1u;
					} else {
						desc.center.x += 1.0f;
					}
				}
				err = w.append_chunk(desc, fx.gaussians.ptr() + base, fx.sh_high.ptr() + uint64_t(base) * 12u, n);
				base += fx.counts[c];
			}
		}
		CHECK(err == ERR_INVALID_DATA);
		CHECK_MESSAGE(w.get_error_message().contains("stream_diverged"), w.get_error_message());
		CHECK_FALSE(FileAccess::exists(path));
		CHECK(ww_list(dir).size() == 0);
	};
	run(with_table, true);
	run(with_table, false);
	run(without_table, true);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] begin_plain refuses specs the plain reader would refuse, creating nothing") {
	const String dir = ww_dir("refuse");
	const String path = dir.path_join("world.gsplatworld");
	PlainSpec good;
	good.splat_count = 16;
	good.chunk_count = 1;

	struct Case {
		const char *name;
		PlainSpec spec;
		CodecParams codec;
		Error expected;
	};
	LocalVector<Case> cases;
	{
		// The D-12 block is valid, but bit 7 needs the chunk table: it is the page directory (W11).
		Case c{ "codec GSQ v1 without the chunk table", good, gs_world_writer::gsq_v1_codec_params(), ERR_INVALID_PARAMETER };
		cases.push_back(c);
	}
	{
		Case c{ "codec GSQ v1 with an out-of-range field", good, gs_world_writer::gsq_v1_codec_params(), ERR_INVALID_PARAMETER };
		c.codec.sh_bits[2] = 9;
		cases.push_back(c);
	}
	{
		Case c{ "codec GSQ v1 with fields left zero", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.codec.codec_id = 1;
		cases.push_back(c);
	}
	{
		Case c{ "codec GSQ v1 at SH degree 4", good, gs_world_writer::gsq_v1_codec_params(), ERR_INVALID_PARAMETER };
		c.spec.sh_degree = 4;
		cases.push_back(c);
	}
	{
		Case c{ "unknown codec", good, CodecParams(), ERR_UNAVAILABLE };
		c.codec.codec_id = 2;
		cases.push_back(c);
	}
	{
		Case c{ "codec field set with codec none", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.codec.zstd_level = 3;
		cases.push_back(c);
	}
	{
		Case c{ "zero splats", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.spec.splat_count = 0;
		cases.push_back(c);
	}
	{
		Case c{ "zero chunks", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.spec.chunk_count = 0;
		cases.push_back(c);
	}
	{
		Case c{ "sh_degree 5", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.spec.sh_degree = 5;
		cases.push_back(c);
	}
	{
		Case c{ "sh_first_order 4", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.spec.sh_first_order = 4;
		cases.push_back(c);
	}
	{
		Case c{ "more than UINT32_MAX SH coefficients", good, CodecParams(), ERR_INVALID_PARAMETER };
		c.spec.splat_count = 0x40000000u;
		c.spec.sh_high_order = 4;
		cases.push_back(c);
	}
	for (const Case &c : cases) {
		INFO(c.name);
		Writer w;
		const Error err = w.begin_plain(path, c.spec, c.codec, SourceMode::SINGLE_PASS, Limits::unbounded());
		CHECK(err == c.expected);
		CHECK(w.get_state() == Writer::State::FAILED);
		CHECK(w.current_pass() == Writer::kNoPass);
		CHECK(ww_list(dir).size() == 0);
	}
	// The baseline spec itself is accepted (so the refusals above are about the field).
	Writer w;
	CHECK(w.begin_plain(path, good, CodecParams(), SourceMode::SINGLE_PASS, Limits::unbounded()) == OK);
	w.abort();
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] SINGLE_PASS spills SH to a scratch sibling and removes it") {
	const String dir = ww_dir("scratch");
	const WWFixture fx = ww_make(1, ShCase::SH3, false, false);
	const String path = dir.path_join("world.gsplatworld");
	Writer w;
	REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), SourceMode::SINGLE_PASS, Limits::unbounded()) == OK);
	CHECK(w.get_pass_count() == 1u);
	uint32_t base = 0;
	for (uint32_t c = 0; c < fx.counts.size(); c++) {
		REQUIRE(w.append_chunk(fx.descs[c], fx.gaussians.ptr() + base, fx.sh_high.ptr() + uint64_t(base) * 12u, fx.counts[c]) == OK);
		base += fx.counts[c];
	}
	const PackedStringArray during = ww_list(dir);
	bool saw_scratch = false;
	for (const String &name : during) {
		saw_scratch = saw_scratch || name.begins_with("world.gsplatworld.sh_scratch.tmp.");
	}
	CHECK(saw_scratch);
	CHECK(during.size() == 2); // output temp + SH scratch
	REQUIRE(w.finalize() == OK);
	const PackedStringArray after = ww_list(dir);
	if (after.size() != 1) {
		FAIL("expected only the published world, got ", after.size(), " entries");
		return;
	}
	CHECK(after[0] == "world.gsplatworld");
	CHECK(uint64_t(ww_read(path).size()) == w.get_expected_file_size());

	// REITERABLE needs no scratch: two passes over the source instead.
	Writer r;
	REQUIRE(r.begin_plain(dir.path_join("r.gsplatworld"), fx.spec, CodecParams(), SourceMode::REITERABLE, Limits::unbounded()) == OK);
	CHECK(r.get_pass_count() == 2u);
	CHECK(ww_list(dir).size() == 2); // world + r's temp
	r.abort();
	ww_remove_dir(dir);
}

#ifdef TESTS_ENABLED
struct WWDrainHook {
	bool drained = true;
	uint32_t calls = 0;
	static bool hook(void *p_userdata) {
		WWDrainHook *self = static_cast<WWDrainHook *>(p_userdata);
		self->calls++;
		return self->drained;
	}
};

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] reader-suspend: readers that do not drain refuse the replace and keep the destination") {
	const String dir = ww_dir("busy");
	const WWFixture fx = ww_make(1, ShCase::SH1, true, false);
	const String path = dir.path_join("world.gsplatworld");
	const PackedByteArray prior = String("prior good world").to_utf8_buffer();
	REQUIRE(ww_write_raw(path, prior));

	WWDrainHook hook;
	hook.drained = false;
	Writer::drain_test_hook = &WWDrainHook::hook;
	Writer::drain_test_userdata = &hook;
	String message;
	const Error err = ww_write(fx, SourceMode::SINGLE_PASS, path, WWMutation::NONE, 1, &message);
	Writer::drain_test_hook = nullptr;
	Writer::drain_test_userdata = nullptr;

	CHECK(hook.calls == 1u);
	CHECK(err == ERR_BUSY);
	CHECK(message.contains("readers_busy"));
	CHECK(ww_read(path) == prior);
	CHECK(ww_list(dir).size() == 1);
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] reader-suspend: the writer replaces a world that a live streaming source holds open") {
	// The #714 situation: a streamed world caches pooled read handles to the very file
	// the writer replaces. On Windows FileAccess opens without FILE_SHARE_DELETE, so
	// without the reader-suspend guard MoveFileExW fails with a sharing violation. On
	// POSIX the guard is a no-op and rename() replaces the open file anyway.
	const String dir = ww_dir("live_reader");
	const String path = dir.path_join("world.gsplatworld");
	const WWFixture old_fx = ww_make(1, ShCase::SH1, true, false);
	REQUIRE(ww_write(old_fx, SourceMode::SINGLE_PASS, path) == OK);

	Ref<GaussianSplatWorld> streamed = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (streamed.is_null()) {
		FAIL("streamed load returned no world");
		return;
	}
	Ref<StagedFileChunkPayloadSource> source = streamed->get_chunk_payload_source();
	if (source.is_null()) {
		FAIL("streamed world has no file-backed payload source");
		return;
	}
	LocalVector<Gaussian> snap;
	LocalVector<Vector3> snap_sh;
	uint32_t first = 0;
	uint32_t high = 0;
	REQUIRE(source->capture_chunk_snapshot(0, 32, snap, snap_sh, first, high));
	REQUIRE(source->get_file_open_count() >= 1u); // a pooled handle now holds the destination open

	WWDrainHook hook; // observe that the guard ran; it reports what the real guard decided
	Writer::drain_test_hook = &WWDrainHook::hook;
	Writer::drain_test_userdata = &hook;
	const WWFixture new_fx = ww_make(2, ShCase::SH3, true, true);
	String message;
	const Error err = ww_write(new_fx, SourceMode::REITERABLE, path, WWMutation::NONE, 2, &message);
	Writer::drain_test_hook = nullptr;
	Writer::drain_test_userdata = nullptr;
	CHECK(hook.calls == 1u);
	CHECK_MESSAGE(err == OK, message);

	const String ref_path = dir.path_join("ref.gsplatworld");
	REQUIRE(ResourceSaver::save(new_fx.world, ref_path) == OK);
	CHECK(ww_first_diff(ww_read(path), ww_read(ref_path)) == -1);
	// The source is re-admitted after the replace and can read again.
	CHECK(source->capture_chunk_snapshot(0, 32, snap, snap_sh, first, high));
	source.unref();
	streamed.unref();
	ww_remove_dir(dir);
}
#endif // TESTS_ENABLED

TEST_CASE("[GaussianSplatting][AtomicWrite] transaction read_at and flush_to_storage (BV-03a additions)") {
	const String dir = ww_dir("txn_read");
	const String path = dir.path_join("t.bin");
	GSAtomicFileTransaction txn;
	REQUIRE(txn.begin(path) == OK);
	PackedByteArray data;
	data.resize(3 * 1024 * 1024 + 17);
	for (int64_t i = 0; i < data.size(); i++) {
		data.write[i] = uint8_t((i * 31 + (i >> 9)) & 0xFF);
	}
	REQUIRE(txn.append(data.ptr(), uint64_t(data.size())) == OK);
	CHECK(txn.flush_to_storage() == OK);

	PackedByteArray back;
	back.resize(data.size());
	CHECK(txn.read_at(0, back.ptrw(), uint64_t(back.size())) == OK);
	CHECK(back == data);
	uint8_t mid[64];
	CHECK(txn.read_at(1234567, mid, sizeof(mid)) == OK);
	CHECK(memcmp(mid, data.ptr() + 1234567, sizeof(mid)) == 0);
	// Past the written end is an argument error and does not poison the transaction.
	CHECK(txn.read_at(uint64_t(data.size()) - 4u, mid, 8) == ERR_INVALID_PARAMETER);
	CHECK(txn.is_open());
	// A rewrite is visible to read_at on the same handle.
	const uint8_t patch[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	REQUIRE(txn.write_at(100, patch, sizeof(patch)) == OK);
	CHECK(txn.read_at(100, mid, 4) == OK);
	CHECK(memcmp(mid, patch, 4) == 0);
	CHECK_FALSE(FileAccess::exists(path)); // still nothing at the final path
	txn.abort();
	CHECK(txn.read_at(0, mid, 1) == ERR_UNCONFIGURED);
	CHECK(txn.flush_to_storage() == ERR_UNCONFIGURED);
	CHECK(ww_list(dir).size() == 0);
	ww_remove_dir(dir);
}

// ---------------------------------------------------------------------------
// BV-03b: Limits, preflight, cancel, deadline, progress, Report (ADR §4.3, W10, W11).
// ---------------------------------------------------------------------------

using gs_world_writer::Phase;
using gs_world_writer::Progress;
using gs_world_writer::Report;

// Drives one writer through every pass; records a progress snapshot after each append.
inline Error ww_drive(Writer &w, const WWFixture &p_fx, SourceMode p_mode, const String &p_path, const Limits &p_limits,
		LocalVector<Progress> *r_snapshots = nullptr) {
	Error err = w.begin_plain(p_path, p_fx.spec, CodecParams(), p_mode, p_limits);
	for (uint32_t pass = w.current_pass(); err == OK && pass != Writer::kNoPass; pass = w.current_pass()) {
		uint32_t base = 0;
		for (uint32_t c = 0; c < p_fx.counts.size() && err == OK; c++) {
			const uint32_t n = p_fx.counts[c];
			const Vector3 *sh = p_fx.sh_high.is_empty() ? nullptr : p_fx.sh_high.ptr() + uint64_t(base) * p_fx.spec.sh_high_order;
			err = w.append_chunk(p_fx.descs[c], p_fx.gaussians.ptr() + base, sh, n);
			base += n;
			if (r_snapshots) {
				r_snapshots->push_back(w.get_progress());
			}
		}
	}
	if (err == OK) {
		err = w.finalize();
	}
	if (r_snapshots) {
		r_snapshots->push_back(w.get_progress());
	}
	return err;
}

// Every refusal must leave the directory as it was: only the prior world, byte-intact.
inline bool ww_only_prior(const String &p_dir, const String &p_path, const PackedByteArray &p_prior) {
	const PackedStringArray names = ww_list(p_dir);
	return names.size() == 1 && names[0] == p_path.get_file() && ww_read(p_path) == p_prior;
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] limits: a field left at 0 refuses at begin_plain with limits_unset, creating nothing") {
	const String dir = ww_dir("limits_unset");
	const WWFixture fx = ww_make(1, ShCase::SH1, true, false);
	const String path = dir.path_join("world.gsplatworld");
	for (int field = 0; field < 5; field++) {
		Limits l = Limits::unbounded();
		uint64_t *fields[5] = { &l.max_ram_bytes, &l.max_scratch_bytes, &l.max_output_bytes, &l.deadline_usec, &l.min_free_disk_margin };
		*fields[field] = 0u;
		Writer w;
		INFO("field ", field);
		CHECK(w.begin_plain(path, fx.spec, CodecParams(), SourceMode::SINGLE_PASS, l) == ERR_INVALID_PARAMETER);
		CHECK(w.get_report().reason == "limits_unset");
		CHECK_FALSE(w.get_report().committed);
		CHECK(w.get_progress().phase == Phase::STOPPED);
		CHECK(ww_list(dir).size() == 0);
	}
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] Report carries the measured numbers; preflight refuses RAM, scratch, output and disk one byte short") {
	const String dir = ww_dir("preflight");
	const WWFixture fx = ww_make(1, ShCase::SH3, true, true);
	const String path = dir.path_join("world.gsplatworld");

	// The reference: an unbounded SINGLE_PASS write (gaussians to the output, SH to the scratch).
	Report ref;
	{
		Writer w;
		REQUIRE(ww_drive(w, fx, SourceMode::SINGLE_PASS, path, Limits::unbounded()) == OK);
		ref = w.get_report();
		const uint64_t file_size = uint64_t(ww_read(path).size());
		const uint64_t sh_bytes = uint64_t(fx.spec.splat_count) * fx.spec.sh_high_order * sizeof(Vector3);
		CHECK(ref.committed);
		CHECK(ref.error == OK);
		CHECK(ref.reason.is_empty());
		CHECK(ref.pass_count == 1u);
		CHECK(ref.chunks_appended == fx.spec.chunk_count);
		CHECK(ref.splats_appended == fx.spec.splat_count);
		CHECK(ref.output_bytes == file_size);
		CHECK(ref.output_estimate_bytes == file_size); // codec none: exact
		CHECK(ref.scratch_peak_bytes == sh_bytes);
		CHECK(ref.scratch_estimate_bytes == sh_bytes);
		// Every output byte once, the scratch once, plus the one header rewrite (W2).
		CHECK(ref.bytes_written == file_size + sh_bytes + gs_world_format::kPrefixBytes);
		CHECK(ref.ram_peak_bytes > (uint64_t(1) << 20u));
		CHECK(ref.ram_peak_bytes <= ref.ram_estimate_bytes);
		CHECK(ref.free_disk_bytes > 0u);
		CHECK(ref.disk_need_bytes == file_size + sh_bytes + gs_world_writer::kDefaultFreeDiskMargin);
	}
	const PackedByteArray prior = ww_read(path); // the published world is now the prior file

	struct Case {
		const char *reason;
		Limits limits;
	};
	Limits ram = Limits::unbounded();
	ram.max_ram_bytes = ref.ram_estimate_bytes - 1u;
	Limits scratch = Limits::unbounded();
	scratch.max_scratch_bytes = ref.scratch_estimate_bytes - 1u;
	Limits out = Limits::unbounded();
	out.max_output_bytes = ref.output_estimate_bytes - 1u;
	const Case cases[] = { { "limit_ram", ram }, { "limit_scratch", scratch }, { "limit_output", out } };
	for (const Case &c : cases) {
		INFO(c.reason);
		Writer w;
		CHECK(w.begin_plain(path, fx.spec, CodecParams(), SourceMode::SINGLE_PASS, c.limits) == ERR_PARAMETER_RANGE_ERROR);
		CHECK(w.get_report().reason == c.reason);
		CHECK(w.get_error_message().contains(c.reason));
		CHECK(w.get_report().output_bytes == 0u); // refused before the first byte
		CHECK(ww_only_prior(dir, path, prior));
	}

	// At the exact estimates the same limits pass.
	{
		Limits exact = Limits::unbounded();
		exact.max_ram_bytes = ref.ram_estimate_bytes;
		exact.max_scratch_bytes = ref.scratch_estimate_bytes;
		exact.max_output_bytes = ref.output_estimate_bytes;
		Writer w;
		CHECK(ww_drive(w, fx, SourceMode::SINGLE_PASS, path, exact) == OK);
		CHECK(ww_read(path) == prior); // same world, same bytes (W7)
	}

#ifdef TESTS_ENABLED
	// Disk: the free space is injected. Unknown (0) and one byte short both refuse.
	struct FreeSpace {
		uint64_t free_bytes = 0;
		String seen_dir;
		static uint64_t hook(const String &p_dir, void *p_userdata) {
			FreeSpace *self = static_cast<FreeSpace *>(p_userdata);
			self->seen_dir = p_dir;
			return self->free_bytes;
		}
	};
	FreeSpace fs;
	Writer::free_space_test_hook = &FreeSpace::hook;
	Writer::free_space_test_userdata = &fs;
	const uint64_t frees[3] = { 0u, ref.disk_need_bytes - 1u, ref.disk_need_bytes };
	Error results[3] = { OK, OK, OK };
	String reasons[3];
	for (int i = 0; i < 3; i++) {
		fs.free_bytes = frees[i];
		Writer w;
		results[i] = ww_drive(w, fx, SourceMode::SINGLE_PASS, path, Limits::unbounded());
		reasons[i] = w.get_report().reason;
	}
	Writer::free_space_test_hook = nullptr;
	Writer::free_space_test_userdata = nullptr;
	CHECK(fs.seen_dir == dir);
	CHECK(results[0] == ERR_FILE_CANT_WRITE);
	CHECK(reasons[0] == "preflight_disk");
	CHECK(results[1] == ERR_FILE_CANT_WRITE);
	CHECK(reasons[1] == "preflight_disk");
	CHECK(results[2] == OK);
	CHECK(reasons[2].is_empty());
	CHECK(ww_only_prior(dir, path, prior));
#endif // TESTS_ENABLED
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] progress is monotonic over both passes and ends at the totals") {
	const String dir = ww_dir("progress");
	const WWFixture fx = ww_make(2, ShCase::SH3, true, false);
	const String path = dir.path_join("world.gsplatworld");
	Writer w;
	LocalVector<Progress> snaps;
	REQUIRE(ww_drive(w, fx, SourceMode::REITERABLE, path, Limits::unbounded(), &snaps) == OK);
	if (snaps.size() != fx.counts.size() * 2u + 1u) {
		FAIL("expected one snapshot per append plus one, got ", snaps.size());
		return;
	}
	bool monotonic = true;
	bool saw_pass_2 = false;
	for (uint32_t i = 1; i < snaps.size(); i++) {
		const Progress &a = snaps[i - 1];
		const Progress &b = snaps[i];
		monotonic = monotonic && b.chunks_done >= a.chunks_done && b.splats_done >= a.splats_done &&
				b.bytes_written >= a.bytes_written && uint32_t(b.phase) >= uint32_t(a.phase);
		saw_pass_2 = saw_pass_2 || b.pass == 2u;
	}
	CHECK(monotonic);
	CHECK(saw_pass_2);
	const Progress &last = snaps[snaps.size() - 1];
	CHECK(last.phase == Phase::COMMITTED);
	CHECK(last.pass_count == 2u);
	CHECK(last.chunks_total == uint64_t(fx.spec.chunk_count) * 2u);
	CHECK(last.chunks_done == last.chunks_total);
	CHECK(last.splats_total == uint64_t(fx.spec.splat_count) * 2u);
	CHECK(last.splats_done == last.splats_total);
	const Report &rep = w.get_report();
	CHECK(last.bytes_written == rep.bytes_written);
	// REITERABLE: no scratch; every output byte once plus the header rewrite.
	CHECK(rep.scratch_peak_bytes == 0u);
	CHECK(rep.bytes_written == uint64_t(ww_read(path).size()) + gs_world_format::kPrefixBytes);
	ww_remove_dir(dir);
}

#ifdef TESTS_ENABLED
// A clock the test controls; it can also request a cancel from inside the writer's
// own per-slice check (as another thread would), recording the bytes written then.
struct WWClock {
	uint64_t now = 1000u;
	uint32_t calls = 0;
	uint32_t cancel_at_call = 0; // 0 = never
	uint64_t advance_at_call = 0; // 0 = never
	uint64_t advance_by = 0;
	Writer *writer = nullptr;
	uint64_t bytes_at_cancel = 0;
	static uint64_t hook(void *p_userdata) {
		WWClock *self = static_cast<WWClock *>(p_userdata);
		self->calls++;
		if (self->cancel_at_call != 0u && self->calls == self->cancel_at_call && self->writer != nullptr) {
			self->bytes_at_cancel = self->writer->get_progress().bytes_written;
			self->writer->request_cancel();
		}
		if (self->advance_at_call != 0u && self->calls == self->advance_at_call) {
			self->now += self->advance_by;
		}
		return self->now;
	}
};

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] cancel: observed within one slice, publishes nothing, keeps the prior world, distinct reason") {
	const String dir = ww_dir("cancel");
	const WWFixture fx = ww_make(1, ShCase::SH3, true, false);
	const String path = dir.path_join("world.gsplatworld");
	const PackedByteArray prior = String("prior good world").to_utf8_buffer();
	REQUIRE(ww_write_raw(path, prior));

	// Between chunks, in both modes: SINGLE_PASS leaves a scratch that must go too.
	for (SourceMode mode : { SourceMode::REITERABLE, SourceMode::SINGLE_PASS }) {
		Writer w;
		REQUIRE(w.begin_plain(path, fx.spec, CodecParams(), mode, Limits::unbounded()) == OK);
		uint32_t base = 0;
		for (uint32_t c = 0; c < 2u; c++) {
			REQUIRE(w.append_chunk(fx.descs[c], fx.gaussians.ptr() + base, fx.sh_high.ptr() + uint64_t(base) * 12u, fx.counts[c]) == OK);
			base += fx.counts[c];
		}
		const Progress before = w.get_progress();
		w.request_cancel();
		CHECK(w.append_chunk(fx.descs[2], fx.gaussians.ptr() + base, fx.sh_high.ptr() + uint64_t(base) * 12u, fx.counts[2]) == ERR_SKIP);
		CHECK(w.get_report().reason == "cancelled");
		CHECK(w.get_error_message().contains("cancelled"));
		const Progress after = w.get_progress();
		CHECK(after.phase == Phase::STOPPED);
		CHECK(after.chunks_done == before.chunks_done);
		CHECK(after.bytes_written == before.bytes_written);
		CHECK(w.finalize() == ERR_SKIP);
		CHECK(ww_only_prior(dir, path, prior));
	}

	// Inside a chunk: the cancel lands during chunk 0's gaussian slice (raised from the
	// writer's own per-slice clock read, as another thread would) and is honoured at the
	// next slice, chunk 0's SH, after at most one further slice (<= 1 MiB).
	WWClock clock;
	Writer w;
	clock.writer = &w;
	Writer::clock_test_hook = &WWClock::hook;
	Writer::clock_test_userdata = &clock;
	Limits l = Limits::unbounded();
	l.deadline_usec = uint64_t(1) << 40u; // finite, so every slice reads the clock
	// Clock reads: 1 begin_plain, 2 the header placeholder slice, 3 append_chunk's entry
	// check, 4 chunk 0's gaussian slice (cancel requested here; that slice is written).
	clock.cancel_at_call = 4u;
	const Error err = ww_drive(w, fx, SourceMode::SINGLE_PASS, path, l);
	Writer::clock_test_hook = nullptr;
	Writer::clock_test_userdata = nullptr;
	CHECK(err == ERR_SKIP);
	CHECK(w.get_report().reason == "cancelled");
	CHECK(clock.bytes_at_cancel == gs_world_format::kPrefixBytes); // only the placeholder header before
	CHECK(w.get_report().bytes_written > clock.bytes_at_cancel); // the in-flight slice completed
	CHECK(w.get_report().bytes_written - clock.bytes_at_cancel <= (uint64_t(1) << 20u));
	CHECK(w.get_report().chunks_appended == 0u); // stopped inside chunk 0, before its SH
	CHECK(ww_only_prior(dir, path, prior));
	ww_remove_dir(dir);
}

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] deadline: exceeded mid-write refuses with ERR_TIMEOUT and publishes nothing") {
	const String dir = ww_dir("deadline");
	const WWFixture fx = ww_make(1, ShCase::SH1, true, false);
	const String path = dir.path_join("world.gsplatworld");
	const PackedByteArray prior = String("prior good world").to_utf8_buffer();
	REQUIRE(ww_write_raw(path, prior));

	WWClock clock;
	Writer::clock_test_hook = &WWClock::hook;
	Writer::clock_test_userdata = &clock;
	Limits l = Limits::unbounded();
	l.deadline_usec = 5000u;
	// Within the deadline the write commits (the clock never moves).
	Writer ok_writer;
	const Error ok_err = ww_drive(ok_writer, fx, SourceMode::SINGLE_PASS, dir.path_join("other.gsplatworld"), l);
	// Then the clock jumps past the deadline after a few checks.
	clock.calls = 0;
	clock.advance_at_call = 4u;
	clock.advance_by = 5001u;
	Writer w;
	const Error err = ww_drive(w, fx, SourceMode::SINGLE_PASS, path, l);
	Writer::clock_test_hook = nullptr;
	Writer::clock_test_userdata = nullptr;

	CHECK(ok_err == OK);
	CHECK(ok_writer.get_report().elapsed_usec == 0u);
	CHECK(err == ERR_TIMEOUT);
	CHECK(w.get_report().reason == "deadline");
	CHECK(w.get_report().elapsed_usec == 5001u);
	DirAccess::remove_absolute(dir.path_join("other.gsplatworld"));
	CHECK(ww_only_prior(dir, path, prior));
	ww_remove_dir(dir);
}

// Codex on #1377: the reader drain can wait (bounded); a cancel that arrives while it drains
// must still stop the replace.
struct WWCancelInDrain {
	Writer *writer = nullptr;
	uint32_t calls = 0;
	static bool hook(void *p_userdata) {
		WWCancelInDrain *self = static_cast<WWCancelInDrain *>(p_userdata);
		self->calls++;
		self->writer->request_cancel();
		return true; // drained
	}
};

TEST_CASE("[GaussianSplatting][WorldIO][WorldWriter] cancel: requested while readers drain still stops the replace") {
	const String dir = ww_dir("cancel_drain");
	const WWFixture fx = ww_make(1, ShCase::SH1, true, false);
	const String path = dir.path_join("world.gsplatworld");
	const PackedByteArray prior = String("prior good world").to_utf8_buffer();
	REQUIRE(ww_write_raw(path, prior));
	Writer w;
	WWCancelInDrain hook;
	hook.writer = &w;
	Writer::drain_test_hook = &WWCancelInDrain::hook;
	Writer::drain_test_userdata = &hook;
	const Error err = ww_drive(w, fx, SourceMode::SINGLE_PASS, path, Limits::unbounded());
	Writer::drain_test_hook = nullptr;
	Writer::drain_test_userdata = nullptr;
	CHECK(hook.calls == 1u);
	CHECK(err == ERR_SKIP);
	CHECK(w.get_report().reason == "cancelled");
	CHECK_FALSE(w.get_report().committed);
	CHECK(ww_only_prior(dir, path, prior));
	ww_remove_dir(dir);
}
#endif // TESTS_ENABLED

} // namespace TestGSWorldWriter
