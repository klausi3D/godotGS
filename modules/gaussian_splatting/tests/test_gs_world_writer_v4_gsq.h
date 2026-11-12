#pragma once

#include "test_gs_world_writer_v4.h"
#include "gs_gsq_bounds_test_helpers.h"
#include "../io/gaussian_splat_world_format.h"
#include "../io/gaussian_splat_world_io.h"
#include "core/error/error_macros.h"
#include "core/io/marshalls.h"

// BV-C4 phase 1: host-only executable contracts, using only master APIs.
// Proposed wire contract: header u32 at 60 = GSQ codec id; layout at 28 is
// unchanged. A node body is one GSQ page in node-relative coordinates. The
// existing 64-byte envelope keeps decoded Gaussian/SH sizes and a zero tail;
// stored_bytes includes that envelope plus the compressed page, not padding.
// Its index SHA covers ALL stored bytes, just as codec-none does.
//
// WriterV4 currently interprets CodecParams as scratch-only. The writer test
// deliberately demands GSQ OUTPUT as well. Until a resident codec-selecting API
// exists, make_coded_package is the minimal reader seam: real encode_page output
// plus existing V4 serializers. It is not evidence of a production writer.
// Unknown ids must report v4_unsupported_codec (new contract). Master reports
// v4_nonzero_reserved_header instead. The frozen legacy gate below must remain
// independent of the new reader so old-reader refusal stays tested after C4.

namespace TestGSWorldWriterV4GSQ {

using namespace TestGSWorldWriterV4;
using namespace gs_world_paged;

static constexpr uint64_t kCodecOffset = 60;
static constexpr uint64_t kIndexOffset = kHeaderBytes;
static constexpr uint64_t kPayloadOffset = kHeaderBytes + kIndexPageBytes;

static void sha(const uint8_t *p_bytes, uint64_t p_size, uint8_t *r_digest) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(p_bytes, p_size);
	hash.finish(r_digest);
}

static bool write_bytes(const String &p_path, const PackedByteArray &p_bytes) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		FAIL("cannot write BV-C4 package");
		return false;
	}
	file->store_buffer(p_bytes.ptr(), p_bytes.size());
	if (file->get_error() != OK) {
		FAIL("short BV-C4 package write");
		return false;
	}
	return true;
}

// Only called on our seven-node, single-index-page construction.
static void reseal_index(PackedByteArray &r_bytes) {
	uint8_t *b = r_bytes.ptrw();
	sha(b + kIndexOffset + kIndexPageHeaderBytes, kNodes * kIndexRecordBytes, b + kIndexOffset + 32);
	sha(b + kIndexOffset + 32, 32, b + 176);
	sha(b, kHeaderBytes - 32, b + kHeaderBytes - 32);
}

static uint64_t index_at(uint32_t p_node) {
	return kIndexOffset + kIndexPageHeaderBytes + p_node * kIndexRecordBytes;
}

struct Oracle {
	uint8_t digest[kNodes][32] = {};
	bool lossy = false;

	bool build(const Fixture &p_fixture) {
		gs_gsq::DecodeContext decoder;
		for (uint32_t i = 0; i < kNodes; ++i) {
			const Body &body = p_fixture.bodies[i];
			LocalVector<uint8_t> page;
			LocalVector<Gaussian> decoded;
			LocalVector<Vector3> sh;
			decoded.resize(kSplats);
			sh.resize(kSplats * kHigh);
			String reason;
			if (gs_gsq::encode_page(body.gaussians.ptr(), body.sh.ptr(), kSplats, 3, gs_gsq::EncodeParams(), page, reason) != OK ||
					decoder.decode_range(page.ptr(), page.size(), 0, kSplats, decoded.ptr(), sh.ptr(), reason) != OK) {
				FAIL("direct source-node GSQ oracle failed: ", reason.utf8().get_data());
				return false;
			}
			gs_gsq::decoded_digest(decoded.ptr(), sh.ptr(), kSplats, 3, digest[i]);
			uint8_t lossless[32];
			gs_gsq::decoded_digest(body.gaussians.ptr(), body.sh.ptr(), kSplats, 3, lossless);
			lossy |= std::memcmp(lossless, digest[i], 32) != 0;
		}
		CHECK(lossy); // A lossless-source comparison must not accidentally pass.
		return true;
	}
};

static bool make_coded_package(const Fixture &p_fixture, const Oracle &p_oracle, PackedByteArray &r_bytes,
		Vector<GaussianSplatHlodPagedSource::NodeRecord> &r_records) {
	if (p_fixture.nodes.size() != kNodes) {
		FAIL("BV-C4 needs the complete synthetic BFS tree");
		return false;
	}
	LocalVector<uint8_t> pages[kNodes];
	r_records.resize(kNodes);
	uint64_t cursor = kPayloadOffset;
	gs_gsq::DecodeContext decoder;
	for (uint32_t i = 0; i < kNodes; ++i) {
		const Body &body = p_fixture.bodies[i];
		LocalVector<Gaussian> relative = body.gaussians;
		if (body.desc.is_leaf) {
			for (Gaussian &g : relative) {
				for (int a = 0; a < 3; ++a) {
					g.position[a] = real_t(double(g.position[a]) - body.desc.cell_center[a]);
				}
			}
		}
		String reason;
		if (gs_gsq::encode_page(relative.ptr(), body.sh.ptr(), kSplats, 3, gs_gsq::EncodeParams(), pages[i], reason) != OK ||
				pages[i].size() <= gs_gsq::kPageHeaderBytes) {
			FAIL("cannot encode a framed GSQ node: ", reason.utf8().get_data());
			return false;
		}
		LocalVector<Vector3> sh;
		sh.resize(kSplats * kHigh);
		if (decoder.decode_range(pages[i].ptr(), pages[i].size(), 0, kSplats, relative.ptr(), sh.ptr(), reason) != OK) {
			FAIL("cannot decode test-local GSQ node");
			return false;
		}
		if (body.desc.is_leaf) {
			reconstruct_leaf_positions(relative.ptr(), kSplats, body.desc.cell_center);
		}
		uint8_t digest[32];
		gs_gsq::decoded_digest(relative.ptr(), sh.ptr(), kSplats, 3, digest);
		// The fixture's dyadic position endpoints make the relative wire encoding
		// agree EXACTLY with independent encode/decode of absolute source leaves.
		if (std::memcmp(digest, p_oracle.digest[i], 32) != 0) {
			FAIL("relative fixture encoding disagrees with the source-node oracle");
			return false;
		}
		auto &record = r_records.write[i];
		record.topology = p_fixture.nodes[i].topology;
		record.is_leaf = body.desc.is_leaf;
		std::memcpy(record.cell_center, body.desc.cell_center, sizeof(record.cell_center));
		if (!compute_node_metrics(relative.ptr(), kSplats, record.cell_center, record.is_leaf, record.metrics, reason)) {
			FAIL("decoded node metrics failed");
			return false;
		}
		record.offset = cursor;
		record.stored_bytes = kPayloadRecordHeaderBytes + pages[i].size();
		record.extent_bytes = align_up_page(record.stored_bytes);
		cursor += record.extent_bytes;
	}
	HeaderFields h;
	h.leaf = p_fixture.spec.leaf_splat_count;
	h.interior = p_fixture.spec.interior_splat_count;
	h.node_count = kNodes;
	h.sh_degree = 3;
	h.sh_first_order = p_fixture.spec.sh_first_order;
	h.sh_high_order = kHigh;
	h.bake_rule = p_fixture.spec.bake_rule_version;
	std::memcpy(h.origin, p_fixture.spec.origin, sizeof(h.origin));
	h.bounds_pos = p_fixture.spec.bounds.position;
	h.bounds_size = p_fixture.spec.bounds.size;
	h.index_offset = kIndexOffset;
	h.index_page_count = 1;
	h.payload_offset = kPayloadOffset;
	h.payload_bytes = cursor - kPayloadOffset;
	h.file_bytes = cursor;
	r_bytes.resize(cursor);
	r_bytes.fill(0);
	uint8_t *b = r_bytes.ptrw();
	write_header(b, h);
	encode_uint32(gs_gsq::kCodecId, b + kCodecOffset);
	// Preserve D-7 generation: identity header followed by stored BODY hashes.
	CryptoCore::SHA256Context generation;
	generation.start();
	generation.update(b, kHeaderBytes - 32);
	for (const auto &page : pages) {
		uint8_t digest[32];
		sha(page.ptr(), page.size(), digest);
		generation.update(digest, 32);
	}
	uint8_t digest[32];
	generation.finish(digest);
	h.content_generation = decode_uint64(digest);
	for (uint32_t i = 0; i < kNodes; ++i) {
		auto &record = r_records.write[i];
		if (!write_payload_header(b + record.offset, i, record, kHigh, h.content_generation)) {
			FAIL("cannot serialize GSQ record envelope");
			return false;
		}
		std::memcpy(b + record.offset + kPayloadRecordHeaderBytes, pages[i].ptr(), pages[i].size());
		sha(b + record.offset, record.stored_bytes, record.digest);
	}
	write_index_page(b + kIndexOffset, 0, r_records.ptr(), kNodes);
	sha(b + kIndexOffset + 32, 32, h.index_digest);
	write_header(b, h);
	encode_uint32(gs_gsq::kCodecId, b + kCodecOffset);
	sha(b, kHeaderBytes - 32, b + kHeaderBytes - 32);
	return true;
}

static bool check_payload(GaussianSplatHlodPagedSource &p_source, uint32_t p_node, const Oracle &p_oracle) {
	Vector<Gaussian> actual;
	Vector<Vector3> sh;
	String reason;
	if (p_source.read_node_payload(p_node, actual, sh, reason) != OK || actual.size() != kSplats || sh.size() != kSplats * kHigh) {
		FAIL("GSQ node read/count failed: ", reason.utf8().get_data());
		return false;
	}
	uint8_t digest[32];
	gs_gsq::decoded_digest(actual.ptr(), sh.ptr(), kSplats, 3, digest);
	CHECK(std::memcmp(digest, p_oracle.digest[p_node], 32) == 0);
	CHECK(reason.is_empty());
	return true;
}

static bool open_coded(const String &p_path, GaussianSplatHlodPagedSource &r_source) {
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	String reason;
	if (r_source.open(p_path, tree, metadata, reason) != OK) {
		FAIL("valid GSQ V4 must open (master rejects byte 60): ", reason.utf8().get_data());
		return false;
	}
	if (tree.nodes.size() != kNodes || r_source.get_nodes().size() != kNodes) {
		FAIL("GSQ open did not publish all nodes");
		return false;
	}
	CHECK_FALSE(tree.interior_resident);
	return true;
}

static void check_stored_records(const PackedByteArray &p_bytes, const GaussianSplatHlodPagedSource &p_source) {
	if (p_bytes.size() < int64_t(kHeaderBytes) || p_source.get_nodes().size() != kNodes) {
		FAIL("missing GSQ header or records");
		return;
	}
	CHECK(decode_uint32(p_bytes.ptr() + kCodecOffset) == gs_gsq::kCodecId);
	CHECK(decode_uint32(p_bytes.ptr() + 28) == GAUSSIAN_STRUCT_LAYOUT_VERSION);
	bool smaller = false;
	for (const auto &record : p_source.get_nodes()) {
		if (record.stored_bytes < kPayloadRecordHeaderBytes || record.offset > uint64_t(p_bytes.size()) ||
				record.stored_bytes > uint64_t(p_bytes.size()) - record.offset) {
			FAIL("stored record is out of file bounds");
			return;
		}
		const uint64_t raw = kPayloadRecordHeaderBytes + uint64_t(kSplats) * (sizeof(Gaussian) + kHigh * sizeof(Vector3));
		smaller |= record.stored_bytes < raw;
		CHECK(record.extent_bytes == align_up_page(record.stored_bytes));
		uint8_t digest[32];
		sha(p_bytes.ptr() + record.offset, record.stored_bytes, digest);
		CHECK(std::memcmp(digest, record.digest, 32) == 0);
		gs_gsq::PageInfo info;
		String reason;
		CHECK(gs_gsq::verify_page(p_bytes.ptr() + record.offset + kPayloadRecordHeaderBytes,
				record.stored_bytes - kPayloadRecordHeaderBytes, info, reason) == OK);
	}
	CHECK(smaller); // RED on master even though its GSQ scratch was compressed.
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] GSQ selection writes compressed records and round-trips every node") {
	Paths paths;
	Fixture fixture;
	Oracle oracle;
	if (!paths.create() || !fixture.build() || !oracle.build(fixture)) {
		return;
	}
	WriterV4 writer;
	if (!begin(writer, paths, fixture, gsq_v1_codec_params()) || !append_all(writer, fixture, paths)) {
		return;
	}
	if (writer.finalize(fixture.nodes) != OK) {
		FAIL("GSQ output assembly failed: ", writer.get_report().reason.utf8().get_data());
		return;
	}
	GaussianSplatHlodPagedSource source;
	PackedByteArray bytes;
	if (!open_coded(paths.final_path, source) || !read_bytes(paths.final_path, bytes)) {
		return;
	}
	check_stored_records(bytes, source);
	check_output_generation(paths, source);
	for (uint32_t i = 0; i < kNodes; ++i) {
		if (!check_payload(source, i, oracle)) {
			return;
		}
	}
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] GSQ wire package decodes exact canonical digests on repeated reads") {
	Paths paths;
	Fixture fixture;
	Oracle oracle;
	PackedByteArray bytes;
	Vector<GaussianSplatHlodPagedSource::NodeRecord> records;
	if (!paths.create() || !fixture.build() || !oracle.build(fixture) || !make_coded_package(fixture, oracle, bytes, records) ||
			!write_bytes(paths.final_path, bytes)) {
		return;
	}
	GaussianSplatHlodPagedSource source;
	if (!open_coded(paths.final_path, source)) {
		return;
	}
	check_stored_records(bytes, source);
	check_output_generation(paths, source);
	// Interleaved leaves/interiors and repeat reads exercise handle seek/reuse
	// without depending on a future pool instrumentation API. No timing claim.
	for (int pass = 0; pass < 3; ++pass) {
		for (uint32_t i : kAppendOrder) {
			if (!check_payload(source, i, oracle)) {
				return;
			}
		}
	}
	CHECK(source.get_stats().read_failures == 0);
}

// The exact rejecting gate from b2034d2ddfd's parse_header, not a second V4
// parser. Its positive control is a real resident-writer package below.
static Error master_reserved_word_gate(const PackedByteArray &p_bytes, String &r_reason) {
	if (p_bytes.size() < int64_t(kHeaderBytes)) {
		r_reason = "test_header_missing";
		return ERR_FILE_CORRUPT;
	}
	if (decode_uint32(p_bytes.ptr() + kCodecOffset) != 0u) {
		r_reason = "v4_nonzero_reserved_header";
		return ERR_FILE_CORRUPT;
	}
	r_reason = String();
	return OK;
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] GSQ identity refuses old readers and unknown codec has a distinct reason") {
	Paths paths;
	Fixture fixture;
	Oracle oracle;
	PackedByteArray bytes;
	Vector<GaussianSplatHlodPagedSource::NodeRecord> records;
	if (!paths.create() || !fixture.build() || !oracle.build(fixture) || !make_coded_package(fixture, oracle, bytes, records)) {
		return;
	}
	String reason;
	CHECK(master_reserved_word_gate(bytes, reason) == ERR_FILE_CORRUPT);
	CHECK(reason == "v4_nonzero_reserved_header");
	gs_world_format::HeaderLayout layout;
	CHECK(gs_world_format::select_header(decode_uint32(bytes.ptr() + 4), decode_uint32(bytes.ptr() + 8), bytes.size(), layout) == ERR_FILE_CORRUPT);
	CHECK(gs_world_format::select_header(4u, gs_world_format::kFlagHasHlod, bytes.size(), layout) == ERR_FILE_CORRUPT);
	encode_uint32(0xFFFFFFFFu, bytes.ptrw() + kCodecOffset);
	reseal_index(bytes); // Ensure the refusal is about the codec, not a stale SHA.
	if (!write_bytes(paths.final_path, bytes)) {
		return;
	}
	GaussianSplatHlodPagedSource source;
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	CHECK(source.open(paths.final_path, tree, metadata, reason) == ERR_FILE_CORRUPT);
	CHECK(reason == "v4_unsupported_codec"); // RED: master says nonzero_reserved_header.
	CHECK_FALSE(source.is_open());
	CHECK(source.get_nodes().is_empty());
	CHECK(tree.nodes.is_empty());
	CHECK(metadata.is_empty());
}

enum class Damage { BYTE_FLIP, TRUNCATED, STORED_PAST_EXTENT };

static void check_damage(Damage p_damage) {
	Paths paths;
	Fixture fixture;
	Oracle oracle;
	PackedByteArray bytes;
	Vector<GaussianSplatHlodPagedSource::NodeRecord> records;
	if (!paths.create() || !fixture.build() || !oracle.build(fixture) || !make_coded_package(fixture, oracle, bytes, records) ||
			!write_bytes(paths.final_path, bytes)) {
		return;
	}
	// A valid compressed positive control is mandatory: refusing everything at
	// the old reserved-word gate must NOT make corruption tests look green.
	GaussianSplatHlodPagedSource good;
	if (!open_coded(paths.final_path, good) || !check_payload(good, kNodes - 1, oracle)) {
		return;
	}
	if (records.size() != kNodes) {
		FAIL("missing corruption target");
		return;
	}
	const uint32_t victim = kNodes - 1;
	const auto &record = records[victim];
	if (p_damage == Damage::BYTE_FLIP) {
		bytes.ptrw()[record.offset + kPayloadRecordHeaderBytes + gs_gsq::kPageHeaderBytes] ^= 0x40;
	} else if (p_damage == Damage::TRUNCATED) {
		bytes.resize(record.offset + record.stored_bytes - 1); // Actual body byte, not padding.
	} else {
		encode_uint64(record.extent_bytes + 1, bytes.ptrw() + index_at(victim) + 136);
		reseal_index(bytes);
	}
	if (!write_bytes(paths.final_path, bytes)) {
		return;
	}
	String reason;
	GaussianSplatHlodPagedSource changed;
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	const Error opened = changed.open(paths.final_path, tree, metadata, reason);
	if (p_damage != Damage::BYTE_FLIP) {
		CHECK(opened == ERR_FILE_CORRUPT);
		CHECK(reason == (p_damage == Damage::TRUNCATED ? "v4_truncated_file" : "v4_payload_record_size_inconsistent"));
		CHECK_FALSE(changed.is_open());
		CHECK(changed.get_nodes().is_empty());
		CHECK(tree.nodes.is_empty());
		CHECK(metadata.is_empty());
	}
	if (p_damage == Damage::STORED_PAST_EXTENT) {
		return;
	}
	// Seed outputs: failure must clear old data as well as any partial decode.
	Vector<Gaussian> output;
	Vector<Vector3> sh;
	output.resize(1);
	sh.resize(1);
	if (p_damage == Damage::BYTE_FLIP) {
		if (opened != OK) {
			CHECK(opened == ERR_FILE_CORRUPT);
			CHECK(reason == "v4_payload_digest_mismatch");
			CHECK_FALSE(changed.is_open());
			CHECK(tree.nodes.is_empty());
			CHECK(changed.get_nodes().is_empty());
			return; // Eager verification is allowed too.
		}
		CHECK(changed.read_node_payload(victim, output, sh, reason) == ERR_FILE_CORRUPT);
		CHECK(reason == "v4_payload_digest_mismatch");
		CHECK(output.is_empty());
		CHECK(sh.is_empty());
		// A failed decode must not poison a reused handle/context for another node.
		check_payload(changed, 0, oracle);
	} else {
		CHECK(good.read_node_payload(victim, output, sh, reason) == ERR_FILE_CORRUPT);
		CHECK(reason == "v4_file_changed_length");
		CHECK(output.is_empty());
		CHECK(sh.is_empty());
	}
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] flipped compressed byte refuses without partial payload") {
	check_damage(Damage::BYTE_FLIP);
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] truncated compressed record refuses fresh and already-open readers") {
	check_damage(Damage::TRUNCATED);
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] compressed stored_bytes beyond extent refuses after resealing index") {
	check_damage(Damage::STORED_PAST_EXTENT);
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] codec none preserves resident bytes and zero codec fields") {
	Paths paths;
	Fixture fixture;
	if (!paths.create() || !fixture.build()) {
		return;
	}
	if (fixture.world.is_null()) {
		FAIL("missing resident world");
		return;
	}
	const String reference_path = paths.dir.path_join("none_reference.gsplatworld");
	String reason;
	if (GaussianSplatHlodPagedSource::write_package(*fixture.world.ptr(), reference_path, reason) != OK) {
		FAIL("resident codec-none producer failed");
		return;
	}
	WriterV4 writer;
	if (!begin(writer, paths, fixture, CodecParams()) || !append_all(writer, fixture, paths)) {
		return;
	}
	if (writer.finalize(fixture.nodes) != OK) {
		FAIL("codec-none out-of-core producer failed");
		return;
	}
	PackedByteArray reference, actual;
	GaussianSplatHlodPagedSource source;
	if (!read_bytes(reference_path, reference) || !read_bytes(paths.final_path, actual) || !open_source(paths, source)) {
		return;
	}
	CHECK(actual == reference); // Existing independent paths; no invented golden.
	if (actual.size() < int64_t(kHeaderBytes)) {
		FAIL("missing codec-none header");
		return;
	}
	CHECK(master_reserved_word_gate(reference, reason) == OK);
	CHECK(master_reserved_word_gate(actual, reason) == OK);
	CHECK(decode_uint32(actual.ptr() + 28) == GAUSSIAN_STRUCT_LAYOUT_VERSION);
	for (const auto &record : source.get_nodes()) {
		if (record.offset > uint64_t(actual.size()) || kPayloadRecordHeaderBytes > uint64_t(actual.size()) - record.offset) {
			FAIL("codec-none envelope outside file");
			return;
		}
		CHECK(record.stored_bytes == kPayloadRecordHeaderBytes + uint64_t(kSplats) * (sizeof(Gaussian) + kHigh * sizeof(Vector3)));
		CHECK(decode_uint32(actual.ptr() + record.offset + 48) == GAUSSIAN_STRUCT_LAYOUT_VERSION);
		for (uint64_t tail = 52; tail < kPayloadRecordHeaderBytes; ++tail) {
			CHECK(actual[record.offset + tail] == 0);
		}
	}
}

// Keep the existing seven-node BFS topology, but compute bounds with the bake's
// actual three-sigma/outward-rounding functions instead of generous test boxes.
static bool tight_quantisation_fixture(Fixture &r_fixture, GaussianSplatHlodTree &r_bake) {
	if (!r_fixture.build()) {
		return false;
	}
	r_bake.leaf_splat_count = 4 * kSplats;
	r_bake.interior_splat_count = 3 * kSplats;
	r_bake.sh_high_order_count = kHigh;
	r_bake.interior_resident = true;
	LocalVector<Gaussian> leaves;
	LocalVector<Vector3> leaf_sh;
	if (!gs_hlod::fallible_resize(r_bake.nodes, kNodes, "tight GSQ test nodes") ||
			!gs_hlod::fallible_resize(r_bake.interior_gaussians, 3 * kSplats, "tight GSQ test interiors") ||
			!gs_hlod::fallible_resize(r_bake.interior_sh_high_order, 3 * kSplats * kHigh, "tight GSQ test interior SH") ||
			!gs_hlod::fallible_resize(leaves, 4 * kSplats, "tight GSQ test leaves") ||
			!gs_hlod::fallible_resize(leaf_sh, 4 * kSplats * kHigh, "tight GSQ test leaf SH")) {
		FAIL("cannot allocate tight GSQ fixture");
		return false;
	}
	// Y-scale endpoints have exact Q16 logs -5 and -4. This interior value
	// maps near code 1.75 of 1023, rounds UP to code 2, and stays above its
	// source even after Q16 truncation. Position extrema and identity rotations
	// are exactly representable before encoding. Position dequantisation may
	// move the upper Y endpoint inward; scale rounding still pushes support out.
	const float victim_scale = float(std::exp2(-5.0 + 1.75 / 1023.0));
	for (uint32_t i = 0; i < kNodes; ++i) {
		auto &node = r_bake.nodes[i];
		node = r_fixture.nodes[i].topology;
		auto &body = r_fixture.bodies[i];
		for (uint32_t j = 0; j < kSplats; ++j) {
			Gaussian &g = body.gaussians[j];
			g.rotation = Quaternion(0, 0, 0, 1);
			g.scale = Vector3(0.03125f, 0.03125f, 0.03125f);
			if (node.is_leaf()) {
				g.scale.y = j % 3 == 2 ? victim_scale : (j % 3 == 1 ? 0.0625f : 0.03125f);
				g.position = Vector3(real_t(body.desc.cell_center[0]), j % 3 == 2 ? 2.5f : 1.5f, 2.0f);
			} else {
				// Interior payloads stay strictly inside the children's world Y/Z
				// bounds. Their bounds can grow only by bottom-up child propagation.
				g.position = Vector3(0, real_t(2.0 - body.desc.cell_center[1]), real_t(2.0 - body.desc.cell_center[2]));
			}
			const uint32_t slot = (node.is_leaf() ? i - 3 : i) * kSplats + j;
			if (node.is_leaf()) {
				leaves[slot] = g;
			} else {
				r_bake.interior_gaussians[slot] = g;
			}
			for (uint32_t k = 0; k < kHigh; ++k) {
				if (node.is_leaf()) {
					leaf_sh[slot * kHigh + k] = body.sh[j * kHigh + k];
				} else {
					r_bake.interior_sh_high_order[slot * kHigh + k] = body.sh[j * kHigh + k];
				}
			}
		}
	}
	for (int i = kNodes - 1; i >= 0; --i) {
		auto &node = r_bake.nodes[i];
		const auto &body = r_fixture.bodies[i];
		const double *center = body.desc.cell_center;
		const double shift[3] = { node.is_leaf() ? 0.0 : center[0], node.is_leaf() ? 0.0 : center[1], node.is_leaf() ? 0.0 : center[2] };
		const gs_hlod::SplatSpan span{ body.gaussians.ptr(), body.sh.ptr(), kSplats, kHigh };
		gs_hlod::bake_bounds::BoundsAccumulator bounds;
		gs_hlod::bake_bounds::add_payload_bounds(span, shift, bounds);
		for (uint32_t c = node.first_child; c < node.first_child + node.child_count; ++c) {
			const auto &child = r_bake.nodes[c];
			const double *cc = r_fixture.bodies[c].desc.cell_center;
			const double lo[3] = { cc[0] + child.aabb_min.x, cc[1] + child.aabb_min.y, cc[2] + child.aabb_min.z };
			const double hi[3] = { cc[0] + child.aabb_max.x, cc[1] + child.aabb_max.y, cc[2] + child.aabb_max.z };
			bounds.add(lo, hi);
		}
		double mid[3];
		for (int a = 0; a < 3; ++a) {
			node.aabb_min[a] = gs_hlod::bake_bounds::round_down_offset(center[a], bounds.lo[a]);
			node.aabb_max[a] = gs_hlod::bake_bounds::round_up_offset(center[a], bounds.hi[a]);
			mid[a] = 0.5 * ((center[a] + double(node.aabb_min[a])) + (center[a] + double(node.aabb_max[a])));
		}
		double radius = gs_hlod::bake_bounds::payload_radius(span, shift, mid);
		for (int a = 0; a < 3; ++a) {
			radius = MAX(radius, 0.5 * (double(node.aabb_max[a]) - double(node.aabb_min[a])));
		}
		for (uint32_t c = node.first_child; c < node.first_child + node.child_count; ++c) {
			const auto &child = r_bake.nodes[c];
			const double *cc = r_fixture.bodies[c].desc.cell_center;
			double distance_squared = 0;
			for (int a = 0; a < 3; ++a) {
				const double delta = mid[a] - 0.5 * ((cc[a] + double(child.aabb_min[a])) + (cc[a] + double(child.aabb_max[a])));
				distance_squared += delta * delta;
			}
			radius = MAX(radius, std::sqrt(distance_squared) + double(child.radius));
		}
		node.radius = gs_hlod::bake_bounds::round_up(radius);
		r_fixture.nodes[i].topology = node;
	}
	String reason;
	if (!gs_hlod_validate_tree(r_bake, &reason)) {
		FAIL("tight fixture tree invalid: ", doctest::String(reason.utf8().get_data()));
		return false;
	}
	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussian_payload(leaves, leaf_sh, 3, kHigh, false, false);
	r_fixture.world->set_gaussian_data(data);
	GaussianSplatHlodTree resident_tree = r_bake;
	r_fixture.world->set_hlod_tree(std::move(resident_tree));
	r_fixture.spec.bounds = r_fixture.world->get_bounds();
	return true;
}

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] GSQ unions tight bake bounds with decoded support and propagates bounded inflation") {
	Paths paths;
	Fixture fixture;
	GaussianSplatHlodTree bake;
	if (!paths.create() || !tight_quantisation_fixture(fixture, bake)) {
		return;
	}
	const CodecParams codec = gsq_v1_codec_params();
	gs_gsq::EncodeParams encode_params;
	String reason;
	if (gsq_encode_params(codec, encode_params, reason) != OK) {
		FAIL("cannot get GSQ parameters");
		return;
	}
	std::vector<TestGSQBounds::Budget> budgets(kNodes);
	std::vector<String> decoded_digests(kNodes);
	uint32_t escaping_leaves = 0;
	gs_gsq::DecodeContext decoder;
	for (uint32_t i = 0; i < kNodes; ++i) {
		const auto &body = fixture.bodies[i];
		LocalVector<Gaussian> relative = body.gaussians;
		if (body.desc.is_leaf) {
			for (Gaussian &g : relative) {
				for (int a = 0; a < 3; ++a) {
					g.position[a] = real_t(double(g.position[a]) - body.desc.cell_center[a]);
				}
			}
		}
		// Positive fixture control: the exact same bounds contain the source.
		if (!validate_node_payload(body.desc.cell_center, kHigh, bake.nodes[i], relative.ptr(), body.sh.ptr(), reason)) {
			FAIL("tight unquantised payload must validate: ", doctest::String(reason.utf8().get_data()));
			return;
		}
		LocalVector<uint8_t> page;
		Vector<Gaussian> decoded;
		Vector<Vector3> decoded_sh;
		if (!gs_resize_or_fail(decoded, kSplats, "tight GSQ test decoded") ||
				!gs_resize_or_fail(decoded_sh, kSplats * kHigh, "tight GSQ test decoded SH")) {
			FAIL("cannot allocate tight GSQ oracle");
			return;
		}
		gs_gsq::PageInfo page_info;
		if (gs_gsq::encode_page(relative.ptr(), body.sh.ptr(), kSplats, 3, encode_params, page, reason) != OK ||
				gs_gsq::verify_page(page.ptr(), page.size(), page_info, reason) != OK ||
				decoder.decode_range(page.ptr(), page.size(), 0, kSplats, decoded.ptrw(), decoded_sh.ptrw(), reason) != OK ||
				!TestGSQBounds::quantisation_budget(body.gaussians.ptr(), kSplats, body.desc.cell_center, body.desc.is_leaf, page_info, codec, budgets[i])) {
			FAIL("tight GSQ oracle failed: ", doctest::String(reason.utf8().get_data()));
			return;
		}
		if (body.desc.is_leaf) {
			// The adversary is proved using the real codec, not a guessed epsilon.
			CHECK(decoded[2].position.y <= relative[2].position.y);
			for (int a = 0; a < 3; ++a) {
				CHECK(std::abs(double(decoded[2].position[a]) - double(relative[2].position[a])) <= budgets[i].axis[a]);
			}
			CHECK(decoded[2].scale.y > relative[2].scale.y);
			double cov[6];
			gs_hlod::splat_covariance(decoded[2], cov);
			CHECK(double(decoded[2].position.y) + 3 * std::sqrt(cov[3]) > double(bake.nodes[i].aabb_max.y));
			const bool contained = validate_node_payload(body.desc.cell_center, kHigh, bake.nodes[i], decoded.ptr(), decoded_sh.ptr(), reason);
			CHECK_FALSE(contained);
			CHECK(reason == "HLOD node AABB does not contain its payload's three-sigma support");
			if (!contained) {
				++escaping_leaves;
			}
			reconstruct_leaf_positions(decoded.ptrw(), kSplats, body.desc.cell_center);
		}
		uint8_t digest[32];
		gs_gsq::decoded_digest(decoded.ptr(), decoded_sh.ptr(), kSplats, 3, digest);
		decoded_digests[i] = String::hex_encode_buffer(digest, 32);
	}
	if (escaping_leaves != 4) {
		FAIL("all four tight leaves must expose GSQ support expansion");
		return;
	}
	// Independent codec-none producer pins the complete output bytes, not just
	// equality between two calls to the writer being changed in phase B.
	const String reference_path = paths.dir.path_join("tight_none_reference.gsplatworld");
	if (GaussianSplatHlodPagedSource::write_package(*fixture.world.ptr(), reference_path, reason) != OK) {
		FAIL("tight codec-none reference failed: ", doctest::String(reason.utf8().get_data()));
		return;
	}
	for (bool gsq : { false, true }) {
		WriterV4 writer;
		if (!begin(writer, paths, fixture, gsq ? codec : CodecParams())) {
			return;
		}
		for (uint32_t i : kAppendOrder) {
			if (!append_one(writer, fixture, i)) {
				return;
			}
		}
		const Error finalized = writer.finalize(fixture.nodes);
		INFO("codec=", gsq ? doctest::String("gsq") : doctest::String("none"), " finalize reason=",
				doctest::String(writer.get_report().full_reason.utf8().get_data()));
		CHECK(finalized == OK); // RED on b2e2fbadfdb: v4_invalid_payload: HLOD node AABB does not contain ...
		if (finalized != OK) {
			return;
		}
		GaussianSplatHlodPagedSource source;
		GaussianSplatHlodTree tree;
		Dictionary metadata;
		if (source.open(paths.final_path, tree, metadata, reason) != OK || tree.nodes.size() != kNodes) {
			FAIL("tight output reopen failed: ", doctest::String(reason.utf8().get_data()));
			return;
		}
		CHECK(source.get_codec_id() == (gsq ? kCodecGsqV1 : kCodecNone));
		for (uint32_t i = 0; i < kNodes; ++i) {
			Vector<Gaussian> actual;
			Vector<Vector3> sh;
			if (source.read_node_payload(i, actual, sh, reason) != OK || !TestGSQBounds::check_decoded_support(source, tree, i, actual, sh)) {
				FAIL("tight output payload failed: ", doctest::String(reason.utf8().get_data()));
				return;
			}
			TestGSQBounds::check_unchanged_fields(tree.nodes[i], bake.nodes[i]);
			if (gsq) {
				uint8_t digest[32];
				gs_gsq::decoded_digest(actual.ptr(), sh.ptr(), kSplats, 3, digest);
				CHECK(String::hex_encode_buffer(digest, 32) == decoded_digests[i]);
			} else {
				CHECK(tree.nodes[i].aabb_min == bake.nodes[i].aabb_min);
				CHECK(tree.nodes[i].aabb_max == bake.nodes[i].aabb_max);
				CHECK(tree.nodes[i].radius == bake.nodes[i].radius);
			}
		}
		if (gsq) {
			TestGSQBounds::check_tree_bounds(bake, tree, budgets);
			// Leaves require growth, and the interior source payloads do not.
			// Thus growth at both ancestor levels proves bottom-up propagation.
			for (uint32_t i = 0; i < kNodes; ++i) {
				CHECK(tree.nodes[i].aabb_max.y > bake.nodes[i].aabb_max.y);
			}
		} else {
			PackedByteArray reference, actual;
			if (!read_bytes(reference_path, reference) || !read_bytes(paths.final_path, actual)) {
				return;
			}
			CHECK(actual == reference);
		}
	}
}

// Capture the actual format loader refusal, not a diagnostic reconstructed by
// the test. Leave normal engine error reporting enabled as well.
struct ScopedHeaderBoundsLoadErrors : public ErrorHandlerList {
	String messages;
	static void capture(void *p_userdata, const char *, const char *, int, const char *p_error,
			const char *p_message, bool, ErrorHandlerType p_type) {
		if (p_type != ERR_HANDLER_ERROR) {
			return;
		}
		auto *self = static_cast<ScopedHeaderBoundsLoadErrors *>(p_userdata);
		if (!self->messages.is_empty()) {
			self->messages += "\n";
		}
		self->messages += p_error ? String::utf8(p_error) : String();
		if (p_message && p_message[0]) {
			self->messages += String(" | ") + String::utf8(p_message);
		}
	}
	ScopedHeaderBoundsLoadErrors() {
		errfunc = capture;
		userdata = this;
		add_error_handler(this);
	}
	~ScopedHeaderBoundsLoadErrors() { remove_error_handler(this); }
	ScopedHeaderBoundsLoadErrors(const ScopedHeaderBoundsLoadErrors &) = delete;
	ScopedHeaderBoundsLoadErrors &operator=(const ScopedHeaderBoundsLoadErrors &) = delete;
};

TEST_CASE("[WorldWriterV4][GaussianSplatting][WorldIO][BVC4] GSQ inflated root remains enclosed by header bounds through the normal resource loader") {
	Fixture fixture;
	GaussianSplatHlodTree bake;
	if (!tight_quantisation_fixture(fixture, bake) || bake.nodes.size() != kNodes) {
		FAIL("header enclosure regression needs the complete tight bake tree");
		return;
	}
	fixture.spec.bounds = bake.node_world_aabb(bake.nodes[0]);
	double bake_center[3];
	bake.node_cell_center(bake.nodes[0], bake_center);
	// Fixture control: no header padding or AABB conversion loss is allowed to
	// mask GSQ growth (or make the codec-none control fail before the regression).
	for (int a = 0; a < 3; ++a) {
		const double lo = double(fixture.spec.bounds.position[a]);
		const double hi = lo + double(fixture.spec.bounds.size[a]);
		const bool exact = lo == bake_center[a] + double(bake.nodes[0].aabb_min[a]) &&
				hi == bake_center[a] + double(bake.nodes[0].aabb_max[a]);
		CHECK(exact);
		if (!exact) {
			return;
		}
	}
	for (bool gsq : { false, true }) {
		INFO("codec=", gsq ? doctest::String("gsq") : doctest::String("none"));
		Paths paths; // Separate files; a previously loaded resource cannot mask GSQ output.
		WriterV4 writer;
		if (!paths.create() || !begin(writer, paths, fixture, gsq ? gsq_v1_codec_params() : CodecParams())) {
			return;
		}
		for (uint32_t i : kAppendOrder) {
			if (!append_one(writer, fixture, i)) {
				return;
			}
		}
		const Error finalized = writer.finalize(fixture.nodes);
		INFO("finalize reason=", doctest::String(writer.get_report().full_reason.utf8().get_data()));
		CHECK(finalized == OK);
		if (finalized != OK) {
			return;
		}
		GaussianSplatHlodPagedSource source;
		GaussianSplatHlodTree tree;
		Dictionary metadata;
		String reason;
		if (source.open(paths.final_path, tree, metadata, reason) != OK || tree.nodes.size() != kNodes) {
			FAIL("header enclosure output reopen failed: ", doctest::String(reason.utf8().get_data()));
			return;
		}
		CHECK(source.get_codec_id() == (gsq ? kCodecGsqV1 : kCodecNone));
		double center[3], root_lo[3], root_hi[3];
		tree.node_cell_center(tree.nodes[0], center);
		bool escapes_spec = false;
		for (int a = 0; a < 3; ++a) {
			root_lo[a] = center[a] + double(tree.nodes[0].aabb_min[a]);
			root_hi[a] = center[a] + double(tree.nodes[0].aabb_max[a]);
			const double spec_lo = double(fixture.spec.bounds.position[a]);
			const double spec_hi = spec_lo + double(fixture.spec.bounds.size[a]);
			escapes_spec |= root_lo[a] < spec_lo || root_hi[a] > spec_hi;
		}
		CHECK(escapes_spec == gsq); // GSQ must actually exercise the loader's enclosure gate.
		if (escapes_spec != gsq) {
			return;
		}
		// This is the normal .gsplatworld ResourceLoader implementation, including
		// _load_gsplatworld_paged's header/root check. No resident load or GPU setup.
		ResourceFormatLoaderGaussianSplatWorld loader;
		Error load_error = ERR_BUG;
		Ref<GaussianSplatWorld> loaded;
		String loader_messages;
		{
			ScopedHeaderBoundsLoadErrors errors;
			loaded = loader.load(paths.final_path, "", &load_error, false, nullptr, ResourceFormatLoader::CACHE_MODE_IGNORE);
			loader_messages = errors.messages;
		}
		INFO("loader error=", int(load_error), " refusal=", doctest::String(loader_messages.utf8().get_data()));
		CHECK(load_error == OK); // RED on 922b3f2ab54: header world bounds ... do not enclose the HLOD root.
		CHECK(loaded.is_valid());
		if (load_error != OK || loaded.is_null()) {
			return;
		}
		CHECK(loaded->has_hlod_paged_source());
		const AABB header_bounds = source.get_info().bounds;
		CHECK(loaded->get_bounds() == header_bounds);
		for (int a = 0; a < 3; ++a) {
			CHECK(double(header_bounds.position[a]) <= root_lo[a]);
			CHECK(double(header_bounds.position[a]) + double(header_bounds.size[a]) >= root_hi[a]);
		}
		if (!gsq) {
			CHECK(header_bounds.position == fixture.spec.bounds.position);
			CHECK(header_bounds.size == fixture.spec.bounds.size);
		}
	}
}

} // namespace TestGSWorldWriterV4GSQ
