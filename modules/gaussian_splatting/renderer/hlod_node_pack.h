#ifndef GS_HLOD_NODE_PACK_H
#define GS_HLOD_NODE_PACK_H

// Packs the payload and GPU metadata of one HLOD node (ADR 6.2). This is the single packing path of the HLOD
// route: the whole-bundle builder (renderer/resident_instance_contract_publisher.cpp) and the node streamer
// (renderer/hlod_node_streamer.cpp) both call it, so a node packs identically whether it is built up front or
// streamed on demand. Pure CPU; reads only the immutable runtime snapshot, so it is safe on a worker thread.

#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/streaming_quantization.h"
#include "gaussian_gpu_layout.h"

namespace gs_hlod_pack {

struct Params {
	bool relative = false; // Node-relative positions (relative bundle) instead of asset-space (canonical).
	bool quantized = false;
	uint32_t position_bits = 0;
	uint32_t scale_bits = 0;
};

// Output of one node. `raw` receives p_data.nodes[node].topology.payload_count PackedGaussian records when not
// quantized, `quantized` the same count of PackedGaussianQuantized records otherwise (exactly one is used).
// `meta` is the node's ChunkMetaGPU with atlas_base = p_atlas_base; `quant` is its ChunkQuantizationGPU
// (written only when quantized, start_index = p_atlas_base).
struct Output {
	PackedGaussian *raw = nullptr;
	PackedGaussianQuantized *quantized = nullptr;
	ChunkMetaGPU *meta = nullptr;
	ChunkQuantizationGPU *quant = nullptr;
	SHCompressionMetrics *metrics = nullptr;
};

// Returns false for an out-of-range node or a missing output pointer; nothing is written then.
bool pack_node(const GaussianSplatHlodRuntimeData &p_data, uint32_t p_node, const Params &p_params,
		uint32_t p_atlas_base, const Output &p_out);

} // namespace gs_hlod_pack

#endif
