#pragma once

// Captured legacy producer files and exact identities live under
// tests/fixtures/legacy_world_producer/. Missing inputs are a failure.
#include "test_gaussian_layout_fingerprint.h"

namespace TestAuthenticLegacyWorld {
static bool check_leaf_values(const Gaussian &p_got) {
	const int id = p_got.painterly_meta == gaussian_pack_painterly_meta(23u, 5u) ? 0
			: p_got.painterly_meta == gaussian_pack_painterly_meta(24u, 5u) ? 1 : -1;
	if (id < 0) {
		FAIL("authentic fixture leaf identity was lost");
		return false;
	}
	CHECK(p_got.position.is_equal_approx(Vector3(4.0f + id, 5.0f, 6.0f)));
	CHECK(p_got.scale.is_equal_approx(Vector3(0.25f, 0.5f, 0.75f)));
	CHECK(p_got.sh_dc.is_equal_approx(Color(0.25f, 0.5f, 0.75f, 1.0f)));
	CHECK_EQ(p_got.opacity, 0.75f);
	CHECK(p_got.normal.is_equal_approx(Vector3(0.0f, 1.0f, 0.0f)));
	CHECK(p_got.brush_axes.is_equal_approx(Vector2(1.0f, 1.0f)));
	CHECK_EQ(p_got.render_meta, 0u);
	return true;
}
static bool check_pair(const Gaussian &p_a, const Gaussian &p_b) {
	if (!check_leaf_values(p_a) || !check_leaf_values(p_b)) {
		return false;
	}
	CHECK_NE(p_a.painterly_meta, p_b.painterly_meta);
	return true;
}
static bool check_loaded_world(const Ref<GaussianSplatWorld> &p_world, bool p_hlod, bool p_staged) {
	if (p_world.is_null() || p_world->get_splat_count() != 2) {
		FAIL("production loader must publish two authentic leaves");
		return false;
	}
	CHECK_EQ(p_world->has_hlod_tree(), p_hlod);
	if (!p_staged) {
		CHECK_FALSE(p_world->has_chunk_payload_source());
		Ref<GaussianData> data = p_world->get_gaussian_data();
		if (data.is_null() || data->get_count() != 2) {
			FAIL("resident legacy payload must decode both leaves");
			return false;
		}
		return check_pair(data->get_gaussian(0), data->get_gaussian(1));
	}
	CHECK(p_world->get_gaussian_data().is_null());
	Ref<StagedFileChunkPayloadSource> source = p_world->get_chunk_payload_source();
	if (source.is_null() || !source->is_valid()) {
		FAIL("production legacy loader must publish a valid staged source");
		return false;
	}
	LocalVector<Gaussian> capture;
	LocalVector<Vector3> sh;
	uint32_t first = 0u, high = 0u;
	for (int repeat = 0; repeat < 2; repeat++) {
		if (!source->capture_chunk_snapshot(0u, 2u, capture, sh, first, high) || capture.size() != 2u) {
			FAIL("legacy contiguous capture must decode both leaves");
			return false;
		}
		if (!check_pair(capture[0], capture[1])) {
			return false;
		}
		CHECK_EQ(source->get_file_open_count(), 1u);
	}
	if (capture.size() != 2u) {
		FAIL("both cached contiguous controls must retain two decoded leaves");
		return false;
	}
	const uint32_t original_first = capture[0].painterly_meta;
	const uint32_t original_second = capture[1].painterly_meta;
	const uint32_t reverse[] = {1u, 0u};
	if (!source->capture_indexed_chunk_snapshot(reverse, 2u, capture, sh, first, high) || capture.size() != 2u) {
		FAIL("legacy indexed capture must decode both leaves");
		return false;
	}
	CHECK_EQ(source->get_file_open_count(), 1u);
	CHECK_EQ(capture[0].painterly_meta, original_second);
	CHECK_EQ(capture[1].painterly_meta, original_first);
	return check_pair(capture[0], capture[1]);
}
} // namespace TestAuthenticLegacyWorld

TEST_CASE("[GaussianSplatting][WorldIO] released plain v1 and HLOD v2 producer files load and import") {
	using namespace TestGaussianLayoutFingerprint;
	using namespace TestAuthenticLegacyWorld;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (bool hlod : {false, true}) {
		for (int mode = 0; mode < 3; mode++) {
			const String prefix = hlod ? "hlod_v2" : "plain_v1";
			const String suffix = mode == 0 ? "streamable" : mode == 1 ? "resident" : "compressed";
			const String path = String("tests/fixtures/legacy_world_producer/").path_join(prefix + "_" + suffix + ".gsplatworld");
			if (!FileAccess::exists(path)) {
				FAIL("required captured legacy fixture missing: ", path.utf8().get_data());
				return;
			}
			const PackedByteArray source_bytes = FileAccess::get_file_as_bytes(path);
			CHECK_EQ(read_u32_at(path, 4u), hlod ? 2u : 1u);
			CHECK_EQ(read_u64_at(path, 56u), hlod ? 184u : 104u);
			const uint32_t flags = read_u32_at(path, 8u);
			CHECK_EQ((flags & (1u << 6u)) != 0u, hlod);
			CHECK_EQ((flags & (1u << 5u)) != 0u, mode != 0);
			CHECK_EQ((flags & (1u << 4u)) != 0u, mode == 2);
			Error err = ERR_BUG;
			Ref<GaussianSplatWorld> streamed = loader.load(path, "", &err);
			if (err != OK || !check_loaded_world(streamed, hlod, mode == 0)) {
				FAIL("production streaming load failed");
				return;
			}
			Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
			if (err != OK || !check_loaded_world(resident, hlod, false)) {
				FAIL("production resident load failed");
				return;
			}
#ifdef TOOLS_ENABLED
			Ref<ResourceImporterGSplatWorld> importer;
			importer.instantiate();
			CHECK_EQ(importer->get_format_version(), 6);
			const String destination = fixture_path("authentic_legacy_import", "");
			HashMap<StringName, Variant> options;
			Variant metadata;
			if (importer->import(ResourceUID::INVALID_ID, path, destination, options, nullptr, nullptr, &metadata) != OK) {
				FAIL("actual importer must consume the authentic legacy producer file");
				return;
			}
			const String output = destination + ".gsplatworld";
			CHECK(FileAccess::get_file_as_bytes(path) == source_bytes);
			// Both authentic plain v1 and supported rule-1 HLOD v2 producers now
			// migrate to current-rule v3; the source and all payload modes stay intact.
			CHECK_EQ(String(Dictionary(metadata)["hlod_skip_reason"]), String());
			CHECK_EQ(uint32_t(Dictionary(metadata)["source_hlod_rule"]), hlod ? 1u : 0u);
			CHECK_EQ(read_u32_at(output, 4u), 3u);
			CHECK_EQ(read_u32_at(output, 104u), GAUSSIAN_STRUCT_LAYOUT_VERSION);
			CHECK_EQ(read_u64_at(output, 56u), 192u);
			Ref<GaussianSplatWorld> imported = loader.load_resident(output, &err);
			if (err != OK || !check_loaded_world(imported, true, false)) {
				FAIL("actual imported output must retain the two authentic leaves");
				return;
			}
			CHECK_EQ(imported->get_hlod_tree().bake_rule_version, 2u);
			imported.unref();
			remove_fixture(output);
#endif
		}
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][MalformedCorpus] captured legacy world identities and extents reject before replacing an import") {
	using namespace TestGaussianLayoutFingerprint;
	for (bool hlod : {false, true}) {
		const String source = String("tests/fixtures/legacy_world_producer/").path_join(hlod ? "hlod_v2_streamable.gsplatworld" : "plain_v1_streamable.gsplatworld");
		if (!FileAccess::exists(source)) {
			FAIL("required actual producer fixture missing");
			return;
		}
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(source);
		const uint64_t header = hlod ? 184u : 104u;
		if (uint64_t(bytes.size()) <= header) {
			FAIL("the positive authentic producer capture must contain payload bytes");
			return;
		}
#ifdef TOOLS_ENABLED
		Ref<ResourceImporterGSplatWorld> importer;
		importer.instantiate();
		HashMap<StringName, Variant> options;
		const String destination = fixture_path("legacy_prior_import", "");
		if (importer->import(ResourceUID::INVALID_ID, source, destination, options, nullptr, nullptr, nullptr) != OK) {
			FAIL("establish prior good import from actual producer capture");
			return;
		}
		const PackedByteArray previous = FileAccess::get_file_as_bytes(destination + ".gsplatworld");
#endif
		struct Mutation {
			uint64_t offset;
			uint64_t value;
			bool wide;
			Error expected;
		};
		const uint32_t flags = read_u32_at(source, 8u);
		const Mutation controls[] = {
			{4u, 4u, false, ERR_FILE_CORRUPT},
			{8u, hlod ? flags & ~(1u << 6u) : flags | (1u << 6u), false, hlod ? ERR_FILE_UNRECOGNIZED : ERR_FILE_CORRUPT},
			{56u, header - 1u, true, ERR_FILE_CORRUPT},
			{56u, uint64_t(bytes.size()) + 1u, true, ERR_FILE_CORRUPT},
		};
		const String malformed = fixture_path("legacy_mutated", ".gsplatworld");
		for (int control = 0; control < 6; control++) {
			if (control == 5 && !hlod) {
				continue; // only HLOD has the explicit reserved extension word
			}
			Error expected = control < 4 ? controls[control].expected : ERR_FILE_CORRUPT;
			{
				Ref<FileAccess> file = FileAccess::open(malformed, FileAccess::WRITE);
				const uint64_t count = control == 4 ? header - 1u : uint64_t(bytes.size());
				if (file.is_null() || !file->store_buffer(bytes.ptr(), count)) {
					FAIL("write separately labeled mutation of authentic producer bytes");
					return;
				}
				if (control < 4) {
					file->seek(controls[control].offset);
					if (controls[control].wide) {
						file->store_64(controls[control].value);
					} else {
						file->store_32(uint32_t(controls[control].value));
					}
				} else if (control == 5) {
					file->seek(172u); // actual legacy HLOD extension reserved0
					file->store_32(1u);
				}
			}
			check_world_refused(malformed, expected);
#ifdef TOOLS_ENABLED
			CHECK_EQ(importer->import(ResourceUID::INVALID_ID, malformed, destination, options, nullptr, nullptr, nullptr), expected);
			CHECK(FileAccess::get_file_as_bytes(destination + ".gsplatworld") == previous);
#endif
		}
		remove_fixture(malformed);
#ifdef TOOLS_ENABLED
		remove_fixture(destination + ".gsplatworld");
#endif
	}
}
