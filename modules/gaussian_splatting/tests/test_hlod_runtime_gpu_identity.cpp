#include "test_macros.h"
#include "../renderer/gpu_sorter.h"
#include "../renderer/tile_renderer.h"
#include "../renderer/instance_pipeline_contract.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "servers/rendering/rendering_server.h"

void gs_hlod_runtime_gpu_tests_link() {}

namespace TestHlodRuntimeGpuIdentity {
TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU] allocated sorter byte ledger excludes external buffers and survives shutdown") {
	RenderingServer *server = RenderingServer::get_singleton();
	if (!server) { FAIL("RenderingServer required for actual GPU allocation test"); return; }
	RenderingDevice *rd = server->create_local_rendering_device();
	if (!rd) { FAIL("A local RenderingDevice is required"); return; }
	Ref<RadixSort> sorter;
	sorter.instantiate();
	const uint64_t before = rd->get_memory_usage(RD::MEMORY_BUFFERS);
	const Error initialized = sorter->initialize(rd, 4096);
	if (initialized != OK) { FAIL("RadixSort initialization failed"); sorter->shutdown(); memdelete(rd); return; }
	uint64_t bytes = 0;
	CHECK(sorter->get_allocated_buffer_bytes(bytes));
	CHECK(bytes > 0);
	CHECK(bytes == rd->get_memory_usage(RD::MEMORY_BUFFERS) - before);
	const RID external = rd->storage_buffer_create(256);
	if (!external.is_valid()) { FAIL("external control buffer allocation failed"); sorter->shutdown(); memdelete(rd); return; }
	uint64_t after_external = 0;
	CHECK(sorter->get_allocated_buffer_bytes(after_external));
	CHECK(after_external == bytes);
	sorter->shutdown();
	uint64_t after_shutdown = UINT64_MAX;
	CHECK(sorter->get_allocated_buffer_bytes(after_shutdown));
	CHECK(after_shutdown == 0);
	rd->free(external);
	sorter.unref();
	memdelete(rd);
}
TEST_CASE("[GaussianSplatting][HLOD] unchanged RIDs with new cut content invalidate binding identity") {
	Vector<TileRenderer::RenderParams> sequence;
	sequence.resize(4); // Constant identity sequence; no GPU resource is dereferenced.
	sequence.write[0].hlod_relative = true;
	sequence.write[0].hlod_source_generation = 7;
	sequence.write[0].hlod_content_generation = 10;
	sequence.write[1] = sequence[0];
	sequence.write[2] = sequence[0];
	sequence.write[2].hlod_content_generation = 11;
	sequence.write[3] = sequence[2];
	sequence.write[3].hlod_relative = false;
	const Vector<uint64_t> trace = TileRenderer::_test_instance_pipeline_binding_generation_trace(sequence);
	if (trace.size() != 4) { FAIL("complete identity trace missing"); return; }
	CHECK(trace[0] == 1);
	CHECK(trace[1] == 1);
	CHECK(trace[2] == 2);
	CHECK(trace[3] == 3);
	CHECK(sizeof(SplatRefGPU) == 8);
	CHECK(sizeof(ChunkMetaGPU) == 64);
	CHECK(offsetof(ChunkMetaGPU, node_center_x) == 48);
	CHECK(offsetof(ChunkMetaGPU, node_center_y) == 52);
	CHECK(offsetof(ChunkMetaGPU, node_center_z) == 56);
}
}
