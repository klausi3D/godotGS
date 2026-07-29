#include "resident_instance_contract_publisher.h"

#include "../core/gs_vector_alloc.h"
#include "gaussian_gpu_layout.h"
#include "gaussian_splat_renderer.h"
#include "gpu_sorting_config.h"
#include "instance_pipeline_contract.h"
#include "quantization_config.h"
#include "../core/streaming_quantization.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../resources/color_grading_resource.h"
#include "resident_atlas_budget.h"
#include "../core/gs_project_settings.h"
#include "core/config/project_settings.h"
#include "servers/rendering/rendering_device.h"

#include <algorithm>
#include <cstring>
#include <cstdint>

namespace {

using GaussianSplatting::InstancePipelineContract::InvariantViolationReason;
constexpr uint32_t kPrimaryResidentAssetId = 0u;

struct ResidentChunkDescriptor {
	AABB bounds;
	Vector<uint32_t> source_indices;
	uint32_t start_idx = 0;
	uint32_t count = 0;
	bool source_index_remapped = false;
};

struct ResidentAssetDescriptor {
	// Full 64-bit host submission identity (asset_records key / ObjectID). Must stay
	// 64-bit so it remains the collision-free key into the published remap. The dense
	// atlas slot below stays 32-bit (sequential, collision-free, GPU-facing).
	uint64_t submission_asset_id = 0;
	uint32_t dense_asset_id = 0;
	Ref<GaussianData> data;
	Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
};

static uint64_t _mix_generation(uint64_t p_accum, uint64_t p_value) {
	uint64_t x = p_accum + 0x9e3779b97f4a7c15ULL;
	x ^= p_value + (x << 6) + (x >> 2);
	return x;
}

static AABB _compute_contiguous_chunk_bounds(const LocalVector<Gaussian> &p_gaussians, uint32_t p_start, uint32_t p_count) {
	if (p_count == 0 || p_start >= p_gaussians.size()) {
		return AABB();
	}

	Vector3 min_pos = p_gaussians[p_start].position;
	Vector3 max_pos = p_gaussians[p_start].position;
	const uint32_t end = MIN<uint32_t>(p_start + p_count, p_gaussians.size());
	for (uint32_t i = p_start + 1; i < end; i++) {
		const Vector3 &pos = p_gaussians[i].position;
		min_pos.x = MIN(min_pos.x, pos.x);
		min_pos.y = MIN(min_pos.y, pos.y);
		min_pos.z = MIN(min_pos.z, pos.z);
		max_pos.x = MAX(max_pos.x, pos.x);
		max_pos.y = MAX(max_pos.y, pos.y);
		max_pos.z = MAX(max_pos.z, pos.z);
	}
	Vector3 size = max_pos - min_pos;
	if (size == Vector3()) {
		size = Vector3(0.001f, 0.001f, 0.001f);
	}
	return AABB(min_pos, size);
}

static GaussianDCEncoding _resolve_data_dc_encoding(const Ref<GaussianData> &p_data) {
	if (p_data.is_null() || p_data->get_count() <= 0) {
		return GAUSSIAN_DC_ENCODING_LEGACY_BIAS;
	}
	return gaussian_get_dc_encoding(p_data->get_gaussian(0).render_meta);
}

template <typename T>
static bool _upload_typed_storage_buffer(GaussianSplatRenderer *p_renderer, RenderingDevice *p_rd, RID &r_buffer,
		uint32_t &r_buffer_size, const char *p_label, const Vector<T> &p_data) {
	if (p_data.is_empty()) {
		p_renderer->free_owned_resource(p_rd, r_buffer);
		r_buffer_size = 0;
		return false;
	}

	const uint64_t required_size_u64 = uint64_t(p_data.size()) * sizeof(T);
	static constexpr uint64_t kMaxResidentUploadBytes = uint64_t(2) * 1024 * 1024 * 1024; // 2 GB staging limit
	if (required_size_u64 > kMaxResidentUploadBytes) {
		return false;
	}
	const uint32_t required_size = uint32_t(required_size_u64);
	Vector<uint8_t> upload_bytes;
	upload_bytes.resize(required_size);
	memcpy(upload_bytes.ptrw(), p_data.ptr(), required_size);

	if (!r_buffer.is_valid() || r_buffer_size != required_size) {
		p_renderer->free_owned_resource(p_rd, r_buffer);
		r_buffer = p_rd->storage_buffer_create(required_size, upload_bytes);
		if (!r_buffer.is_valid()) {
			r_buffer_size = 0;
			return false;
		}
		p_rd->set_resource_name(r_buffer, p_label);
		p_renderer->track_resource_owner(r_buffer, p_rd);
		r_buffer_size = required_size;
		return true;
	}

	p_rd->buffer_update(r_buffer, 0, required_size, upload_bytes.ptr());
	return true;
}

static void _append_chunk_descriptors_for_asset(const ResidentAssetDescriptor &p_asset,
		LocalVector<ResidentChunkDescriptor> &r_chunks) {
	const uint32_t total_count = p_asset.data.is_valid() ? uint32_t(MAX(0, p_asset.data->get_count())) : 0u;
	if (total_count == 0) {
		return;
	}

	if (!p_asset.static_chunks.is_empty()) {
		for (int chunk_idx = 0; chunk_idx < p_asset.static_chunks.size(); chunk_idx++) {
			const GaussianSplatRenderer::StaticChunk &chunk = p_asset.static_chunks[chunk_idx];
			if (chunk.indices.is_empty()) {
				continue;
			}
			uint32_t offset = 0;
			while (offset < uint32_t(chunk.indices.size())) {
				const uint32_t split_count = MIN<uint32_t>(GaussianStreamingSystem::CHUNK_SIZE,
						uint32_t(chunk.indices.size()) - offset);
				ResidentChunkDescriptor descriptor;
				descriptor.bounds = chunk.bounds;
				descriptor.count = split_count;
				descriptor.source_index_remapped = true;
				// #794: the copy loop is bounded by split_count, not by
				// source_indices.size(), so an ignored resize failure would write past
				// the end and hard-trap. Publishing a descriptor whose count says
				// split_count but whose source_indices are empty would be worse than
				// dropping it -- #793 showed exactly that shape reaching
				// buffer_update() as a null pointer with a nonzero byte count. So bail
				// out of publishing entirely rather than emit a partial chunk set.
				if (!gs_resize_or_fail(descriptor.source_indices, (int64_t)split_count,
							"ResidentInstanceContractPublisher::_append_chunk_descriptors_for_asset")) {
					r_chunks.clear();
					return;
				}
				for (uint32_t local_idx = 0; local_idx < split_count; local_idx++) {
					descriptor.source_indices.write[local_idx] = chunk.indices[offset + local_idx];
				}
				r_chunks.push_back(descriptor);
				offset += split_count;
			}
		}
		return;
	}

	const LocalVector<Gaussian> &gaussians = p_asset.data->get_gaussian_storage();
	for (uint32_t start = 0; start < total_count; start += GaussianStreamingSystem::CHUNK_SIZE) {
		const uint32_t count = MIN<uint32_t>(GaussianStreamingSystem::CHUNK_SIZE, total_count - start);
		ResidentChunkDescriptor descriptor;
		descriptor.start_idx = start;
		descriptor.count = count;
		descriptor.bounds = _compute_contiguous_chunk_bounds(gaussians, start, count);
		r_chunks.push_back(descriptor);
	}
}

} // namespace

namespace ResidentInstanceContractPublisher {

bool publish_resident_direct_data_contract(GaussianSplatRenderer *p_renderer, String *r_reason) {
	ERR_FAIL_NULL_V(p_renderer, false);

	if (r_reason) {
		*r_reason = String();
	}

	if (!p_renderer->ensure_rendering_device("resident_instance_contract_publish")) {
		if (r_reason) {
			*r_reason = "rendering_device_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	// Per-chunk quantization: the resident atlas packs the 80-byte PackedGaussianQuantized
	// layout with per-chunk bounds (GS-PERF-Q80B). The in-memory resident data lets us
	// compute exact per-chunk bounds (unlike the out-of-core streaming path), and the
	// resident atlas already carries a chunk structure with a ChunkMetaGPU per chunk.
	const bool quantize_atlas = g_quantization_config.per_chunk_quantization;

	RenderingDevice *rd = p_renderer->get_device_state().rd;
	if (rd == nullptr) {
		if (r_reason) {
			*r_reason = "rendering_device_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	// Resident atlas VRAM budget (#321 follow-up). A default GaussianSplatNode3D is
	// resident-only by contract, so an over-large atlas cannot fall back to streaming and
	// would OOM / device-lost on a low-end GPU. Clamp the packed atlas to the device VRAM
	// budget (or an explicit project override) and pack an importance-ordered subset below
	// instead. Unknown/UMA budgets and capable GPUs resolve to the 2 GB staging ceiling, so
	// the effective cap == the staging ceiling and nothing below changes.
	const uint64_t device_budget_bytes = rd->get_device_memory_budget();
	const uint32_t resident_atlas_override_mb = gs::settings::get_uint(ProjectSettings::get_singleton(),
			"rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 0u);
	const uint64_t effective_atlas_cap_bytes =
			ResidentAtlasBudget::compute_effective_atlas_cap_bytes(device_budget_bytes, resident_atlas_override_mb);

	const Ref<GaussianData> primary_data = p_renderer->get_scene_state().gaussian_data;
	const bool has_primary_data = primary_data.is_valid() && primary_data->get_count() > 0;

	LocalVector<ResidentAssetDescriptor> assets;
	assets.reserve(8);

	ResidentAssetDescriptor primary_asset;
	primary_asset.submission_asset_id = kPrimaryResidentAssetId;
	primary_asset.dense_asset_id = 0;
	primary_asset.data = primary_data;
	if (has_primary_data) {
		primary_asset.static_chunks = p_renderer->get_static_chunks();
	}
	assets.push_back(primary_asset);

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	LocalVector<InstanceAssetRegistration> instance_assets;
	if (director != nullptr) {
		// Stable-superset collection: include every registered asset regardless of any
		// instance's current visibility or shadow-casting state. Visibility/casts_shadow
		// flips therefore never mutate atlas membership and never trigger a full atlas
		// repack via the atlas_generation gate below. The per-instance shadow filter is
		// still applied later by build_instance_buffer_for_renderer when populating the
		// instance buffer rows.
		director->collect_registered_assets_for_renderer(p_renderer, instance_assets);
	}
	std::sort(instance_assets.ptr(), instance_assets.ptr() + instance_assets.size(),
			[](const InstanceAssetRegistration &a, const InstanceAssetRegistration &b) {
				return a.asset_id < b.asset_id;
			});

	uint32_t next_dense_id = 1;
	for (const InstanceAssetRegistration &entry : instance_assets) {
		if (entry.asset_id == 0 || entry.data.is_null()) {
			continue;
		}
		ResidentAssetDescriptor asset;
		asset.submission_asset_id = entry.asset_id;
		asset.dense_asset_id = next_dense_id++;
		asset.data = entry.data;
		assets.push_back(asset);
	}

	// Generation split: atlas_generation hashes only inputs that affect packed atlas content
	// (asset list + per-asset content_revision + primary data + static chunk topology +
	// quantization config). instance_generation hashes everything else (per-instance state,
	// effectors, grading defaults, shadow filter, max_splats). The full early-out below
	// requires both to match the cached source_generation; if only instance_generation
	// changed, we skip the expensive atlas pack/upload and re-run only the cheap
	// per-instance buffer + grading buffer paths.
	uint64_t atlas_generation = 0x6a09e667f3bcc909ULL;
	atlas_generation = _mix_generation(atlas_generation, has_primary_data ? uint64_t(primary_data->get_instance_id()) : 0ULL);
	atlas_generation = _mix_generation(atlas_generation, has_primary_data ? primary_data->get_content_revision() : 0ULL);
	const Ref<GPUCuller> &gpu_culler = p_renderer->get_subsystem_state().gpu_culler;
	atlas_generation = _mix_generation(atlas_generation,
			gpu_culler.is_valid() ? gpu_culler->get_state().static_chunks_revision : 0ULL);
	for (const ResidentAssetDescriptor &asset : assets) {
		atlas_generation = _mix_generation(atlas_generation, asset.submission_asset_id);
		atlas_generation = _mix_generation(atlas_generation, asset.data.is_valid() ? uint64_t(asset.data->get_instance_id()) : 0ULL);
		atlas_generation = _mix_generation(atlas_generation, asset.data.is_valid() ? asset.data->get_content_revision() : 0ULL);
	}
	// Only perturb the atlas hash when the VRAM cap actually differs from the staging
	// ceiling (low-end device budget or an explicit override). On capable GPUs the cap
	// equals the ceiling, so the hash stays byte-identical to pre-#321 builds and there is
	// no one-time repack on deploy. The cap is a stable per-device fixpoint, so once mixed
	// it does not cause per-frame repacks.
	if (effective_atlas_cap_bytes != ResidentAtlasBudget::kResidentStagingCeilingBytes) {
		atlas_generation = _mix_generation(atlas_generation, effective_atlas_cap_bytes);
	}
	// Quantization config changes the PACKED ATLAS CONTENT (80-byte quantized layout +
	// the per-chunk quant params) vs the 144-byte raw layout, so it must perturb the
	// atlas hash — the "quantization config" input the comment above lists. Without this,
	// flipping quantization at runtime (e.g. a quality-tier switch to a quantized tier)
	// leaves atlas_changed false and the pack/upload is skipped, so the toggle is silently
	// ignored until an unrelated content/topology change lands.
	atlas_generation = _mix_generation(atlas_generation, quantize_atlas ? 1ULL : 0ULL);
	if (quantize_atlas) {
		atlas_generation = _mix_generation(atlas_generation, uint64_t(MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX)));
		atlas_generation = _mix_generation(atlas_generation, uint64_t(MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX)));
		atlas_generation = _mix_generation(atlas_generation, g_quantization_config.quantize_scales ? 1ULL : 0ULL);
	}

	uint64_t instance_generation = 0xbb67ae8584caa73bULL;
	if (director != nullptr) {
		instance_generation = _mix_generation(instance_generation, director->get_instance_generation_for_renderer(p_renderer));
		instance_generation = _mix_generation(instance_generation, director->get_sphere_effector_generation_for_renderer(p_renderer));
	}
	// Mix the renderer-wide grading-defaults counter so resident direct-data
	// grading rows (rows with no per-instance grading ref in director-less
	// direct-data flows) force a republish when the renderer's default
	// ColorGradingResource is swapped or mutated in place. The streaming
	// orchestrator already consumes this atomic in its fingerprint; the
	// resident publisher must do the same or these grading rows stay stale
	// until an unrelated content/topology change lands.
	instance_generation = _mix_generation(instance_generation,
			p_renderer->get_resource_state().instance_grading_defaults_generation.load(std::memory_order_relaxed));
	instance_generation = _mix_generation(instance_generation, p_renderer->is_shadow_instance_filter_enabled() ? 1ULL : 0ULL);
	instance_generation = _mix_generation(instance_generation, uint64_t(p_renderer->get_performance_settings().max_splats));

	const uint64_t source_generation = _mix_generation(atlas_generation, instance_generation);

	{
		const GaussianSplatRenderer::ResourceState &resource_state = p_renderer->get_resource_state();
		const GaussianRenderPipeline::InstancePipelineBuffers &published_buffers = p_renderer->get_instance_pipeline_buffers();
		auto owner_ok = [&](const RID &p_rid) {
			return !p_rid.is_valid() || p_renderer->get_resource_owner(p_rid, rd) == rd;
		};
		if (p_renderer->get_instance_backend_policy() == GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT &&
				p_renderer->has_instance_pipeline_buffers() &&
				p_renderer->has_instance_asset_remap() &&
				resource_state.instance_pipeline_contract_generation == source_generation &&
				resource_state.instance_pipeline_upload_generation == source_generation &&
				GaussianSplatting::InstancePipelineContract::has_atlas_buffers(published_buffers) &&
				GaussianSplatting::InstancePipelineContract::has_cull_buffers(published_buffers) &&
				GaussianSplatting::InstancePipelineContract::has_sort_buffers(published_buffers) &&
				GaussianSplatting::InstancePipelineContract::has_raster_buffers(published_buffers) &&
				owner_ok(resource_state.instance_visible_chunk_buffer) &&
				owner_ok(resource_state.instance_splat_ref_buffer) &&
				owner_ok(resource_state.instance_counter_buffer) &&
				owner_ok(resource_state.instance_chunk_dispatch_buffer) &&
				owner_ok(resource_state.instance_indirect_count_buffer) &&
				owner_ok(resource_state.instance_count_buffer)) {
			return true;
		}
	}

	LocalVector<InstanceDataGPU> instances;
	// Parallel to `instances`: each instance's FULL 64-bit submission asset identity,
	// the collision-free key into the published remap. Threaded into
	// update_instance_buffer() so two assets whose ObjectIDs collide in the low 32 bits
	// resolve to distinct dense atlas slots instead of aliasing.
	LocalVector<uint64_t> instance_submission_asset_ids;
	// Director "owns" this renderer's instance set when there's a world
	// submission for it. In that case an empty `instances` result is
	// intentional — e.g. a shadow-filtered pass that legitimately produced
	// no shadow casters, or a frame where every instance was visibility-
	// culled — and we must NOT fabricate a primary identity instance.
	// Doing so would draw the entire submission asset into shadow maps or
	// resurrect culled scenes. The bootstrap below is only for standalone
	// direct-data renderers (tests, tools, editor preview that bind raw
	// `set_gaussian_data()` with no world submission).
	const bool director_owns_instance_set = director != nullptr &&
			director->has_world_submission_for_renderer(p_renderer);
	if (director != nullptr) {
		director->build_instance_buffer_for_renderer(p_renderer, instances,
				p_renderer->is_shadow_instance_filter_enabled(), &instance_submission_asset_ids);
	}
	if (instances.is_empty() && has_primary_data && !director_owns_instance_set) {
		InstanceDataGPU bootstrap_instance = {};
		bootstrap_instance.rotation[3] = 1.0f;
		bootstrap_instance.inv_rotation[3] = 1.0f;
		bootstrap_instance.translation_scale[3] = 1.0f;
		bootstrap_instance.params[0] = 1.0f;
		bootstrap_instance.params[1] = 1.0f;
		bootstrap_instance.params[2] = 1.0f;
		bootstrap_instance.wind_params[3] = 1.0f;
		bootstrap_instance.effect_params[0] = 1.0f;
		bootstrap_instance.effect_params[1] = 1.0f;
		bootstrap_instance.ids[0] = kPrimaryResidentAssetId;
		bootstrap_instance.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY |
				GS_INSTANCE_FLAG_SCALE_IDENTITY |
				GS_INSTANCE_FLAG_TRANSLATION_ZERO;
		instances.push_back(bootstrap_instance);
		instance_submission_asset_ids.push_back(uint64_t(kPrimaryResidentAssetId));
	}

	if (instances.is_empty()) {
		if (r_reason) {
			*r_reason = "resident_no_instances";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	GaussianSplatRenderer::ResourceState &resource_state = p_renderer->get_resource_state();

	// Fast path: when atlas content is unchanged we skip the expensive pack_gaussians_range
	// loop and the four atlas storage-buffer uploads, falling through to the cheap per-instance
	// buffer + grading buffer refresh below. This is what makes per-instance churn (visibility
	// flips, transform tweens on interactables, wind animation, opacity changes, color grading
	// edits) cost ~1ms instead of repacking the whole resident atlas (which used to cost
	// hundreds of ms on dense scenes).
	//
	// Atlas buffer ownership must also match the current RenderingDevice. After a device
	// migration/reset the cached atlas RIDs would still pass the generation check but reference
	// freed resources from the previous device, and the contract would publish dead RIDs. The
	// cull/sort/raster buffers below have their own ensure_owner() remediation; the atlas
	// buffers do not, so we force a full repack here when any atlas RID has been re-owned.
	auto atlas_owner_ok = [&](const RID &p_rid) {
		return !p_rid.is_valid() || p_renderer->get_resource_owner(p_rid, rd) == rd;
	};
	const bool atlas_buffers_owned = atlas_owner_ok(resource_state.resident_atlas_gaussian_buffer) &&
			atlas_owner_ok(resource_state.resident_asset_meta_buffer) &&
			atlas_owner_ok(resource_state.resident_chunk_meta_buffer) &&
			atlas_owner_ok(resource_state.resident_asset_chunk_index_buffer);
	const bool atlas_changed = resource_state.instance_pipeline_atlas_generation != atlas_generation ||
			!p_renderer->has_instance_pipeline_buffers() ||
			!atlas_buffers_owned;

	uint32_t atlas_gaussian_count = 0;
	uint32_t atlas_max_chunk_count_per_asset = 0;
	uint32_t atlas_max_chunk_splats = 0;

	if (atlas_changed) {
		// If the atlas RIDs survived a device migration their cached values still pass the size
		// check inside _upload_typed_storage_buffer(), which would then call buffer_update() on
		// a RID that belongs to the old RenderingDevice. Drop our references to any foreign-
		// owned atlas RIDs first so the upload path recreates them on the current device. We do
		// not call free_owned_resource() because the previous device owns them and is responsible
		// for its own teardown -- mirrors the ensure_owner() pattern used for the cull/sort
		// buffers further down.
		if (!atlas_buffers_owned) {
			auto reset_foreign_atlas_rid = [&](RID &r_buffer, uint32_t &r_size) {
				if (r_buffer.is_valid() && p_renderer->get_resource_owner(r_buffer, rd) != rd) {
					r_buffer = RID();
					r_size = 0;
				}
			};
			reset_foreign_atlas_rid(resource_state.resident_atlas_gaussian_buffer,
					resource_state.resident_atlas_gaussian_buffer_size);
			reset_foreign_atlas_rid(resource_state.resident_asset_meta_buffer,
					resource_state.resident_asset_meta_buffer_size);
			reset_foreign_atlas_rid(resource_state.resident_chunk_meta_buffer,
					resource_state.resident_chunk_meta_buffer_size);
			reset_foreign_atlas_rid(resource_state.resident_asset_chunk_index_buffer,
					resource_state.resident_asset_chunk_index_buffer_size);
		}

		// Estimate the total source size, then decide the clamp plan. A default resident
		// GaussianSplatNode3D has no streaming fallback (resident-only by contract), so an
		// over-budget atlas is THINNED to fit rather than rejected -- rejecting would black-frame
		// the node under the #351 single-route invariant. The thinned atlas is hard-capped at
		// target_keep (<= effective cap <= the 2 GB per-buffer staging limit) by the global budget
		// below, so it always fits the upload regardless of source size. We only reject a source
		// beyond a generous COST ceiling: the slow-path pack loop still scans the whole source
		// once (generation-gated), so an absurdly large single resident asset (use streaming for
		// that) is bounded here.
		ResidentAtlasBudget::SubsetPlan subset_plan;
		{
			static constexpr uint64_t kResidentStagingUploadBytes = uint64_t(2) * 1024 * 1024 * 1024;
			static constexpr uint64_t kMaxResidentClampSourceBytes = uint64_t(4) * kResidentStagingUploadBytes;
			uint64_t total_gaussians = 0;
			for (const ResidentAssetDescriptor &asset : assets) {
				if (asset.data.is_valid()) {
					total_gaussians += uint64_t(asset.data->get_count());
				}
			}
			if (total_gaussians * sizeof(PackedGaussian) > kMaxResidentClampSourceBytes) {
				if (r_reason) {
					*r_reason = "resident_dataset_exceeds_clamp_source_limit";
				}
				// Do NOT clear instance pipeline buffers here — the streaming
				// orchestrator may have already published its own atlas/cull
				// buffers.  Clearing them would force MISSING_CULL_INPUTS on
				// every subsequent frame, preventing the streaming path from
				// ever becoming valid.
				return false;
			}
			// Decide whether the atlas fits the device VRAM budget, and if not, the global keep
			// ratio + target for the importance-ordered subset packed per chunk below. A source
			// above the staging limit clamps down to target_keep <= staging limit and renders
			// reduced instead of black-framing. The packed size drives how many splats fit the
			// cap, so the quantized (80 B) atlas must budget at 80 B — otherwise the clamp
			// over-thins it as if it were 144 B and the quantization VRAM win is thrown away.
			const uint32_t packed_atlas_stride = quantize_atlas
					? uint32_t(sizeof(PackedGaussianQuantized))
					: uint32_t(sizeof(PackedGaussian));
			subset_plan = ResidentAtlasBudget::compute_subset_plan(total_gaussians,
					effective_atlas_cap_bytes, packed_atlas_stride);
		}

		Vector<AssetMetaGPU> asset_meta_cpu;
		asset_meta_cpu.resize(next_dense_id);
		Vector<AssetChunkIndexGPU> asset_chunk_index_cpu;
		Vector<ChunkMetaGPU> chunk_meta_cpu;
		Vector<PackedGaussian> atlas_gaussian_cpu;
		// Parallel quantized atlas + per-chunk bounds, populated only when quantize_atlas.
		// The 144-byte atlas_gaussian_cpu stays empty in that case (and vice-versa); exactly
		// one is uploaded, keeping the non-quantized path byte-identical.
		Vector<PackedGaussianQuantized> atlas_gaussian_quantized_cpu;
		Vector<ChunkQuantizationGPU> quantization_gpu_cpu;

		uint32_t max_chunk_count_per_asset = 0;
		uint32_t max_chunk_splats = 0;
		// Hard global budget for the importance-LOD clamp. The per-chunk thinning below keeps
		// at least one splat from each non-empty chunk for coverage, which can overshoot the
		// device VRAM target on datasets with many tiny chunks; capping the cumulative kept
		// count here guarantees the resident atlas never exceeds the budget (Codex #420).
		// UINT32_MAX == unbounded when the atlas is not being thinned.
		uint32_t resident_remaining_budget = subset_plan.reduced
				? uint32_t(MIN<uint64_t>(subset_plan.target_keep, uint64_t(UINT32_MAX)))
				: UINT32_MAX;

		for (uint32_t asset_index = 0; asset_index < assets.size(); asset_index++) {
			const ResidentAssetDescriptor &asset = assets[asset_index];
			asset_meta_cpu.write[asset.dense_asset_id] = AssetMetaGPU();

			if (asset.data.is_null() || asset.data->get_count() <= 0) {
				continue;
			}

			LocalVector<ResidentChunkDescriptor> chunk_descriptors;
			_append_chunk_descriptors_for_asset(asset, chunk_descriptors);
			if (chunk_descriptors.is_empty()) {
				continue;
			}

			AssetMetaGPU asset_meta = {};
			asset_meta.lod_count = 1;
			asset_meta.sh_degree = asset.data->get_sh_degree();
			asset_meta.flags = gs_pack_asset_gpu_flags(asset.data->get_2d_mode(), _resolve_data_dc_encoding(asset.data));
			const AABB asset_bounds = asset.data->get_aabb();
			const Vector3 asset_center = asset_bounds.get_center();
			const Vector3 asset_half = asset_bounds.size * 0.5f;
			asset_meta.bounds_center_local[0] = asset_center.x;
			asset_meta.bounds_center_local[1] = asset_center.y;
			asset_meta.bounds_center_local[2] = asset_center.z;
			asset_meta.bounds_radius_local = asset_half.length();
			asset_meta.chunk_index_base = asset_chunk_index_cpu.size();
			// chunk_index_count / lod_ranges AND max_chunk_count_per_asset are finalized AFTER
			// the chunk loop: importance thinning under the global budget may drop whole chunks
			// (0 kept), so both the asset's chunk count and the dispatch / auxiliary-buffer sizing
			// must reflect the chunks actually emitted, not chunk_descriptors.size().

			// Give this asset a share of the keep budget proportional to its splat count so a
			// later (e.g. currently visible/casting) asset cannot be starved to zero chunks by
			// an earlier large asset in the stable superset. Visibility-independent, so the atlas
			// stays stable across visibility flips (Codex #420).
			uint32_t asset_remaining_budget = resident_remaining_budget;
			if (subset_plan.reduced) {
				asset_remaining_budget = ResidentAtlasBudget::resident_asset_budget(subset_plan.target_keep,
						uint64_t(asset.data->get_count()), subset_plan.source_count, resident_remaining_budget);
			}
			const uint32_t asset_budget_start = asset_remaining_budget;

			for (uint32_t chunk_index = 0; chunk_index < chunk_descriptors.size(); chunk_index++) {
				const ResidentChunkDescriptor &descriptor = chunk_descriptors[chunk_index];
				LocalVector<Gaussian> gaussian_snapshot;
				LocalVector<Vector3> sh_high_order_snapshot;
				uint32_t sh_first_order = 0;
				uint32_t sh_high_order = 0;
				bool captured = false;
				if (descriptor.source_index_remapped) {
					captured = asset.data->capture_indexed_chunk_snapshot(descriptor.source_indices.ptr(), descriptor.count,
							gaussian_snapshot, sh_high_order_snapshot, sh_first_order, sh_high_order);
				} else {
					captured = asset.data->capture_chunk_snapshot(descriptor.start_idx, descriptor.count,
							gaussian_snapshot, sh_high_order_snapshot, sh_first_order, sh_high_order);
				}
				if (!captured || gaussian_snapshot.size() != descriptor.count) {
					if (r_reason) {
						*r_reason = "resident_chunk_snapshot_failed";
					}
					p_renderer->clear_instance_pipeline_buffers();
					return false;
				}

				// Importance-ordered LOD: when the atlas is over the VRAM budget, keep the top
				// splats of THIS chunk (by the culler's opacity*scale importance) so every
				// spatial region keeps its most important splats -- preserves coverage on
				// real-scan content while the global budget below bounds the total. Compacts the
				// gaussian payload and its parallel high-order SH block in place, so sh_coeffs is
				// taken AFTER compaction.
				uint32_t chunk_pack_count = descriptor.count;
				if (subset_plan.reduced) {
					chunk_pack_count = ResidentAtlasBudget::compact_chunk_by_importance(
							gaussian_snapshot, sh_high_order_snapshot, sh_high_order,
							subset_plan.keep_ratio, asset_remaining_budget);
					if (chunk_pack_count == 0) {
						// Global VRAM budget exhausted (or this chunk's quota rounded to 0):
						// drop the chunk rather than emit a zero-splat ChunkMeta.
						continue;
					}
				}

				SHCompressionMetrics sh_metrics;
				const Vector3 *sh_coeffs = sh_high_order_snapshot.is_empty() ? nullptr : sh_high_order_snapshot.ptr();
				// atlas_base is the SPLAT index into the atlas buffer (the shader indexes
				// gaussians[atlas_base + i]); it advances by chunk in whichever atlas is active.
				uint32_t atlas_base = 0;
				// quant_id: the global chunk index the shader uses to dereference BOTH the
				// quantization buffer and this chunk's ChunkMeta — it is exactly the position
				// this chunk_meta takes in chunk_meta_cpu.
				const uint32_t quant_id = chunk_meta_cpu.size();

				if (quantize_atlas) {
					if (quant_id > 0xFFFFu) {
						// chunk_id is a uint16 field in PackedGaussianQuantized; an atlas with
						// more than 65535 chunks cannot address its per-chunk bounds. Fail the
						// publish (all-or-nothing) so the caller keeps the legacy path.
						if (r_reason) {
							*r_reason = "resident_quantization_chunk_overflow";
						}
						p_renderer->clear_instance_pipeline_buffers();
						return false;
					}
					// Per-chunk bounds MUST be computed on the THINNED snapshot (the exact splats
					// uploaded), not descriptor.count, or the shader would dequantize against
					// bounds that don't match the stored positions.
					// Scale quantization is MANDATORY here: the 80-byte layout has no unquantized
					// scale field, and the shader substitutes scale=vec3(1.0) when scale_bits==0
					// (which renders every splat as a unit blob). g_quantization_config.quantize_scales
					// only chooses the bit depth story for callers that CAN fall back to FP32 scale;
					// the resident quantized atlas cannot, so scales are always quantized.
					// Clamp to the uint16 slot width: position_bits is project-settable up to 24,
					// which the 80-byte layout cannot store (see GS_QUANTIZED_BITS_MAX). The
					// clamped value flows into ChunkQuantizationGPU.position_bits too, so CPU
					// packing and GPU dequantization agree.
					const uint32_t q_position_bits = MIN(g_quantization_config.position_bits, GS_QUANTIZED_BITS_MAX);
					const uint32_t q_scale_bits = MIN(g_quantization_config.scale_bits, GS_QUANTIZED_BITS_MAX);
					ChunkQuantizationInfo chunk_quant;
					chunk_quant.compute_from_gaussians(gaussian_snapshot, 0, chunk_pack_count,
							q_position_bits, q_scale_bits,
							/*quantize_scale=*/true);

					atlas_base = atlas_gaussian_quantized_cpu.size();
					const int prev_size = atlas_gaussian_quantized_cpu.size();
					if (atlas_gaussian_quantized_cpu.resize(prev_size + int(chunk_pack_count)) != OK) {
						if (r_reason) {
							*r_reason = "resident_quantized_atlas_alloc_failed";
						}
						p_renderer->clear_instance_pipeline_buffers();
						return false;
					}
					pack_gaussians_range_quantized(gaussian_snapshot, 0, chunk_pack_count,
							chunk_quant, uint16_t(quant_id),
							atlas_gaussian_quantized_cpu.ptrw() + prev_size, sh_metrics, sh_coeffs,
							sh_first_order, sh_high_order);

					ChunkQuantizationGPU quant_gpu = {};
					quant_gpu.position_min[0] = chunk_quant.position_min.x;
					quant_gpu.position_min[1] = chunk_quant.position_min.y;
					quant_gpu.position_min[2] = chunk_quant.position_min.z;
					quant_gpu.position_bits = chunk_quant.position_bits;
					quant_gpu.position_range[0] = chunk_quant.position_range.x;
					quant_gpu.position_range[1] = chunk_quant.position_range.y;
					quant_gpu.position_range[2] = chunk_quant.position_range.z;
					quant_gpu.scale_bits = chunk_quant.scales_quantized ? chunk_quant.scale_bits : 0u;
					quant_gpu.scale_min[0] = chunk_quant.scale_min.x;
					quant_gpu.scale_min[1] = chunk_quant.scale_min.y;
					quant_gpu.scale_min[2] = chunk_quant.scale_min.z;
					quant_gpu.start_index = atlas_base;
					quant_gpu.scale_range[0] = chunk_quant.scale_range.x;
					quant_gpu.scale_range[1] = chunk_quant.scale_range.y;
					quant_gpu.scale_range[2] = chunk_quant.scale_range.z;
					quant_gpu.count = chunk_pack_count;
					quantization_gpu_cpu.push_back(quant_gpu);
				} else {
					Vector<PackedGaussian> packed_chunk;
					// #787: abort the publish if the chunk could not be packed. Continuing would
					// append zero records while chunk_meta.splat_count below still reports
					// chunk_pack_count, publishing atlas metadata that points at splats which
					// were never written. Mirrors the quantized branch's alloc-failure handling.
					if (!pack_gaussians_range(gaussian_snapshot, 0, chunk_pack_count, packed_chunk, sh_metrics, sh_coeffs,
								sh_first_order, sh_high_order) ||
							(uint32_t)packed_chunk.size() != chunk_pack_count) {
						if (r_reason) {
							*r_reason = "resident_atlas_pack_failed";
						}
						// Drop the previously published contract like every other failure path
						// in this function does. Leaving it live keeps has_instance_pipeline_-
						// buffers() true and the old atlas/cull/sort RIDs allocated while the
						// new contract is rejected, so later frames retry the same pack against
						// the same memory pressure that just failed.
						p_renderer->clear_instance_pipeline_buffers();
						return false;
					}

					atlas_base = atlas_gaussian_cpu.size();
					for (int i = 0; i < packed_chunk.size(); i++) {
						atlas_gaussian_cpu.push_back(packed_chunk[i]);
					}
				}

				ChunkMetaGPU chunk_meta = {};
				chunk_meta.atlas_base = atlas_base;
				chunk_meta.splat_count = chunk_pack_count;
				// Per-chunk quantization: quant_base is the chunk's index into the quantization
				// buffer (== quant_id == this chunk's ChunkMeta index); the shader reads
				// quantization_buffer.chunks[quant_base]. quant_count is 1 (one bounds set/chunk).
				chunk_meta.quant_base = quantize_atlas ? quant_id : 0u;
				chunk_meta.quant_count = quantize_atlas ? 1u : 0u;
				const Vector3 chunk_center = descriptor.bounds.get_center();
				const Vector3 chunk_half = descriptor.bounds.size * 0.5f;
				chunk_meta.bounds_center_local[0] = chunk_center.x;
				chunk_meta.bounds_center_local[1] = chunk_center.y;
				chunk_meta.bounds_center_local[2] = chunk_center.z;
				chunk_meta.bounds_radius_local = chunk_half.length();
				chunk_meta.asset_id = asset.dense_asset_id;
				chunk_meta.lod_level = 0;
				chunk_meta.flags = asset_meta.flags;
				chunk_meta.sh_limit = CLAMP(asset.data->get_sh_degree(), 0u, 3u);
				chunk_meta_cpu.push_back(chunk_meta);

				AssetChunkIndexGPU chunk_index_gpu = {};
				chunk_index_gpu.chunk_id = chunk_meta_cpu.size() - 1;
				asset_chunk_index_cpu.push_back(chunk_index_gpu);
				max_chunk_splats = MAX(max_chunk_splats, chunk_pack_count);
			}

			// Return this asset's unspent budget to the global pool so later assets can use it.
			if (subset_plan.reduced) {
				resident_remaining_budget -= (asset_budget_start - asset_remaining_budget);
			}

			// Finalize the asset's chunk-index range from the chunks actually emitted (some may
			// have been dropped to 0 splats by the importance-LOD budget above), and size
			// Stage-A dispatch + the visible-chunk/splat-ref/sort buffers from that emitted count
			// so the clamp shrinks the auxiliary buffers too, not just the atlas (Codex #420).
			asset_meta.chunk_index_count = asset_chunk_index_cpu.size() - asset_meta.chunk_index_base;
			asset_meta.lod_ranges[0].base = asset_meta.chunk_index_base;
			asset_meta.lod_ranges[0].count = asset_meta.chunk_index_count;
			max_chunk_count_per_asset = MAX<uint32_t>(max_chunk_count_per_asset, asset_meta.chunk_index_count);
			asset_meta_cpu.write[asset.dense_asset_id] = asset_meta;
		}

		const uint32_t atlas_splat_total = quantize_atlas
				? uint32_t(atlas_gaussian_quantized_cpu.size())
				: uint32_t(atlas_gaussian_cpu.size());
		if (atlas_splat_total == 0) {
			if (r_reason) {
				*r_reason = "resident_atlas_empty";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}

		// Atlas gaussian buffer: exactly one of the two layouts is uploaded. The quantized
		// path also uploads the per-chunk bounds buffer the shader dereferences by quant_id.
		bool atlas_upload_ok = quantize_atlas
				? _upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_atlas_gaussian_buffer,
						resource_state.resident_atlas_gaussian_buffer_size, "GS_ResidentAtlasGaussians", atlas_gaussian_quantized_cpu)
				: _upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_atlas_gaussian_buffer,
						resource_state.resident_atlas_gaussian_buffer_size, "GS_ResidentAtlasGaussians", atlas_gaussian_cpu);
		if (!atlas_upload_ok ||
				!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_asset_meta_buffer,
					resource_state.resident_asset_meta_buffer_size, "GS_ResidentAssetMeta", asset_meta_cpu) ||
				!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_chunk_meta_buffer,
					resource_state.resident_chunk_meta_buffer_size, "GS_ResidentChunkMeta", chunk_meta_cpu) ||
				!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_asset_chunk_index_buffer,
					resource_state.resident_asset_chunk_index_buffer_size, "GS_ResidentAssetChunkIndex", asset_chunk_index_cpu)) {
			if (r_reason) {
				*r_reason = "resident_dataset_upload_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		if (quantize_atlas) {
			if (!_upload_typed_storage_buffer(p_renderer, rd, resource_state.resident_quantization_buffer,
						resource_state.resident_quantization_buffer_size, "GS_ResidentQuantization", quantization_gpu_cpu)) {
				if (r_reason) {
					*r_reason = "resident_quantization_upload_failed";
				}
				p_renderer->clear_instance_pipeline_buffers();
				return false;
			}
		} else {
			// Release any stale quantization buffer from a previous quantized publish so the
			// non-quantized contract carries no dangling per-chunk bounds.
			p_renderer->free_owned_resource(rd, resource_state.resident_quantization_buffer);
			resource_state.resident_quantization_buffer_size = 0;
		}

		atlas_gaussian_count = atlas_splat_total;
		atlas_max_chunk_count_per_asset = max_chunk_count_per_asset;
		atlas_max_chunk_splats = max_chunk_splats;

		// Cache derived sizes so the next instance-only update can re-publish the contract
		// without re-running the pack loop.
		resource_state.resident_atlas_gaussian_count = atlas_gaussian_count;
		resource_state.resident_dispatch_chunk_count = atlas_max_chunk_count_per_asset;
		resource_state.resident_max_chunk_splats = atlas_max_chunk_splats;
		resource_state.resident_atlas_pack_count++;
		// VRAM-budget LOD clamp telemetry (observable; never a silent drop). Cached so the
		// fast instance-only path re-surfaces the same values; the packed count is
		// resident_atlas_gaussian_count above.
		resource_state.resident_atlas_reduced = subset_plan.reduced;
		resource_state.resident_atlas_source_count = uint32_t(subset_plan.source_count);
		resource_state.resident_atlas_keep_ratio = subset_plan.reduced ? float(subset_plan.keep_ratio) : 1.0f;
		if (subset_plan.reduced) {
			WARN_PRINT_ONCE(vformat("[ResidentContract] Atlas exceeds the resident VRAM budget; thinned by "
					"importance to fit: %d -> %d splats (keep_ratio=%.3f, cap=%d MB). Reduce the asset's splat "
					"count or render it via GaussianSplatWorld3D streaming for full density.",
					int(subset_plan.source_count), int(atlas_gaussian_count), subset_plan.keep_ratio,
					int(subset_plan.cap_bytes >> 20)));
		}
	} else {
		// Atlas unchanged. Reuse the cached size metrics from the last slow-path publish.
		// Atlas storage buffers (resident_atlas_gaussian_buffer etc.) keep their current
		// contents and RIDs.
		atlas_gaussian_count = resource_state.resident_atlas_gaussian_count;
		atlas_max_chunk_count_per_asset = resource_state.resident_dispatch_chunk_count;
		atlas_max_chunk_splats = resource_state.resident_max_chunk_splats;
	}

	const uint32_t instance_count = instances.size();
	GaussianRenderPipeline::InstancePipelineBuffers buffers;
	buffers.atlas_gaussian_buffer = resource_state.resident_atlas_gaussian_buffer;
	buffers.atlas_gaussian_count = atlas_gaussian_count;
	buffers.asset_meta_buffer = resource_state.resident_asset_meta_buffer;
	buffers.chunk_meta_buffer = resource_state.resident_chunk_meta_buffer;
	buffers.asset_chunk_index_buffer = resource_state.resident_asset_chunk_index_buffer;
	// Derive from the resource-state buffer so the cached instance-only re-publish path
	// (which skips the pack loop) restores the quantized contract too: a quantized publish
	// leaves resident_quantization_buffer valid; a non-quantized one frees it.
	buffers.quantization_required = resource_state.resident_quantization_buffer.is_valid();
	buffers.quantization_buffer = resource_state.resident_quantization_buffer;
	buffers.dispatch_chunk_count = MAX<uint32_t>(1u, atlas_max_chunk_count_per_asset);
	buffers.max_chunk_splats = MAX<uint32_t>(1u, atlas_max_chunk_splats);

	uint64_t max_visible_splats_u64 = atlas_gaussian_count;
	const int configured_max_splats = p_renderer->get_performance_settings().max_splats;
	if (configured_max_splats > 0) {
		max_visible_splats_u64 = MIN<uint64_t>(max_visible_splats_u64, uint64_t(configured_max_splats));
	}
	const uint64_t instance_requirement = uint64_t(instance_count) * uint64_t(buffers.dispatch_chunk_count) *
			uint64_t(buffers.max_chunk_splats);
	max_visible_splats_u64 = MAX<uint64_t>(max_visible_splats_u64, instance_requirement);
	const uint64_t sort_cap = g_gpu_sorting_config.max_sort_elements > 0
			? uint64_t(g_gpu_sorting_config.max_sort_elements)
			: uint64_t(UINT32_MAX);
	buffers.max_visible_splats = uint32_t(MIN<uint64_t>(max_visible_splats_u64, sort_cap));
	uint64_t max_visible_chunks_u64 = uint64_t(instance_count) * uint64_t(buffers.dispatch_chunk_count);
	buffers.max_visible_chunks = uint32_t(MIN<uint64_t>(max_visible_chunks_u64,
			uint64_t(MAX<uint32_t>(1u, buffers.max_visible_splats))));

	auto ensure_owner = [&](RID &r_buffer, uint32_t *r_capacity) {
		if (r_buffer.is_valid() && p_renderer->get_resource_owner(r_buffer, rd) != rd) {
			p_renderer->free_owned_resource(rd, r_buffer);
			if (r_capacity != nullptr) {
				*r_capacity = 0;
			}
		}
	};
	ensure_owner(resource_state.instance_visible_chunk_buffer, &resource_state.instance_visible_chunk_capacity);
	ensure_owner(resource_state.instance_splat_ref_buffer, &resource_state.instance_splat_ref_capacity);
	ensure_owner(resource_state.instance_counter_buffer, nullptr);
	ensure_owner(resource_state.instance_chunk_dispatch_buffer, nullptr);
	ensure_owner(resource_state.instance_indirect_count_buffer, nullptr);
	ensure_owner(resource_state.instance_count_buffer, nullptr);

	if (!resource_state.instance_visible_chunk_buffer.is_valid() ||
			resource_state.instance_visible_chunk_capacity < buffers.max_visible_chunks) {
		p_renderer->free_owned_resource(rd, resource_state.instance_visible_chunk_buffer);
		const uint32_t buffer_size = MAX<uint32_t>(1u, buffers.max_visible_chunks) * sizeof(VisibleChunkRefGPU);
		resource_state.instance_visible_chunk_buffer = rd->storage_buffer_create(buffer_size);
		if (!resource_state.instance_visible_chunk_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_visible_chunk_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_visible_chunk_buffer, "GS_InstanceVisibleChunks");
		p_renderer->track_resource_owner(resource_state.instance_visible_chunk_buffer, rd);
		resource_state.instance_visible_chunk_capacity = MAX<uint32_t>(1u, buffers.max_visible_chunks);
	}
	buffers.visible_chunk_buffer = resource_state.instance_visible_chunk_buffer;

	if (!resource_state.instance_splat_ref_buffer.is_valid() ||
			resource_state.instance_splat_ref_capacity < buffers.max_visible_splats) {
		p_renderer->free_owned_resource(rd, resource_state.instance_splat_ref_buffer);
		const uint32_t buffer_size = MAX<uint32_t>(1u, buffers.max_visible_splats) * sizeof(SplatRefGPU);
		resource_state.instance_splat_ref_buffer = rd->storage_buffer_create(buffer_size);
		if (!resource_state.instance_splat_ref_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_splat_ref_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_splat_ref_buffer, "GS_InstanceSplatRefs");
		p_renderer->track_resource_owner(resource_state.instance_splat_ref_buffer, rd);
		resource_state.instance_splat_ref_capacity = MAX<uint32_t>(1u, buffers.max_visible_splats);
	}
	buffers.splat_ref_buffer = resource_state.instance_splat_ref_buffer;

	if (!resource_state.instance_counter_buffer.is_valid()) {
		// 3 uints: [0]=visible_chunk_count, [1]=overflowed_chunks, [2]=frustum_culled_chunks
		// (M0 telemetry; written only by frustum_cull.glsl, survives the Stage-B counter reuse).
		resource_state.instance_counter_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 3);
		if (!resource_state.instance_counter_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_counter_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_counter_buffer, "GS_InstanceCounters");
		p_renderer->track_resource_owner(resource_state.instance_counter_buffer, rd);
	}
	buffers.counter_buffer = resource_state.instance_counter_buffer;

	if (!resource_state.instance_chunk_dispatch_buffer.is_valid()) {
		resource_state.instance_chunk_dispatch_buffer = rd->storage_buffer_create(
				sizeof(uint32_t) * 3, Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		if (!resource_state.instance_chunk_dispatch_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_chunk_dispatch_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_chunk_dispatch_buffer, "GS_InstanceChunkDispatch");
		p_renderer->track_resource_owner(resource_state.instance_chunk_dispatch_buffer, rd);
	}
	buffers.chunk_dispatch_buffer = resource_state.instance_chunk_dispatch_buffer;

	if (!resource_state.instance_indirect_count_buffer.is_valid()) {
		resource_state.instance_indirect_count_buffer = rd->storage_buffer_create(
				sizeof(GaussianSplatting::IndirectDispatchLayout), Vector<uint8_t>(),
				RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		if (!resource_state.instance_indirect_count_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_indirect_count_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_indirect_count_buffer, "GS_InstanceIndirectCount");
		p_renderer->track_resource_owner(resource_state.instance_indirect_count_buffer, rd);
	}
	buffers.indirect_count_buffer = resource_state.instance_indirect_count_buffer;

	if (!resource_state.instance_count_buffer.is_valid()) {
		// Zero-init: the instance-count clamp accumulates a STICKY overflow_flag
		// (C4b/G4, Channel B), so the buffer must start clean and never accumulate
		// onto uninitialized memory.
		Vector<uint8_t> instance_count_init;
		instance_count_init.resize(sizeof(GaussianSplatting::IndirectDispatchLayout));
		instance_count_init.fill(0);
		resource_state.instance_count_buffer = rd->storage_buffer_create(
				instance_count_init.size(), instance_count_init);
		if (!resource_state.instance_count_buffer.is_valid()) {
			if (r_reason) {
				*r_reason = "resident_instance_count_buffer_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		rd->set_resource_name(resource_state.instance_count_buffer, "GS_InstanceCount");
		p_renderer->track_resource_owner(resource_state.instance_count_buffer, rd);
	}
	buffers.instance_count_buffer = resource_state.instance_count_buffer;

	// Mirror the render_streaming_orchestrator pattern: forward the persistent
	// per-renderer instance_grading_buffer RID through the local `buffers`
	// snapshot so it survives publish_instance_pipeline_contract() below. The
	// subsequent update_instance_grading_buffer() call (~line 687) refreshes
	// the GPU contents; this line ensures the RID itself lands in
	// instance_pipeline_buffers even on paths that hit clear-then-republish
	// sequences, so first_raster_violation() doesn't read a stale RID() in
	// the gap before update_instance_grading_buffer() runs.
	buffers.instance_grading_buffer = resource_state.instance_grading_buffer;

	Ref<GPUSortingPipeline> sorting_pipeline = p_renderer->get_subsystem_state().sorting_pipeline;
	if (sorting_pipeline.is_null()) {
		if (r_reason) {
			*r_reason = "resident_sorting_pipeline_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}
	const uint32_t required_sort_capacity = MAX<uint32_t>(1u, buffers.max_visible_splats);
	if (required_sort_capacity > sorting_pipeline->get_max_elements()) {
		sorting_pipeline->rebuild_sorter(required_sort_capacity);
	}
	sorting_pipeline->ensure_buffers(required_sort_capacity);
	const SortBufferHandles sort_handles = sorting_pipeline->get_buffer_handles();
	if (!sort_handles.valid) {
		if (r_reason) {
			*r_reason = "resident_sort_buffers_unavailable";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}
	buffers.sort_key_buffer = sort_handles.keys_buffer;
	buffers.sort_value_buffer = sort_handles.indices_buffer;
	if (sort_handles.capacity > 0 && buffers.max_visible_splats > sort_handles.capacity) {
		buffers.max_visible_splats = sort_handles.capacity;
		buffers.max_visible_chunks = MIN<uint32_t>(buffers.max_visible_chunks, buffers.max_visible_splats);
	}

	GaussianRenderPipeline::PublishedInstanceAssetRemap remap;
	remap.asset_to_dense_id.insert(kPrimaryResidentAssetId, 0u);
	for (const ResidentAssetDescriptor &asset : assets) {
		remap.asset_to_dense_id.insert(asset.submission_asset_id, asset.dense_asset_id);
	}
	remap.generation = source_generation == 0 ? 1u : source_generation;
	remap.valid = true;

	p_renderer->publish_instance_pipeline_contract(buffers, remap,
			GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT, source_generation, "atlas_emulation");
	resource_state.instance_pipeline_contract_generation = source_generation;
	resource_state.instance_pipeline_atlas_generation = atlas_generation;
	resource_state.instance_pipeline_content_generation = source_generation;
	resource_state.instance_pipeline_contract_fingerprint = 0;
	resource_state.instance_pipeline_upload_generation = source_generation;
	resource_state.instance_pipeline_upload_fingerprint = 0;

	if (!p_renderer->update_instance_buffer(instances, remap, &instance_submission_asset_ids)) {
		if (r_reason) {
			*r_reason = "resident_instance_upload_failed";
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	// Upload per-instance color grading. Walks the same director instance list so rows
	// line up 1:1 with SplatRefGPU.instance_id. Must run whenever instance_buffer is
	// re-uploaded because the instance list may have changed rows.
	{
		LocalVector<InstanceGradingGPU> gradings;
		if (director != nullptr) {
			director->build_instance_grading_buffer_for_renderer(p_renderer, gradings,
					p_renderer->is_shadow_instance_filter_enabled());
		}
		if (gradings.is_empty() && !instances.is_empty()) {
			// If director records are absent but the resident direct-data path
			// injected a bootstrap instance above, seed grading rows from the
			// renderer-wide color_grading so direct-data / worldless renderers
			// keep their grading on this path instead of being forced to neutral.
			const Ref<ColorGradingResource> renderer_default = p_renderer->get_color_grading();
			gradings.resize(instances.size());
			for (uint32_t i = 0; i < gradings.size(); ++i) {
				GaussianSplatSceneDirector::fill_instance_grading_entry(renderer_default, gradings[i]);
			}
		}
		// Contract: the grading buffer MUST have the same row count as the
		// instance buffer. Shader indexing (instance_buffer.instances[splat_ref.instance_id]
		// and instance_grading_buffer.gradings[splat_ref.instance_id]) relies on 1:1
		// parity. If the filter logic ever drifts between the two director build
		// steps, early-fail here instead of uploading a short buffer the shader
		// would then read past end-of-array.
		if (gradings.size() != instances.size()) {
			ERR_PRINT_ONCE(vformat(
					"[ResidentContract] grading buffer row count (%d) does not match instance buffer row count (%d). "
					"build_instance_grading_buffer_for_renderer and build_instance_buffer_for_renderer must apply identical filters.",
					int(gradings.size()), int(instances.size())));
			if (r_reason) {
				*r_reason = "resident_grading_instance_row_mismatch";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
		if (!p_renderer->update_instance_grading_buffer(gradings)) {
			if (r_reason) {
				*r_reason = "resident_grading_upload_failed";
			}
			p_renderer->clear_instance_pipeline_buffers();
			return false;
		}
	}

	const GaussianRenderPipeline::InstancePipelineBuffers &published_buffers = p_renderer->get_instance_pipeline_buffers();
	InvariantViolationReason violation_reason = InvariantViolationReason::NONE;
	if (!GaussianSplatting::InstancePipelineContract::has_atlas_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_atlas_violation(published_buffers);
	} else if (!GaussianSplatting::InstancePipelineContract::has_cull_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_cull_violation(published_buffers);
	} else if (!GaussianSplatting::InstancePipelineContract::has_sort_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_sort_violation(published_buffers);
	} else if (!GaussianSplatting::InstancePipelineContract::has_raster_buffers(published_buffers)) {
		violation_reason = GaussianSplatting::InstancePipelineContract::first_raster_violation(published_buffers);
	}
	if (violation_reason != InvariantViolationReason::NONE) {
		if (r_reason) {
			*r_reason = GaussianSplatting::InstancePipelineContract::get_violation_reason_name(violation_reason);
		}
		p_renderer->clear_instance_pipeline_buffers();
		return false;
	}

	return true;
}

} // namespace ResidentInstanceContractPublisher
