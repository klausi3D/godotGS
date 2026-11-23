#pragma once

#include "test_macros.h"
#include "../core/gaussian_semantic.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gs_region_override_buffer.h"
#include "../renderer/gs_shadow_caster.h"
#include "../renderer/hlod_shadow_caster_plan.h"
#include "../renderer/shader_compilation_helper.h"
#include "../shaders/gs_shadow_caster.glsl.gen.h"

#include <cmath>
#include <cstring>

TEST_CASE("[GaussianSplatting][HLOD][Sort][RequiresGPU][B3] baked wind weights and O4 read back caster displacement with W4 forcing control") {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd) { FAIL("GpuSorting RenderingDevice required for B3 displacement readback"); return; }
    struct Resources {
        RenderingDevice *rd;
        LocalVector<RID> owned;
        ~Resources() { for (uint32_t i = owned.size(); i-- > 0;) { rd->free(owned[i]); } }
        RID keep(RID p_rid) { if (p_rid.is_valid()) { owned.push_back(p_rid); } return p_rid; }
        RID buffer(uint32_t p_size, const void *p_data) {
            Vector<uint8_t> bytes;
            if (bytes.resize(p_size) != OK) { return RID(); }
            memcpy(bytes.ptrw(), p_data, p_size);
            return keep(rd->storage_buffer_create(p_size, bytes));
        }
    } resources{ rd, {} };
    constexpr int size = 128;
    RD::TextureFormat format;
    format.format = RD::DATA_FORMAT_D32_SFLOAT;
    format.width = format.height = size;
    format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
    if (!rd->texture_is_format_supported_for_usage(format.format, format.usage_bits)) {
        FAIL("B3 requires a D32 target with readback"); return;
    }
    const RID target = resources.keep(rd->texture_create(format, RD::TextureView()));
    if (!target.is_valid()) { FAIL("B3 target unavailable"); return; }
    Vector<RID> attachments;
    attachments.push_back(target);
    const RID framebuffer = resources.keep(rd->framebuffer_create(attachments));
    if (!framebuffer.is_valid()) { FAIL("B3 framebuffer unavailable"); return; }
    RID shaders[2], pipelines[2];
    for (int forced = 0; forced < 2; ++forced) {
        GsShadowCasterShaderRD source;
        Vector<String> variants;
        variants.push_back(String());
        // This template has no #VERSION_DEFINES marker, so ShaderRD variant
        // defines are not injected. Pass W4's test-only control to the final
        // graphics compiler, which inserts it after #version in each stage.
        Vector<String> defines;
        if (forced) { defines.push_back("#define GS_TESTS_ENABLED 1\n#define GS_TEST_LEGACY_WIND_WEIGHT 1\n"); }
        source.initialize(variants);
        const RID version = source.version_create();
        if (!version.is_valid()) { FAIL("B3 production caster version unavailable"); return; }
        const Vector<String> stages = source.version_build_variant_stage_sources(version, 0);
        source.version_free(version);
        if (stages.size() <= RD::SHADER_STAGE_FRAGMENT) { FAIL("B3 production stages unavailable"); return; }
        String error;
        String processed_vertex;
        shaders[forced] = resources.keep(ShaderCompilationHelper::compile_graphics_shader_on_device(rd,
                stages[RD::SHADER_STAGE_VERTEX], stages[RD::SHADER_STAGE_FRAGMENT], "b3_wind_pixels", defines, &error, &processed_vertex));
        if (!shaders[forced].is_valid()) { FAIL("B3 shader compilation failed: ", error.utf8().get_data()); return; }
        const int control_pos = processed_vertex.find("#define GS_TEST_LEGACY_WIND_WEIGHT 1");
        if (forced) {
            CHECK(processed_vertex.find("#define GS_TESTS_ENABLED 1") >= 0);
            CHECK(control_pos >= 0);
            CHECK(control_pos < processed_vertex.find("float gs_splat_wind_weight("));
        } else {
            CHECK(control_pos == -1);
        }
        RD::PipelineRasterizationState raster;
        raster.cull_mode = RD::POLYGON_CULL_DISABLED;
        RD::PipelineDepthStencilState depth;
        depth.enable_depth_test = depth.enable_depth_write = true;
        depth.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
        pipelines[forced] = resources.keep(rd->render_pipeline_create(shaders[forced], rd->framebuffer_get_format(framebuffer), RD::INVALID_ID,
                RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), depth, RD::PipelineColorBlendState::create_disabled(0)));
        if (!pipelines[forced].is_valid()) { FAIL("B3 pipeline unavailable"); return; }
    }
    // Rest, rigid, full, half, forced rigid, AUTO legacy, ON legacy, OFF semantic,
    // instance legacy. Equal rest position/time makes the amplitude ratios observable.
    const uint32_t words[] = { 0u, gaussian_pack_semantic(1, 0), gaussian_pack_semantic(1, 255),
        gaussian_pack_semantic(1, 128), gaussian_pack_semantic(1, 0), 0u, 0u, gaussian_pack_semantic(1, 255), 0u };
    double centers[9] = {};
    Vector<uint8_t> pixels[9];
    for (int sample = 0; sample < 9; ++sample) {
        const int forced = sample == 4 ? 1 : 0;
        PackedGaussian g = {};
        g.position[2] = 0.5f;
        g.opacity = 1.0f;
        g.rotation[3] = 1.0f;
        g.scale[0] = g.scale[1] = g.scale[2] = 0.05f;
        g.semantic = words[sample];
        InstanceDataGPU rows[2] = {};
        for (auto &row : rows) {
            row.rotation[3] = row.inv_rotation[3] = row.translation_scale[3] = 1.0f;
            row.params[0] = row.params[2] = 1.0f;
            row.params[3] = float(GS_INSTANCE_WIND_MODE_SEMANTIC_INHERIT);
            row.ids[1] = GS_INSTANCE_FLAG_ROTATION_IDENTITY | GS_INSTANCE_FLAG_SCALE_IDENTITY | GS_INSTANCE_FLAG_TRANSLATION_ZERO;
        }
        if (sample == 6 || sample == 8) { rows[sample == 8 ? 1 : 0].params[3] = float(GS_INSTANCE_WIND_MODE_INHERIT); }
        if (sample == 7) { rows[0].params[3] = float(GS_INSTANCE_WIND_MODE_FORCE_DISABLED); }
        ChunkMetaGPU chunk = {};
        chunk.splat_count = 1;
        DrawnNodeGPU drawn = { sample == 8 ? 1u : 0u, 0u, 1.0f, 0u, { 0, 0, 0, 0 } };
        const uint32_t casts[2] = { 1u, 1u }, page = 0u;
        GsSplatShadowCaster::DeformationUniform deformation;
        deformation.wind_dir_strength[0] = 1.0f;
        deformation.wind_dir_strength[3] = sample == 0 ? 0.0f : 0.5f;
        // Rest at (0,0,0.5); lattice interpolation gives a tiny phase offset,
        // so sin(pi/2 + noise) is close to 1. The host oracle pins exact amplitude.
        deformation.wind_time_config[0] = 1.57079632679f;
        deformation.wind_time_config[1] = 1.0f;
        deformation.wind_time_config[3] = 1.0f;
        const RegionOverrideGPU neutral_region = GSRegionOverrideBuffer::neutral();
        const RID inputs[] = { resources.buffer(sizeof(g), &g), resources.buffer(sizeof(rows), rows),
            resources.buffer(sizeof(chunk), &chunk), resources.buffer(sizeof(drawn), &drawn),
            resources.buffer(sizeof(casts), casts), resources.buffer(sizeof(page), &page),
            resources.buffer(sizeof(deformation), &deformation),
            resources.buffer(sizeof(RegionOverrideGPU), &neutral_region) };
        Vector<RD::Uniform> uniforms;
        for (int binding = 0; binding < 8; ++binding) {
            if (!inputs[binding].is_valid()) { FAIL("B3 input buffer unavailable"); return; }
            RD::Uniform u;
            u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
            u.binding = binding;
            u.append_id(inputs[binding]);
            uniforms.push_back(u);
        }
        const RID set = resources.keep(rd->uniform_set_create(uniforms, shaders[forced], 0));
        if (!set.is_valid()) { FAIL("B3 uniform set unavailable"); return; }
        // atlas[0] = 0: the shader bounds by the array length (BV-17 fallback; this atlas is far below 2^31 bytes).
        struct Push { float matrix[16]; float viewport[4]; uint32_t counts[4]; uint32_t atlas[4]; } push{};
        static_assert(sizeof(Push) == 112, "B3 caster push layout");
        for (int axis = 0; axis < 4; ++axis) { push.matrix[axis * 5] = 1.0f; }
        push.viewport[0] = push.viewport[1] = float(size);
        push.viewport[2] = gs_shadow_caster::kAlphaThreshold;
        push.viewport[3] = gs_shadow_caster::kLowPassTexels2;
        push.counts[0] = push.counts[1] = 1;
        const float opacity = 1.0f;
        memcpy(&push.counts[3], &opacity, sizeof(opacity));
        const RD::DrawListID list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_DEPTH, VectorView<Color>(), 0.0f);
        if (list == RD::INVALID_ID) { FAIL("B3 draw list unavailable"); return; }
        rd->draw_list_bind_render_pipeline(list, pipelines[forced]);
        rd->draw_list_bind_uniform_set(list, set, 0);
        rd->draw_list_set_push_constant(list, &push, sizeof(push));
        rd->draw_list_draw(list, false, 1, 6);
        rd->draw_list_end();
        pixels[sample] = rd->texture_get_data(target, 0);
        if (pixels[sample].size() != size * size * int(sizeof(float))) { FAIL("B3 complete depth readback required"); return; }
        int covered = 0;
        double sum = 0;
        for (int i = 0; i < size * size; ++i) {
            float z;
            memcpy(&z, pixels[sample].ptr() + i * sizeof(float), sizeof(z));
            if (z > 0.0f) { sum += double(i % size) + 0.5; ++covered; }
        }
        if (covered == 0) { FAIL("B3 no measured caster texels for sample ", sample); return; }
        centers[sample] = sum / double(covered);
    }
    CHECK(centers[1] == centers[0]); // W4: exactly 0 px.
    CHECK(pixels[1] == pixels[0]);
    CHECK(centers[4] - centers[0] > 1.0); // Observable legacy forcing control.
    const double full = centers[2] - centers[0], half = centers[3] - centers[0];
    CHECK(std::abs(full - 32.0) <= 1.0);
    CHECK(std::abs(half - full * 128.0 / 255.0) <= 1.0);
    CHECK(pixels[5] == pixels[0]); // AUTO legacy is still under active project wind.
    CHECK(std::abs((centers[6] - centers[0]) - 6.4) <= 1.0); // ON legacy: opacity 1 -> 0.2.
    CHECK(pixels[7] == pixels[0]); // OFF semantic is still.
    CHECK(pixels[8] == pixels[6]); // Instance legacy retains the same heuristic.
}
