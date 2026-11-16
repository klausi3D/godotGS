extern "C" int test_hlod_page_store_identity_cpp_force_link() { return 0; }

#include "gs_hlod_page_store_test_fixture.h"
#include "../io/gs_payload_page_store.h"
#include "../renderer/hlod_node_pack.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"

#include <cstddef>
#include <cstring>

namespace TestHlodPageStoreIdentity {

// Both before/after paths use this independent source/codec oracle and the
// existing packer. Phase 2 must retain these exact assertions: routing both
// readers through the store cannot make their expected payload self-derived.
static bool check_packed(const TestHlodPageStore::Fixture &p_fixture, uint32_t p_node,
		const Vector<Gaussian> &p_actual, const Vector<Vector3> &p_sh,
		const Vector<Gaussian> &p_expected, const Vector<Vector3> &p_expected_sh) {
	if (p_fixture.snapshot.is_null() || p_fixture.snapshot->nodes.size() != p_fixture.node_count ||
			p_node >= p_fixture.node_count || p_actual.size() != p_fixture.splats_per_node ||
			p_expected.size() != p_fixture.splats_per_node ||
			p_sh.size() != int64_t(p_fixture.splats_per_node) * p_fixture.high_order ||
			p_expected_sh.size() != p_sh.size()) {
		FAIL("D3-2 V4 identity requires complete node payloads before packing");
		return false;
	}
	uint8_t actual_digest[gs_gsq::kDigestBytes];
	uint8_t expected_digest[gs_gsq::kDigestBytes];
	gs_gsq::decoded_digest(p_actual.ptr(), p_sh.ptr(), p_fixture.splats_per_node, p_fixture.degree, actual_digest);
	gs_gsq::decoded_digest(p_expected.ptr(), p_expected_sh.ptr(), p_fixture.splats_per_node, p_fixture.degree, expected_digest);
	if (std::memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0) {
		FAIL("D3-2 V4 node payload differs from independently generated source/codec golden");
		return false;
	}
	gs_hlod_pack::PayloadView actual_view;
	actual_view.gaussians = p_actual.ptr();
	actual_view.sh = p_sh.ptr();
	gs_hlod_pack::PayloadView expected_view;
	expected_view.gaussians = p_expected.ptr();
	expected_view.sh = p_expected_sh.ptr();
	for (bool quantized : { false, true }) {
		CAPTURE(quantized);
		gs_hlod_pack::Params params;
		params.relative = true;
		params.quantized = quantized;
		params.position_bits = 16;
		params.scale_bits = 12;
		Vector<PackedGaussian> actual_raw, expected_raw;
		Vector<PackedGaussianQuantized> actual_quantized, expected_quantized;
		if (!gs_resize_or_fail(actual_raw, p_fixture.splats_per_node, "D3-2 actual raw") ||
				!gs_resize_or_fail(expected_raw, p_fixture.splats_per_node, "D3-2 expected raw") ||
				!gs_resize_or_fail(actual_quantized, p_fixture.splats_per_node, "D3-2 actual quantized") ||
				!gs_resize_or_fail(expected_quantized, p_fixture.splats_per_node, "D3-2 expected quantized")) {
			FAIL("D3-2 V4 packed identity allocation failed");
			return false;
		}
		ChunkMetaGPU actual_meta{}, expected_meta{};
		ChunkQuantizationGPU actual_quant{}, expected_quant{};
		SHCompressionMetrics actual_metrics{}, expected_metrics{};
		gs_hlod_pack::Output actual_output;
		actual_output.raw = actual_raw.ptrw();
		actual_output.quantized = actual_quantized.ptrw();
		actual_output.meta = &actual_meta;
		actual_output.quant = &actual_quant;
		actual_output.metrics = &actual_metrics;
		gs_hlod_pack::Output expected_output;
		expected_output.raw = expected_raw.ptrw();
		expected_output.quantized = expected_quantized.ptrw();
		expected_output.meta = &expected_meta;
		expected_output.quant = &expected_quant;
		expected_output.metrics = &expected_metrics;
		const uint32_t atlas_base = p_node * p_fixture.splats_per_node;
		if (!gs_hlod_pack::pack_node(*p_fixture.snapshot.ptr(), p_node, params, atlas_base, actual_output, &actual_view) ||
				!gs_hlod_pack::pack_node(*p_fixture.snapshot.ptr(), p_node, params, atlas_base, expected_output, &expected_view)) {
			FAIL("D3-2 V4 raw and quantized identity paths must both pack");
			return false;
		}
		if (std::memcmp(&actual_meta, &expected_meta, sizeof(actual_meta)) != 0) {
			FAIL("D3-2 V4 packed GPU metadata differs from the pre-migration packer golden");
			return false;
		}
		if (quantized) {
			if (std::memcmp(&actual_quant, &expected_quant, sizeof(actual_quant)) != 0 ||
					std::memcmp(actual_quantized.ptr(), expected_quantized.ptr(),
							size_t(p_fixture.splats_per_node) * sizeof(PackedGaussianQuantized)) != 0) {
				FAIL("D3-2 V4 quantized GPU records differ from the pre-migration packer golden");
				return false;
			}
		} else if (std::memcmp(actual_raw.ptr(), expected_raw.ptr(),
					size_t(p_fixture.splats_per_node) * sizeof(PackedGaussian)) != 0) {
			FAIL("D3-2 V4 raw GPU records differ from the pre-migration packer golden");
			return false;
		}
	}
	return true;
}

static void identity(bool p_gsq, bool p_store_path) {
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(p_gsq)) {
		return;
	}
	if (fixture.source.is_null() || fixture.snapshot.is_null()) {
		FAIL("D3-2 V4 identity fixture requires its source and paged snapshot");
		return;
	}
	PayloadPageStore store;
	if (p_store_path) {
		PayloadPageStore::Config config;
		config.reader_count = 1;
		config.max_bytes_in_flight = 8u * 1024u * 1024u;
		config.max_idle_handles = 0;
		config.decoder_retention_cap = 16;
		config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		config.collect_content_digests = true;
		if (store.start(config) != OK) {
			FAIL("D3-2 V4 identity requires a running common page store");
			return;
		}
		if (fixture.source->install_in_page_store(store, 1) != OK) {
			FAIL("D3-2 V4 descriptors must install in the runtime page store (phase-1 stub refuses)");
			return;
		}
	}
	for (uint32_t node = 0; node < fixture.node_count; ++node) {
		CAPTURE(node);
		Vector<Gaussian> actual, expected;
		Vector<Vector3> actual_sh, expected_sh;
		String reason;
		if (!fixture.expected_payload(node, expected, expected_sh)) {
			return;
		}
		const Error error = p_store_path ? fixture.source->read_node_payload_from_store(store, 17, node + 1u,
				node, actual, actual_sh, reason) : fixture.source->read_node_payload(node, actual, actual_sh, reason);
		if (error != OK) {
			FAIL("D3-2 V4 identity node read must succeed through the selected transport: ", reason.utf8().get_data());
			return;
		}
		if (!check_packed(fixture, node, actual, actual_sh, expected, expected_sh)) {
			return;
		}
		if (p_store_path) {
			uint8_t digest[gs_gsq::kDigestBytes];
			gs_gsq::decoded_digest(expected.ptr(), expected_sh.ptr(), fixture.splats_per_node, fixture.degree, digest);
			const Dictionary digests = store.get_telemetry("hlod").get("content_digests", Dictionary());
			CHECK(digests.size() == int64_t(node + 1));
			CHECK(String(digests.get(String::num_uint64(node), "")) == String::hex_encode_buffer(digest, sizeof(digest)));
		}
	}
	if (p_store_path) {
		const PayloadPageStore::Stats stats = store.stats();
		const Dictionary exported = store.get_telemetry("hlod");
		CHECK(int64_t(exported.get("record_verify_nsec", -1)) > 0);
		CHECK(int64_t(exported.get("node_load_nsec", -1)) >= int64_t(exported.get("record_verify_nsec", -1)));
		CHECK(int64_t(exported.get("nodes_loaded", -1)) == fixture.node_count);
		CHECK(String(exported.get("record_verify_scope", "")) == "v4_record_envelope_sha256_and_optional_payload_validation_worker_cpu_sum");
		CHECK(String(exported.get("node_load_scope", "")) == "successful_runtime_node_load_wall_sum_including_io_wait");
		// V4 carries no decoded reference digest, so no decoded-digest stage
		// ran. Its unavailable timing stays NIL, outside this subtraction.
		CHECK(exported.get("digest_nsec", Variant()).get_type() == Variant::NIL);
		CHECK(int64_t(exported.get("decode_remainder_nsec", -1)) == int64_t(exported.get("decode_nsec", -2)) -
				int64_t(exported.get("verify_nsec", -3)));
		if (p_gsq) {
			CHECK(String(exported.get("digest_status", "")) == "not_available");
			CHECK(String(exported.get("verify_scope", "")) == "gsq_header_parse_only_stored_bytes_covered_by_v4_record_digest");
			CHECK(int64_t(exported.get("pages_stored_digest_covered_by_record", -1)) == fixture.node_count);
			CHECK(stats.pages_decoded_digest_unavailable == fixture.node_count);
			CHECK(stats.pages_decoded_digest_skipped == 0);
		}
		if (stats.reads.completed_ok != fixture.node_count || stats.reads.bytes_read == 0) {
			FAIL("D3-2 V4 identity must perform every node read through the common store service");
			return;
		}
		if (stats.live_reservations != 0 || stats.reservations_acquired != stats.reservations_released) {
			FAIL("D3-2 V4 identity reads must release every stored-page reservation");
			return;
		}
		if (p_gsq && (stats.decoder_leases != fixture.node_count || stats.decoder_returns != stats.decoder_leases ||
				stats.decoders_leased != 0 || stats.pages_verified != fixture.node_count ||
				stats.pages_decoded != fixture.node_count ||
				stats.splats_decoded != uint64_t(fixture.node_count) * fixture.splats_per_node)) {
			FAIL("D3-2 GSQ V4 reads must use and return common-store decoder leases and count every verified decoded node");
			return;
		}
		// The telemetry collection follows source replacement; cumulative worker
		// counters must not leak into the next source's per-page-in arithmetic.
		if (fixture.source->install_in_page_store(store, 2) != OK) {
			FAIL("runtime verification counters require a fresh source collection");
			return;
		}
		const Dictionary fresh = store.get_telemetry("hlod");
		CHECK(int64_t(fresh.get("record_verify_nsec", -1)) == 0);
		CHECK(int64_t(fresh.get("node_load_nsec", -1)) == 0);
		CHECK(int64_t(fresh.get("nodes_loaded", -1)) == 0);
		CHECK(int64_t(fresh.get("pages_stored_digest_covered_by_record", -1)) == 0);
		CHECK(store.stats().reads.completed_ok == stats.reads.completed_ok);
	}
}

enum Corruption { ENVELOPE, CONTENT_GENERATION, STORED_BODY, GSQ_DIGEST_FIELD, GSQ_FRAME, GSQ_RESEALED_FRAME, TRUNCATED };

static bool corrupt_record(bool p_gsq, bool p_store_path, Corruption p_corruption, bool p_full_validation = true) {
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(p_gsq)) {
		return false;
	}
	if (fixture.source.is_null() || fixture.source->get_nodes().is_empty()) {
		FAIL("D3-2 corrupt-record test requires a real writer-produced V4 record");
		return false;
	}
	PayloadPageStore store;
	if (p_store_path) {
		fixture.source->_test_set_runtime_full_payload_validation(p_full_validation);
		PayloadPageStore::Config config;
		config.reader_count = 1;
		config.max_bytes_in_flight = 8u * 1024u * 1024u;
		config.max_idle_handles = 0;
		config.decoder_retention_cap = 16;
		config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		if (store.start(config) != OK) {
			FAIL("D3-2 corrupt-record test requires a running common page store");
			return false;
		}
		if (fixture.source->install_in_page_store(store, 1) != OK) {
			FAIL("D3-2 V4 descriptors must install in the runtime page store (phase-1 stub refuses)");
			return false;
		}
	}
	Error read_error = ERR_BUG;
	Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(fixture.path, &read_error);
	const GaussianSplatHlodPagedSource::NodeRecord &record = fixture.source->get_nodes()[0];
	const uint64_t body = gs_world_paged::kPayloadRecordHeaderBytes;
	if (read_error != OK || record.offset > uint64_t(bytes.size()) ||
			record.stored_bytes > uint64_t(bytes.size()) - record.offset || record.stored_bytes <= body) {
		FAIL("D3-2 corrupt-record fixture must contain its complete envelope and stored body");
		return false;
	}
	// Mutate actual production output after source/catalog capture. Header magic,
	// stamp and stored digest exercise three independent base V4 refusal rules.
	if (p_corruption == TRUNCATED) {
		if (!gs_resize_or_fail(bytes, record.offset + record.stored_bytes - 1u, "runtime truncated record")) {
			FAIL("could not truncate the isolated runtime record fixture");
			return false;
		}
	} else {
		uint64_t within_record = p_corruption == ENVELOPE ? 0u :
				(p_corruption == CONTENT_GENERATION ? 32u : body);
		if (p_corruption == GSQ_DIGEST_FIELD || p_corruption == GSQ_FRAME || p_corruption == GSQ_RESEALED_FRAME) {
			within_record = body + (p_corruption == GSQ_DIGEST_FIELD ? 32u : gs_gsq::kPageHeaderBytes);
			if (!p_gsq || within_record >= record.stored_bytes) {
				FAIL("runtime GSQ corruption requires a stored page header and frame");
				return false;
			}
		}
		bytes.ptrw()[record.offset + within_record] ^= 1u;
		if (p_corruption == GSQ_RESEALED_FRAME) {
			// Deliberately repair the inner digest only: verify_page must accept
			// this page, while the unchanged V4 digest must still refuse it.
			uint8_t *page = bytes.ptrw() + record.offset + body;
			const uint64_t page_bytes = record.stored_bytes - body;
			CryptoCore::SHA256Context hash;
			hash.start();
			hash.update(page, 32u);
			hash.update(page + gs_gsq::kHeaderBytes, page_bytes - gs_gsq::kHeaderBytes);
			hash.finish(page + 32u);
			gs_gsq::PageInfo info;
			String inner_reason;
			if (gs_gsq::verify_page(page, page_bytes, info, inner_reason) != OK) {
				FAIL("resealed GSQ page must pass its inner stored-digest check: ", inner_reason.utf8().get_data());
				return false;
			}
		}
	}
	{
		Ref<FileAccess> file = FileAccess::open(fixture.path, FileAccess::WRITE);
		if (file.is_null()) {
			FAIL("D3-2 corrupt-record test could not reopen its isolated temporary package");
			return false;
		}
		if (!file->store_buffer(bytes.ptr(), bytes.size())) {
			FAIL("D3-2 corrupt-record test could not write its isolated temporary package");
			return false;
		}
		file->flush();
		if (file->get_error() != OK) {
			FAIL("D3-2 corrupt-record test could not flush its isolated temporary package");
			return false;
		}
	}
	Vector<Gaussian> gaussians;
	Vector<Vector3> sh;
	if (!gs_resize_or_fail(gaussians, 1, "D3-2 corrupt-record preexisting gaussians") ||
			!gs_resize_or_fail(sh, 1, "D3-2 corrupt-record preexisting SH")) {
		FAIL("D3-2 corrupt-record test requires nonempty output to prove failure clears it");
		return false;
	}
	String reason;
	const Error error = p_store_path ? fixture.source->read_node_payload_from_store(store, 17, 1, 0,
			gaussians, sh, reason) : fixture.source->read_node_payload(0, gaussians, sh, reason);
	const String expected = p_corruption == ENVELOPE ? "v4_payload_record_header_mismatch" :
			(p_corruption == CONTENT_GENERATION ? "v4_content_generation_mismatch" :
					(p_corruption == TRUNCATED ? "v4_payload_truncated" : "v4_payload_digest_mismatch"));
	const Error expected_error = p_corruption == TRUNCATED ? ERR_FILE_EOF : ERR_FILE_CORRUPT;
	if (error != expected_error || reason != expected || !gaussians.is_empty() || !sh.is_empty()) {
		FAIL("D3-2 V4 transport must preserve the base envelope/stamp/digest refusal and clear existing output: expected ",
				expected.utf8().get_data(), "; got ", reason.utf8().get_data());
		return false;
	}
	if (p_store_path) {
		const Dictionary telemetry = store.get_telemetry("hlod");
		CHECK(int64_t(telemetry.get("pages_decoded", -1)) == 0);
		CHECK(int64_t(telemetry.get("pages_stored_digest_covered_by_record", -1)) == 0);
		const PayloadPageStore::Stats stats = store.stats();
		if (stats.live_reservations != 0 || stats.reservations_acquired != stats.reservations_released ||
				stats.retained_bytes != 0 || stats.decoders_leased != 0) {
			FAIL("D3-2 corrupt V4 records must release every common-store page and decoder lease");
			return false;
		}
	}
	return true;
}

static bool write_package_bytes(const String &p_path, const Vector<uint8_t> &p_bytes) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		FAIL("runtime verification test could not open its isolated package for writing");
		return false;
	}
	if (!file->store_buffer(p_bytes.ptr(), p_bytes.size())) {
		FAIL("runtime verification test could not write its isolated package");
		return false;
	}
	file->flush();
	if (file->get_error() != OK) {
		FAIL("runtime verification test could not flush its isolated package");
		return false;
	}
	return true;
}

static void sha256(const uint8_t *p_bytes, uint64_t p_size, uint8_t *r_digest) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(p_bytes, p_size);
	hash.finish(r_digest);
}

static void digest_consistent_invalid_payload() {
	TestHlodPageStore::Fixture fixture;
	if (!fixture.build(false)) {
		return;
	}
	if (fixture.source.is_null() || fixture.source->get_nodes().is_empty()) {
		FAIL("semantic validation test requires a writer-produced raw node");
		return;
	}
	Error read_error = ERR_BUG;
	Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(fixture.path, &read_error);
	const GaussianSplatHlodPagedSource::NodeRecord &record = fixture.source->get_nodes()[0];
	if (read_error != OK || uint64_t(bytes.size()) < gs_world_paged::kHeaderBytes ||
			record.offset > uint64_t(bytes.size()) || record.stored_bytes > uint64_t(bytes.size()) - record.offset ||
			record.stored_bytes < gs_world_paged::kPayloadRecordHeaderBytes + sizeof(Gaussian)) {
		FAIL("semantic validation fixture must contain a complete raw record and header");
		return;
	}
	const uint64_t index = decode_uint64(bytes.ptr() + 112u);
	const uint64_t page_count = decode_uint64(bytes.ptr() + 120u);
	const uint64_t records_bytes = uint64_t(fixture.node_count) * gs_world_paged::kIndexRecordBytes;
	if (page_count != 1u || fixture.node_count > gs_world_paged::kRecordsPerIndexPage ||
			index > uint64_t(bytes.size()) || gs_world_paged::kIndexPageHeaderBytes + records_bytes > uint64_t(bytes.size()) - index) {
		FAIL("semantic validation reseal helper requires the fixture's single complete index page");
		return;
	}
	// A malformed-input construction derived from real producer output. Repair
	// every digest, so only semantic validation can reject the NaN opacity.
	const uint64_t opacity = record.offset + gs_world_paged::kPayloadRecordHeaderBytes + offsetof(Gaussian, opacity);
	encode_uint32(0x7fc00000u, bytes.ptrw() + opacity);
	const uint64_t index_record = index + gs_world_paged::kIndexPageHeaderBytes;
	sha256(bytes.ptr() + record.offset, record.stored_bytes, bytes.ptrw() + index_record + 192u);
	sha256(bytes.ptr() + index_record, records_bytes, bytes.ptrw() + index + 32u);
	sha256(bytes.ptr() + index + 32u, 32u, bytes.ptrw() + 176u);
	sha256(bytes.ptr(), gs_world_paged::kHeaderBytes - 32u, bytes.ptrw() + gs_world_paged::kHeaderBytes - 32u);
	if (!write_package_bytes(fixture.path, bytes)) {
		return;
	}
	Ref<GaussianSplatHlodPagedSource> source;
	source.instantiate();
	if (source.is_null()) {
		FAIL("semantic validation test could not allocate a source");
		return;
	}
	GaussianSplatHlodTree tree;
	Dictionary metadata;
	String reason;
	if (source->open(fixture.path, tree, metadata, reason) != OK) {
		FAIL("digest-consistent semantic fixture must open before loading a node: ", reason.utf8().get_data());
		return;
	}
	PayloadPageStore store;
	PayloadPageStore::Config config;
	config.reader_count = 1;
	config.max_idle_handles = 0;
	config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
	source->_test_set_runtime_full_payload_validation(true);
	if (store.start(config) != OK || source->install_in_page_store(store, 1) != OK) {
		FAIL("semantic validation test requires an installed runtime source");
		return;
	}
	Vector<Gaussian> gaussians;
	Vector<Vector3> sh;
	// Explicit ON exercises semantic rejection independently of global settings.
	CHECK(source->read_node_payload_from_store(store, 17, 1, 0, gaussians, sh, reason) == ERR_FILE_CORRUPT);
	CHECK(reason.begins_with("v4_payload_invalid: "));
	CHECK(gaussians.is_empty());
	CHECK(sh.is_empty());
	source->_test_set_runtime_full_payload_validation(false);
	if (source->read_node_payload_from_store(store, 17, 2, 0, gaussians, sh, reason) != OK ||
			gaussians.size() != fixture.splats_per_node) {
		FAIL("explicitly disabled semantic validation must expose the digest-consistent payload: ", reason.utf8().get_data());
		return;
	}
	uint32_t opacity_bits = 0;
	std::memcpy(&opacity_bits, &gaussians[0].opacity, sizeof(opacity_bits));
	CHECK(opacity_bits == 0x7fc00000u);
	// The runtime override must never weaken the offline/import reader.
	CHECK(source->read_node_payload(0, gaussians, sh, reason) == ERR_FILE_CORRUPT);
	CHECK(reason.begins_with("v4_payload_invalid: "));
	CHECK(gaussians.is_empty());
	CHECK(sh.is_empty());
	source->_test_set_runtime_full_payload_validation(true);
	CHECK(source->read_node_payload_from_store(store, 17, 3, 0, gaussians, sh, reason) == ERR_FILE_CORRUPT);
	CHECK(reason.begins_with("v4_payload_invalid: "));
	CHECK(gaussians.is_empty());
	CHECK(sh.is_empty());
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3StoreIdentity] codec-none base payload and raw/quantized records match independent goldens") {
	identity(false, false);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3StoreIdentity] GSQ base payload and raw/quantized records match independent goldens") {
	identity(true, false);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3StoreIdentity] codec-none common-store payload and raw/quantized records match independent goldens") {
	identity(false, true);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3StoreIdentity] GSQ common-store payload and raw/quantized records match independent goldens") {
	identity(true, true);
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][D3StoreIdentity] common store preserves V4 envelope generation and stored-digest refusals without output") {
	// The old path is the phase-1 positive control; the new path must retain
	// precisely the same reasons and output-clearing contract in phase 2.
	for (bool store_path : { false, true }) {
		for (bool gsq : { false, true }) {
			for (Corruption corruption : { ENVELOPE, CONTENT_GENERATION, STORED_BODY }) {
				CAPTURE(store_path);
				CAPTURE(gsq);
				CAPTURE(corruption);
				if (!corrupt_record(gsq, store_path, corruption)) {
					return;
				}
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][RuntimeVerify] full runtime payload validation defaults on") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings) {
		FAIL("runtime validation default requires ProjectSettings");
		return;
	}
	const String key = "rendering/gaussian_splatting/hlod/runtime_full_payload_validation";
	CHECK(settings->has_setting(key));
	const Variant initial = settings->property_get_revert(key);
	CHECK(initial.get_type() == Variant::BOOL);
	CHECK(bool(initial));
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][RuntimeVerify] cheap GSQ runtime loads reject all record-byte corruption without output") {
	for (Corruption corruption : { ENVELOPE, GSQ_DIGEST_FIELD, GSQ_FRAME, GSQ_RESEALED_FRAME }) {
		CAPTURE(corruption);
		if (!corrupt_record(true, true, corruption, false)) {
			return;
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][RuntimeVerify] cheap runtime loads reject a short record without a file-size preflight") {
	for (bool gsq : { false, true }) {
		CAPTURE(gsq);
		if (!corrupt_record(gsq, true, TRUNCATED, false)) {
			return;
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][RuntimeVerify] appended bytes cannot change a runtime node and offline length checks remain strict") {
	for (bool gsq : { false, true }) {
		CAPTURE(gsq);
		TestHlodPageStore::Fixture fixture;
		if (!fixture.build(gsq)) {
			return;
		}
		if (fixture.source.is_null()) {
			FAIL("appended-byte test requires its writer-produced source");
			return;
		}
		PayloadPageStore store;
		PayloadPageStore::Config config;
		config.reader_count = 1;
		config.max_idle_handles = 0;
		config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		fixture.source->_test_set_runtime_full_payload_validation(false);
		if (store.start(config) != OK || fixture.source->install_in_page_store(store, 1) != OK) {
			FAIL("appended-byte test requires an installed runtime source");
			return;
		}
		Error read_error = ERR_BUG;
		Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(fixture.path, &read_error);
		if (read_error != OK || bytes.is_empty()) {
			FAIL("appended-byte test must read the original package");
			return;
		}
		if (!gs_resize_or_fail(bytes, uint64_t(bytes.size()) + 1u, "runtime appended byte")) {
			FAIL("appended-byte test could not grow the isolated package");
			return;
		}
		bytes.ptrw()[bytes.size() - 1] = 0xa5u;
		if (!write_package_bytes(fixture.path, bytes)) {
			return;
		}
		Vector<Gaussian> actual, expected;
		Vector<Vector3> sh, expected_sh;
		String reason;
		for (uint32_t node = 0; node < fixture.node_count; ++node) {
			CAPTURE(node);
			if (fixture.source->read_node_payload_from_store(store, 17, node + 1u, node, actual, sh, reason) != OK ||
					!fixture.expected_payload(node, expected, expected_sh)) {
				FAIL("appending bytes outside the installed catalog must preserve the runtime node: ", reason.utf8().get_data());
				return;
			}
			// Includes leaves: disabling checks must retain centre reconstruction.
			if (!check_packed(fixture, node, actual, sh, expected, expected_sh)) {
				return;
			}
		}
		CHECK(fixture.source->read_node_payload(0, actual, sh, reason) == ERR_FILE_CORRUPT);
		CHECK(reason == "v4_file_changed_length");
		CHECK(actual.is_empty());
		CHECK(sh.is_empty());
	}
}

TEST_CASE("[GaussianSplatting][HLOD][PagedV4][WorldIO][RuntimeVerify] semantic validation rejects a digest-consistent invalid payload and remains mandatory offline") {
	digest_consistent_invalid_payload();
}

} // namespace TestHlodPageStoreIdentity
