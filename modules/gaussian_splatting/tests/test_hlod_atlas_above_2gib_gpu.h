#pragma once
#include "test_macros.h"
#include "../compute/depth_compute.glsl.gen.h"
#include "../compute/compute_infrastructure.h"
#include "../core/gaussian_semantic.h"
#include "../renderer/device_vram_ledger.h"
#include "../renderer/gpu_sorting_constants.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/streaming_quantization.h"
#include "../renderer/gs_region_override_buffer.h"
#include "../renderer/splat_ref_encoding.h"
#include "../renderer/sorting_contract.h"
#include <cstring>

// BV-17: the streamed HLOD atlas is one storage buffer and may exceed 2^31 bytes. depth_compute.glsl used to bound the page
// lookup with atlas_gaussian_buffer.gaussians.length(); on the RTX 3090 (Windows, Vulkan, maxStorageBufferRange 4294967295)
// that length is wrong for a binding above 2^31 bytes and every splat was rejected (the reef world drew nothing at a
// 2,148,450,304 B atlas). The pass now takes the record count from the host (InstanceDepthParamsGPU::atlas_info.x).
// This case draws one splat from a record whose byte offset is above 2^31, through the production HLOD relative depth pass.
namespace TestHlodAtlasAbove2GiB {
struct Resources {
    RenderingDevice *rd;
    LocalVector<RID> owned;
    ~Resources() { for (uint32_t i = owned.size(); i-- > 0;) { rd->free(owned[i]); } }
    RID keep(RID p_rid) { if (p_rid.is_valid()) { owned.push_back(p_rid); } return p_rid; }
    RID buffer(uint32_t p_size, const void *p_data, bool p_uniform = false) {
        Vector<uint8_t> bytes; if (bytes.resize(p_size) != OK) { return RID(); }
        memcpy(bytes.ptrw(), p_data, p_size);
        return keep(p_uniform ? rd->uniform_buffer_create(p_size, bytes) : rd->storage_buffer_create(p_size, bytes));
    }
};
} // namespace TestHlodAtlasAbove2GiB

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU][BV-17] the relative depth pass draws a record above 2^31 bytes of a streamed atlas") {
    using namespace TestHlodAtlasAbove2GiB;
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("BV-17 above-2GiB case requires the GpuSorting RenderingDevice"); return; }
    Resources resources{ rd, {} };

    DepthComputeShaderRD source;
    Vector<String> variants; variants.push_back(String()); source.initialize(variants);
    RID version = source.version_create();
    if (!version.is_valid()) { FAIL("BV-17 depth version"); return; }
    const auto native = source.version_get_native_source_code(version); source.version_free(version);
    if (native.versions.is_empty()) { FAIL("BV-17 depth source"); return; }
    String code;
    for (const auto &stage : native.versions[0].stages) { if (stage.name == "compute") { code = stage.code; } }
    const int version_start = code.find("#version");
    if (code.is_empty() || version_start < 0) { FAIL("BV-17 requires the production compute stage and version"); return; }
    const int line = code.find("\n", version_start);
    if (line < 0) { FAIL("BV-17 depth preamble"); return; }
    const String defines = "#define GS_HLOD_RELATIVE_PAYLOAD 1\n#define GS_SORT_PAD_DEPTH_VALUE " + String::num_scientific(GaussianSplatting::kSortPadDepth) + "\n";
    code = code.substr(0, line + 1) + defines + code.substr(line + 1);
    const bool has_region_binding = code.find("RegionOverrideBuffer") >= 0;
    RID shader;
    const auto compiled = GaussianSplatting::ComputeInfrastructure::compile_compute_shader_from_source(rd, "BV17.Depth", code, "BV17Depth", shader);
    if (!compiled.ok() || !shader.is_valid()) { FAIL("BV-17 production depth compilation"); return; }
    resources.keep(shader);
    const RID pipeline = resources.keep(rd->compute_pipeline_create(shader));
    if (!pipeline.is_valid()) { FAIL("BV-17 depth pipeline"); return; }

    // An atlas one page past 2^31 bytes of 176 B records, the size the reef world failed at, and one splat in its last page.
    const uint64_t page_bytes = uint64_t(sizeof(PackedGaussian)) * GaussianSplatHlodRuntimeData::DEFAULT_PAGE_SPLATS;
    const uint32_t pages = uint32_t((uint64_t(1) << 31) / page_bytes + 1u);
    const uint64_t atlas_bytes = uint64_t(pages) * page_bytes;
    const uint32_t atlas_records = pages * GaussianSplatHlodRuntimeData::DEFAULT_PAGE_SPLATS;
    const uint32_t page = pages - 1u;
    const uint64_t record_offset = uint64_t(page) * page_bytes;
    if (atlas_bytes <= (uint64_t(1) << 31) || record_offset + page_bytes <= (uint64_t(1) << 31) ||
            atlas_bytes > DeviceVramLedger::kMaxAtlasBufferBytes) {
        FAIL("BV-17 fixture is not an atlas above 2^31 bytes within one buffer: ", atlas_bytes, " B");
        return;
    }
    const RID atlas = resources.keep(rd->storage_buffer_create(uint32_t(atlas_bytes)));
    if (!atlas.is_valid()) { FAIL("BV-17 could not create a ", atlas_bytes, " B atlas buffer"); return; }
    Gaussian g = {}; g.position = Vector3(0, 0, -5); g.scale = Vector3(.05, .05, .05); g.rotation = Quaternion(); g.opacity = 1;
    g.semantic = gaussian_pack_semantic(1, 0);
    PackedGaussian raw; SHCompressionMetrics metrics;
    pack_gaussian(g, raw, metrics, nullptr, 0, 0, 0);
    if (rd->buffer_update(atlas, uint32_t(record_offset), sizeof(raw), &raw) != OK) { FAIL("BV-17 record upload above 2^31"); return; }

    InstanceDataGPU row = {}; row.rotation[3] = row.inv_rotation[3] = row.translation_scale[3] = row.params[0] = 1;
    row.placement_pad[0] = 2;
    ChunkMetaGPU chunk = {}; chunk.splat_count = 1; chunk.page_table_ref = 1; // The node's pages start at table entry 0.
    DrawnNodeGPU drawn = { 0, 0, 1, 0, { 0, 0, 0, 0 } };
    uint32_t zeros[2] = {}, none = UINT32_MAX, table_entry = page;
    const RID row_buffer = resources.buffer(sizeof(row), &row), chunk_buffer = resources.buffer(sizeof(chunk), &chunk);
    const RID visible_buffer = resources.buffer(sizeof(drawn), &drawn);
    const RID ref_buffer = resources.buffer(sizeof(zeros), zeros), key_buffer = resources.buffer(sizeof(zeros), zeros);
    const RID value_buffer = resources.buffer(sizeof(none), &none), counter_buffer = resources.buffer(sizeof(zeros), zeros);
    const RID sidecar = resources.buffer(sizeof(none), &none), page_table = resources.buffer(sizeof(table_entry), &table_entry);
    RegionOverrideGPU entries[2] = { GSRegionOverrideBuffer::neutral(), GSRegionOverrideBuffer::neutral() };
    const RID region_table = resources.buffer(sizeof(entries), entries);

    InstanceDepthParamsGPU params = {};
    for (int axis = 0; axis < 4; ++axis) { params.view_matrix[axis * 5] = 1; }
    params.visible_chunk_count = params.max_visible_splats = 1;
    params.cull_frustum_radius[0] = 1;
    // The host record count: the production value is the bundle's atlas_gaussian_count (records the buffer holds).
    params.atlas_info[0] = atlas_records;
    const RID params_buffer = resources.buffer(sizeof(params), &params, true);

    Vector<RD::Uniform> uniforms;
    auto bind = [&](int p_binding, RID p_buffer, bool p_uniform = false) {
        if (!p_buffer.is_valid()) { return false; }
        RD::Uniform value; value.uniform_type = p_uniform ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
        value.binding = p_binding; value.append_id(p_buffer); uniforms.push_back(value); return true;
    };
    const RID buffers[] = { atlas, RID(), row_buffer, chunk_buffer, visible_buffer, ref_buffer, key_buffer, value_buffer, counter_buffer };
    for (int binding = 0; binding < 9; ++binding) {
        if (binding != 1 && !bind(binding, buffers[binding])) { FAIL("BV-17 input buffer ", binding); return; }
    }
    if (!bind(9, params_buffer, true) || !bind(10, sidecar) || !bind(11, page_table) || (has_region_binding && !bind(12, region_table))) {
        FAIL("BV-17 table buffers"); return;
    }
    const RID set = resources.keep(rd->uniform_set_create(uniforms, shader, 0));
    if (!set.is_valid()) { FAIL("BV-17 depth uniforms"); return; }
    const auto list = rd->compute_list_begin();
    if (list == RD::INVALID_ID) { FAIL("BV-17 compute list unavailable"); return; }
    rd->compute_list_bind_compute_pipeline(list, pipeline); rd->compute_list_bind_uniform_set(list, set, 0);
    rd->compute_list_dispatch(list, 1, 1, 1); rd->compute_list_end();
    const Vector<uint8_t> counts = rd->buffer_get_data(counter_buffer);
    const Vector<uint8_t> refs = rd->buffer_get_data(ref_buffer);
    if (counts.size() != sizeof(zeros) || refs.size() < int(sizeof(uint32_t) * 2)) { FAIL("BV-17 readback"); return; }
    uint32_t counter[2]; memcpy(counter, counts.ptr(), sizeof(counter));
    // The splat above 2^31 bytes is accepted, not rejected as out of the atlas (overflowed_splats).
    CHECK(counter[0] == 1u);
    CHECK(counter[1] == 0u);
    // And the splat ref names that record.
    SplatRefGPU ref; memcpy(&ref, refs.ptr(), MIN(sizeof(ref), size_t(refs.size())));
    CHECK(gs_splat_ref::gs_splat_ref_atlas_index(ref.atlas_index) == page * GaussianSplatHlodRuntimeData::DEFAULT_PAGE_SPLATS);
}
