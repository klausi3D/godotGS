#include "quantization_config.h"
#include "gaussian_gpu_layout.h"
#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "../core/gs_project_settings.h"
#include "../core/quality_tier_config.h"
#include "../logger/gs_logger.h"

// Project settings paths
const String QuantizationConfig::SECTION_PATH = "rendering/gaussian_splatting/compression/";
const String QuantizationConfig::PER_CHUNK_QUANTIZATION_PATH = SECTION_PATH + "per_chunk_quantization";
const String QuantizationConfig::POSITION_BITS_PATH = SECTION_PATH + "position_bits";
const String QuantizationConfig::SCALE_BITS_PATH = SECTION_PATH + "scale_bits";
const String QuantizationConfig::QUANTIZE_SCALES_PATH = SECTION_PATH + "quantize_scales";

// Global instance
QuantizationConfig g_quantization_config;

void QuantizationConfig::load_from_project_settings() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    // Sentinel-based tier seeding for per_chunk_quantization.
    // -1 means "not explicitly set by user" -- check active tier.
    int raw_quantization = gs::settings::get_int(ps, PER_CHUNK_QUANTIZATION_PATH, -1);
    if (raw_quantization < 0) {
        // Sentinel: user never set this.
        per_chunk_quantization = false; // Code default.
        const String tier_preset = ps->get_setting("rendering/gaussian_splatting/quality/tier_preset", "custom");
        QualityTierConfig tier_config;
        if (get_quality_tier_config(tier_preset, tier_config) && tier_config.quantization_enabled >= 0) {
            per_chunk_quantization = (tier_config.quantization_enabled != 0);
        }
    } else {
        per_chunk_quantization = (raw_quantization != 0);
    }

    position_bits = ps->get_setting(POSITION_BITS_PATH, 16);
    scale_bits = ps->get_setting(SCALE_BITS_PATH, 12);
    quantize_scales = ps->get_setting(QUANTIZE_SCALES_PATH, false);

    // Clamp values to valid ranges. position_bits is capped at 16, NOT 24: the
    // 80-byte PackedGaussianQuantized stores quantized_position as uint16[3], so a
    // value >16 would quantize to >65535 on the CPU (silently truncated by the
    // uint16 store) while the GLSL dequantize uses the full 1/((1<<bits)-1) scale
    // -> corrupted positions. Capping here keeps the single bits value used by both
    // the packer and the uploaded ChunkQuantization within what the format stores.
    position_bits = CLAMP(position_bits, 8u, 16u);
    scale_bits = CLAMP(scale_bits, 8u, 16u);

    if (per_chunk_quantization) {
        print_config_summary();
    }
}

void QuantizationConfig::save_to_project_settings() const {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    ps->set_setting(PER_CHUNK_QUANTIZATION_PATH, per_chunk_quantization ? 1 : 0); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    ps->set_setting(POSITION_BITS_PATH, (int)position_bits); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    ps->set_setting(SCALE_BITS_PATH, (int)scale_bits); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    ps->set_setting(QUANTIZE_SCALES_PATH, quantize_scales); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION

    ps->save();

    GS_LOG_STREAMING_INFO("[Quantization Config] Configuration saved to project settings");
}

void QuantizationConfig::reset_to_defaults() {
    per_chunk_quantization = false;
    position_bits = 16;
    scale_bits = 12;
    quantize_scales = false;

    GS_LOG_STREAMING_INFO("[Quantization Config] Reset to default configuration");
}

bool QuantizationConfig::validate() const {
    // Position bits must be in valid range (16 max: uint16 storage in the 80-byte layout).
    if (position_bits < 8 || position_bits > 16) {
        return false;
    }

    // Scale bits must be in valid range
    if (scale_bits < 8 || scale_bits > 16) {
        return false;
    }

    return true;
}

String QuantizationConfig::get_validation_errors() const {
    String errors;

    if (position_bits < 8) {
        errors += "Position bits must be >= 8\n";
    }
    if (position_bits > 16) {
        errors += "Position bits must be <= 16 (uint16 storage in the 80-byte quantized layout)\n";
    }
    if (scale_bits < 8) {
        errors += "Scale bits must be >= 8\n";
    }
    if (scale_bits > 16) {
        errors += "Scale bits must be <= 16\n";
    }

    return errors;
}

float QuantizationConfig::get_position_compression_ratio() const {
    if (!per_chunk_quantization) {
        return 1.0f;
    }

    // Original: 3 floats (12 bytes) per position
    // Quantized: 3 * position_bits / 8 bytes. The per-chunk position bounds
    // (6 floats = 24 bytes) are amortized over the fixed 65536-splat chunk
    // (GaussianStreamingSystem::CHUNK_SIZE), so the per-splat overhead is
    // ~0.0004 bytes -- negligible and omitted from the estimate.
    float original_bytes = 12.0f;
    float quantized_bytes = (3.0f * float(position_bits)) / 8.0f;

    return original_bytes / quantized_bytes;
}

float QuantizationConfig::get_scale_compression_ratio() const {
    if (!per_chunk_quantization || !quantize_scales) {
        return 1.0f;
    }

    // Original: 3 floats (12 bytes) per scale
    // Quantized: 3 * scale_bits / 8 bytes. Per-chunk bounds overhead is
    // amortized over the fixed 65536-splat chunk and negligible (see
    // get_position_compression_ratio), so it is omitted here.
    float original_bytes = 12.0f;
    float quantized_bytes = (3.0f * float(scale_bits)) / 8.0f;

    return original_bytes / quantized_bytes;
}

float QuantizationConfig::get_total_compression_ratio() const {
    if (!per_chunk_quantization) {
        return 1.0f;
    }

    // Derived from the layout, never restated: this used to hard-code 144 and
    // would have silently reported a wrong ratio the moment the struct changed
    // size -- which it just did (144 -> 128).
    // Position: 12 bytes, Scale: 12 bytes
    const float total_bytes = float(sizeof(PackedGaussian));
    const float position_bytes = 12.0f;
    const float scale_bytes = 12.0f;

    float saved_position = position_bytes * (1.0f - 1.0f / get_position_compression_ratio());
    float saved_scale = quantize_scales ? scale_bytes * (1.0f - 1.0f / get_scale_compression_ratio()) : 0.0f;

    float new_size = total_bytes - saved_position - saved_scale;
    return total_bytes / new_size;
}

void QuantizationConfig::print_config_summary() const {
    GS_LOG_STREAMING_INFO("[Quantization Config] ========== Configuration Summary ==========");
    GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Per-Chunk Quantization: %s",
            per_chunk_quantization ? "ENABLED" : "disabled"));

    if (per_chunk_quantization) {
        GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Position Bits: %d (%d levels)",
                position_bits, get_position_levels()));
        GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Scale Quantization: %s",
                quantize_scales ? "enabled" : "disabled"));
        if (quantize_scales) {
            GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Scale Bits: %d (%d levels)",
                    scale_bits, get_scale_levels()));
        }

        float pos_ratio = get_position_compression_ratio();
        float total_ratio = get_total_compression_ratio();

        GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Position Compression: %.2fx",
                pos_ratio));
        if (quantize_scales) {
            GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Scale Compression: %.2fx",
                    get_scale_compression_ratio()));
        }
        GS_LOG_STREAMING_INFO(vformat("[Quantization Config] Total Compression: %.2fx (%.1f%% savings)",
                total_ratio, (1.0f - 1.0f / total_ratio) * 100.0f));
    }

    GS_LOG_STREAMING_INFO("[Quantization Config] ================================================");
}

void register_quantization_project_settings() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    // Per-chunk quantization enable (sentinel -1 = auto from tier, 0 = off, 1 = on).
    //
    // TRADEOFF (measured, not a free win — see docs/performance/gs_quantization_tradeoff.md):
    // the 80-byte quantized atlas is -44% VRAM/splat (144 -> 80 B) but adds per-splat
    // dequantization ALU in the binning/depth/raster shaders. On dense-2M at a FIXED splat
    // count (RTX 3090) this measured +13.6% p50 / +16.2% p99 frame time. It is a VRAM<->compute
    // trade: a win when VRAM-bound (fit an otherwise-OOM scene, or ~2x more splats in the same
    // budget), a net loss with VRAM headroom. Keep it opt-in; only enable under VRAM pressure.
    if (!ps->has_setting(QuantizationConfig::PER_CHUNK_QUANTIZATION_PATH)) {
        ps->set_setting(QuantizationConfig::PER_CHUNK_QUANTIZATION_PATH, -1); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(QuantizationConfig::PER_CHUNK_QUANTIZATION_PATH, -1);
    ps->set_custom_property_info(PropertyInfo(
        Variant::INT,
        QuantizationConfig::PER_CHUNK_QUANTIZATION_PATH,
        PROPERTY_HINT_ENUM,
        "Auto (Tier Default):-1,Disabled:0,Enabled:1"
    ));

    // Position bits
    if (!ps->has_setting(QuantizationConfig::POSITION_BITS_PATH)) {
        ps->set_setting(QuantizationConfig::POSITION_BITS_PATH, 16);
    }
    ps->set_initial_value(QuantizationConfig::POSITION_BITS_PATH, 16);
    ps->set_custom_property_info(PropertyInfo(
        Variant::INT,
        QuantizationConfig::POSITION_BITS_PATH,
        PROPERTY_HINT_RANGE,
        "8,16,1" // 16 max: uint16 storage in the 80-byte quantized layout.
    ));

    // Scale bits
    if (!ps->has_setting(QuantizationConfig::SCALE_BITS_PATH)) {
        ps->set_setting(QuantizationConfig::SCALE_BITS_PATH, 12);
    }
    ps->set_initial_value(QuantizationConfig::SCALE_BITS_PATH, 12);
    ps->set_custom_property_info(PropertyInfo(
        Variant::INT,
        QuantizationConfig::SCALE_BITS_PATH,
        PROPERTY_HINT_RANGE,
        "8,16,1"
    ));

    // Quantize scales
    if (!ps->has_setting(QuantizationConfig::QUANTIZE_SCALES_PATH)) {
        ps->set_setting(QuantizationConfig::QUANTIZE_SCALES_PATH, false);
    }
    ps->set_initial_value(QuantizationConfig::QUANTIZE_SCALES_PATH, false);
    ps->set_custom_property_info(PropertyInfo(
        Variant::BOOL,
        QuantizationConfig::QUANTIZE_SCALES_PATH,
        PROPERTY_HINT_NONE,
        ""
    ));
}

void initialize_quantization_config() {
    // Register project settings first
    register_quantization_project_settings();

    // Load configuration
    g_quantization_config.load_from_project_settings();

    if (!g_quantization_config.validate()) {
        GS_LOG_STREAMING_ERROR("[Quantization Config] Invalid configuration detected:");
        GS_LOG_STREAMING_ERROR(g_quantization_config.get_validation_errors());
        GS_LOG_STREAMING_INFO("[Quantization Config] Resetting to defaults...");
        g_quantization_config.reset_to_defaults();
        g_quantization_config.save_to_project_settings();
    }
}
