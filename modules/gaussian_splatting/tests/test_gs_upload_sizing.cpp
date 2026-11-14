extern "C" int test_gs_upload_sizing_cpp_force_link() { return 0; }

#include "../core/gaussian_streaming.h"
#include "../core/gs_upload_sizing.h"
#include "../core/streaming_upload_pipeline.h"
#include "../renderer/gaussian_gpu_layout.h"

#include "test_macros.h"

namespace TestGSUploadSizing {

TEST_CASE("[GaussianSplatting][WorldIO][D3Sizing] Worker sizing treats unknown inputs and reserved pool capacity explicitly") {
	struct CPUCase {
		uint32_t cpus;
		uint32_t expected;
	};
	const CPUCase cpu_cases[] = {
		{ 0, 16 }, { 1, 2 }, { 2, 2 }, { 3, 2 }, { 4, 2 },
		{ 8, 4 }, { 12, 6 }, { 16, 8 }, { 32, 16 }, { 64, 16 },
	};
	for (const CPUCase &entry : cpu_cases) {
		CAPTURE(entry.cpus);
		CHECK(GSUploadSizing::worker_count(16, entry.cpus, 0, 0) == entry.expected);
		CHECK(GSUploadSizing::worker_count(0, entry.cpus, 32, 0) == 0);
		CHECK(GSUploadSizing::worker_count(1, entry.cpus, 32, 0) == 1);
	}
	struct PoolCase {
		uint32_t pool;
		uint32_t reserved;
		uint32_t expected;
	};
	const PoolCase pool_cases[] = {
		{ 0, 0, 16 }, { 0, UINT32_MAX, 16 }, { 1, 0, 1 }, { 1, 1, 0 },
		{ 1, 2, 0 }, { 2, 0, 2 }, { 3, 1, 2 }, { 8, 1, 7 },
		{ 16, 2, 14 }, { 32, 2, 16 }, { 32, UINT32_MAX, 0 },
	};
	for (const PoolCase &entry : pool_cases) {
		CAPTURE(entry.pool);
		CAPTURE(entry.reserved);
		CHECK(GSUploadSizing::worker_count(16, 32, entry.pool, entry.reserved) == entry.expected);
		CHECK(GSUploadSizing::worker_count(0, 32, entry.pool, entry.reserved) == 0);
	}
	CHECK(GSUploadSizing::worker_count(16, 0, 8, 1) == 7);
	CHECK(GSUploadSizing::worker_count(16, 8, 32, 1) == 4);
	CHECK(GSUploadSizing::worker_count(3, 32, 32, 0) == 3);
	CHECK(GSUploadSizing::worker_count(8, 32, 32, 0) == 8);
	CHECK(GSUploadSizing::worker_count(16, 64, 4, 1) == 3);
	CHECK(GSUploadSizing::worker_count(UINT32_MAX, UINT32_MAX, 0, 0) == UINT32_MAX / 2);
}

TEST_CASE("[GaussianSplatting][WorldIO][D3Sizing] Staging and frame limits preserve zero semantics and checked MiB division") {
	constexpr uint64_t mib = uint64_t(1024) * 1024;
	struct StagingCase {
		uint64_t mb;
		uint32_t frames;
		uint64_t expected;
	};
	const StagingCase cases[] = {
		{ 0, 0, 0 }, { 0, 2, 0 }, { 128, 0, 0 },
		{ 128, 2, 64 * mib }, { 128, 3, 44739242 },
		{ 1, 3, 349525 }, { 1, UINT32_MAX, 0 }, { UINT32_MAX, 1, uint64_t(UINT32_MAX) * mib },
		{ UINT64_MAX / mib, 1, (UINT64_MAX / mib) * mib },
		{ UINT64_MAX / mib + 1, 1, UINT64_MAX },
		{ UINT64_MAX, 1, UINT64_MAX },
		{ UINT64_MAX, UINT32_MAX, (uint64_t(UINT32_MAX) + 2) * mib },
	};
	for (const StagingCase &entry : cases) {
		CAPTURE(entry.mb);
		CAPTURE(entry.frames);
		CHECK(GSUploadSizing::staging_clamp_bytes(entry.mb, entry.frames) == entry.expected);
	}
	CHECK(GSUploadSizing::frame_limit_bytes(0, 0, true) == UINT64_MAX);
	CHECK(GSUploadSizing::frame_limit_bytes(0, 0, false) == 0);
	CHECK(GSUploadSizing::frame_limit_bytes(0, 8 * mib, true) == 8 * mib);
	CHECK(GSUploadSizing::frame_limit_bytes(0, 8 * mib, false) == 0);
	CHECK(GSUploadSizing::frame_limit_bytes(32 * mib, 0, false) == 32 * mib);
	CHECK(GSUploadSizing::frame_limit_bytes(32 * mib, 64 * mib, false) == 32 * mib);
	CHECK(GSUploadSizing::frame_limit_bytes(32 * mib, 8 * mib, false) == 8 * mib);
	CHECK(GSUploadSizing::frame_limit_bytes(UINT64_MAX, UINT64_MAX - 1, true) == UINT64_MAX - 1);
}

TEST_CASE("[GaussianSplatting][WorldIO][D3Sizing] Ready windows use caller units and saturate without byte overflow") {
	const uint64_t units[] = { 1, 3, 48, 64, 144, 248 };
	const uint32_t jobs[] = { 0, 1, 4, 16, UINT32_MAX };
	for (uint64_t unit : units) {
		const uint64_t limits[] = { 1, unit, unit * 3 - 1, unit * 8 };
		for (uint64_t limit : limits) {
			for (uint32_t max_jobs : jobs) {
				CAPTURE(unit);
				CAPTURE(limit);
				CAPTURE(max_jobs);
				const uint64_t expected = MAX(uint64_t(max_jobs), limit * 2 / unit);
				CHECK(GSUploadSizing::ready_window(limit, unit, max_jobs) == expected);
			}
		}
	}
	CHECK(GSUploadSizing::ready_window(0, 48, 16) == 0);
	CHECK(GSUploadSizing::ready_window(UINT64_MAX, 48, 16) == 0);
	CHECK(GSUploadSizing::ready_window(1024, 0, 16) == 0);
	CHECK(GSUploadSizing::ready_window(UINT64_MAX - 1, 1, 16) == UINT32_MAX);
	CHECK(GSUploadSizing::ready_window(UINT64_MAX - 1, UINT64_MAX, 0) == 1);
	CHECK(GSUploadSizing::ready_window(65536, 48, 0) != GSUploadSizing::ready_window(65536, 144, 0));

	CHECK(GSUploadSizing::dequeue_batch(0, 16, 4) == 4);
	CHECK(GSUploadSizing::dequeue_batch(1, 16, 4) == 4);
	CHECK(GSUploadSizing::dequeue_batch(2, 0, 4) == 1);
	CHECK(GSUploadSizing::dequeue_batch(8, 4, 4) == 1);
	CHECK(GSUploadSizing::dequeue_batch(2, 16, 4) == 4);
	CHECK(GSUploadSizing::dequeue_batch(2, 6, 2) == 2);
	CHECK(GSUploadSizing::dequeue_batch(8, 4, 0) == 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][D3Sizing] Record byte products reject overflow and slices make only aligned progress") {
	const uint64_t strides[] = { 1, 3, 48, 64, 144, 248 };
	for (uint64_t stride : strides) {
		CAPTURE(stride);
		uint64_t bytes = UINT64_MAX;
		CHECK(GSUploadSizing::checked_byte_count(7, stride, bytes));
		CHECK(bytes == 7 * stride);
		CHECK(GSUploadSizing::checked_byte_count(UINT64_MAX / stride, stride, bytes));
		CHECK(bytes == (UINT64_MAX / stride) * stride);
		if (stride > 1) {
			CHECK_FALSE(GSUploadSizing::checked_byte_count(UINT64_MAX / stride + 1, stride, bytes));
			CHECK(bytes == 0);
		}

		const uint64_t node_bytes = 11 * stride;
		const GSUploadSizing::Slice whole = GSUploadSizing::slice(node_bytes, 0, node_bytes, stride);
		CHECK(whole.valid);
		CHECK(whole.bytes == node_bytes);
		CHECK(whole.is_final);
		const GSUploadSizing::Slice stalled = GSUploadSizing::slice(node_bytes, 0, stride - 1, stride);
		CHECK(stalled.valid);
		CHECK(stalled.bytes == 0);
		CHECK_FALSE(stalled.is_final);
		const GSUploadSizing::Slice first = GSUploadSizing::slice(node_bytes, 0, 4 * stride + stride - 1, stride);
		CHECK(first.valid);
		CHECK(first.bytes == 4 * stride);
		CHECK_FALSE(first.is_final);
		const GSUploadSizing::Slice middle = GSUploadSizing::slice(node_bytes, 4 * stride, 4 * stride, stride);
		CHECK(middle.valid);
		CHECK(middle.bytes == 4 * stride);
		CHECK_FALSE(middle.is_final);
		// The final three records fit exactly, or leave unused allowance. Neither
		// case may round up to the four-record allowance or publish one slice early.
		const GSUploadSizing::Slice exact = GSUploadSizing::slice(node_bytes, 8 * stride, 3 * stride, stride);
		const GSUploadSizing::Slice tail = GSUploadSizing::slice(node_bytes, 8 * stride, 4 * stride, stride);
		CHECK(exact.valid);
		CHECK(exact.bytes == 3 * stride);
		CHECK(exact.is_final);
		CHECK(tail.valid);
		CHECK(tail.bytes == 3 * stride);
		CHECK(tail.is_final);
		const GSUploadSizing::Slice complete = GSUploadSizing::slice(node_bytes, node_bytes, 0, stride);
		CHECK(complete.valid);
		CHECK(complete.bytes == 0);
		CHECK(complete.is_final);
		const GSUploadSizing::Slice invalid = GSUploadSizing::slice(node_bytes, node_bytes + stride, UINT64_MAX, stride);
		CHECK_FALSE(invalid.valid);
		CHECK(invalid.bytes == 0);
		CHECK_FALSE(invalid.is_final);
	}
	uint64_t bytes = 1;
	CHECK(GSUploadSizing::checked_byte_count(0, UINT64_MAX, bytes));
	CHECK(bytes == 0);
	CHECK(GSUploadSizing::checked_byte_count(UINT64_MAX, 0, bytes));
	CHECK(bytes == 0);
	const GSUploadSizing::Slice empty = GSUploadSizing::slice(0, 0, 0, 48);
	CHECK(empty.valid);
	CHECK(empty.bytes == 0);
	CHECK(empty.is_final);
	CHECK_FALSE(GSUploadSizing::slice(48, 0, 48, 0).valid);
	CHECK_FALSE(GSUploadSizing::slice(49, 0, 48, 48).valid);
	CHECK_FALSE(GSUploadSizing::slice(96, 1, 48, 48).valid);
	const GSUploadSizing::Slice large = GSUploadSizing::slice(UINT64_MAX - 15, UINT64_MAX - 63, UINT64_MAX, 48);
	CHECK(large.valid);
	CHECK(large.bytes == 48);
	CHECK(large.is_final);
}

TEST_CASE("[GaussianSplatting][WorldIO][D3Sizing] Chunk sizing wrappers match the frozen pre-extraction formulas") {
	// Frozen from f8395daf7cbc00f096313e2ff118d17a5af07c70. Do not obtain these
	// expected values through GSUploadSizing: this protects caller identity.
	const uint32_t settings[] = { 0, 1, 2, 3, 8, 16, UINT32_MAX };
	const uint32_t cpus[] = { 0, 1, 2, 3, 4, 8, 12, 16, 32, 64 };
	for (uint32_t setting : settings) {
		for (uint32_t cpu : cpus) {
			CAPTURE(setting);
			CAPTURE(cpu);
			const uint32_t expected = setting == 0 ? 0 : MIN(setting, MAX(2u, cpu / 2u));
			CHECK(StreamingUploadPipeline::compute_pack_worker_count(setting, cpu) == expected);
		}
	}
	const uint32_t staging_mb[] = { 0, 1, 8, 32, 128, UINT32_MAX };
	const uint32_t frames[] = { 0, 1, 2, 3, 8, UINT32_MAX };
	for (uint32_t mb : staging_mb) {
		for (uint32_t frame_count : frames) {
			CAPTURE(mb);
			CAPTURE(frame_count);
			const uint64_t expected = mb == 0 || frame_count == 0 ? 0 : uint64_t(mb) * 1024 * 1024 / frame_count;
			CHECK(StreamingUploadPipeline::compute_staging_upload_clamp_bytes(mb, frame_count) == expected);
		}
	}
	const uint32_t workers[] = { 0, 1, 2, 3, 4, 8, 16, UINT32_MAX };
	const uint32_t jobs[] = { 0, 1, 4, 8, 16, 64, UINT32_MAX };
	for (uint32_t worker : workers) {
		for (uint32_t max_jobs : jobs) {
			CAPTURE(worker);
			CAPTURE(max_jobs);
			const uint32_t expected = worker <= 1 ? 4u : (max_jobs == 0 ? 1u : CLAMP(max_jobs / worker, 1u, 4u));
			CHECK(StreamingUploadPipeline::compute_pack_dequeue_batch(worker, max_jobs) == expected);
		}
	}
	const uint64_t full_chunk = uint64_t(GaussianStreamingSystem::CHUNK_SIZE) * sizeof(PackedGaussian);
	// Existing finite inputs do not overflow the reference's two-frame product.
	// Helper overflow boundaries are checked independently above.
	const uint64_t frame_limits[] = { 0, 1, full_chunk / 2 - 1, full_chunk / 2, full_chunk,
		full_chunk * 3, uint64_t(128) * 1024 * 1024, UINT64_MAX / 2, UINT64_MAX };
	for (uint64_t limit : frame_limits) {
		for (uint32_t max_jobs : jobs) {
			CAPTURE(limit);
			CAPTURE(max_jobs);
			const uint32_t expected = limit == 0 || limit == UINT64_MAX ? 0 :
					MAX(max_jobs, uint32_t(MIN<uint64_t>(limit * 2 / full_chunk, UINT32_MAX)));
			CHECK(StreamingUploadPipeline::compute_max_ready_upload_jobs(limit, max_jobs) == expected);
		}
	}
	GaussianStreamingSystem system;
	StreamingUploadPipeline &uploads = system._internal_get_upload_pipeline();
	const uint64_t configured[] = { 0, 1, full_chunk, uint64_t(32) * 1024 * 1024, UINT32_MAX };
	const uint64_t clamps[] = { 0, 1, full_chunk / 2, uint64_t(64) * 1024 * 1024, UINT64_MAX };
	for (uint64_t setting : configured) {
		for (uint64_t clamp : clamps) {
			CAPTURE(setting);
			CAPTURE(clamp);
			uploads.max_upload_bytes_per_frame = setting;
			uploads.upload_staging_clamp_bytes_per_frame = clamp;
			const uint64_t configured_limit = setting == 0 ? UINT64_MAX : setting;
			const uint64_t expected = clamp == 0 ? configured_limit : MIN(configured_limit, clamp);
			CHECK(uploads.get_frame_upload_limit_bytes() == expected);
		}
	}
}

} // namespace TestGSUploadSizing
