extern "C" int test_hlod_page_store_identity_cpp_force_link() { return 0; }

#include "gs_hlod_page_store_test_fixture.h"
#include "../io/gs_payload_page_store.h"
#include "../renderer/hlod_node_pack.h"
#include "core/io/file_access.h"

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
	}
	if (p_store_path) {
		const PayloadPageStore::Stats stats = store.stats();
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
	}
}

enum Corruption { ENVELOPE, CONTENT_GENERATION, STORED_BODY };

static bool corrupt_record(bool p_gsq, bool p_store_path, Corruption p_corruption) {
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
	const uint64_t within_record = p_corruption == ENVELOPE ? 0u :
			(p_corruption == CONTENT_GENERATION ? 32u : body);
	bytes.ptrw()[record.offset + within_record] ^= 1u;
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
			(p_corruption == CONTENT_GENERATION ? "v4_content_generation_mismatch" : "v4_payload_digest_mismatch");
	if (error != ERR_FILE_CORRUPT || reason != expected || !gaussians.is_empty() || !sh.is_empty()) {
		FAIL("D3-2 V4 transport must preserve the base envelope/stamp/digest refusal and clear existing output: expected ",
				expected.utf8().get_data(), "; got ", reason.utf8().get_data());
		return false;
	}
	if (p_store_path) {
		const PayloadPageStore::Stats stats = store.stats();
		if (stats.live_reservations != 0 || stats.reservations_acquired != stats.reservations_released ||
				stats.retained_bytes != 0 || stats.decoders_leased != 0) {
			FAIL("D3-2 corrupt V4 records must release every common-store page and decoder lease");
			return false;
		}
	}
	return true;
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

} // namespace TestHlodPageStoreIdentity
