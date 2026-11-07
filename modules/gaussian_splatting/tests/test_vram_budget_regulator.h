#pragma once

#include "test_macros.h"

#include "../core/gaussian_streaming.h"
#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "gs_test_setting_guard.h"

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] Unknown capacity uses conservative project default") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE(project_settings != nullptr);

    const String tier_apply_setting = "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets";
    const String vram_budget_setting = "rendering/gaussian_splatting/streaming/vram_budget_mb";
    ProjectSettingGuard tier_apply_guard(project_settings, tier_apply_setting);
    ProjectSettingGuard vram_budget_guard(project_settings, vram_budget_setting);

    project_settings->set_setting(tier_apply_setting, false);
    project_settings->set_setting(vram_budget_setting, STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB);

    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    REQUIRE(regulator.is_valid());

    regulator->initialize(nullptr);

    Dictionary stats = regulator->get_debug_stats_dictionary();
    CHECK_FALSE(bool(stats.get("device_capacity_known", true)));
    CHECK(int64_t(stats.get("requested_budget_mb", int64_t(-1))) == int64_t(STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB));
    CHECK(Math::is_equal_approx(float(stats.get("budget_mb", -1.0f)), float(STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB)));
    CHECK(String(stats.get("requested_source_budget_mb", String())) == String("project_default"));
    CHECK(String(stats.get("source_budget_mb", String())) == String("unknown_capacity_fallback"));
    CHECK(bool(stats.get("budget_uses_unknown_capacity_fallback", false)));
    CHECK_FALSE(bool(stats.get("budget_unverified", true)));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] Unknown device capacity remains explicit") {
    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    REQUIRE(regulator.is_valid());

    VRAMBudgetConfig override_config;
    override_config.budget_mb = 1234;
    override_config.min_chunks = 3;
    override_config.max_chunks = 9;
    override_config.auto_regulate_enabled = false;

    regulator->set_config_override(override_config);
    regulator->initialize(nullptr);

    Dictionary stats = regulator->get_debug_stats_dictionary();
    CHECK_FALSE(bool(stats.get("device_memory_queryable", true)));
    CHECK_FALSE(bool(stats.get("device_total_known", true)));
    CHECK(uint64_t(stats.get("device_total_bytes", uint64_t(1))) == uint64_t(0));
    CHECK(uint64_t(stats.get("device_reported_bytes", uint64_t(1))) == uint64_t(0));

    const float budget_mb = float(stats.get("budget_mb", -1.0f));
    CHECK(Math::is_equal_approx(budget_mb, 1234.0f));
    CHECK(int64_t(stats.get("requested_budget_mb", int64_t(-1))) == int64_t(1234));
    CHECK(String(stats.get("source_budget_mb", String())) == String("runtime_override"));
    CHECK(String(stats.get("requested_source_budget_mb", String())) == String("runtime_override"));
    CHECK_FALSE(bool(stats.get("budget_uses_unknown_capacity_fallback", true)));
    CHECK(bool(stats.get("budget_unverified", false)));
    CHECK(regulator->get_current_max_chunks() == 9u);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] Unknown capacity preserves project VRAM override") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE(project_settings != nullptr);

    const String tier_apply_setting = "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets";
    const String vram_budget_setting = "rendering/gaussian_splatting/streaming/vram_budget_mb";
    ProjectSettingGuard tier_apply_guard(project_settings, tier_apply_setting);
    ProjectSettingGuard vram_budget_guard(project_settings, vram_budget_setting);

    project_settings->set_setting(tier_apply_setting, false);
    project_settings->set_setting(vram_budget_setting, 4096);

    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    REQUIRE(regulator.is_valid());

    regulator->initialize(nullptr);

    Dictionary stats = regulator->get_debug_stats_dictionary();
    CHECK_FALSE(bool(stats.get("device_capacity_known", true)));
    CHECK(int64_t(stats.get("requested_budget_mb", int64_t(-1))) == int64_t(4096));
    CHECK(Math::is_equal_approx(float(stats.get("budget_mb", -1.0f)), 4096.0f));
    CHECK(String(stats.get("requested_source_budget_mb", String())) == String("project_override"));
    CHECK(String(stats.get("source_budget_mb", String())) == String("project_override"));
    CHECK_FALSE(bool(stats.get("budget_uses_unknown_capacity_fallback", true)));
    CHECK(bool(stats.get("budget_unverified", false)));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] Known device capacity verifies budget under capacity (#321)") {
    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    REQUIRE(regulator.is_valid());

    VRAMBudgetConfig override_config;
    override_config.budget_mb = 4096;
    override_config.min_chunks = 2;
    override_config.max_chunks = 16;
    override_config.auto_regulate_enabled = false;
    // Trusted capacity supplied out-of-band: 8 GB device, budget fits under it.
    override_config.device_capacity_bytes = uint64_t(8192) * 1024u * 1024u;
    override_config.device_capacity_known = true;

    regulator->set_config_override(override_config);
    regulator->initialize(nullptr);

    Dictionary stats = regulator->get_debug_stats_dictionary();
    // The capacity-known path must be reachable and reported as such (#321):
    // previously _query_device_memory() unconditionally forced unknown.
    CHECK(bool(stats.get("device_capacity_known", false)));
    CHECK(uint64_t(stats.get("device_capacity_bytes", uint64_t(0))) == uint64_t(8192) * 1024u * 1024u);
    // Budget fits under the real capacity, so it is verified and not clamped or
    // routed through the conservative unknown-capacity fallback.
    CHECK(Math::is_equal_approx(float(stats.get("budget_mb", -1.0f)), 4096.0f));
    CHECK(bool(stats.get("budget_capacity_verified", false)));
    CHECK_FALSE(bool(stats.get("budget_uses_unknown_capacity_fallback", true)));
    CHECK_FALSE(bool(stats.get("budget_unverified", true)));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] Clearing a capacity override drops stale device capacity (Codex #411)") {
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    REQUIRE(project_settings != nullptr);

    const String tier_apply_setting = "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets";
    const String vram_budget_setting = "rendering/gaussian_splatting/streaming/vram_budget_mb";
    ProjectSettingGuard tier_apply_guard(project_settings, tier_apply_setting);
    ProjectSettingGuard vram_budget_guard(project_settings, vram_budget_setting);

    // A project that asks for a large budget while true device capacity is unknown.
    project_settings->set_setting(tier_apply_setting, false);
    project_settings->set_setting(vram_budget_setting, 12288);

    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    REQUIRE(regulator.is_valid());

    // First install an override that carries a trusted (small) device capacity.
    VRAMBudgetConfig override_config;
    override_config.budget_mb = 12288;
    override_config.min_chunks = 2;
    override_config.max_chunks = 16;
    override_config.auto_regulate_enabled = false;
    override_config.device_capacity_bytes = uint64_t(4096) * 1024u * 1024u; // 4 GB device.
    override_config.device_capacity_known = true;
    regulator->set_config_override(override_config);

    Dictionary override_stats = regulator->get_debug_stats_dictionary();
    CHECK(bool(override_stats.get("device_capacity_known", false)));
    CHECK(Math::is_equal_approx(float(override_stats.get("budget_mb", -1.0f)), 4096.0f));

    // Clearing the override must NOT leave the regulator believing the device still
    // has a known 4 GB capacity. The project config carries no trusted capacity, so
    // it must follow the unknown-capacity path (not stay verified / clamped to 4 GB).
    regulator->clear_config_override();

    Dictionary cleared_stats = regulator->get_debug_stats_dictionary();
    CHECK_FALSE(bool(cleared_stats.get("device_capacity_known", true)));
    CHECK(uint64_t(cleared_stats.get("device_capacity_bytes", uint64_t(1))) == uint64_t(0));
    CHECK_FALSE(bool(cleared_stats.get("budget_capacity_verified", true)));
    CHECK(String(cleared_stats.get("source_budget_mb", String())) != String("detected_capacity_clamp"));
    // 12288 MB from a project override with unknown capacity is unverified, not
    // clamped to the cleared override's 4 GB.
    CHECK(bool(cleared_stats.get("budget_unverified", false)));
    CHECK(int64_t(cleared_stats.get("requested_budget_mb", int64_t(-1))) == int64_t(12288));
    CHECK(Math::is_equal_approx(float(cleared_stats.get("budget_mb", -1.0f)), 12288.0f));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] Known device capacity clamps an over-large budget (#321)") {
    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    REQUIRE(regulator.is_valid());

    VRAMBudgetConfig override_config;
    // Request more than the device actually has; must clamp to real capacity.
    override_config.budget_mb = 12288;
    override_config.min_chunks = 2;
    override_config.max_chunks = 16;
    override_config.auto_regulate_enabled = false;
    override_config.device_capacity_bytes = uint64_t(4096) * 1024u * 1024u; // 4 GB device.
    override_config.device_capacity_known = true;

    regulator->set_config_override(override_config);
    regulator->initialize(nullptr);

    Dictionary stats = regulator->get_debug_stats_dictionary();
    CHECK(bool(stats.get("device_capacity_known", false)));
    // requested_budget_mb preserves the pre-clamp ask; budget_mb is clamped to
    // the detected device capacity rather than left at the unsafe request.
    CHECK(int64_t(stats.get("requested_budget_mb", int64_t(-1))) == int64_t(12288));
    CHECK(Math::is_equal_approx(float(stats.get("budget_mb", -1.0f)), 4096.0f));
    CHECK(String(stats.get("source_budget_mb", String())) == String("detected_capacity_clamp"));
    CHECK(bool(stats.get("budget_capacity_verified", false)));
    CHECK_FALSE(bool(stats.get("budget_uses_unknown_capacity_fallback", true)));
}

// BV-04a (ADR adr-beyond-vram-worlds.md D-6): whether and why the regulator fired is published
// by kind, with the frame and the LOD multiplier changes, so the M1 probe can label a run
// settings_drifted. These drive VRAMBudgetRegulator::update() directly; no device is needed.
namespace {

Ref<VRAMBudgetRegulator> _make_bv04a_regulator() {
    Ref<VRAMBudgetRegulator> regulator;
    regulator.instantiate();
    VRAMBudgetConfig config;
    config.budget_mb = 1000;
    config.min_chunks = 2;
    config.max_chunks = 20;
    config.auto_regulate_enabled = true;
    config.warning_threshold_percent = 85;
    config.regulation_step_percent = 5.0f; // step = 1 chunk of 20
    regulator->set_config_override(config);
    regulator->initialize(nullptr);
    return regulator;
}

uint64_t _bv04a_usage_at_percent(const Ref<VRAMBudgetRegulator> &p_regulator, uint32_t p_percent) {
    return p_regulator->get_debug_stats().budget_bytes / 100u * p_percent;
}

} // namespace

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] BV-04a the thrash ratchet is published as a thrash reduction with its LOD multiplier change") {
    Ref<VRAMBudgetRegulator> regulator = _make_bv04a_regulator();
    if (regulator->get_current_max_chunks() != 20u || regulator->get_debug_stats().budget_bytes == 0) {
        FAIL("fixture precondition: chunk cap 20 and a non-zero budget");
        return;
    }
    Dictionary stats = regulator->get_debug_stats_dictionary();
    CHECK(String(stats.get("last_regulation_reason", String())) == String("none"));
    CHECK(int64_t(stats.get("thrash_reductions", int64_t(-1))) == 0);
    CHECK(int64_t(stats.get("lod_multiplier_adjustments", int64_t(-1))) == 0);

    // 70 % usage sits between the step-up (< 59.5 %) and step-down (> 85 %) bands, so only
    // the thrash detector (visible refetches inside its wall-clock window) can fire.
    const uint64_t usage = _bv04a_usage_at_percent(regulator, 70);
    for (uint64_t frame = 1; frame <= 35; frame++) {
        regulator->update(usage, usage, 10, 1, 1, frame, 1, frame * 16667);
    }
    stats = regulator->get_debug_stats_dictionary();
    CHECK(int64_t(stats.get("thrash_reductions", int64_t(-1))) == 1);
    CHECK(int64_t(stats.get("budget_reductions", int64_t(-1))) == 0);
    CHECK(int64_t(stats.get("headroom_increases", int64_t(-1))) == 0);
    CHECK(int64_t(stats.get("regulation_adjustments", int64_t(-1))) == 1);
    CHECK(int64_t(stats.get("lod_multiplier_adjustments", int64_t(-1))) == 1);
    CHECK(int64_t(stats.get("last_regulation_frame", int64_t(-1))) == 30); // MIN_FRAMES_BETWEEN_ADJUSTMENTS
    CHECK(String(stats.get("last_regulation_reason", String())) == String("thrash_reduction"));
    CHECK(regulator->get_current_max_chunks() == 18u);
    CHECK(regulator->get_lod_distance_multiplier() == doctest::Approx(1.1f));
}

// Drives the regulator at p_fps for p_seconds with p_refetches_per_second visible refetches,
// spread evenly in wall-clock time, and a load plus an eviction in every frame.
int64_t _bv08a_thrash_reductions(uint32_t p_fps, double p_seconds, double p_refetches_per_second) {
    Ref<VRAMBudgetRegulator> regulator = _make_bv04a_regulator();
    const uint64_t usage = _bv04a_usage_at_percent(regulator, 70); // no budget step in either direction
    const uint64_t frames = uint64_t(p_seconds * p_fps);
    double owed = 0.0;
    for (uint64_t frame = 1; frame <= frames; frame++) {
        owed += p_refetches_per_second / p_fps;
        const uint32_t refetches = uint32_t(owed);
        owed -= refetches;
        regulator->update(usage, usage, 10, 1, 1, frame, refetches, frame * 1000000u / p_fps);
    }
    return int64_t(regulator->get_debug_stats_dictionary().get("thrash_reductions", int64_t(-1)));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] BV-08a thrash is visible-set refetch in a wall-clock window (C-thrash)") {
    // Cap 20 trips at 8 refetches inside 2 s. Both frame rates must give the same answer for
    // the same wall-clock behaviour.
    const uint32_t frame_rates[2] = { 60, 200 };
    for (uint32_t fps : frame_rates) {
        CAPTURE(fps);
        // Negative control: a starved budget refetches what it just evicted, and it must trip.
        CHECK(_bv08a_thrash_reductions(fps, 3.0, 30.0) >= 1);
        // A fly-through at a full cap loads and evicts every frame, but nothing comes back.
        CHECK(_bv08a_thrash_reductions(fps, 3.0, 0.0) == 0);
        // Refetches too sparse to reach 8 in any 2 s window: a frame-count window would see
        // fewer at 200 fps than at 60 fps, and one counting all history would trip here.
        CHECK(_bv08a_thrash_reductions(fps, 20.0, 2.0) == 0);
    }
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator] BV-04a budget step-down and headroom step-up are published by kind") {
    Ref<VRAMBudgetRegulator> regulator = _make_bv04a_regulator();
    if (regulator->get_current_max_chunks() != 20u || regulator->get_debug_stats().budget_bytes == 0) {
        FAIL("fixture precondition: chunk cap 20 and a non-zero budget");
        return;
    }
    // No evictions, so the thrash detector stays quiet. Over the threshold: one step down.
    const uint64_t over = _bv04a_usage_at_percent(regulator, 90);
    for (uint64_t frame = 1; frame <= 30; frame++) {
        regulator->update(over, over, 10, 0, 0, frame);
    }
    Dictionary stats = regulator->get_debug_stats_dictionary();
    CHECK(int64_t(stats.get("budget_reductions", int64_t(-1))) == 1);
    CHECK(int64_t(stats.get("thrash_reductions", int64_t(-1))) == 0);
    CHECK(int64_t(stats.get("last_regulation_frame", int64_t(-1))) == 30);
    CHECK(String(stats.get("last_regulation_reason", String())) == String("budget_reduction"));
    CHECK(regulator->get_current_max_chunks() == 19u);

    // Well under the threshold: one step back up. The multiplier is still 1.0, so it is not
    // an LOD multiplier adjustment.
    const uint64_t under = _bv04a_usage_at_percent(regulator, 10);
    for (uint64_t frame = 31; frame <= 60; frame++) {
        regulator->update(under, under, 10, 0, 0, frame);
    }
    stats = regulator->get_debug_stats_dictionary();
    CHECK(int64_t(stats.get("headroom_increases", int64_t(-1))) == 1);
    CHECK(int64_t(stats.get("regulation_adjustments", int64_t(-1))) == 2);
    CHECK(int64_t(stats.get("lod_multiplier_adjustments", int64_t(-1))) == 0);
    CHECK(int64_t(stats.get("last_regulation_frame", int64_t(-1))) == 60);
    CHECK(String(stats.get("last_regulation_reason", String())) == String("headroom_increase"));
    CHECK(regulator->get_current_max_chunks() == 20u);
}
