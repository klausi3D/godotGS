#pragma once

#include "gs_hlod_page_store_test_fixture.h"
#include "gs_test_pump.h"
#include "../lod/hlod_edit.h"
#include "../renderer/hlod_node_pack.h"
#include "../renderer/hlod_node_streamer.h"

#include <cstring>

namespace TestHlodEditShadowTransfer {

using Streamer = HlodNodeStreamer;
using Residency = gs_hlod_runtime::NodeResidency;
using TestGaussianSplatting::gs_pump_until;

// Reuse the production-written D3-2 corpus without modifying its writer, codec,
// fixture or on-disk format. Expected leaf attributes come from its generator.
struct Fixture {
	TestHlodPageStore::Fixture source;
	Ref<GaussianSplatHlodRuntimeData> edited;
	Vector<uint32_t> leaves;
	Vector<uint32_t> stale;

	bool build(uint32_t p_depth = 4, uint32_t p_splats = 256, uint32_t p_edit_count = 16) {
		if (!source.build(false, p_depth, p_splats, 0) || source.snapshot.is_null()) { return false; }
		Vector<gs_hlod_runtime::LeafEdit> edits;
		for (uint32_t node = 0; node < source.node_count && leaves.size() < p_edit_count; ++node) {
			if (source.snapshot->nodes[node].topology.child_count != 0) { continue; }
			Vector<Gaussian> old;
			Vector<Vector3> sh;
			if (!source.expected_payload(node, old, sh)) { return false; }
			gs_hlod_runtime::LeafEdit edit;
			edit.node = node;
			// Keep every dyadic endpoint so the leaf centre remains exact;
			// counts and colour both distinguish the new revision from the old one.
			for (int64_t i = 0; i < old.size() / 2; ++i) {
				Gaussian g = old[i];
				g.sh_dc = Color(1, 0, 0, 1);
				edit.gaussians.push_back(g);
			}
			edits.push_back(edit);
			leaves.push_back(node);
		}
		if (leaves.size() != p_edit_count || p_edit_count == 0) {
			FAIL("D3-3 fixture needs the requested nonempty leaf set"); return false;
		}
		String reason;
		if (gs_hlod_runtime::make_edited_snapshot(source.snapshot, 0, edits.ptr(), edits.size(), edited, reason) != gs_hlod_runtime::EditStatus::OK || edited.is_null()) {
			FAIL("D3-3 edit factory rejected the synthetic edit: ", reason.utf8().get_data()); return false;
		}
		for (uint32_t node = 0; node < source.node_count; ++node) {
			if (edited->is_stale(node)) { stale.push_back(node); }
		}
		return true;
	}

	uint64_t publication_bytes(bool p_quantized) const {
		return uint64_t(leaves.size() + stale.size()) *
				(sizeof(ChunkMetaGPU) + (p_quantized ? sizeof(ChunkQuantizationGPU) : 0));
	}

	Streamer::Config config(bool p_quantized = false) const {
		Streamer::Config cfg;
		cfg.quantized = p_quantized;
		cfg.position_bits = p_quantized ? 16 : 0;
		cfg.scale_bits = p_quantized ? 16 : 0;
		const uint32_t pages = (source.splats_per_node + cfg.page_splats - 1) / cfg.page_splats;
		cfg.atlas_pages = (source.node_count + 2 * leaves.size()) * pages;
		const uint64_t stride = p_quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian);
		uint64_t output_storage = 0;
		gs_hlod_runtime::allocation_bytes(source.splats_per_node / 2, stride, output_storage);
		cfg.max_inflight_read_bytes = 2 * output_storage;
		// A leaf itself spans frames; final publication still fits atomically.
		cfg.upload_bytes_per_frame = MAX(publication_bytes(p_quantized), uint64_t(source.splats_per_node / 4) * stride);
		return cfg;
	}

	bool pack(uint32_t p_node, bool p_edited, const Streamer::Config &p_config, uint32_t p_atlas_base,
			Vector<uint8_t> &r_bytes, ChunkMetaGPU &r_meta, ChunkQuantizationGPU &r_quant) const {
		const Ref<GaussianSplatHlodRuntimeData> &snapshot = p_edited ? edited : source.snapshot;
		if (snapshot.is_null() || p_node >= uint32_t(snapshot->nodes.size())) {
			FAIL("D3-3 golden needs an existing snapshot node"); return false;
		}
		const uint32_t count = snapshot->nodes[p_node].topology.payload_count;
		Vector<Gaussian> gaussians;
		Vector<Vector3> sh;
		gs_hlod_pack::PayloadView view;
		if (!p_edited || !snapshot->find_edited(p_node)) {
			if (!source.expected_payload(p_node, gaussians, sh)) { return false; }
			view.gaussians = gaussians.ptr();
			view.sh = sh.ptr();
		}
		gs_hlod_pack::Params params;
		params.relative = true;
		params.quantized = p_config.quantized;
		params.position_bits = p_config.position_bits;
		params.scale_bits = p_config.scale_bits;
		Vector<PackedGaussian> raw;
		Vector<PackedGaussianQuantized> quantized;
		if ((p_config.quantized ? quantized.resize(count) : raw.resize(count)) != OK) {
			FAIL("D3-3 packed golden allocation failed"); return false;
		}
		gs_hlod_pack::Output output;
		SHCompressionMetrics metrics;
		output.raw = raw.ptrw();
		output.quantized = quantized.ptrw();
		output.meta = &r_meta;
		output.quant = &r_quant;
		output.metrics = &metrics;
		if (!gs_hlod_pack::pack_node(*snapshot.ptr(), p_node, params, p_atlas_base, output, view.gaussians ? &view : nullptr)) {
			FAIL("D3-3 independent packed golden failed"); return false;
		}
		const uint64_t bytes = uint64_t(count) * (p_config.quantized ? sizeof(PackedGaussianQuantized) : sizeof(PackedGaussian));
		if (r_bytes.resize(bytes) != OK) { FAIL("D3-3 golden bytes allocation failed"); return false; }
		memcpy(r_bytes.ptrw(), p_config.quantized ? static_cast<const void *>(quantized.ptr()) : static_cast<const void *>(raw.ptr()), size_t(bytes));
		return true;
	}
};

} // namespace TestHlodEditShadowTransfer
