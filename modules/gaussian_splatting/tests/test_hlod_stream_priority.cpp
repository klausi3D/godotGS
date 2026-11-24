extern "C" int test_hlod_stream_priority_cpp_force_link() { return 0; }

// HLOD streaming service order (ADR adr-hlod-streaming.md 6.6: missing children are requested "in
// refinement-priority order"). The streamer must serve the CURRENT frame's priority, not the order in which
// requests or IO completions happened to arrive: a near region's transition, demanded after a far one, must
// not wait behind it in the read queue or in the upload queue.
//
// Real streamer, real worker pool, real page store and residency; the CPU upload sink only replaces the
// device writes. One worker slot makes the read order observable: completion order equals start order.

#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../io/gs_payload_page_store.h"
#include "../renderer/hlod_node_streamer.h"
#include "core/os/os.h"

namespace TestHlodStreamPriority {

using Streamer = HlodNodeStreamer;
using TestGaussianSplatting::gs_pump_until;

// Binary fixture tree of depth 3: root 0, interiors 1..6, leaves 7..14. The two leaf groups below are whole
// transitions (siblings of one parent): near = children of node 3, far = children of node 6.
static constexpr uint32_t kNearA = 7, kNearB = 8, kFarA = 13, kFarB = 14;

struct Harness {
	TestHlodPageStore::Fixture fixture;
	PayloadPageStore store;
	Streamer streamer;
	Streamer::Config cfg;
	Streamer::TestControl control;

	~Harness() { streamer.shutdown(); store.shutdown(); }

	bool init(uint64_t p_upload_bytes = 32ull * 1024 * 1024) {
		if (!WorkerThreadPool::get_singleton() || !fixture.build(false, 3, 256, 0) || fixture.node_count != 15) {
			FAIL("priority harness needs the real worker pool and a 15-node production-written tree"); return false;
		}
		cfg.atlas_pages = fixture.node_count * ((fixture.splats_per_node + cfg.page_splats - 1) / cfg.page_splats);
		cfg.upload_bytes_per_frame = p_upload_bytes;
		cfg.logical_cpus = 32;
		cfg.pool_threads = 1; // One pack slot: the read order is the start order.
		PayloadPageStore::Config sc;
		sc.reader_count = 1;
		sc.max_bytes_in_flight = cfg.max_inflight_read_bytes;
		sc.max_idle_handles = 0;
		sc.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
		if (store.start(sc) != OK || streamer.init_with_page_store(fixture.snapshot, cfg, 21, 41, store) != OK) {
			FAIL("priority harness: real streamer initialization failed"); return false;
		}
		control.cpu_upload_sink = true;
		return set_control();
	}
	bool set_control() {
		if (streamer._test_set_control(control) != OK) { FAIL("priority harness: test control unavailable"); return false; }
		return true;
	}
	static gs_hlod_runtime::DesiredRecord want(uint32_t p_node, float p_priority = 0.0f) {
		gs_hlod_runtime::DesiredRecord record;
		record.node = p_node;
		record.transition.priority = p_priority;
		return record;
	}
	void frame(const LocalVector<gs_hlod_runtime::DesiredRecord> &p_demand) {
		streamer.begin_frame();
		streamer.update(nullptr, Streamer::Buffers(), p_demand.ptr(), p_demand.size(), nullptr, 0);
	}
	Vector<uint32_t> published() const {
		Vector<uint32_t> order;
		for (const Streamer::TestPublication &p : streamer._test_publications()) { order.push_back(p.node); }
		return order;
	}
	static int position(const Vector<uint32_t> &p_order, uint32_t p_node) {
		for (int i = 0; i < p_order.size(); ++i) {
			if (p_order[i] == p_node) { return i; }
		}
		return -1;
	}
	static String describe(const Vector<uint32_t> &p_order) {
		String out;
		for (uint32_t node : p_order) { out += (out.is_empty() ? "" : ",") + itos(node); }
		return out;
	}
};

TEST_CASE("[GaussianSplatting][HLOD][StreamPriority] queued reads are served in the current demand priority, not request order") {
	Harness h;
	if (!h.init()) { return; }
	// Frame 1: only the far transition is wanted. Its first read takes the single slot and is held there; the
	// sibling waits in the read queue.
	h.control.hold_workers = true;
	if (!h.set_control()) { return; }
	LocalVector<gs_hlod_runtime::DesiredRecord> far_only;
	far_only.push_back(Harness::want(kFarA));
	far_only.push_back(Harness::want(kFarB));
	h.frame(far_only);
	const auto started = gs_pump_until([&]() {
		h.streamer.update(nullptr, Streamer::Buffers(), far_only.ptr(), far_only.size(), nullptr, 0);
		return h.streamer._test_snapshot().jobs_started >= 1;
	});
	if (!started.ready()) { FAIL("the far transition never started its first read: ", started.describe().utf8().get_data()); return; }
	// Frame 2: the camera has moved; the near transition is now the most important one (listed first, ADR 6.6),
	// the far one is still wanted behind it.
	LocalVector<gs_hlod_runtime::DesiredRecord> near_first;
	near_first.push_back(Harness::want(kNearA));
	near_first.push_back(Harness::want(kNearB));
	near_first.push_back(Harness::want(kFarA));
	near_first.push_back(Harness::want(kFarB));
	h.frame(near_first);
	h.control.hold_workers = false;
	if (!h.set_control()) { return; }
	Vector<uint32_t> order;
	const auto done = gs_pump_until([&]() {
		h.frame(near_first);
		order = h.published();
		return order.size() >= 4;
	});
	if (!done.ready()) { FAIL("not every node published: ", Harness::describe(order).utf8().get_data()); return; }
	const CharString described = Harness::describe(order).utf8();
	INFO("publication order: ", described.get_data());
	CHECK(order.size() == 4);
	CHECK(Harness::position(order, kFarA) == 0); // Already reading before the near demand existed: not preempted.
	const int far_b = Harness::position(order, kFarB);
	CHECK(far_b >= 0);
	CHECK(Harness::position(order, kNearA) >= 0);
	CHECK(Harness::position(order, kNearB) >= 0);
	CHECK(Harness::position(order, kNearA) < far_b);
	CHECK(Harness::position(order, kNearB) < far_b);
}

TEST_CASE("[GaussianSplatting][HLOD][StreamPriority] decoded payloads upload in the current demand priority, not IO completion order") {
	// Room for one node and its metadata per frame, not for two: whole-node admission serves one node per frame.
	const uint64_t node_bytes = 256ull * sizeof(PackedGaussian);
	Harness h;
	if (!h.init(node_bytes + node_bytes / 2)) { return; }
	h.control.hold_uploads = true;
	if (!h.set_control()) { return; }
	LocalVector<gs_hlod_runtime::DesiredRecord> far_only;
	far_only.push_back(Harness::want(kFarA));
	far_only.push_back(Harness::want(kFarB));
	const auto far_ready = gs_pump_until([&]() {
		h.frame(far_only);
		return h.streamer._test_snapshot().ready_outputs >= 2;
	});
	if (!far_ready.ready()) { FAIL("far payloads never decoded: ", far_ready.describe().utf8().get_data()); return; }
	LocalVector<gs_hlod_runtime::DesiredRecord> near_first;
	near_first.push_back(Harness::want(kNearA));
	near_first.push_back(Harness::want(kNearB));
	near_first.push_back(Harness::want(kFarA));
	near_first.push_back(Harness::want(kFarB));
	const auto all_ready = gs_pump_until([&]() {
		h.frame(near_first);
		return h.streamer._test_snapshot().ready_outputs >= 4;
	});
	if (!all_ready.ready()) { FAIL("near payloads never decoded: ", all_ready.describe().utf8().get_data()); return; }
	CHECK(h.published().is_empty());
	h.control.hold_uploads = false;
	if (!h.set_control()) { return; }
	Vector<uint32_t> order;
	const auto done = gs_pump_until([&]() {
		h.frame(near_first);
		order = h.published();
		return order.size() >= 4;
	});
	if (!done.ready()) { FAIL("not every node published: ", Harness::describe(order).utf8().get_data()); return; }
	const CharString described = Harness::describe(order).utf8();
	INFO("publication order: ", described.get_data());
	CHECK(order.size() == 4);
	CHECK(Harness::position(order, kNearA) == 0);
	CHECK(Harness::position(order, kNearB) == 1);
	CHECK(Harness::position(order, kFarA) == 2);
	CHECK(Harness::position(order, kFarB) == 3);
}

TEST_CASE("[GaussianSplatting][HLOD][StreamPriority] explicit priorities outrank demand position and are re-derived every frame") {
	Harness h;
	if (!h.init()) { return; }
	h.control.hold_workers = true;
	if (!h.set_control()) { return; }
	// Frame 1: listed far-first, but the near transition carries the larger projected error. The request order
	// (and with it the per-frame request cap) follows the priority, so the near node takes the single slot.
	LocalVector<gs_hlod_runtime::DesiredRecord> frame1;
	frame1.push_back(Harness::want(kFarA, 1.0f));
	frame1.push_back(Harness::want(kFarB, 1.0f));
	frame1.push_back(Harness::want(kNearA, 5.0f));
	frame1.push_back(Harness::want(kNearB, 5.0f));
	h.frame(frame1);
	const auto started = gs_pump_until([&]() {
		h.streamer.update(nullptr, Streamer::Buffers(), frame1.ptr(), frame1.size(), nullptr, 0);
		return h.streamer._test_snapshot().jobs_started >= 1;
	});
	if (!started.ready()) { FAIL("no read started: ", started.describe().utf8().get_data()); return; }
	// Frame 2: the camera turned; now the far transition matters more. What is still queued follows the NEW order.
	LocalVector<gs_hlod_runtime::DesiredRecord> frame2;
	frame2.push_back(Harness::want(kNearA, 1.0f));
	frame2.push_back(Harness::want(kNearB, 1.0f));
	frame2.push_back(Harness::want(kFarA, 5.0f));
	frame2.push_back(Harness::want(kFarB, 5.0f));
	h.frame(frame2);
	h.control.hold_workers = false;
	if (!h.set_control()) { return; }
	Vector<uint32_t> order;
	const auto done = gs_pump_until([&]() {
		h.frame(frame2);
		order = h.published();
		return order.size() >= 4;
	});
	if (!done.ready()) { FAIL("not every node published: ", Harness::describe(order).utf8().get_data()); return; }
	const CharString described = Harness::describe(order).utf8();
	INFO("publication order: ", described.get_data());
	CHECK(order.size() == 4);
	CHECK(Harness::position(order, kNearA) == 0); // Started in frame 1, when it was the most important.
	CHECK(Harness::position(order, kFarA) == 1);
	CHECK(Harness::position(order, kFarB) == 2);
	CHECK(Harness::position(order, kNearB) == 3);
}

// Decoded far payloads wait while near ones (larger error) decode; then uploads run one node per frame.
static bool run_upload_scenario(Harness &h, Vector<uint32_t> &r_order) {
	h.control.hold_uploads = true;
	if (!h.set_control()) { return false; }
	LocalVector<gs_hlod_runtime::DesiredRecord> far_only;
	far_only.push_back(Harness::want(kFarA, 1.0f));
	far_only.push_back(Harness::want(kFarB, 1.0f));
	const auto far_ready = gs_pump_until([&]() { h.frame(far_only); return h.streamer._test_snapshot().ready_outputs >= 2; });
	if (!far_ready.ready()) { FAIL("far payloads never decoded"); return false; }
	LocalVector<gs_hlod_runtime::DesiredRecord> both;
	both.push_back(Harness::want(kNearA, 5.0f));
	both.push_back(Harness::want(kNearB, 5.0f));
	both.push_back(Harness::want(kFarA, 1.0f));
	both.push_back(Harness::want(kFarB, 1.0f));
	const auto all_ready = gs_pump_until([&]() { h.frame(both); return h.streamer._test_snapshot().ready_outputs >= 4; });
	if (!all_ready.ready()) { FAIL("near payloads never decoded"); return false; }
	h.control.hold_uploads = false;
	if (!h.set_control()) { return false; }
	const auto done = gs_pump_until([&]() { h.frame(both); r_order = h.published(); return r_order.size() >= 4; });
	if (!done.ready()) { FAIL("not every node published"); return false; }
	return true;
}

TEST_CASE("[GaussianSplatting][HLOD][StreamPriority] the inversion metric is zero for priority service and discriminates the FIFO forcing control") {
	const uint64_t node_bytes = 256ull * sizeof(PackedGaussian);
	{
		Harness h;
		if (!h.init(node_bytes + node_bytes / 2)) { return; }
		CHECK_FALSE(h.streamer.get_telemetry().priority_fifo_forced);
		Vector<uint32_t> order;
		if (!run_upload_scenario(h, order)) { return; }
		CHECK(Harness::position(order, kNearA) == 0);
		const auto &t = h.streamer.get_telemetry();
		CHECK(t.priority_measured);
		CHECK(t.priority_uploads_total == 4);
		CHECK(t.priority_inversions_total == 0);
		CHECK(t.priority_ready_inversions_total == 0);
	}
	{
		// The pre-fix first-in-first-out service, forced. Both far uploads overtake both waiting near nodes; a metric
		// that stayed zero here would make the zero above meaningless.
		OS::get_singleton()->set_environment("GODOTGS_TEST_HLOD_FIFO_SERVICE", "1");
		Harness h;
		const bool initialized = h.init(node_bytes + node_bytes / 2);
		OS::get_singleton()->unset_environment("GODOTGS_TEST_HLOD_FIFO_SERVICE");
		if (!initialized) { return; }
		CHECK(h.streamer.get_telemetry().priority_fifo_forced);
		Vector<uint32_t> order;
		if (!run_upload_scenario(h, order)) { return; }
		CHECK(Harness::position(order, kFarA) == 0); // The forced old order really ran.
		const auto &t = h.streamer.get_telemetry();
		CHECK(t.priority_uploads_total == 4);
		CHECK(t.priority_inversions_total == 2);
		CHECK(t.priority_ready_inversions_total == 2);
	}
}

} // namespace TestHlodStreamPriority
