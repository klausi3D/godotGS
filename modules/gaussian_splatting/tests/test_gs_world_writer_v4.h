#pragma once

// BV-18a only: spill, BFS assembly, deletion and W1. No old-reader refusal
// test applies: there is no new format identity here. The V4 codec field and
// its old-reader refusal are BV-C4; W13 and the full refusal matrix are BV-18b.
// These tests deliberately fail against the ERR_UNAVAILABLE scaffolding.
// No REQUIRE preconditions: this build does not abort on REQUIRE failure.

#include "test_macros.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gs_world_writer_v4.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include <cstring>
#include <utility>

namespace TestGSWorldWriterV4 {

using namespace gs_world_writer;
constexpr uint32_t kNodes = 7;
constexpr uint32_t kSplats = 256;
constexpr uint32_t kHigh = 12;
constexpr uint32_t kAppendOrder[kNodes] = { 5, 1, 4, 0, 6, 2, 3 };

static bool list_dir(const String &p_path, PackedStringArray &r_names) {
	r_names.clear();
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		FAIL("cannot inspect directory: ", p_path.utf8().get_data());
		return false;
	}
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name != "." && name != "..") {
			r_names.push_back(name);
		}
	}
	dir->list_dir_end();
	r_names.sort();
	return true;
}

struct Paths {
	String dir;
	String scratch;
	String final_path;
	bool create() {
		static uint32_t serial = 0;
		dir = OS::get_singleton()->get_temp_path().path_join(
				"gs_ww_v4_" + itos(OS::get_singleton()->get_ticks_usec()) + "_" + itos(serial++));
		scratch = dir.path_join("scratch");
		final_path = dir.path_join("world.gsplatworld");
		if (DirAccess::make_dir_recursive_absolute(scratch) != OK) {
			FAIL("cannot create isolated V4 test directory");
			return false;
		}
		return true;
	}
	~Paths() {
		if (dir.is_empty() || !DirAccess::dir_exists_absolute(dir)) {
			return;
		}
		// Cleanup is after assertions and after WriterV4 destruction. Never hides
		// a leak from the W1 checks; remove only files in this test's fresh dirs.
		PackedStringArray names;
		if (list_dir(scratch, names)) {
			for (const String &name : names) {
				DirAccess::remove_absolute(scratch.path_join(name));
			}
		}
		DirAccess::remove_absolute(scratch);
		if (list_dir(dir, names)) {
			for (const String &name : names) {
				DirAccess::remove_absolute(dir.path_join(name));
			}
		}
		DirAccess::remove_absolute(dir);
	}
};

static bool read_bytes(const String &p_path, PackedByteArray &r_bytes) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || file->get_length() == 0) {
		FAIL("expected a readable nonempty file: ", p_path.utf8().get_data());
		return false;
	}
	r_bytes.resize(file->get_length());
	if (file->get_buffer(r_bytes.ptrw(), r_bytes.size()) != uint64_t(r_bytes.size())) {
		FAIL("short fixture read");
		return false;
	}
	return true;
}

static bool seed_destination(const Paths &p_paths, bool p_existing, PackedByteArray &r_original) {
	r_original.clear();
	if (!p_existing) {
		return true;
	}
	Ref<FileAccess> file = FileAccess::open(p_paths.final_path, FileAccess::WRITE);
	if (file.is_null()) {
		FAIL("cannot seed pre-existing destination");
		return false;
	}
	const uint8_t sentinel[] = { 0x47, 0x53, 0, 0xFF, 3, 2, 1, 0x80 };
	file->store_buffer(sentinel, sizeof(sentinel));
	if (file->get_error() != OK) {
		FAIL("cannot write pre-existing destination");
		return false;
	}
	file.unref();
	return read_bytes(p_paths.final_path, r_original);
}

static void check_destination(const Paths &p_paths, const PackedByteArray &p_original) {
	if (p_original.is_empty()) {
		CHECK_FALSE(FileAccess::exists(p_paths.final_path));
	} else {
		PackedByteArray actual;
		if (!read_bytes(p_paths.final_path, actual)) {
			return;
		}
		CHECK(actual == p_original);
	}
}

static void check_w1(const Paths &p_paths, const PackedByteArray &p_original) {
	check_destination(p_paths, p_original);
	PackedStringArray names;
	if (!list_dir(p_paths.scratch, names)) {
		return;
	}
	CHECK(names.is_empty());
	if (!list_dir(p_paths.dir, names)) {
		return;
	}
	// This checks output-temp leaks too, not merely the scratch directory.
	CHECK(names.size() == (p_original.is_empty() ? 1 : 2));
	for (const String &name : names) {
		CHECK((name == "scratch" || (!p_original.is_empty() && name == "world.gsplatworld")));
	}
}

struct Body {
	V4BodyDesc desc;
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh;
};

struct Fixture {
	Ref<GaussianSplatWorld> world;
	PagedV4Spec spec;
	Body bodies[kNodes];
	LocalVector<V4Node> nodes;

	bool build() {
		// Constructed synthetic topology, not an imitation of serialized bytes.
		// BFS: root 0 -> {1,2}; 1 -> {3,4}; 2 -> {5,6}. Five segments:
		// stitch (99,0), tile (10,1)/(20,1), tile (10,2)/(20,2).
		GaussianSplatHlodTree tree;
		tree.nodes.resize(kNodes);
		tree.leaf_splat_count = 4 * kSplats;
		tree.interior_splat_count = 3 * kSplats;
		tree.sh_high_order_count = kHigh;
		tree.interior_resident = true;
		tree.interior_gaussians.resize(3 * kSplats);
		tree.interior_sh_high_order.resize(3 * kSplats * kHigh);
		LocalVector<Gaussian> leaves;
		LocalVector<Vector3> leaf_sh;
		leaves.resize(4 * kSplats);
		leaf_sh.resize(4 * kSplats * kHigh);
		nodes.resize(kNodes);
		for (uint32_t i = 0; i < kNodes; ++i) {
			GaussianSplatHlodNode &node = tree.nodes[i];
			const uint32_t depth = i == 0 ? 0 : (i < 3 ? 1 : 2);
			const bool leaf = depth == 2;
			node.cell.e = 4 - int32_t(depth);
			node.cell.ix = i == 0 ? 0 : (i < 3 ? i - 1 : i - 3);
			node.parent = i == 0 ? gs_hlod::kNoParent : (i < 3 ? 0 : (i < 5 ? 1 : 2));
			node.first_child = leaf ? 0 : 2 * i + 1;
			node.child_count = leaf ? 0 : 2;
			node.height = 2 - depth;
			node.flags = leaf ? gs_hlod::NODE_KIND_LEAF : gs_hlod::NODE_KIND_MERGED;
			node.geometric_error = leaf ? 0.0f : float(2 - depth);
			const float half = float(8 >> depth);
			node.aabb_min = Vector3(-half, -half, -half);
			node.aabb_max = Vector3(half, half, half);
			node.radius = depth == 0 ? 40.0f : (depth == 1 ? 16.0f : 4.0f);
			node.payload_count = kSplats;
			node.payload_first = (leaf ? i - 3 : i + 4) * kSplats;
			Body &body = bodies[i];
			body.desc.segment.bucket = i == 0 ? 99 : ((i == 1 || i == 3 || i == 4) ? 10 : 20);
			body.desc.segment.depth = depth;
			body.desc.is_leaf = leaf;
			tree.node_cell_center(node, body.desc.cell_center);
			body.gaussians.resize(kSplats);
			body.sh.resize(kSplats * kHigh);
			for (uint32_t j = 0; j < kSplats; ++j) {
				Gaussian g;
				// Dyadic endpoints retain exact leaf-relative round trips even after GSQ.
				g.position = Vector3(j % 2 ? 0.125f : -0.125f, 0.0f, 0.0f);
				if (leaf) {
					for (int a = 0; a < 3; ++a) {
						g.position[a] += real_t(body.desc.cell_center[a]);
					}
				}
				g.scale = Vector3(0.03125f, 0.043f + float(j % 3) * 0.003f, 0.071f);
				g.opacity = 0.6f;
				g.sh_dc = Color(0.25f + float(i) * 0.03125f, 0.5f, 0.75f, 1.0f);
				g.sh_1[0] = Vector3(0.13f, -0.07f, 0.01f);
				g.render_meta = i % 2 ? GAUSSIAN_DC_ENCODING_LINEAR_RGB : GAUSSIAN_DC_ENCODING_LEGACY_BIAS;
				body.gaussians[j] = g;
				const uint32_t slot = (leaf ? i - 3 : i) * kSplats + j;
				if (leaf) {
					leaves[slot] = g;
				} else {
					tree.interior_gaussians[slot] = g;
				}
				for (uint32_t k = 0; k < kHigh; ++k) {
					const Vector3 sh(float(k + 1) * 0.013f, -0.027f, float(j % 3) * 0.019f);
					body.sh[j * kHigh + k] = sh;
					if (leaf) {
						leaf_sh[slot * kHigh + k] = sh;
					} else {
						tree.interior_sh_high_order[slot * kHigh + k] = sh;
					}
				}
			}
			nodes[i].topology = node;
		}
		String reason;
		if (!gs_hlod_validate_tree(tree, &reason)) {
			FAIL("invalid three-level fixture: ", reason.utf8().get_data());
			return false;
		}
		Ref<GaussianData> data;
		data.instantiate();
		data->set_gaussian_payload(leaves, leaf_sh, 3, kHigh, false, false);
		world.instantiate();
		world->set_gaussian_data(data);
		world->set_hlod_tree(std::move(tree));
		Dictionary metadata;
		metadata["source"] = "BV-18a three-level fixture";
		metadata["revision"] = 7;
		world->set_metadata(metadata);
		spec.node_count = kNodes;
		spec.leaf_splat_count = 4 * kSplats;
		spec.interior_splat_count = 3 * kSplats;
		spec.sh_degree = 3;
		spec.sh_first_order = world->get_sh_first_order_count();
		spec.sh_high_order = kHigh;
		spec.bounds = world->get_bounds();
		spec.metadata = metadata;
		return true;
	}
};

static bool begin(WriterV4 &p_writer, const Paths &p_paths, const Fixture &p_fixture,
		const CodecParams &p_codec = CodecParams(), const Limits &p_limits = Limits::unbounded()) {
	if (p_writer.begin_paged(p_paths.final_path, p_paths.scratch, p_fixture.spec, p_codec, p_limits) != OK) {
		FAIL("begin_paged failed: ", p_writer.get_report().reason.utf8().get_data());
		return false;
	}
	return true;
}

static bool append_one(WriterV4 &p_writer, Fixture &p_fixture, uint32_t p_node) {
	if (p_node >= kNodes || p_fixture.nodes.size() != kNodes) {
		FAIL("invalid fixture node index");
		return false;
	}
	const Body &body = p_fixture.bodies[p_node];
	if (p_writer.append_node(body.desc, body.gaussians.ptr(), body.sh.ptr(), kSplats, p_fixture.nodes[p_node].body) != OK) {
		FAIL("append_node failed: ", p_writer.get_report().reason.utf8().get_data());
		return false;
	}
	return true;
}

static bool append_all(WriterV4 &p_writer, Fixture &p_fixture, const Paths &p_paths, const PackedByteArray &p_original = PackedByteArray()) {
	for (uint32_t i : kAppendOrder) {
		if (!append_one(p_writer, p_fixture, i)) {
			return false;
		}
		check_destination(p_paths, p_original);
	}
	return true;
}

static bool open_source(const Paths &p_paths, GaussianSplatHlodPagedSource &r_source) {
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	String reason;
	if (r_source.open(p_paths.final_path, tree, metadata, reason) != OK) {
		FAIL("V4 output does not open: ", reason.utf8().get_data());
		return false;
	}
	if (r_source.get_nodes().size() != kNodes || tree.nodes.size() != kNodes) {
		FAIL("output must contain all seven nodes");
		return false;
	}
	CHECK(tree.nodes[0].height == 2);
	CHECK(metadata["source"] == Variant("BV-18a three-level fixture"));
	return true;
}

static void check_metrics(const gs_world_paged::NodeMetrics &p_actual, const gs_world_paged::NodeMetrics &p_expected) {
	CHECK(p_actual.footprint == p_expected.footprint);
	CHECK(p_actual.dc_encoding == p_expected.dc_encoding);
	CHECK(p_actual.homogeneous_dc == p_expected.homogeneous_dc);
	CHECK(p_actual.center_exact == p_expected.center_exact);
}

static void body_digest(const Gaussian *p_gaussians, const Vector3 *p_sh, uint8_t r_digest[32]) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(reinterpret_cast<const uint8_t *>(p_gaussians), kSplats * sizeof(Gaussian));
	hash.update(reinterpret_cast<const uint8_t *>(p_sh), kSplats * kHigh * sizeof(Vector3));
	hash.finish(r_digest);
}

static void check_output_generation(const Paths &p_paths, const GaussianSplatHlodPagedSource &p_source) {
	PackedByteArray bytes;
	if (!read_bytes(p_paths.final_path, bytes)) {
		return;
	}
	if (uint64_t(bytes.size()) < gs_world_paged::kHeaderBytes || p_source.get_nodes().size() != kNodes) {
		FAIL("generation oracle needs a complete V4 header and node table");
		return;
	}
	// V4 identity header = final header with generation (offset 168) and index
	// digest (176) zeroed. Metadata digest remains part of the identity. Hash the
	// actual UNCOMPRESSED on-disk bodies, not spill/page digests or record headers.
	uint8_t identity[gs_world_paged::kHeaderBytes];
	std::memcpy(identity, bytes.ptr(), sizeof(identity));
	std::memset(identity + 168, 0, 8 + 32);
	CryptoCore::SHA256Context generation;
	generation.start();
	generation.update(identity, sizeof(identity) - 32);
	for (const GaussianSplatHlodPagedSource::NodeRecord &node : p_source.get_nodes()) {
		if (node.stored_bytes < gs_world_paged::kPayloadRecordHeaderBytes || node.offset > uint64_t(bytes.size()) ||
				node.stored_bytes > uint64_t(bytes.size()) - node.offset) {
			FAIL("generation oracle refuses an out-of-file body extent");
			return;
		}
		uint8_t digest[32];
		CryptoCore::SHA256Context body;
		body.start();
		body.update(bytes.ptr() + node.offset + gs_world_paged::kPayloadRecordHeaderBytes,
				node.stored_bytes - gs_world_paged::kPayloadRecordHeaderBytes);
		body.finish(digest);
		generation.update(digest, sizeof(digest));
	}
	uint8_t digest[32];
	generation.finish(digest);
	uint64_t expected = 0;
	std::memcpy(&expected, digest, sizeof(expected));
	CHECK(p_source.get_info().content_generation == expected);
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] codec none shuffled spill is byte-identical to the resident V4 writer") {
	Paths paths;
	Fixture fixture;
	if (!paths.create() || !fixture.build()) {
		return;
	}
	const String reference_path = paths.dir.path_join("reference.gsplatworld");
	String reason;
	if (fixture.world.is_null()) {
		FAIL("missing resident reference world");
		return;
	}
	if (GaussianSplatHlodPagedSource::write_package(*fixture.world.ptr(), reference_path, reason) != OK) {
		FAIL("reference producer failed: ", reason.utf8().get_data());
		return;
	}
	WriterV4 writer;
	if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths)) {
		return;
	}
	if (writer.finalize(fixture.nodes) != OK) {
		FAIL("lossless assembly failed: ", writer.get_report().reason.utf8().get_data());
		return;
	}
	GaussianSplatHlodPagedSource source;
	if (!open_source(paths, source)) {
		return;
	}
	PackedByteArray reference, actual;
	if (!read_bytes(reference_path, reference) || !read_bytes(paths.final_path, actual)) {
		return;
	}
	// Includes header, offsets, body/record/index digests and content_generation.
	CHECK(actual == reference);
	CHECK(writer.get_report().committed);
	CHECK(writer.get_report().error == OK);
	CHECK(writer.get_report().reason.is_empty());
	CHECK(writer.get_report().nodes == kNodes);
	CHECK(writer.get_report().output_bytes == uint64_t(actual.size()));
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] GSQ spill metrics and every output payload describe decoded runtime values") {
	Paths paths;
	Fixture fixture;
	if (!paths.create() || !fixture.build()) {
		return;
	}
	WriterV4 writer;
	const CodecParams codec = gsq_v1_codec_params();
	if (!begin(writer, paths, fixture, codec) || !append_all(writer, fixture, paths)) {
		return;
	}
	V4SpillRecord spills[kNodes];
	LocalVector<Gaussian> decoded[kNodes];
	LocalVector<Vector3> decoded_sh[kNodes];
	gs_gsq::EncodeParams params;
	String reason;
	if (gsq_encode_params(codec, params, reason) != OK || fixture.nodes.size() != kNodes) {
		FAIL("GSQ oracle parameters or fixture table invalid");
		return;
	}
	gs_gsq::DecodeContext decoder;
	bool changed_footprint = false;
	for (uint32_t i = 0; i < kNodes; ++i) {
		const Body &body = fixture.bodies[i];
		LocalVector<uint8_t> page;
		if (gs_gsq::encode_page(body.gaussians.ptr(), body.sh.ptr(), kSplats, 3, params, page, reason) != OK) {
			FAIL("independent GSQ oracle encode failed: ", reason.utf8().get_data());
			return;
		}
		decoded[i].resize(kSplats);
		decoded_sh[i].resize(kSplats * kHigh);
		if (decoder.decode_range(page.ptr(), page.size(), 0, kSplats, decoded[i].ptr(), decoded_sh[i].ptr(), reason) != OK ||
				writer.get_spill_record(fixture.nodes[i].body, spills[i]) != OK) {
			FAIL("GSQ decode or spill-record lookup failed");
			return;
		}
		gs_world_paged::NodeMetrics expected, original;
		if (!gs_world_paged::compute_node_metrics(decoded[i].ptr(), kSplats, body.desc.cell_center, body.desc.is_leaf, expected, reason) ||
				!gs_world_paged::compute_node_metrics(body.gaussians.ptr(), kSplats, body.desc.cell_center, body.desc.is_leaf, original, reason)) {
			FAIL("fixture metrics failed");
			return;
		}
		changed_footprint |= expected.footprint != original.footprint;
		check_metrics(spills[i].metrics, expected);
		CHECK(spills[i].desc.segment == body.desc.segment);
		CHECK(spills[i].desc.is_leaf == body.desc.is_leaf);
		for (int a = 0; a < 3; ++a) {
			CHECK(spills[i].desc.cell_center[a] == body.desc.cell_center[a]);
		}
		CHECK(spills[i].splat_count == kSplats);
		CHECK(spills[i].stored_bytes == page.size());
		uint8_t stored_digest[32];
		CryptoCore::SHA256Context hash;
		hash.start();
		hash.update(page.ptr(), page.size());
		hash.finish(stored_digest);
		CHECK(std::memcmp(spills[i].stored_digest, stored_digest, 32) == 0);
		LocalVector<Gaussian> relative = decoded[i];
		if (body.desc.is_leaf) {
			for (Gaussian &g : relative) {
				for (int a = 0; a < 3; ++a) {
					g.position[a] = real_t(double(g.position[a]) - body.desc.cell_center[a]);
				}
			}
		}
		uint8_t output_digest[32];
		body_digest(relative.ptr(), decoded_sh[i].ptr(), output_digest);
		CHECK(std::memcmp(spills[i].output_body_digest, output_digest, 32) == 0);
	}
	// Prevent an exact/constant fixture from making pre-encode metrics look correct.
	CHECK(changed_footprint);
	const uint64_t raw_bytes = uint64_t(kNodes) * kSplats * (sizeof(Gaussian) + kHigh * sizeof(Vector3));
	CHECK(writer.get_report().total_scratch_bytes > 0);
	CHECK(writer.get_report().total_scratch_bytes < raw_bytes);
	if (writer.finalize(fixture.nodes) != OK) {
		FAIL("GSQ-spill assembly failed: ", writer.get_report().reason.utf8().get_data());
		return;
	}
	GaussianSplatHlodPagedSource source;
	if (!open_source(paths, source)) {
		return;
	}
	check_output_generation(paths, source);
	for (uint32_t i = 0; i < kNodes; ++i) {
		Vector<Gaussian> actual;
		Vector<Vector3> actual_sh;
		if (source.read_node_payload(i, actual, actual_sh, reason) != OK || actual.size() != kSplats || actual_sh.size() != kSplats * kHigh) {
			FAIL("missing or malformed decoded output node: ", i);
			return;
		}
		// Canonical digests compare every runtime field and SH coefficient bitwise,
		// excluding only authoring padding; no tolerance can hide lossy differences.
		uint8_t actual_digest[32], expected_digest[32];
		gs_gsq::decoded_digest(actual.ptr(), actual_sh.ptr(), kSplats, 3, actual_digest);
		gs_gsq::decoded_digest(decoded[i].ptr(), decoded_sh[i].ptr(), kSplats, 3, expected_digest);
		CHECK(std::memcmp(actual_digest, expected_digest, 32) == 0);
		check_metrics(source.get_nodes()[i].metrics, spills[i].metrics);
	}
	PackedStringArray names;
	if (!list_dir(paths.scratch, names)) {
		return;
	}
	CHECK(names.is_empty());
	CHECK(writer.get_report().committed);
	CHECK(writer.get_report().error == OK);
	CHECK(writer.get_report().reason.is_empty());
}

struct AssemblyProbe {
	WriterV4 *writer = nullptr;
	const Paths *paths = nullptr;
	PackedByteArray original;
	V4SpillRecord spills[kNodes];
	LocalVector<V4SegmentReport> segments;
	uint32_t calls = 0;
	uint64_t observed_peak = 0;
	uint64_t previous_output_bytes = 0;
	bool cancel_after_first = false;
	bool saw_early_deletion = false;

	bool prepare(WriterV4 &p_writer, const Paths &p_paths, const Fixture &p_fixture, const PackedByteArray &p_original = PackedByteArray()) {
		writer = &p_writer;
		paths = &p_paths;
		original = p_original;
		if (p_fixture.nodes.size() != kNodes) {
			FAIL("probe requires the complete node table");
			return false;
		}
		segments = writer->get_report().segments;
		if (segments.size() != 5) {
			FAIL("expected exactly one file for each of the five (bucket,depth) segments");
			return false;
		}
		for (uint32_t i = 0; i < kNodes; ++i) {
			if (writer->get_spill_record(p_fixture.nodes[i].body, spills[i]) != OK) {
				FAIL("probe could not inspect spill record");
				return false;
			}
		}
		uint32_t total_bodies = 0;
		for (uint32_t s = 0; s < segments.size(); ++s) {
			const V4SegmentReport &segment = segments[s];
			CHECK(segment.spill_bytes > 0);
			CHECK(FileAccess::exists(segment.path));
			CHECK(segment.path.get_base_dir() == paths->scratch);
			for (uint32_t t = 0; t < s; ++t) {
				CHECK_FALSE(segment.key == segments[t].key);
				CHECK(segment.path != segments[t].path);
			}
			uint32_t bodies = 0;
			for (const V4SpillRecord &spill : spills) {
				if (spill.desc.segment == segment.key) {
					++bodies;
					CHECK(spill.segment_path == segment.path);
					CHECK(spill.offset <= segment.spill_bytes);
					CHECK(spill.stored_bytes <= segment.spill_bytes);
					CHECK(spill.offset + spill.stored_bytes <= segment.spill_bytes);
				}
			}
			CHECK(bodies == segment.body_count);
			CHECK(bodies > 0);
			total_bodies += bodies;
			observed_peak += segment.spill_bytes;
		}
		CHECK(total_bodies == kNodes);
		CHECK(observed_peak == writer->get_report().total_scratch_bytes);
		PackedStringArray names;
		if (!list_dir(paths->scratch, names)) {
			return false;
		}
		CHECK(names.size() == 5);
		writer->set_assembly_observer(observe, this);
		return true;
	}

	static void observe(const WriterV4::AssemblyEvent &p_event, void *p_userdata) {
		if (!p_userdata) {
			FAIL("missing observer context");
			return;
		}
		AssemblyProbe &probe = *static_cast<AssemblyProbe *>(p_userdata);
		if (!probe.writer || !probe.paths || p_event.node_index >= kNodes) {
			FAIL("invalid assembly observer event");
			return;
		}
		CHECK(p_event.node_index == probe.calls);
		++probe.calls;
		CHECK(FileAccess::exists(p_event.output_temp_path));
		CHECK(p_event.output_temp_path != probe.paths->final_path);
		CHECK(p_event.output_temp_path.get_base_dir() == probe.paths->dir);
		CHECK(p_event.output_bytes > probe.previous_output_bytes);
		probe.previous_output_bytes = p_event.output_bytes;
		check_destination(*probe.paths, probe.original);
		uint64_t live_bytes = p_event.output_bytes;
		PackedStringArray expected_names;
		uint32_t deleted = 0;
		for (const V4SegmentReport &segment : probe.segments) {
			uint32_t last = 0;
			bool found = false;
			for (uint32_t i = 0; i < kNodes; ++i) {
				if (probe.spills[i].desc.segment == segment.key) {
					last = i;
					found = true;
				}
			}
			if (!found) {
				FAIL("segment has no body in canonical BFS table");
				return;
			}
			const bool should_exist = p_event.node_index < last;
			// Checks both early deletion (multi-body segment) and late deletion.
			CHECK(FileAccess::exists(segment.path) == should_exist);
			if (should_exist) {
				expected_names.push_back(segment.path.get_file());
				live_bytes += segment.spill_bytes;
			} else {
				++deleted;
			}
		}
		expected_names.sort();
		PackedStringArray actual_names;
		if (!list_dir(probe.paths->scratch, actual_names)) {
			return;
		}
		CHECK(actual_names == expected_names);
		CHECK(probe.writer->get_report().segments_deleted == deleted);
		if (deleted > 0 && !expected_names.is_empty() && p_event.node_index + 1 < kNodes) {
			probe.saw_early_deletion = true;
		}
		probe.observed_peak = MAX(probe.observed_peak, live_bytes);
		if (probe.cancel_after_first && probe.calls == 1) {
			// Real assembly has copied a body; remaining segments still exist.
			CHECK(p_event.output_bytes > 0);
			CHECK_FALSE(expected_names.is_empty());
			probe.writer->request_cancel();
		}
	}
};

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] assembly deletes each segment immediately at its last BFS body and bounds peak disk") {
	// Lossless mode is the worst scratch size and makes the disk bound meaningful.
	Paths paths;
	Fixture fixture;
	if (!paths.create() || !fixture.build()) {
		return;
	}
	AssemblyProbe probe; // Outlives the writer and its callback registration.
	WriterV4 writer;
	if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths) || !probe.prepare(writer, paths, fixture)) {
		return;
	}
	if (writer.finalize(fixture.nodes) != OK) {
		FAIL("progressive assembly failed: ", writer.get_report().reason.utf8().get_data());
		return;
	}
	CHECK(probe.calls == kNodes); // An unwired observer cannot pass.
	CHECK(probe.saw_early_deletion);
	const V4Report &report = writer.get_report();
	CHECK(report.segments_deleted == 5);
	CHECK(report.nodes == kNodes);
	CHECK(report.committed);
	CHECK(report.error == OK);
	CHECK(report.reason.is_empty());
	uint64_t largest_segment = 0;
	for (const V4SegmentReport &segment : probe.segments) {
		largest_segment = MAX(largest_segment, segment.spill_bytes);
	}
	PackedByteArray output;
	if (!read_bytes(paths.final_path, output)) {
		return;
	}
	CHECK(report.output_bytes == uint64_t(output.size()));
	CHECK(report.peak_scratch_plus_output_bytes >= probe.observed_peak);
	CHECK(report.peak_scratch_plus_output_bytes >= report.output_bytes);
	// One 4 KiB page of slack for final record alignment/framing. Logical file
	// lengths only; filesystem allocation units and the pre-existing target do
	// not belong to the writer's scratch+output accounting.
	constexpr uint64_t slack = gs_world_paged::kPageBytes;
	CHECK(report.peak_scratch_plus_output_bytes <= report.output_bytes + largest_segment + slack);
	PackedStringArray names;
	if (!list_dir(paths.scratch, names)) {
		return;
	}
	CHECK(names.is_empty());
	if (!list_dir(paths.dir, names)) {
		return;
	}
	CHECK(names.size() == 2); // scratch directory + final; no output temp.
	CHECK(writer.get_progress().phase == Phase::COMMITTED);
}

enum class Refusal {
	UNUSED,
	REUSED,
	DEPTH,
	CENTER,
	LEAF,
	CHILD_RANGE,
	PARENT_ORDER,
};

static void refuse(Refusal p_case, const String &p_reason) {
	for (bool existing : { false, true }) {
		Paths paths;
		Fixture fixture;
		PackedByteArray original;
		if (!paths.create() || !fixture.build() || !seed_destination(paths, existing, original)) {
			return;
		}
		if (fixture.nodes.size() != kNodes) {
			FAIL("refusal fixture requires seven nodes");
			return;
		}
		// Alter only one spill fact; the supplied topology remains valid.
		if (p_case == Refusal::DEPTH) {
			++fixture.bodies[3].desc.segment.depth;
		} else if (p_case == Refusal::CENTER) {
			fixture.bodies[3].desc.cell_center[0] += 0.25;
		} else if (p_case == Refusal::LEAF) {
			fixture.bodies[3].desc.is_leaf = false;
		}
		WriterV4 writer;
		if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths, original)) {
			return;
		}
		if (p_case == Refusal::UNUSED) {
			const Body &body = fixture.bodies[3];
			V4BodyHandle unused;
			if (writer.append_node(body.desc, body.gaussians.ptr(), body.sh.ptr(), kSplats, unused) != OK) {
				FAIL("unused-handle setup must spill the extra body successfully");
				return;
			}
		} else if (p_case == Refusal::REUSED) {
			fixture.nodes[4].body = fixture.nodes[3].body;
		} else if (p_case == Refusal::CHILD_RANGE) {
			fixture.nodes[1].topology.first_child = 4; // Skips child 3, claims child 5 of node 2.
		} else if (p_case == Refusal::PARENT_ORDER) {
			fixture.nodes[3].topology.parent = 5; // Parent follows its child.
		}
		CHECK(writer.finalize(fixture.nodes) == ERR_INVALID_DATA);
		CHECK(writer.get_report().error == ERR_INVALID_DATA);
		CHECK(writer.get_report().reason == p_reason);
		CHECK_FALSE(writer.get_report().committed);
		check_w1(paths, original); // Before destruction, so failures must clean immediately.
	}
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] unused spilled handle refuses with W1 cleanup") {
	refuse(Refusal::UNUSED, "v4_handle_unused");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] duplicate handle refuses with W1 cleanup") {
	refuse(Refusal::REUSED, "v4_handle_reused");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] spill depth must equal topology depth") {
	refuse(Refusal::DEPTH, "v4_depth_mismatch");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] spill cell centre must equal topology cell centre") {
	refuse(Refusal::CENTER, "v4_cell_center_mismatch");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] spill leaf flag must equal topology leaf flag") {
	refuse(Refusal::LEAF, "v4_is_leaf_mismatch");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] non-contiguous child range refuses with W1 cleanup") {
	refuse(Refusal::CHILD_RANGE, "v4_children_not_contiguous");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] child before parent refuses with W1 cleanup") {
	refuse(Refusal::PARENT_ORDER, "v4_parent_not_before_child");
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] every unset limit refuses before creating files") {
	Fixture fixture;
	if (!fixture.build()) {
		return;
	}
	for (bool existing : { false, true }) {
		for (uint32_t unset = 0; unset < 5; ++unset) {
			Paths paths;
			PackedByteArray original;
			if (!paths.create() || !seed_destination(paths, existing, original)) {
				return;
			}
			Limits limits = Limits::unbounded();
			uint64_t *fields[] = { &limits.max_ram_bytes, &limits.max_scratch_bytes, &limits.max_output_bytes,
				&limits.deadline_usec, &limits.min_free_disk_margin };
			*fields[unset] = 0;
			WriterV4 writer;
			CHECK(writer.begin_paged(paths.final_path, paths.scratch, fixture.spec, CodecParams(), limits) == ERR_INVALID_PARAMETER);
			CHECK(writer.get_report().error == ERR_INVALID_PARAMETER);
			CHECK(writer.get_report().reason == "limits_unset");
			CHECK_FALSE(writer.get_report().committed);
			check_w1(paths, original);
		}
	}
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] scratch limit trips during spill and removes earlier segments") {
	for (bool existing : { false, true }) {
		Paths paths;
		Fixture fixture;
		PackedByteArray original;
		if (!paths.create() || !fixture.build() || !seed_destination(paths, existing, original)) {
			return;
		}
		Limits limits = Limits::unbounded();
		const uint64_t raw_body_bytes = uint64_t(kSplats) * (sizeof(Gaussian) + kHigh * sizeof(Vector3));
		limits.max_scratch_bytes = 2 * raw_body_bytes - 1; // One body fits; two raw bodies cannot.
		WriterV4 writer;
		if (!begin(writer, paths, fixture, CodecParams(), limits) || !append_one(writer, fixture, 5)) {
			return;
		}
		CHECK(writer.get_report().total_scratch_bytes >= raw_body_bytes);
		CHECK(writer.get_report().total_scratch_bytes <= limits.max_scratch_bytes);
		PackedStringArray names;
		if (!list_dir(paths.scratch, names)) {
			return;
		}
		CHECK(names.size() == 1); // Proves the refusal has an earlier segment to clean.
		const Body &body = fixture.bodies[1];
		V4BodyHandle rejected;
		CHECK(writer.append_node(body.desc, body.gaussians.ptr(), body.sh.ptr(), kSplats, rejected) == ERR_PARAMETER_RANGE_ERROR);
		CHECK(writer.get_report().error == ERR_PARAMETER_RANGE_ERROR);
		CHECK(writer.get_report().reason == "limit_scratch");
		CHECK_FALSE(writer.get_report().committed);
		check_w1(paths, original);
	}
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] cancel during assembly returns ERR_SKIP cancelled and preserves W1") {
	for (bool existing : { false, true }) {
		Paths paths;
		Fixture fixture;
		PackedByteArray original;
		if (!paths.create() || !fixture.build() || !seed_destination(paths, existing, original)) {
			return;
		}
		AssemblyProbe probe;
		WriterV4 writer;
		if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths, original) || !probe.prepare(writer, paths, fixture, original)) {
			return;
		}
		probe.cancel_after_first = true;
		CHECK(writer.finalize(fixture.nodes) == ERR_SKIP);
		CHECK(probe.calls > 0); // An unwired hook or cancellation before assembly cannot pass.
		CHECK(probe.saw_early_deletion);
		CHECK(writer.get_report().error == ERR_SKIP);
		CHECK(writer.get_report().reason == "cancelled");
		CHECK_FALSE(writer.get_report().committed);
		CHECK(writer.get_progress().phase == Phase::STOPPED);
		check_w1(paths, original);
	}
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO] abort and destructor remove all spills without touching the destination") {
	for (bool existing : { false, true }) {
		for (bool explicit_abort : { false, true }) {
			Paths paths;
			Fixture fixture;
			PackedByteArray original;
			if (!paths.create() || !fixture.build() || !seed_destination(paths, existing, original)) {
				return;
			}
			{
				WriterV4 writer;
				if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths, original)) {
					return;
				}
				PackedStringArray names;
				if (!list_dir(paths.scratch, names)) {
					return;
				}
				CHECK(names.size() == 5);
				if (explicit_abort) {
					writer.abort();
					check_w1(paths, original);
					writer.abort(); // Idempotent, including preservation of an old final.
				}
			}
			check_w1(paths, original);
		}
	}
} // namespace TestGSWorldWriterV4
