#include "streaming_chunk_bake.h"

#include "../core/gaussian_data.h"
#include "../core/gs_vector_alloc.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_streaming.h"
#include "../core/streaming_quantization.h"

#include <cstring>

namespace StreamingChunkBakeIO {

PackedByteArray serialize_records(const Vector<StreamingChunkBakeRecord> &p_records) {
    PackedByteArray bytes;
    const int num = p_records.size();
    if (num <= 0) {
        return bytes;
    }
    const int byte_count = num * int(sizeof(StreamingChunkBakeRecord));
    // #798: memcpy through ptrw(). On a failed resize ptrw() is nullptr (CowData's
    // _fork_allocate(0) unrefs and leaves _ptr null), and memcpy to null is UB -- unlike
    // the write[] class in #794 there is no CRASH_BAD_INDEX to turn it into a named trap.
    // Fail closed to the empty array this function already returns for "nothing to do".
    if (!gs_resize_or_fail(bytes, byte_count, "StreamingChunkBakeIO::serialize_records")) {
        return PackedByteArray();
    }
    memcpy(bytes.ptrw(), p_records.ptr(), size_t(byte_count));
    return bytes;
}

bool deserialize_records(const PackedByteArray &p_bytes, Vector<StreamingChunkBakeRecord> &r_records) {
    r_records.clear();
    const int byte_count = p_bytes.size();
    if (byte_count <= 0) {
        return true;
    }
    if (byte_count % int(sizeof(StreamingChunkBakeRecord)) != 0) {
        return false;
    }
    const int num = byte_count / int(sizeof(StreamingChunkBakeRecord));
    // #798: `num` is derived from the INPUT BYTE COUNT, i.e. from file data, so this
    // allocation is attacker/corruption-influenced and reachable without memory pressure.
    // memcpy through a null ptrw() is UB; fail closed via this function's existing bool
    // contract, leaving r_records cleared as the caller already expects on failure.
    if (!gs_resize_or_fail(r_records, num, "StreamingChunkBakeIO::deserialize_records")) {
        return false;
    }
    memcpy(r_records.ptrw(), p_bytes.ptr(), size_t(byte_count));
    return true;
}

}  // namespace StreamingChunkBakeIO

void bake_streaming_chunks_for_asset(
        Ref<GaussianSplatAsset> p_asset,
        const Ref<GaussianData> &p_data,
        bool p_include_primary_morton,
        const ChunkQuantizationRuntimeParams *p_quant_params) {
    if (p_asset.is_null() || p_data.is_null()) {
        return;
    }
    const int total = p_data->get_count();
    if (total <= 0) {
        return;
    }

    const uint32_t chunk_size = GaussianStreamingSystem::CHUNK_SIZE;
    const uint32_t splat_count = uint32_t(total);
    const uint32_t num_chunks = (splat_count + chunk_size - 1) / chunk_size;

    Vector<StreamingChunkBakeRecord> records;
    // #798 review (post-merge review of #802): the derived sweep MISSED this one because the
    // matching `records.write[int(i)]` is ~38 lines below, outside the 11-line window the
    // predicate scanned. It is the #794 write[] class: the loop is bounded by num_chunks, not
    // by records.size(), so a failed resize traps in CRASH_BAD_INDEX. Fail closed the same way
    // the early bails in this function do -- leave the asset's streaming records untouched, so
    // has_baked_streaming_chunks() stays false and the runtime takes its full-rebuild path.
    if (!gs_resize_or_fail(records, int64_t(num_chunks), "StreamingChunkBake::bake chunk records")) {
        return;
    }

    // Per-asset registration path does NOT consume Morton-sorted primary
    // source indices (see gaussian_streaming.cpp build_primary_spatial=false
    // at the call site); keep them empty unless explicitly asked.
    PackedInt32Array primary_indices;
    if (p_include_primary_morton) {
        // Reserved for future primary-spatial bakes. Per the current plan,
        // per-asset bakes leave this empty; runtime fast-path skips Morton
        // remap unless the bake provides indices.
        // #798: splat_count is asset-sized. `write` is a raw ptrw() pointer and the
        // loop is bounded by splat_count, not by primary_indices.size(), so a failed
        // resize gives a null `write` and the very first store is a wild write -- no
        // CRASH_BAD_INDEX stands in the way. Fail closed to the empty array that the
        // surrounding contract already treats as "no Morton indices baked".
        if (!gs_resize_or_fail(primary_indices, int64_t(splat_count),
                    "StreamingChunkBake::bake primary Morton indices")) {
            primary_indices = PackedInt32Array();
        } else {
            int32_t *write = primary_indices.ptrw();
            for (uint32_t i = 0; i < splat_count; i++) {
                write[i] = int32_t(i);
            }
        }
    }

    const bool bake_quant = p_quant_params && p_quant_params->enabled;
    LocalVector<ChunkQuantizationInfo> baked_quant;
    if (bake_quant) {
        baked_quant.resize(num_chunks);
    }

    const LocalVector<Gaussian> *gaussians_storage = nullptr;
    if (bake_quant) {
        gaussians_storage = &p_data->get_gaussian_storage();
    }

    for (uint32_t i = 0; i < num_chunks; i++) {
        StreamingChunkBakeRecord &rec = records.write[int(i)];
        rec.start_idx = i * chunk_size;
        rec.count = MIN(chunk_size, splat_count - rec.start_idx);

        Vector3 center;
        AABB bounds;
        bool bounds_initialized = false;
        float max_radius = 0.0f;

        for (uint32_t local_idx = 0; local_idx < rec.count; local_idx++) {
            const uint32_t source_index = rec.start_idx + local_idx;
            const Gaussian g = p_data->get_gaussian(int(source_index));
            const Vector3 pos = g.position;
            center += pos;

            const float radius = MAX(MAX(g.scale.x, g.scale.y), g.scale.z);
            max_radius = MAX(max_radius, radius);
            const AABB splat_aabb(pos - Vector3(radius, radius, radius),
                    Vector3(radius * 2, radius * 2, radius * 2));

            if (!bounds_initialized) {
                bounds = splat_aabb;
                bounds_initialized = true;
            } else {
                bounds = bounds.merge(splat_aabb);
            }
        }

        if (rec.count > 0) {
            center /= float(rec.count);
        }
        rec.center = center;
        rec.max_radius = max_radius;
        rec.bounds = bounds;

        if (bake_quant && gaussians_storage) {
            baked_quant[i].compute_from_gaussians(
                    *gaussians_storage,
                    rec.start_idx,
                    rec.count,
                    p_quant_params->position_bits,
                    p_quant_params->scale_bits,
                    p_quant_params->scales_quantized);
        }
    }

    PackedByteArray records_bytes = StreamingChunkBakeIO::serialize_records(records);
    p_asset->set_streaming_chunk_records(records_bytes);
    p_asset->set_streaming_primary_source_indices(primary_indices);
    p_asset->set_streaming_chunk_size_used(chunk_size);

    if (bake_quant) {
        PackedByteArray quant_bytes;
        // #798: memcpy through ptrw(); null on a failed resize. Fail closed to the SAME
        // empty-records state the bake_quant=false branch below produces, so the asset
        // ends up in a state the loader already handles rather than a novel one.
        if (!gs_resize_or_fail(quant_bytes, int64_t(num_chunks) * int64_t(sizeof(ChunkQuantizationInfo)),
                    "StreamingChunkBake::bake quantization records")) {
            p_asset->set_streaming_quantization_records(PackedByteArray());
        } else {
            memcpy(quant_bytes.ptrw(), baked_quant.ptr(), num_chunks * sizeof(ChunkQuantizationInfo));
            p_asset->set_streaming_quantization_records(quant_bytes);
        }
    } else {
        p_asset->set_streaming_quantization_records(PackedByteArray());
    }
}
