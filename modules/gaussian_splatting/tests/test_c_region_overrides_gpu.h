#pragma once
#include "test_macros.h"
#include "../compute/depth_compute.glsl.gen.h"
#include "../renderer/gs_region_override_buffer.h"
#include "../renderer/gs_shadow_caster.h"
#include "../renderer/hlod_shadow_caster_plan.h"
#include "../renderer/shader_compilation_helper.h"
#include "../shaders/gs_shadow_caster.glsl.gen.h"
#include "../renderer/gpu_sorting_constants.h"
#include "../renderer/sorting_contract.h"
#include "../compute/compute_infrastructure.h"
#include "../core/gaussian_semantic.h"
#include "../core/streaming_quantization.h"
#include <cstring>

namespace TestCRegionGPU {
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
}

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU][C] R3 hidden regions allocate zero sort records with raster-only control and no caster pixels") {
    using namespace TestCRegionGPU;
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("C R3 requires GpuSorting RenderingDevice"); return; }
    Resources resources{rd,{}};
    for (bool quantized : {false,true}) {
        for (bool relative : {false,true}) {
            DepthComputeShaderRD source;
            Vector<String> variants; variants.push_back(String()); source.initialize(variants);
            RID version = source.version_create();
            if (!version.is_valid()) { FAIL("C R3 depth version"); return; }
            const auto native = source.version_get_native_source_code(version); source.version_free(version);
            if (native.versions.is_empty()) { FAIL("C R3 depth source"); return; }
            String code;
            for (const auto &stage : native.versions[0].stages) { if (stage.name == "compute") { code = stage.code; } }
            const int version_start = code.find("#version");
            if (code.is_empty() || version_start < 0) { FAIL("C R3 requires the production compute stage and version"); return; }
            const int line = code.find("\n", version_start);
            if (line < 0) { FAIL("C R3 depth preamble"); return; }
            String defines = "#define GS_TESTS_ENABLED 1\n#define GS_TEST_REGION_CONTROLS 1\n#define GS_SORT_PAD_DEPTH_VALUE " + String::num_scientific(GaussianSplatting::kSortPadDepth) + "\n";
            if (quantized) { defines += "#define USE_QUANTIZED_GAUSSIANS 1\n"; }
            if (relative) { defines += "#define GS_HLOD_RELATIVE_PAYLOAD 1\n"; }
            code = code.substr(0,line+1) + defines + code.substr(line+1);
            const bool has_region_binding = code.find("RegionOverrideBuffer") >= 0;
            RID shader;
            const auto compiled = GaussianSplatting::ComputeInfrastructure::compile_compute_shader_from_source(rd,"C.R3.Depth",code,"CRegionDepth",shader);
            if (!compiled.ok() || !shader.is_valid()) { FAIL("C R3 production depth compilation"); return; }
            resources.keep(shader);
            const RID pipeline = resources.keep(rd->compute_pipeline_create(shader));
            if (!pipeline.is_valid()) { FAIL("C R3 depth pipeline"); return; }
            Gaussian g = {}; g.position = Vector3(0,0,-5); g.scale = Vector3(.05,.05,.05); g.rotation = Quaternion(); g.opacity = 1;
            g.semantic = gaussian_pack_semantic(1,0);
            PackedGaussian raw; PackedGaussianQuantized packed;
            SHCompressionMetrics metrics;
            ChunkQuantizationInfo quant;
            quant.position_min = quant.position_max = g.position; quant.position_range = Vector3();
            quant.scale_min = quant.scale_max = g.scale; quant.scale_range = Vector3();
            quant.position_bits = quant.scale_bits = 16; quant.scales_quantized = true;
            pack_gaussian(g,raw,metrics,nullptr,0,0,0);
            pack_gaussian_quantized(g,quant,0,packed,metrics,nullptr,0,0,0);
            ChunkQuantizationGPU quant_gpu = {}; quant_gpu.position_min[2] = -5; quant_gpu.position_bits = quant_gpu.scale_bits = 16;
            for (float &scale : quant_gpu.scale_min) { scale = .05f; } quant_gpu.count = 1;
            InstanceDataGPU row = {}; row.rotation[3] = row.inv_rotation[3] = row.translation_scale[3] = row.params[0] = 1;
            row.lod[1] = 0; row.placement_pad[0] = 2;
            ChunkMetaGPU chunk = {}; chunk.splat_count = 1;
            DrawnNodeGPU drawn = {0,0,1,0,{0,0,0,0}};
            VisibleChunkRefGPU visible = {0,0};
            uint32_t zeros[2] = {}, page = UINT32_MAX;
            InstanceDepthParamsGPU params = {};
            for (int axis=0;axis<4;++axis) { params.view_matrix[axis*5] = 1; }
            params.visible_chunk_count = params.max_visible_splats = 1;
            params.cull_frustum_radius[0] = 1;
            RegionOverrideGPU entries[2] = {GSRegionOverrideBuffer::neutral(),GSRegionOverrideBuffer::neutral()};
            const RID atlas = resources.buffer(quantized ? sizeof(packed) : sizeof(raw),quantized ? static_cast<void *>(&packed) : static_cast<void *>(&raw));
            const RID quant_buffer = resources.buffer(sizeof(quant_gpu),&quant_gpu);
            const RID row_buffer = resources.buffer(sizeof(row),&row), chunk_buffer = resources.buffer(sizeof(chunk),&chunk);
            const RID visible_buffer = resources.buffer(relative ? sizeof(drawn) : sizeof(visible), relative ? static_cast<void *>(&drawn) : static_cast<void *>(&visible));
            const RID ref_buffer = resources.buffer(sizeof(zeros),zeros), key_buffer = resources.buffer(sizeof(zeros),zeros);
            const RID value_buffer = resources.buffer(sizeof(page),&page), counter_buffer = resources.buffer(sizeof(zeros),zeros);
            const RID params_buffer = resources.buffer(sizeof(params),&params,true), table = resources.buffer(sizeof(entries),entries);
            const RID sidecar = resources.buffer(sizeof(page),&page), pages = resources.buffer(sizeof(page),&page);
            Vector<RD::Uniform> uniforms;
            auto bind = [&](int binding, RID buffer, bool uniform=false) {
                if (!buffer.is_valid()) { return false; }
                RD::Uniform value; value.uniform_type = uniform ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
                value.binding = binding; value.append_id(buffer); uniforms.push_back(value); return true;
            };
            const RID buffers[] = {atlas,quant_buffer,row_buffer,chunk_buffer,visible_buffer,ref_buffer,key_buffer,value_buffer,counter_buffer};
            for (int binding=0;binding<9;++binding) { if (binding != 1 || quantized) { if (!bind(binding,buffers[binding])) { FAIL("C R3 input buffer"); return; } } }
            if (!bind(9,params_buffer,true) || (relative && (!bind(10,sidecar) || !bind(11,pages))) || (has_region_binding && !bind(12,table))) { FAIL("C R3 table buffers"); return; }
            const RID set = resources.keep(rd->uniform_set_create(uniforms,shader,0));
            if (!set.is_valid()) { FAIL("C R3 depth uniforms"); return; }
            for (int sample=0;sample<4;++sample) {
                entries[1] = GSRegionOverrideBuffer::neutral();
                entries[1].flags = sample == 1 || sample == 2 ? GS_REGION_HIDE : 0;
                if (sample == 3) { entries[1].emissive_opacity[1] = 0; }
                row.ids[1] = sample == 2 ? GS_INSTANCE_TEST_HIDE_ONLY_IN_RASTER : 0;
                if (rd->buffer_update(table,0,sizeof(entries),entries) != OK || rd->buffer_update(row_buffer,0,sizeof(row),&row) != OK || rd->buffer_update(counter_buffer,0,sizeof(zeros),zeros) != OK) { FAIL("C R3 upload"); return; }
                const auto list = rd->compute_list_begin();
                if (list == RD::INVALID_ID) { FAIL("C R3 compute list unavailable"); return; }
                rd->compute_list_bind_compute_pipeline(list,pipeline); rd->compute_list_bind_uniform_set(list,set,0);
                rd->compute_list_dispatch(list,1,1,1); rd->compute_list_end();
                const auto bytes = rd->buffer_get_data(counter_buffer);
                if (bytes.size() != sizeof(zeros)) { FAIL("C R3 count readback"); return; }
                uint32_t count; memcpy(&count,bytes.ptr(),sizeof(count));
                CHECK(count == (sample == 0 || sample == 2 ? 1u : 0u));
            }
            // The dedicated HLOD caster remains raw-only (ADR 6.6.1).
            // Quantized input still runs every sort-counter assertion above.
            gs_shadow_caster::FrameRecord caster_record;
            caster_record.valid = true; caster_record.quantized = quantized;
            caster_record.draw_count = caster_record.max_chunk_splats = 1;
            caster_record.world_splats = 1;
            const auto caster_plan = gs_shadow_caster::plan_pass(caster_record,true,true);
            if (quantized) {
                CHECK(caster_plan.refusal == gs_shadow_caster::Refusal::QUANTIZED_PAYLOAD);
                CHECK(String(gs_shadow_caster::refusal_reason(caster_plan.refusal)) == "shadow_caster_quantized_payload_unsupported");
                CHECK(!caster_plan.draws());
                continue;
            }
            CHECK(caster_plan.draws());
            // Actual raw caster target: positive baseline, hide flags and zero opacity.
            raw.position[2] = .5f;
            if (rd->buffer_update(atlas,0,sizeof(raw),&raw) != OK) { FAIL("C R3 caster payload"); return; }
            RD::TextureFormat format; format.format = RD::DATA_FORMAT_D32_SFLOAT; format.width = format.height = 64;
            format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
            if (!rd->texture_is_format_supported_for_usage(format.format,format.usage_bits)) { FAIL("C R3 requires D32 caster attachment and readback support"); return; }
            const RID target = resources.keep(rd->texture_create(format,RD::TextureView()));
            if (!target.is_valid()) { FAIL("C R3 caster target"); return; }
            Vector<RID> attachments; attachments.push_back(target);
            const RID framebuffer = resources.keep(rd->framebuffer_create(attachments));
            if (!framebuffer.is_valid()) { FAIL("C R3 caster framebuffer"); return; }
            const RID draw_buffer = resources.buffer(sizeof(drawn),&drawn);
            if (!draw_buffer.is_valid()) { FAIL("C R3 caster draw buffer"); return; }
            // Like B3's caster readback, compile and draw the production shader
            // directly. The RD-only harness has no compositor UniformSetCacheRD,
            // which GsSplatShadowCaster::draw() requires in the live renderer.
            GsShadowCasterShaderRD caster_source;
            Vector<String> caster_variants; caster_variants.push_back(String()); caster_source.initialize(caster_variants);
            const RID caster_version = caster_source.version_create();
            if (!caster_version.is_valid()) { FAIL("C R3 raw caster shader version"); return; }
            const Vector<String> caster_stages = caster_source.version_build_variant_stage_sources(caster_version,0);
            caster_source.version_free(caster_version);
            if (caster_stages.size() <= RD::SHADER_STAGE_FRAGMENT) { FAIL("C R3 raw caster shader stages"); return; }
            String error, processed_vertex;
            const RID caster_shader = resources.keep(ShaderCompilationHelper::compile_graphics_shader_on_device(rd,
                    caster_stages[RD::SHADER_STAGE_VERTEX],caster_stages[RD::SHADER_STAGE_FRAGMENT],
                    "c_region_caster_pixels",Vector<String>(),&error,&processed_vertex));
            if (!caster_shader.is_valid()) { FAIL("C R3 raw caster shader compilation: ",error.utf8().get_data()); return; }
            RD::PipelineRasterizationState raster; raster.cull_mode = RD::POLYGON_CULL_DISABLED;
            RD::PipelineDepthStencilState depth; depth.enable_depth_test = depth.enable_depth_write = true;
            depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
            const RID caster_pipeline = resources.keep(rd->render_pipeline_create(caster_shader,rd->framebuffer_get_format(framebuffer),
                    RD::INVALID_ID,RD::RENDER_PRIMITIVE_TRIANGLES,raster,RD::PipelineMultisampleState(),depth,
                    RD::PipelineColorBlendState::create_disabled(0)));
            if (!caster_pipeline.is_valid()) { FAIL("C R3 raw caster pipeline"); return; }
            const uint32_t casts = 1;
            const GsSplatShadowCaster::DeformationUniform deformation{};
            const RID cast_buffer = resources.buffer(sizeof(casts),&casts), deformation_buffer = resources.buffer(sizeof(deformation),&deformation);
            const RID caster_buffers[] = {atlas,row_buffer,chunk_buffer,draw_buffer,cast_buffer,pages,deformation_buffer,table};
            Vector<RD::Uniform> caster_uniforms;
            const bool caster_has_region_binding = processed_vertex.find("RegionOverrideBuffer") >= 0;
            for (int binding=0;binding<(caster_has_region_binding ? 8 : 7);++binding) {
                if (!caster_buffers[binding].is_valid()) { FAIL("C R3 raw caster input buffer"); return; }
                RD::Uniform uniform; uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
                uniform.binding = binding; uniform.append_id(caster_buffers[binding]); caster_uniforms.push_back(uniform);
            }
            const RID caster_set = resources.keep(rd->uniform_set_create(caster_uniforms,caster_shader,0));
            if (!caster_set.is_valid()) { FAIL("C R3 raw caster uniform set"); return; }
            struct CasterPush { float matrix[16]; float viewport[4]; uint32_t counts[4]; } push{};
            static_assert(sizeof(CasterPush) == 96, "C caster push layout");
            static_assert(sizeof(CasterPush) % 16 == 0, "C caster push alignment");
            for (int axis=0;axis<4;++axis) { push.matrix[axis*5] = 1; }
            push.viewport[0] = push.viewport[1] = 64;
            push.viewport[2] = gs_shadow_caster::kAlphaThreshold; push.viewport[3] = gs_shadow_caster::kLowPassTexels2;
            push.counts[0] = push.counts[1] = push.counts[2] = 1;
            const float opacity = 1; memcpy(&push.counts[3],&opacity,sizeof(opacity));
            for (int sample=0;sample<4;++sample) {
                const uint32_t flags = sample == 1 ? GS_REGION_HIDE : sample == 2 ? GS_REGION_NO_SHADOW : 0u;
                entries[1] = GSRegionOverrideBuffer::neutral(); entries[1].flags = flags;
                if (sample == 3) { entries[1].emissive_opacity[1] = 0; }
                if (rd->buffer_update(table,0,sizeof(entries),entries) != OK) { FAIL("C R3 caster override"); return; }
                const auto clear = rd->draw_list_begin(framebuffer,RD::DRAW_CLEAR_DEPTH,VectorView<Color>(),0.0f);
                if (clear == RD::INVALID_ID) { FAIL("C R3 caster clear"); return; }
                rd->draw_list_end();
                const auto list = rd->draw_list_begin(framebuffer,RD::DRAW_DEFAULT_ALL,VectorView<Color>(),0.0f);
                if (list == RD::INVALID_ID) { FAIL("C R3 raw caster draw list"); return; }
                rd->draw_list_bind_render_pipeline(list,caster_pipeline); rd->draw_list_bind_uniform_set(list,caster_set,0);
                rd->draw_list_set_push_constant(list,&push,sizeof(push)); rd->draw_list_draw(list,false,1,6); rd->draw_list_end();
                const auto pixels = rd->texture_get_data(target,0);
                if (pixels.size() != 64*64*int(sizeof(float))) { FAIL("C R3 caster readback"); return; }
                int covered=0; for (int i=0;i<64*64;++i) { float z; memcpy(&z,pixels.ptr()+i*sizeof(float),sizeof(float)); covered += z>0; }
                if (sample == 0) { CHECK(covered > 0); } else { CHECK(covered == 0); }
            }
        }
    }
}
