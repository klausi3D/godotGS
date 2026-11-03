#include "hlod_node_pack.h"

namespace gs_hlod_pack {

bool pack_node(const GaussianSplatHlodRuntimeData &p_data, uint32_t p_node, const Params &p_params,
		uint32_t p_atlas_base, const Output &p_out, const PayloadView *p_view) {
	// Package F1: an edited leaf is always served from its overlay, whoever packs it (the base snapshot's leaf array still
	// holds the old splats).
	PayloadView overlay_view;
	if (!p_view && p_node < uint32_t(p_data.nodes.size())) {
		if (const GaussianSplatHlodRuntimeData::EditedLeaf *edited = p_data.find_edited(p_node)) {
			if (uint64_t(edited->gaussians.size()) != p_data.nodes[p_node].topology.payload_count) {
				return false;
			}
			overlay_view.gaussians = edited->gaussians.ptr();
			overlay_view.sh = edited->sh.is_empty() ? nullptr : edited->sh.ptr();
			p_view = &overlay_view;
		}
	}
	if (p_node >= uint32_t(p_data.nodes.size()) || !p_out.meta || !p_out.metrics ||
			(p_params.quantized ? (!p_out.quantized || !p_out.quant) : !p_out.raw) ||
			(p_data.is_paged() && (!p_view || !p_view->gaussians || (p_data.high_order_count != 0 && !p_view->sh)))) {
		return false;
	}
	const auto &node = p_data.nodes[p_node];
	// The payload of the node inside the runtime snapshot (leaf then interior order), not the atlas address.
	const uint32_t first = uint32_t(node.topology.payload_first);
	const uint32_t count = node.topology.payload_count;
	ChunkMetaGPU &meta = *p_out.meta;
	meta = ChunkMetaGPU{};
	meta.atlas_base = p_atlas_base;
	meta.splat_count = count;
	meta.quant_base = p_node;
	meta.quant_count = p_params.quantized ? 1 : 0;
	meta.sh_limit = p_data.sh_degree;
	meta.flags = gs_pack_asset_gpu_flags(p_data.mode_2d, GaussianDCEncoding(node.dc_encoding));
	for (int a = 0; a < 3; ++a) { meta.bounds_center_local[a] = node.bounds_center[a]; }
	meta.bounds_radius_local = node.topology.radius;
	// Unread by the shaders since C2 (the per-draw camera offset places the node); kept for the layout.
	meta.node_center_x = p_params.relative ? node.payload_center.x : 0.0f;
	meta.node_center_y = p_params.relative ? node.payload_center.y : 0.0f;
	meta.node_center_z = p_params.relative ? node.payload_center.z : 0.0f;
	// i is the index inside the node; the snapshot index (leaf then interior order) is first + i.
	auto source_gaussian = [&](uint32_t i) -> const Gaussian & {
		return p_view ? p_view->gaussians[i] : p_data.gaussian(first + i);
	};
	auto source_sh = [&](uint32_t i) -> const Vector3 * {
		if (p_data.high_order_count == 0) {
			return nullptr;
		}
		return p_view ? p_view->sh + uint64_t(i) * p_data.high_order_count : p_data.sh(first + i);
	};
	const bool leaf_payload = first < p_data.leaf_count;
	auto local_gaussian = [&](uint32_t i) {
		Gaussian g = source_gaussian(i);
		if (p_params.relative && leaf_payload) {
			for (int a = 0; a < 3; ++a) {
				g.position[a] = float(double(g.position[a]) - node.exact_cell_center[a]);
			}
		}
		return g;
	};
	ChunkQuantizationInfo quant;
	if (p_params.quantized) {
		// The existing codec accepts LocalVector only. Build its same min/range inputs
		// directly from this immutable span; avoid an infallible temporary allocation.
		quant.position_bits = p_params.position_bits;
		quant.scale_bits = p_params.scale_bits;
		quant.scales_quantized = true;
		const Gaussian first_g = local_gaussian(0);
		quant.position_min = quant.position_max = first_g.position;
		quant.scale_min = quant.scale_max = first_g.scale;
		for (uint32_t i = 1; i < count; ++i) {
			const Gaussian g = local_gaussian(i);
			quant.position_min = quant.position_min.min(g.position);
			quant.position_max = quant.position_max.max(g.position);
			quant.scale_min = quant.scale_min.min(g.scale);
			quant.scale_max = quant.scale_max.max(g.scale);
		}
		quant.position_range = (quant.position_max - quant.position_min).max(Vector3(1e-6f, 1e-6f, 1e-6f));
		quant.scale_range = (quant.scale_max - quant.scale_min).max(Vector3(1e-6f, 1e-6f, 1e-6f));
		ChunkQuantizationGPU &gpu = *p_out.quant;
		gpu = ChunkQuantizationGPU{};
		for (int a = 0; a < 3; ++a) {
			gpu.position_min[a] = quant.position_min[a];
			gpu.position_range[a] = quant.position_range[a];
			gpu.scale_min[a] = quant.scale_min[a];
			gpu.scale_range[a] = quant.scale_range[a];
		}
		gpu.position_bits = quant.position_bits;
		gpu.scale_bits = quant.scale_bits;
		gpu.start_index = p_atlas_base;
		gpu.count = count;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const Gaussian g = local_gaussian(i);
		if (p_params.quantized) {
			pack_gaussian_quantized(g, quant, uint16_t(p_node), p_out.quantized[i], *p_out.metrics, source_sh(i),
					p_data.sh_first_order_count(), p_data.high_order_count);
		} else {
			pack_gaussian(g, p_out.raw[i], *p_out.metrics, source_sh(i),
					p_data.sh_first_order_count(), p_data.high_order_count);
		}
	}
	return true;
}

} // namespace gs_hlod_pack
