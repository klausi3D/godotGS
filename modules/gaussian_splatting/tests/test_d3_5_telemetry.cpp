extern "C" int test_d3_5_telemetry_cpp_force_link() { return 0; }

#include "gs_hlod_page_store_test_fixture.h"
#include "gs_payload_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/hlod_node_streamer.h"
#include "../renderer/render_debug_state_orchestrator.h"
#include "../renderer/render_diagnostics_orchestrator.h"
#include "core/io/json.h"
#include "core/os/thread.h"

#include <initializer_list>

// D3-5 phase 1: only existing C++ APIs; the future export is accessed by string.
// The CPU sink replaces device writes, not transport, workers, packing or export.
namespace TestD35Telemetry {

using TestGaussianSplatting::gs_pump_until;

static bool dictionary(const Dictionary &p_parent, const char *p_key, Dictionary &r_out) {
	if (!p_parent.has(p_key) || p_parent[p_key].get_type() != Variant::DICTIONARY) {
		FAIL("D3-5 required exported Dictionary is absent: ", p_key);
		return false;
	}
	r_out = p_parent[p_key];
	return true;
}

static int64_t number(const Dictionary &p_block, const char *p_key) {
	if (!p_block.has(p_key) || p_block[p_key].get_type() != Variant::INT) {
		FAIL("D3-5 required integer measurement is absent: ", p_key);
		return -1;
	}
	return int64_t(p_block[p_key]);
}

static void common(const Dictionary &p_block, const char *p_route, uint64_t p_generation) {
	CHECK(String(p_block.get("route", "")) == p_route);
	// Epoch is the immutable source instance, independently read by the harness;
	// neither the checker nor the capture relabels cumulative old-source counters.
	CHECK(number(p_block, "source_generation") == int64_t(p_generation));
	CHECK(number(p_block, "collection_epoch") == int64_t(p_generation));
	for (const char *key : { "captures", "captures_on_worker_threads", "pages_decoded", "splats_decoded",
			"pages_decoded_on_worker_threads", "stored_bytes_read", "read_requests", "decode_nsec" }) {
		CHECK_MESSAGE(number(p_block, key) > 0, key);
	}
	for (const char *key : { "captures_failed", "captures_on_main_thread", "pages_decoded_on_main_thread",
			"page_failures", "read_failures" }) {
		CHECK_MESSAGE(number(p_block, key) == 0, key);
	}
	CHECK(p_block.has("last_failure_reason"));
	CHECK(String(p_block.get("last_failure_reason", "absent")).is_empty());
	CHECK(String(p_block.get("verify_scope", "")).length() > 0);
	CHECK(String(p_block.get("stage_time_scope", "")) == "worker_cpu_sum");
	for (const char *key : { "verify_nsec", "decode_remainder_nsec", "read_latency_nsec", "read_queue_nsec",
			"pages_decoded_digest_checked", "pages_decoded_digest_skipped" }) {
		CHECK_MESSAGE(number(p_block, key) >= 0, key);
	}
	CHECK(number(p_block, "captures") == number(p_block, "captures_on_worker_threads") + number(p_block, "captures_on_main_thread"));
	CHECK(number(p_block, "pages_decoded") == number(p_block, "pages_decoded_on_worker_threads") + number(p_block, "pages_decoded_on_main_thread"));
	CHECK(number(p_block, "pages_decoded") == number(p_block, "pages_decoded_digest_checked") + number(p_block, "pages_decoded_digest_skipped"));
	const String digest = p_block.get("digest_status", "absent");
	CHECK((digest == "checked" || digest == "skipped"));
	if (digest == "skipped") {
		CHECK(number(p_block, "pages_decoded_digest_checked") == 0);
		CHECK(number(p_block, "pages_decoded_digest_skipped") > 0);
		CHECK(p_block.has("digest_nsec"));
		CHECK(p_block.get("digest_nsec", -1).get_type() == Variant::NIL);
	} else if (digest == "checked") {
		CHECK(number(p_block, "pages_decoded_digest_checked") > 0);
		CHECK(number(p_block, "pages_decoded_digest_skipped") == 0);
		CHECK(number(p_block, "digest_nsec") > 0);
	}
	const int64_t digest_nsec = digest == "checked" ? number(p_block, "digest_nsec") : 0;
	CHECK(number(p_block, "decode_nsec") == number(p_block, "verify_nsec") + number(p_block, "decode_remainder_nsec") + digest_nsec);
	// #1441: no GPU measurement exists for this CPU producer. Measured CPU queue
	// bytes may be zero; an unavailable VRAM measurement may not masquerade as 0.
	if (p_block.has("gpu_memory_usage_mb")) {
		CHECK(p_block["gpu_memory_usage_mb"].get_type() == Variant::NIL);
	}
}

static void capture(const char *p_name, const Dictionary &p_report) {
	const String text = JSON::stringify(p_report, "\t");
	if (DirAccess::make_dir_recursive_absolute("tests/output/d3_5") != OK) {
		FAIL("D3-5 cannot create producer capture directory");
		return;
	}
	// baseline_qa.yml uploads root *test_results.json AFTER the strict CPU lane.
	for (const String &path : { String(p_name), String("tests/output/d3_5").path_join(p_name) }) {
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			FAIL("D3-5 cannot write raw producer capture: ", path.utf8().get_data());
			return;
		}
		file->store_string(text);
		file->flush();
		CHECK(file->get_error() == OK);
	}
}

struct HlodRun {
	// Owner teardown is explicit: streamer -> store -> renderer -> fixture.
	TestHlodPageStore::Fixture fixture;
	Ref<GaussianSplatRenderer> renderer;
	HlodNodeStreamer::Config config;
	HlodNodeStreamer::TestControl control;
	gs_hlod_runtime::DesiredRecord demand;

	~HlodRun() {
		if (renderer.is_valid()) {
			auto &state = renderer->get_resource_state();
			if (state.hlod_streamer.is_valid()) { state.hlod_streamer->shutdown(); }
			state.hlod_streamer.unref();
			state.hlod_page_store.reset();
		}
	}

	HlodNodeStreamer &streamer() { return *renderer->get_resource_state().hlod_streamer.ptr(); }
	PayloadPageStore &store() { return *renderer->get_resource_state().hlod_page_store; }

	bool init() {
		if (!WorkerThreadPool::get_singleton() || !fixture.build(true, 1, 64, 0)) {
			FAIL("D3-5 needs an actual V4 GSQ source and worker pool");
			return false;
		}
		renderer.instantiate();
		if (renderer.is_null() || !renderer->debug_state_orchestrator) {
			FAIL("D3-5 renderer diagnostics must initialize without a GPU");
			return false;
		}
		auto &state = renderer->get_resource_state();
		state.hlod_page_store = std::make_unique<PayloadPageStore>();
		PayloadPageStore::Config store_config;
		store_config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		if (state.hlod_page_store->start(store_config) != OK) {
			FAIL("D3-5 real page store must start");
			return false;
		}
		state.hlod_streamer.instantiate();
		if (state.hlod_streamer.is_null()) {
			FAIL("D3-5 streamer allocation failed");
			return false;
		}
		config.atlas_pages = fixture.node_count;
		config.staging_upload_clamp_bytes = 1024; // Per-instance test slice; runtime defaults stay frozen.
		if (streamer().init_with_page_store(fixture.snapshot, config, 21, 41, store()) != OK) {
			FAIL("D3-5 real HLOD streamer must use the common store");
			return false;
		}
		state.hlod_streaming = true;
		state.hlod_snapshot = fixture.snapshot;
		control.cpu_upload_sink = true;
		control.hold_mailbox = true;
		control.hold_uploads = true;
		return set_control();
	}

	bool set_control() {
		if (streamer()._test_set_control(control) != OK) {
			FAIL("D3-5 CPU sink controls must reach the real streamer");
			return false;
		}
		return true;
	}

	void step() {
		streamer().begin_frame();
		streamer().update(nullptr, HlodNodeStreamer::Buffers(), &demand, 1, nullptr, 0);
	}

	template <typename Predicate>
	bool until(const char *p_condition, Predicate p_ready) {
		const auto outcome = gs_pump_until([&]() { step(); return p_ready(); }, 10000000, 0);
		if (!outcome.ready()) {
			FAIL("D3-5 timed out waiting for ", p_condition, ": ", outcome.describe().utf8().get_data());
			return false;
		}
		return true;
	}

	Dictionary stats() {
		RenderDiagnosticsOrchestrator::Dependencies deps;
		deps.renderer = renderer.ptr();
		deps.debug_state_orchestrator = renderer->debug_state_orchestrator.get();
		deps.build_device_capability_report = []() { return Dictionary(); };
		RenderDiagnosticsOrchestrator diagnostics(deps);
		return diagnostics.build_render_stats(); // Actual renderer export, never a hand-built telemetry block.
	}
};

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Telemetry] D3-5 real HLOD export covers stages queues transfers and replacement epoch") {
	// Both source files outlive the streamer's worker drain, including on failure.
	TestHlodPageStore::Fixture replacement;
	HlodRun run;
	if (!run.init()) { return; }
	if (!run.until("packed output in mailbox", [&]() { return run.streamer()._test_snapshot().mailbox_bytes > 0; })) { return; }
	const auto mailbox_sample = run.streamer()._test_snapshot();
	const Dictionary mailbox_stats = run.stats();
	run.control.hold_mailbox = false;
	if (!run.set_control() || !run.until("packed output in ready queue", [&]() { return run.streamer()._test_snapshot().ready_bytes > 0; })) { return; }
	const auto ready_sample = run.streamer()._test_snapshot();
	const Dictionary ready_stats = run.stats();
	const uint64_t held_since = OS::get_singleton()->get_ticks_usec();
	if (!run.until("20 ms residence in ready queue", [&]() { return OS::get_singleton()->get_ticks_usec() - held_since >= 20000; })) { return; }
	run.control.hold_uploads = false;
	if (!run.set_control() || !run.until("first partial upload", [&]() { return run.streamer()._test_snapshot().partial_upload_bytes > 0; })) { return; }
	const Dictionary partial_stats = run.stats();
	run.control.hold_uploads = true;
	if (!run.set_control()) { return; }
	const uint64_t transfer_since = OS::get_singleton()->get_ticks_usec();
	if (!run.until("20 ms between first and final upload slices", [&]() { return OS::get_singleton()->get_ticks_usec() - transfer_since >= 20000; })) { return; }
	run.control.hold_uploads = false;
	if (!run.set_control() || !run.until("root residency after final CPU sink upload", [&]() { return run.streamer().get_telemetry().resident == 1; })) { return; }
	const Dictionary completed_stats = run.stats();
	const uint64_t first_generation = uint64_t(run.fixture.source->get_instance_id());
	// A different source object, not merely the same file reopened under an old epoch.
	if (!replacement.build(true, 1, 64, 0) || run.streamer()._test_replace_source(replacement.snapshot, 22, 41) != OK) {
		FAIL("D3-5 source replacement must install a new real V4 source");
		return;
	}
	run.renderer->get_resource_state().hlod_snapshot = replacement.snapshot;
	if (!run.until("replacement root residency", [&]() { return run.streamer().get_telemetry().resident == 1; })) { return; }
	const Dictionary replaced_stats = run.stats();
	const uint64_t second_generation = uint64_t(replacement.source->get_instance_id());
	Dictionary report;
	report["source_generation"] = int64_t(first_generation);
	report["replacement_source_generation"] = int64_t(second_generation);
	report["mailbox"] = mailbox_stats;
	report["ready"] = ready_stats;
	report["partial"] = partial_stats;
	report["completed"] = completed_stats;
	report["replaced"] = replaced_stats;
	capture("d3_5_hlod_test_results.json", report); // Capture even a red export; never bless it as a fixture.
	CHECK(run.store().stats().pages_decoded > 0);
	CHECK(run.store().stats().pages_decoded_on_worker_threads > 0);
	Dictionary mailbox, ready, partial, completed, replaced;
	if (!dictionary(mailbox_stats, "payload_telemetry", mailbox) || !dictionary(ready_stats, "payload_telemetry", ready) ||
			!dictionary(partial_stats, "payload_telemetry", partial) || !dictionary(completed_stats, "payload_telemetry", completed) ||
			!dictionary(replaced_stats, "payload_telemetry", replaced)) { return; }
	common(mailbox, "hlod", first_generation);
	common(ready, "hlod", first_generation);
	common(partial, "hlod", first_generation);
	common(completed, "hlod", first_generation);
	common(replaced, "hlod", second_generation);
	CHECK(number(completed, "pages_decoded") == 1);
	CHECK(number(replaced, "pages_decoded") == 1); // No relabelled old-source totals.
	CHECK(number(completed, "splats_decoded") == 64);
	CHECK(number(replaced, "splats_decoded") == 64);
	CHECK(number(replaced, "collection_epoch") != number(completed, "collection_epoch"));
	CHECK(number(mailbox, "mailbox_bytes") == int64_t(mailbox_sample.mailbox_bytes));
	CHECK(number(ready, "ready_bytes") == int64_t(ready_sample.ready_bytes));
	CHECK(number(ready, "packed_bytes") >= number(ready, "ready_bytes"));
	for (const char *owner : { "packed", "ready", "mailbox", "active", "stored", "shadow" }) {
		const String key = String(owner) + "_bytes";
		const String peak = key + "_peak";
		CHECK(number(completed, key.utf8().get_data()) == 0);
		CHECK(number(completed, peak.utf8().get_data()) >= number(ready, key.utf8().get_data()));
	}
	CHECK(number(completed, "mailbox_bytes_peak") >= int64_t(mailbox_sample.mailbox_bytes));
	CHECK(number(completed, "stored_bytes_peak") > 0);
	CHECK(number(completed, "active_bytes_peak") > 0);
	CHECK(number(completed, "pack_calls") > 0);
	CHECK(number(completed, "pack_nsec") > 0); // Pack is not included in decode_nsec.
	CHECK(number(completed, "queue_residence_samples") > 0);
	CHECK(number(completed, "queue_residence_nsec") >= 20000000);
	CHECK(number(completed, "upload_completion_nsec") >= 20000000);
	CHECK(number(completed, "upload_completions") == 1);
	CHECK(number(completed, "upload_slices") > 1);
	CHECK(number(completed, "upload_bytes_this_frame") >= 0);
	uint64_t atlas_bytes = 0, atlas_slices = 0;
	for (const auto &publication : run.streamer()._test_publications()) {
		if (publication.source_instance_id != first_generation) { continue; }
		for (const auto &write : publication.writes) {
			if (write.kind == HlodNodeStreamer::TestWrite::ATLAS) { atlas_bytes += write.bytes; ++atlas_slices; }
		}
	}
	CHECK(atlas_bytes > 0);
	CHECK(number(completed, "upload_bytes") == int64_t(atlas_bytes));
	CHECK(number(completed, "upload_slices") == int64_t(atlas_slices));
}

struct ChunkCapture {
	Ref<GsqPagedChunkPayloadSource> source;
	bool ok = false;
	static void work(void *p_user) {
		auto &self = *static_cast<ChunkCapture *>(p_user);
		LocalVector<Gaussian> gaussians;
		LocalVector<Vector3> sh;
		uint32_t first = 0, high = 0;
		self.ok = self.source->capture_chunk_snapshot(0, self.source->get_count(), gaussians, sh, first, high);
	}
};

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Telemetry] D3-5 real chunk export labels checked and skipped digest work") {
	TestPayloadPageStore::Fixture fixture("d3_5_telemetry");
	if (!fixture.valid) { FAIL("D3-5 real chunk producer fixture failed"); return; }
	Dictionary report;
	for (bool checked : { true, false }) {
		ChunkCapture run;
		run.source.instantiate();
		if (run.source.is_null() || !run.source->configure(fixture.path, fixture.count, fixture.sh_degree,
				fixture.sh_first_order, fixture.sh_high_order, fixture.bounds, fixture.codec_params, fixture.pages)) {
			FAIL("D3-5 real chunk source must configure"); return;
		}
		run.source->set_verify_decoded_digest(checked);
		Thread worker;
		worker.start(ChunkCapture::work, &run);
		if (!worker.is_started()) { FAIL("D3-5 capture worker must start"); return; }
		worker.wait_to_finish();
		if (!run.ok) { FAIL("D3-5 real worker chunk capture failed"); return; }
		const Dictionary stats = run.source->get_decode_stats();
		Dictionary record;
		record["source_generation"] = int64_t(uint64_t(run.source->get_instance_id()));
		record["payload_telemetry"] = stats;
		report[checked ? "checked" : "skipped"] = record;
		common(stats, "chunk", uint64_t(run.source->get_instance_id()));
		CHECK(String(stats.get("digest_status", "absent")) == (checked ? "checked" : "skipped"));
	}
	capture("d3_5_chunk_test_results.json", report);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][D3Telemetry] D3-5 independent IO and decode delays stay in their measured stages") {
	Dictionary blocks;
	for (const char *arm : { "control", "io", "decode" }) {
		HlodRun run;
		if (!run.init()) { return; }
		// Phase 2 test-only seam, dynamically bound under TESTS_ENABLED. It must
		// delay the actual reader/decode work (never fabricate counters). No new
		// C++ symbol is referenced, so the base compiles and fails an assertion.
		if (!run.renderer->has_method("_test_set_payload_telemetry_delays")) {
			FAIL("D3-5 independent reader/decode delay hook is not implemented"); return;
		}
		Dictionary delays;
		delays["read_usec"] = String(arm) == "io" ? 100000 : 0;
		delays["decode_usec"] = String(arm) == "decode" ? 100000 : 0;
		const Variant acknowledgement = run.renderer->call("_test_set_payload_telemetry_delays", delays);
		if (acknowledgement.get_type() != Variant::INT || int64_t(acknowledgement) != OK) {
			FAIL("D3-5 delay hook did not accept its real-work controls"); return;
		}
		run.control.hold_mailbox = false;
		run.control.hold_uploads = false;
		const uint64_t started = OS::get_singleton()->get_ticks_usec();
		if (!run.set_control() || !run.until("delayed root residency", [&]() { return run.streamer().get_telemetry().resident == 1; })) { return; }
		if (String(arm) != "control") {
			CHECK(OS::get_singleton()->get_ticks_usec() - started >= 100000); // A stats-only fake delay cannot pass.
		}
		Dictionary block;
		if (!dictionary(run.stats(), "payload_telemetry", block)) { return; }
		common(block, "hlod", uint64_t(run.fixture.source->get_instance_id()));
		blocks[arm] = block;
	}
	capture("d3_5_delays_test_results.json", blocks);
	Dictionary control, io, decode;
	if (!dictionary(blocks, "control", control) || !dictionary(blocks, "io", io) || !dictionary(blocks, "decode", decode)) { return; }
	// A measured applied duration proves the hook fired inside the timed work.
	// One tiny root node per arm keeps worker sums and one read comparable.
	CHECK(number(io, "test_read_delay_calls") == 1);
	CHECK(number(decode, "test_decode_delay_calls") == 1);
	CHECK(number(io, "test_read_delay_nsec") >= 100000000);
	CHECK(number(decode, "test_decode_delay_nsec") >= 100000000);
	CHECK(number(io, "read_latency_nsec") >= number(io, "test_read_delay_nsec"));
	CHECK(number(decode, "decode_nsec") >= number(decode, "test_decode_delay_nsec"));
	CHECK(number(io, "decode_nsec") - number(control, "decode_nsec") < 50000000);
	CHECK(number(decode, "read_latency_nsec") - number(control, "read_latency_nsec") < 50000000);
	CHECK(number(decode, "pack_nsec") - number(control, "pack_nsec") < 50000000);
	CHECK(number(io, "test_decode_delay_calls") == 0);
	CHECK(number(decode, "test_read_delay_calls") == 0);
}

} // namespace TestD35Telemetry
