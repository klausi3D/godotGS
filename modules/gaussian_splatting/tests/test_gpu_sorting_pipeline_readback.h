#pragma once

#include "test_macros.h"

#include "../interfaces/gpu_sorting_pipeline.h"
#include "../renderer/pipeline_io_contracts.h"

#include <cstring>

namespace TestGaussianSplatting {
namespace {

struct TestSortResultSink : public ISortResultSink {
	int publish_count = 0;
	SortPublicationPayload last_payload;

	void publish_sorted_indices(const SortPublicationPayload &p_payload) override {
		publish_count++;
		last_payload = p_payload;
	}
};

static Vector<uint8_t> _make_indirect_dispatch_payload(uint32_t p_element_count, uint32_t p_overflow_flag,
		uint32_t p_unclamped_total) {
	GaussianSplatting::IndirectDispatchLayout layout = {};
	layout.element_count = p_element_count;
	layout.overflow_flag = p_overflow_flag;
	layout.unclamped_total = p_unclamped_total;

	Vector<uint8_t> payload;
	payload.resize(int(sizeof(layout)));
	std::memcpy(payload.ptrw(), &layout, sizeof(layout));
	return payload;
}

static Vector<uint8_t> _make_sort_indices_payload(const Vector<uint32_t> &p_indices) {
	Vector<uint8_t> payload;
	payload.resize(int(p_indices.size() * sizeof(uint32_t)));
	if (!payload.is_empty()) {
		std::memcpy(payload.ptrw(), p_indices.ptr(), size_t(payload.size()));
	}
	return payload;
}

} // namespace

TEST_CASE("[GaussianSplatting][GPU Sort Pipeline] Stale sort readbacks are ignored after generation advances") {
	Ref<GPUSortingPipeline> pipeline;
	pipeline.instantiate();
	if (!pipeline.is_valid()) {
		FAIL("GPUSortingPipeline failed to instantiate");
		return;
	}

	auto &sort_state = pipeline->_test_sort_readback_state();
	sort_state.pending = true;
	sort_state.generation = 17;
	sort_state.expected_count = 4;
	sort_state.snapshot_indices.resize(4);
	sort_state.snapshot_indices.write[0] = 3;
	sort_state.snapshot_indices.write[1] = 1;
	sort_state.snapshot_indices.write[2] = 0;
	sort_state.snapshot_indices.write[3] = 2;
	TestSortResultSink sink;
	pipeline->set_sort_result_sink(&sink);

	const Vector<uint8_t> payload = _make_sort_indices_payload(sort_state.snapshot_indices);

	pipeline->shutdown();
	CHECK(sort_state.generation == 18);
	CHECK_FALSE(sort_state.pending);
	CHECK(pipeline->_test_get_sort_result_sink() == nullptr);

	sort_state.pending = true;
	sort_state.expected_count = 4;
	sort_state.snapshot_indices.resize(4);
	sort_state.snapshot_indices.write[0] = 3;
	sort_state.snapshot_indices.write[1] = 1;
	sort_state.snapshot_indices.write[2] = 0;
	sort_state.snapshot_indices.write[3] = 2;
	pipeline->set_sort_result_sink(&sink);

	pipeline->_on_sort_readback(payload, 17);
	CHECK(sort_state.pending);
	CHECK(sort_state.generation == 18);
	CHECK(sink.publish_count == 0);
	REQUIRE(sort_state.snapshot_indices.size() == 4);
	CHECK(sort_state.snapshot_indices[0] == 3);
	CHECK(sort_state.snapshot_indices[1] == 1);
	CHECK(sort_state.snapshot_indices[2] == 0);
	CHECK(sort_state.snapshot_indices[3] == 2);
}

TEST_CASE("[GaussianSplatting][GPU Sort Pipeline] Instance-count readbacks reject stale generations and accept the current one") {
	Ref<GPUSortingPipeline> pipeline;
	pipeline.instantiate();
	if (!pipeline.is_valid()) {
		FAIL("GPUSortingPipeline failed to instantiate");
		return;
	}

	auto &count_state = pipeline->_test_instance_count_readback_state();
	count_state.pending = true;
	count_state.generation = 33;
	count_state.pending_frame_counter = 71;
	pipeline->_test_set_last_instance_visible_splat_count_state(12, false, 0);

	const Vector<uint8_t> stale_payload = _make_indirect_dispatch_payload(27, 1, 99);
	pipeline->_on_instance_count_readback(stale_payload, 32);

	CHECK(count_state.pending);
	CHECK(count_state.generation == 33);
	CHECK(count_state.pending_frame_counter == 71);
	CHECK(pipeline->get_last_instance_visible_splat_count() == 12);
	CHECK_FALSE(pipeline->_test_get_last_instance_visible_splat_count_valid());
	CHECK(pipeline->_test_get_last_instance_visible_splat_count_frame() == 0);

	const Vector<uint8_t> current_payload = _make_indirect_dispatch_payload(27, 1, 99);
	pipeline->_on_instance_count_readback(current_payload, 33);

	CHECK_FALSE(count_state.pending);
	CHECK(count_state.generation == 33);
	CHECK(count_state.pending_frame_counter == 0);
	CHECK(pipeline->get_last_instance_visible_splat_count() == 27);
	CHECK(pipeline->_test_get_last_instance_visible_splat_count_valid());
	CHECK(pipeline->_test_get_last_instance_visible_splat_count_frame() == 71);
}

TEST_CASE("[GaussianSplatting][GPU Sort Pipeline] Instance-count overflow_flag raises the persistent overflow-event counter (C4b/G4)") {
	// C4b (exit criterion G4, "no silent degradation"), Channel B: when the instance-count
	// clamp shader raises overflow_flag, _on_instance_count_readback must bump the persistent
	// instance_count_overflow_events counter (and WARN_PRINT_ONCE). This is the CPU-side
	// contract; on-GPU provocation of a real clamp is the GPU-harness lane's job.
	Ref<GPUSortingPipeline> pipeline;
	pipeline.instantiate();
	if (!pipeline.is_valid()) {
		FAIL("GPUSortingPipeline failed to instantiate");
		return;
	}
	CHECK(pipeline->get_instance_count_overflow_events() == 0u);

	auto &count_state = pipeline->_test_instance_count_readback_state();

	// (1) An accepted, NON-overflowing readback must NOT bump the counter.
	count_state.pending = true;
	count_state.generation = 5;
	count_state.pending_frame_counter = 10;
	pipeline->_test_set_last_instance_visible_splat_count_state(0, false, 0);
	pipeline->_on_instance_count_readback(_make_indirect_dispatch_payload(64, 0, 64), 5);
	CHECK(pipeline->get_instance_count_overflow_events() == 0u);

	// (2) An accepted, overflowing readback (overflow_flag=1) bumps the counter once.
	count_state.pending = true;
	count_state.generation = 6;
	count_state.pending_frame_counter = 11;
	pipeline->_on_instance_count_readback(_make_indirect_dispatch_payload(64, 1, 4096), 6);
	CHECK(pipeline->get_instance_count_overflow_events() == 1u);

	// (3) A STALE overflowing readback is rejected on generation mismatch BEFORE the flag
	// check, so it must not bump the counter.
	count_state.pending = true;
	count_state.generation = 8;
	count_state.pending_frame_counter = 20;
	pipeline->_on_instance_count_readback(_make_indirect_dispatch_payload(64, 1, 4096), 7);
	CHECK(pipeline->get_instance_count_overflow_events() == 1u);
}

TEST_CASE("[GaussianSplatting][GPU Sort Pipeline] A single production frame counts an instance-count overflow exactly once (C4b/G4 de-dup)") {
	// C4b (G4): one production frame can be sampled by BOTH the sync bootstrap/recovery path
	// AND the async readback callback (which is accepted because request_frame equals the frame
	// the sync path already recorded). Both funnel through _note_instance_count_overflow(),
	// keyed on the frame counter, so the counter (and WARN) must fire at most ONCE for that
	// frame. Two accepted readbacks for the SAME frame counter model that sync+async double-hit;
	// without the frame-keyed de-dup this would count 2.
	Ref<GPUSortingPipeline> pipeline;
	pipeline.instantiate();
	if (!pipeline.is_valid()) {
		FAIL("GPUSortingPipeline failed to instantiate");
		return;
	}
	CHECK(pipeline->get_instance_count_overflow_events() == 0u);

	auto &count_state = pipeline->_test_instance_count_readback_state();

	// First sample for frame 30 (models the sync path recording the overflow).
	count_state.pending = true;
	count_state.generation = 40;
	count_state.pending_frame_counter = 30;
	pipeline->_test_set_last_instance_visible_splat_count_state(0, false, 0);
	pipeline->_on_instance_count_readback(_make_indirect_dispatch_payload(64, 1, 4096), 40);
	CHECK(pipeline->get_instance_count_overflow_events() == 1u);

	// Second sample for the SAME frame 30 (models the async callback for that frame). It is
	// accepted (request_frame 30 is not < the recorded frame 30) but must NOT re-count.
	count_state.pending = true;
	count_state.generation = 41;
	count_state.pending_frame_counter = 30;
	pipeline->_on_instance_count_readback(_make_indirect_dispatch_payload(64, 1, 4096), 41);
	CHECK(pipeline->get_instance_count_overflow_events() == 1u);

	// A later, distinct frame that also overflows increments again (per-frame, not global).
	count_state.pending = true;
	count_state.generation = 42;
	count_state.pending_frame_counter = 31;
	pipeline->_on_instance_count_readback(_make_indirect_dispatch_payload(64, 1, 4096), 42);
	CHECK(pipeline->get_instance_count_overflow_events() == 2u);
}

TEST_CASE("[GaussianSplatting][GPU Sort Pipeline] Clearing instance pipeline inputs resets readback ownership state") {
	Ref<GPUSortingPipeline> pipeline;
	pipeline.instantiate();
	if (!pipeline.is_valid()) {
		FAIL("GPUSortingPipeline failed to instantiate");
		return;
	}

	auto &count_state = pipeline->_test_instance_count_readback_state();
	count_state.pending = true;
	count_state.generation = 11;
	count_state.pending_frame_counter = 29;
	count_state.bootstrap_sync_attempted = true;
	pipeline->_test_set_last_instance_visible_splat_count_state(42, true, 17);

	pipeline->clear_instance_pipeline_inputs();

	CHECK_FALSE(count_state.pending);
	CHECK(count_state.generation == 12);
	CHECK(count_state.pending_frame_counter == 0);
	CHECK_FALSE(count_state.bootstrap_sync_attempted);
	CHECK(pipeline->get_last_instance_visible_splat_count() == 0);
	CHECK_FALSE(pipeline->_test_get_last_instance_visible_splat_count_valid());
	CHECK(pipeline->_test_get_last_instance_visible_splat_count_frame() == 0);
}

} // namespace TestGaussianSplatting
