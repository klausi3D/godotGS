/**************************************************************************/
/*  test_config_validation.h                                              */
/*  Configuration validation unit tests for Gaussian Splatting module     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"
#include "core/config/project_settings.h"
#include "gs_test_setting_guard.h"
#include "../core/gaussian_splat_manager.h"
#include "../renderer/gpu_sorting_config.h"
#include "../renderer/pipeline_feature_set.h"
#include "../renderer/sorting_config.h"
#include "../renderer/sorting_settings_utils.h"
#include "../renderer/gpu_sorter.h"
#include "../core/gaussian_splat_quality_config.h"
#include "../core/streaming_vram_regulator.h"
#include "../core/streaming_layout_hint.h"
#include "../interfaces/gpu_culler.h"
#include "../lod/lod_config.h"

#include <limits>

namespace TestConfigValidation {

static float _reload_manager_sorting_target_ms() {
	GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
	GaussianSplatManager *owned_manager = nullptr;
	if (!manager) {
		owned_manager = memnew(GaussianSplatManager);
		manager = owned_manager;
	}
	ERR_FAIL_NULL_V(manager, 0.0f);
	manager->initialize_module();
	const float target_sort_time_ms = manager->get_sorting_target_ms();
	if (owned_manager) {
		memdelete(owned_manager);
	}
	return target_sort_time_ms;
}

// =============================================================================
// GPUSortingConfig Validation Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig default values pass validation") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	CHECK(config.validate());
	CHECK(config.get_validation_errors().is_empty());
}

TEST_CASE("[GaussianSplatting][Config] Hidden runtime-affecting ProjectSettings are registered with stable defaults") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const StringName renderdoc_key("rendering/gaussian_splatting/renderdoc_compatibility");
	const StringName depth_test_key("rendering/gaussian_splatting/composite/depth_test");
	const StringName effector_frequency_key("rendering/gaussian_splatting/effects/sphere_effector_frequency");
	const StringName vram_budget_key("rendering/gaussian_splatting/streaming/vram_budget_mb");

	CHECK(ps->has_setting(renderdoc_key));
	CHECK(ps->has_setting(depth_test_key));
	CHECK(ps->has_setting(effector_frequency_key));
	CHECK(ps->has_setting(vram_budget_key));

	CHECK_FALSE(bool(ps->get_setting(renderdoc_key)));
	CHECK(bool(ps->get_setting(depth_test_key)));
	CHECK(Math::is_equal_approx(double(ps->get_setting(effector_frequency_key)), 2.0));
	if (ps->property_can_revert(vram_budget_key)) {
		CHECK(int64_t(ps->property_get_revert(vram_budget_key)) == int64_t(STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB));
	}

	// Formerly-hidden keys registered in slice 1 (issues #163 / #172). Defaults
	// must equal the code defaults the raw reads previously fell back to, so
	// registration stays behavior-neutral.
	const StringName sync_loads_key("rendering/gaussian_splatting/streaming/max_sync_fallback_loads_per_frame");
	const StringName sync_queue_key("rendering/gaussian_splatting/streaming/max_sync_fallback_queue_size");
	const StringName lod_importance_key("rendering/gaussian_splatting/lod/importance_threshold");

	CHECK(ps->has_setting(sync_loads_key));
	CHECK(ps->has_setting(sync_queue_key));
	CHECK(ps->has_setting(lod_importance_key));

	CHECK(int64_t(ps->get_setting(sync_loads_key)) == 1);
	CHECK(int64_t(ps->get_setting(sync_queue_key)) == 2048);
	// -1 sentinel = "auto" (leave importance_cull_threshold to the runtime
	// auto-tuner); registration stays behavior-neutral. Only >= 0 pins a value.
	CHECK(Math::is_equal_approx(double(ps->get_setting(lod_importance_key)), -1.0));

	const char *logging_category_keys[] = {
		"rendering/gaussian_splatting/logging/general",
		"rendering/gaussian_splatting/logging/renderer",
		"rendering/gaussian_splatting/logging/streaming",
		"rendering/gaussian_splatting/logging/gpu_sort",
		"rendering/gaussian_splatting/logging/gpu_memory",
		"rendering/gaussian_splatting/logging/compositor",
		"rendering/gaussian_splatting/logging/command_buffer",
		"rendering/gaussian_splatting/logging/tests",
	};
	for (const char *category_key : logging_category_keys) {
		const StringName key(category_key);
		CHECK(ps->has_setting(key));
		// "inherit" is the k_default_levels sentinel for every category: follow the
		// master verbosity (which defaults to "warn"), so default output is unchanged.
		CHECK(String(ps->get_setting(key)) == "inherit");
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid target_sort_time_ms") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Zero target time is invalid") {
		config.target_sort_time_ms = 0.0f;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Target sort time must be > 0.1ms"));
	}

	SUBCASE("Negative target time is invalid") {
		config.target_sort_time_ms = -1.0f;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Target sort time must be > 0.1ms"));
	}

	SUBCASE("Exactly 0.1ms is invalid (must be greater than)") {
		config.target_sort_time_ms = 0.1f;
		CHECK_FALSE(config.validate());
	}

	SUBCASE("Just above threshold is valid") {
		config.target_sort_time_ms = 0.11f;
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] Sort-target follows the canonical diagnostics project setting") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	// Canonical is diagnostics/sort_target_time_ms (#168); gpu_sorting/ is the
	// older deprecated alias. The middle deprecated alias sorting/ is guarded and
	// cleared so it cannot shadow the canonical/legacy precedence under test.
	const String canonical_path = "rendering/gaussian_splatting/diagnostics/sort_target_time_ms";
	const String deprecated_sorting_path = "rendering/gaussian_splatting/sorting/target_sort_time_ms";
	const String legacy_path = "rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms";
	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	ProjectSettingGuard canonical_guard(project_settings, canonical_path);
	ProjectSettingGuard deprecated_sorting_guard(project_settings, deprecated_sorting_path);
	ProjectSettingGuard legacy_guard(project_settings, legacy_path);
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	if (project_settings->has_setting(deprecated_sorting_path)) {
		project_settings->clear(deprecated_sorting_path);
	}
	// Force the manual config path so GPUSortingConfig reads the sort-target
	// instead of short-circuiting through the default "high" preset.
	project_settings->set_setting(preset_path, "custom");

	SUBCASE("Canonical key wins over the legacy alias for both loaders") {
		project_settings->set_setting(canonical_path, 3.5f);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(3.5f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(3.5f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(3.5f));
	}

	SUBCASE("Explicit canonical default still wins over the legacy alias") {
		project_settings->set_setting(canonical_path, 2.0f);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(2.0f));
	}

	SUBCASE("Runtime canonical edits override legacy alias after startup") {
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		project_settings->set_setting(canonical_path, 4.0f);
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(4.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(4.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(4.0f));
	}

	SUBCASE("Runtime canonical default write overrides the legacy alias immediately") {
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		project_settings->set_setting(canonical_path, 2.0f);
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(2.0f));
	}

	SUBCASE("Runtime canonical edits back to the default still override the legacy alias") {
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.25f);
		initialize_gpu_sorting_config();
		project_settings->set_setting(canonical_path, 4.0f);
		CHECK(_reload_manager_sorting_target_ms() == doctest::Approx(4.0f));
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(4.0f));
		CHECK(SortingStrategyConfig::load_from_project_settings().target_sort_time_ms == doctest::Approx(4.0f));

		project_settings->set_setting(canonical_path, 2.0f);
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(2.0f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(2.0f));
	}

	SUBCASE("Legacy alias fallback stays aligned until projects migrate") {
		REQUIRE(project_settings->has_setting(canonical_path));
		project_settings->clear(canonical_path);
		project_settings->set_setting(legacy_path, 1.75f);
		initialize_gpu_sorting_config();
		const float manager_target_sort_time_ms = _reload_manager_sorting_target_ms();
		project_settings->emit_signal("settings_changed");

		g_gpu_sorting_config.load_from_project_settings();
		const SortingStrategyConfig strategy_config = SortingStrategyConfig::load_from_project_settings();

		CHECK(g_gpu_sorting_config.target_sort_time_ms == doctest::Approx(1.75f));
		CHECK(strategy_config.target_sort_time_ms == doctest::Approx(1.75f));
		CHECK(manager_target_sort_time_ms == doctest::Approx(1.75f));
	}
}

// =============================================================================
// Settings-hygiene S7 renames: canonical key + deprecated alias (#167/#168/#169)
// =============================================================================

TEST_CASE("[GaussianSplatting][Config][Sort] diagnostics/sort_target_time_ms honors deprecated aliases (#168)") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const String canonical_path = "rendering/gaussian_splatting/diagnostics/sort_target_time_ms";
	const String sorting_alias = "rendering/gaussian_splatting/sorting/target_sort_time_ms";
	const String gpu_sorting_alias = "rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms";
	ProjectSettingGuard canonical_guard(project_settings, canonical_path);
	ProjectSettingGuard sorting_guard(project_settings, sorting_alias);
	ProjectSettingGuard gpu_sorting_guard(project_settings, gpu_sorting_alias);

	auto reset_all = [&]() {
		if (project_settings->has_setting(canonical_path)) {
			project_settings->clear(canonical_path);
		}
		if (project_settings->has_setting(sorting_alias)) {
			project_settings->clear(sorting_alias);
		}
		if (project_settings->has_setting(gpu_sorting_alias)) {
			project_settings->clear(gpu_sorting_alias);
		}
	};

	SUBCASE("(a) default: neither key set -> caller fallback, no alias consulted") {
		reset_all();
		CHECK(gs::sorting_settings::get_target_sort_time_ms(project_settings, 2.0f) == doctest::Approx(2.0f));
	}

	SUBCASE("(b) deprecated sorting/ alias supplies the value when canonical is unset") {
		reset_all();
		project_settings->set_setting(sorting_alias, 3.25f);
		CHECK(gs::sorting_settings::get_target_sort_time_ms(project_settings, 2.0f) == doctest::Approx(3.25f));
	}

	SUBCASE("(b') older gpu_sorting/ alias supplies the value when canonical + sorting/ are unset") {
		reset_all();
		project_settings->set_setting(gpu_sorting_alias, 1.75f);
		CHECK(gs::sorting_settings::get_target_sort_time_ms(project_settings, 2.0f) == doctest::Approx(1.75f));
	}

	SUBCASE("(c) explicit canonical wins over both deprecated aliases") {
		reset_all();
		project_settings->set_setting(canonical_path, 5.5f);
		project_settings->set_setting(sorting_alias, 3.25f);
		project_settings->set_setting(gpu_sorting_alias, 1.75f);
		CHECK(gs::sorting_settings::get_target_sort_time_ms(project_settings, 2.0f) == doctest::Approx(5.5f));
	}

	SUBCASE("sorting/ takes precedence over the older gpu_sorting/ alias") {
		reset_all();
		project_settings->set_setting(sorting_alias, 3.25f);
		project_settings->set_setting(gpu_sorting_alias, 1.75f);
		CHECK(gs::sorting_settings::get_target_sort_time_ms(project_settings, 2.0f) == doctest::Approx(3.25f));
	}
}

TEST_CASE("[GaussianSplatting][Config][Streaming] streaming/layout_hint_validation_strict is canonical over the debug/ deprecated alias (#173)") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const String canonical_path = "rendering/gaussian_splatting/streaming/layout_hint_validation_strict";
	const String deprecated_path = "rendering/gaussian_splatting/debug/layout_hint_validation_strict";
	ProjectSettingGuard canonical_guard(project_settings, canonical_path);
	ProjectSettingGuard deprecated_guard(project_settings, deprecated_path);

	auto reset_all = [&]() {
		if (project_settings->has_setting(canonical_path)) {
			project_settings->clear(canonical_path);
		}
		if (project_settings->has_setting(deprecated_path)) {
			project_settings->clear(deprecated_path);
		}
	};

	SUBCASE("(a) default: neither key set -> false, no alias consulted") {
		reset_all();
		CHECK(gs_layout_hint::_layout_hint_strict_validation_enabled() == false);
	}

	SUBCASE("(b) deprecated debug/ alias supplies the value when canonical is unset") {
		reset_all();
		project_settings->set_setting(deprecated_path, true);
		CHECK(gs_layout_hint::_layout_hint_strict_validation_enabled() == true);
	}

	SUBCASE("(c) explicit canonical streaming/ wins when the deprecated alias is unset") {
		reset_all();
		project_settings->set_setting(canonical_path, true);
		CHECK(gs_layout_hint::_layout_hint_strict_validation_enabled() == true);
	}

	SUBCASE("(d) explicit canonical=false wins over deprecated alias=true (canonical precedence)") {
		reset_all();
		project_settings->set_setting(canonical_path, false);
		project_settings->set_setting(deprecated_path, true);
		CHECK(gs_layout_hint::_layout_hint_strict_validation_enabled() == false);
	}

	SUBCASE("(e) explicit canonical=true wins over deprecated alias=false") {
		reset_all();
		project_settings->set_setting(canonical_path, true);
		project_settings->set_setting(deprecated_path, false);
		CHECK(gs_layout_hint::_layout_hint_strict_validation_enabled() == true);
	}
}

TEST_CASE("[GaussianSplatting][Config][Lod] lod/diagnostic_logging honors the debug_visualization alias (#167)") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	register_lod_project_settings(); // ensure the canonical key is registered (idempotent)

	const String canonical_path = "rendering/gaussian_splatting/lod/diagnostic_logging";
	const String deprecated_path = "rendering/gaussian_splatting/lod/debug_visualization";
	ProjectSettingGuard canonical_guard(project_settings, canonical_path);
	ProjectSettingGuard deprecated_guard(project_settings, deprecated_path);

	auto reset_all = [&]() {
		if (project_settings->has_setting(canonical_path)) {
			project_settings->set_setting(canonical_path, false); // canonical is builtin; restore default
		}
		if (project_settings->has_setting(deprecated_path)) {
			project_settings->clear(deprecated_path);
		}
	};
	auto load_flag = [&]() {
		LODConfig config;
		config.load_from_project_settings();
		return config.diagnostic_logging;
	};

	SUBCASE("(a) default: neither key set -> false") {
		reset_all();
		CHECK_FALSE(load_flag());
	}

	SUBCASE("(b) deprecated debug_visualization alias enables diagnostic logging") {
		reset_all();
		project_settings->set_setting(deprecated_path, true);
		CHECK(load_flag());
	}

	SUBCASE("(c) explicit canonical wins over the deprecated alias") {
		reset_all();
		project_settings->set_setting(canonical_path, true);
		project_settings->set_setting(deprecated_path, false);
		CHECK(load_flag());
	}
}

TEST_CASE("[GaussianSplatting][Config][Pipeline] pipeline/enable_all_pipeline_experimental honors the enable_all_experimental alias (#169)") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const String canonical_path = "rendering/gaussian_splatting/pipeline/enable_all_pipeline_experimental";
	const String deprecated_path = "rendering/gaussian_splatting/pipeline/enable_all_experimental";
	ProjectSettingGuard canonical_guard(project_settings, canonical_path);
	ProjectSettingGuard deprecated_guard(project_settings, deprecated_path);

	auto reset_all = [&]() {
		if (project_settings->has_setting(canonical_path)) {
			project_settings->set_setting(canonical_path, false); // canonical is builtin; restore default
		}
		if (project_settings->has_setting(deprecated_path)) {
			project_settings->clear(deprecated_path);
		}
	};
	auto load_flag = [&]() {
		PipelineFeatureSet config;
		config.load_from_project_settings();
		return config.enable_all_pipeline_experimental;
	};

	SUBCASE("(a) default: neither key set -> false") {
		reset_all();
		CHECK_FALSE(load_flag());
	}

	SUBCASE("(b) deprecated enable_all_experimental alias force-enables the bundle") {
		reset_all();
		project_settings->set_setting(deprecated_path, true);
		CHECK(load_flag());
	}

	SUBCASE("(c) explicit canonical wins over the deprecated alias") {
		reset_all();
		project_settings->set_setting(canonical_path, true);
		project_settings->set_setting(deprecated_path, false);
		CHECK(load_flag());
	}
}

TEST_CASE("[GaussianSplatting][Config] Adaptive overlap-budget knobs round-trip through project settings") {
	// Guards the S2 measured-sort-sizing wiring: adaptive_overlap_budget_enabled and
	// max_overlap_records_adaptive_min must be readable from ProjectSettings (GS-PERF-S2
	// defaults: flags ON, adaptive_min 100000), and the adaptive-min accessor must never
	// report a floor above the max_overlap_records hard cap. See gpu_sorting_config.cpp:100-102.
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	const String adaptive_path = GPUSortingConfig::ADAPTIVE_OVERLAP_BUDGET_PATH;
	const String shrink_path = GPUSortingConfig::BOUNDED_BUFFER_SHRINK_PATH;
	const String adaptive_min_path = GPUSortingConfig::MAX_OVERLAP_RECORDS_ADAPTIVE_MIN_PATH;
	const String max_overlap_path = GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH;
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	ProjectSettingGuard adaptive_guard(project_settings, adaptive_path);
	ProjectSettingGuard shrink_guard(project_settings, shrink_path);
	ProjectSettingGuard adaptive_min_guard(project_settings, adaptive_min_path);
	ProjectSettingGuard max_overlap_guard(project_settings, max_overlap_path);

	// Default to the manual ("custom") config path for the load/save subcases; the
	// preset subcases below override this to prove the knobs are ALSO honoured under a
	// named preset (they are orthogonal to the sort layout).
	project_settings->set_setting(preset_path, "custom");

	SUBCASE("Both flags default ON (GS-PERF-S2) and adaptive_min defaults to 100000 when unset") {
		project_settings->clear(adaptive_path);
		project_settings->clear(shrink_path);
		project_settings->clear(adaptive_min_path);
		GPUSortingConfig config;
		config.load_from_project_settings();
		// GS-PERF-S2 flipped both defaults ON; loading with the keys cleared must fall
		// back to the struct/GLOBAL_DEF default (true), not the pre-S2 false.
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 100000u);
	}

	SUBCASE("Enabling both flags + a custom adaptive_min is reflected in the loaded config") {
		project_settings->set_setting(adaptive_path, true);
		project_settings->set_setting(shrink_path, true);
		project_settings->set_setting(adaptive_min_path, 250000);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 250000u);
	}

	SUBCASE("adaptive_min accessor is clamped to the max_overlap_records hard cap") {
		project_settings->set_setting(max_overlap_path, 200000);
		project_settings->set_setting(adaptive_min_path, 5000000); // deliberately above the cap
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.max_overlap_records == 200000u);
		CHECK(config.max_overlap_records_adaptive_min == 5000000u); // raw stored value is preserved
		CHECK(config.get_overlap_records_adaptive_min() == 200000u); // accessor clamps to the cap
	}

	SUBCASE("Negative/zero adaptive_min is clamped to a safe lower bound, not wrapped huge") {
		// A negative project value must not wrap through uint32_t and (via the accessor's
		// upper clamp) pin the floor at the hard cap — that would silently disable shrink.
		project_settings->set_setting(max_overlap_path, 1000000);
		project_settings->set_setting(adaptive_min_path, -1);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.max_overlap_records_adaptive_min == 1u); // -1 clamped to >=1, NOT 4294967295
		CHECK(config.get_overlap_records_adaptive_min() == 1u);
		CHECK(config.get_overlap_records_adaptive_min() < config.max_overlap_records);

		// Zero is clamped to the same >=1 floor (the title's other half).
		project_settings->set_setting(adaptive_min_path, 0);
		config.load_from_project_settings();
		CHECK(config.max_overlap_records_adaptive_min == 1u);
	}

	SUBCASE("GLOBAL_DEF registers the new keys with their defaults") {
		// Round-trips the GLOBAL_DEF wiring (not just save_to_project_settings, which
		// creates the keys itself): clear the keys, re-run registration, and confirm
		// they reappear with the documented defaults.
		project_settings->clear(adaptive_path);
		project_settings->clear(adaptive_min_path);
		initialize_gpu_sorting_config();
		CHECK(project_settings->has_setting(adaptive_path));
		CHECK(project_settings->has_setting(adaptive_min_path));
		// GS-PERF-S2: GLOBAL_DEF now registers the adaptive-overlap flag ON by default.
		CHECK(bool(project_settings->get_setting(adaptive_path)) == true);
		CHECK(int64_t(project_settings->get_setting(adaptive_min_path)) == 100000);
	}

	SUBCASE("reset_to_defaults restores both flags ON (GS-PERF-S2) and adaptive_min to 100000") {
		GPUSortingConfig config;
		config.adaptive_overlap_budget_enabled = false;
		config.bounded_buffer_shrink_enabled = false;
		config.max_overlap_records_adaptive_min = 777000;
		config.reset_to_defaults();
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 100000u);
	}

	SUBCASE("Flags are honored under a named preset, not only custom") {
		// The reclaim knobs are orthogonal to the sort layout, so they must be
		// reachable on the default "high" preset — otherwise the VRAM wins are
		// unavailable to anyone who has not switched to gpu_preset="custom".
		project_settings->set_setting(preset_path, "high");
		project_settings->set_setting(adaptive_path, true);
		project_settings->set_setting(shrink_path, true);
		project_settings->set_setting(adaptive_min_path, 300000);
		GPUSortingConfig config;
		config.load_from_project_settings();
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 300000u);
	}

	SUBCASE("apply_preset establishes the default (ON, GS-PERF-S2) baseline, clearing stale custom state") {
		GPUSortingConfig config;
		config.adaptive_overlap_budget_enabled = false;
		config.bounded_buffer_shrink_enabled = false;
		config.max_overlap_records_adaptive_min = 999000;
		REQUIRE(config.apply_preset("high"));
		// Presets copy the struct defaults, which GS-PERF-S2 set ON.
		CHECK(config.adaptive_overlap_budget_enabled == true);
		CHECK(config.bounded_buffer_shrink_enabled == true);
		CHECK(config.max_overlap_records_adaptive_min == 100000u);
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid max_sort_elements") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Zero elements is invalid") {
		config.max_sort_elements = 0;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Max sort elements must be > 1000"));
	}

	SUBCASE("1000 elements is invalid (must be greater than)") {
		config.max_sort_elements = 1000;
		CHECK_FALSE(config.validate());
	}

	SUBCASE("1001 elements is valid") {
		config.max_sort_elements = 1001;
		CHECK(config.validate());
	}

	SUBCASE("Large element count is valid") {
		config.max_sort_elements = 100000000;
		CHECK(config.validate());
	}

	SUBCASE("Element count above buffer-safe maximum is invalid") {
		config.max_sort_elements = 500000001;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Max sort elements must be <= 500,000,000"));
	}
}

// The scalar 500M ceiling only bounds the temp KEY buffer (500M * 8 B = 4.0 GB).
// The dominant sort-path allocation is the RadixSort per-workgroup/per-bin/per-pass
// histogram, which scales with radix_bits and workgroup_size:
//
//   histogram_bytes = ceil(N / workgroup_size) * (1 << radix_bits) * ceil(key_bits / radix_bits) * 4
//
// RenderingDevice::storage_buffer_create() takes a uint32_t, so an oversized value
// truncates modulo 2^32 and hands back a buffer smaller than the shaders index.
// validate() must therefore reject on TOTAL sort-path allocation, not on the key
// buffer alone.
TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects sort-path allocation that would truncate") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Default knobs at the 500M ceiling stay within the device size type") {
		// wg=256, radix_bits=4, key_bits=64:
		//   workgroups      = ceil(500,000,000 / 256)   = 1,953,125
		//   histogram_bytes = 1,953,125 * 16 * 16 * 4   = 2,000,000,000
		//   temp_keys       = 500,000,000 * 8           = 4,000,000,000
		// Largest is 4,000,000,000 <= UINT32_MAX (4,294,967,295), so this is safe.
		config.max_sort_elements = 500000000;
		CHECK(config.validate());
	}

	SUBCASE("8-bit radix at the 500M ceiling overflows and is rejected") {
		// wg=256, radix_bits=8, key_bits=64:
		//   workgroups      = 1,953,125
		//   histogram_bytes = 1,953,125 * 256 * 8 * 4 = 16,000,000,000  (~14.9 GiB)
		// 16,000,000,000 mod 2^32 = 3,115,098,112 -> would silently truncate.
		config.max_sort_elements = 500000000;
		config.radix_bits = 8;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("overflows the RenderingDevice buffer size type"));
	}

	SUBCASE("Smallest workgroup with 8-bit radix overflows even at the default element count") {
		// wg=64, radix_bits=8, key_bits=64 -> 128 bytes of histogram PER ELEMENT:
		//   workgroups      = ceil(50,000,000 / 64)  = 781,250
		//   histogram_bytes = 781,250 * 256 * 8 * 4  = 6,400,000,000  (~5.96 GiB)
		// 6,400,000,000 mod 2^32 = 2,105,032,704 -> would silently truncate.
		config.max_sort_elements = 50000000; // the shipped default
		config.radix_bits = 8;
		config.workgroup_size = 64;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("overflows the RenderingDevice buffer size type"));
	}

	SUBCASE("Worst-case knobs at the 500M ceiling overflow by a wide margin") {
		// wg=64, radix_bits=8, key_bits=64:
		//   histogram_bytes = ceil(500,000,000 / 64) * 256 * 8 * 4 = 64,000,000,000 (~59.6 GiB)
		// 64,000,000,000 mod 2^32 = 3,870,457,856 -> would silently truncate.
		config.max_sort_elements = 500000000;
		config.radix_bits = 8;
		config.workgroup_size = 64;
		CHECK_FALSE(config.validate());
	}

	SUBCASE("Shipped defaults are well inside the bound") {
		// wg=256, radix_bits=4, key_bits=64, N=50,000,000:
		//   histogram_bytes = ceil(50,000,000 / 256) * 16 * 16 * 4 = 200,000,512 (~191 MiB)
		//   temp_keys       = 400,000,000
		CHECK(config.validate());
		CHECK(config.get_validation_errors().is_empty());
	}

	SUBCASE("8-bit radix remains usable at a genuinely safe element count") {
		// The guard bounds allocation, it does not ban the 8-bit knob.
		// wg=256, radix_bits=8, key_bits=64, N=100,000,000:
		//   histogram_bytes = ceil(100,000,000 / 256) * 256 * 8 * 4 = 3,200,000,000 <= UINT32_MAX
		config.max_sort_elements = 100000000;
		config.radix_bits = 8;
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] sort_path_max_buffer_bytes reports the true 64-bit size") {
	// The helper must report the UNTRUNCATED size so callers can reject; if it
	// wrapped internally the guard would be useless.
	CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(500000000ull, 8, 64, 64) == 64000000000ull);
	CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(500000000ull, 8, 256, 64) == 16000000000ull);
	CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(50000000ull, 8, 64, 64) == 6400000000ull);
	// Default knobs at the ceiling: temp_keys (4.0 GB) dominates the 2.0 GB histogram.
	CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(500000000ull, 4, 256, 64) == 4000000000ull);

	CHECK_FALSE(GPUSortingConstants::sort_path_allocation_fits_device_size(500000000ull, 8, 256, 64));
	CHECK(GPUSortingConstants::sort_path_allocation_fits_device_size(500000000ull, 4, 256, 64));
}

TEST_CASE("[GaussianSplatting][Config] sort_path_max_buffer_bytes is total for any radix_bits") {
	// REGRESSION: the helper used to compute `1ull << radix_bits` after guarding
	// radix_bits only as `> 0`. A shift count at or above the width of the promoted
	// type is UNDEFINED BEHAVIOUR, so radix_bits >= 64 was UB inside the very helper
	// the validation surface calls to describe a bad configuration. The helper must
	// now be TOTAL: defined for every input, with a fail-closed sentinel for radix
	// widths it cannot size, so it can never depend on being called in the right order.
	const uint32_t unsupported_radix[] = {0, 1, 2, 3, 5, 6, 7, 9, 16, 31, 32, 33, 63, 64, 65, 127, 128, 255, UINT32_MAX};
	for (uint32_t radix : unsupported_radix) {
		CHECK_FALSE(GPUSortingConstants::is_supported_radix_bits(radix));
		// Defined result, regardless of element count — including the zero-element
		// path, which must not short-circuit around the unsupported radix.
		CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(50000000ull, radix, 256, 64) ==
				GPUSortingConstants::SORT_PATH_SIZE_UNSUPPORTED);
		CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(0ull, radix, 256, 64) ==
				GPUSortingConstants::SORT_PATH_SIZE_UNSUPPORTED);
		// Fail closed: an unsupported config must never look like it fits.
		CHECK_FALSE(GPUSortingConstants::sort_path_allocation_fits_device_size(50000000ull, radix, 256, 64));
	}

	// The supported widths are unchanged and still size normally.
	CHECK(GPUSortingConstants::is_supported_radix_bits(4));
	CHECK(GPUSortingConstants::is_supported_radix_bits(8));
	CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(0ull, 4, 256, 64) == 0ull);
	CHECK(GPUSortingConstants::sort_path_max_buffer_bytes(50000000ull, 8, 64, 64) == 6400000000ull);
}

// #634: radix_bits and workgroup_size are each validated independently, but the
// RadixSort scatter kernel's shared-memory footprint is a function of their
// PRODUCT. This is a DEVICE-INDEPENDENT (pure CPU) assertion of that derived
// quantity: it computes the requirement through the SAME single-source helper the
// runtime probe (RadixSort::is_supported) uses and compares it against the
// portable Vulkan-1.1 minimum constant — nothing re-derives 18432/16384 by hand.
TEST_CASE("[GaussianSplatting][Config] RadixSort scatter shared-memory product is bounded (#634)") {
	using namespace GPUSortingConstants;

	SUBCASE("The unsupportable (8, 512) product exceeds the guaranteed 16 KB floor") {
		// radix_size = 1<<8 = 256; mask_words = ceil(512/32) = 16;
		// scatter uints = 256 * (2 + 16) = 4608; bytes = 4608 * 4 = 18432.
		const uint32_t bytes = radix_scatter_shared_memory_bytes(8, 512);
		CHECK(bytes == 18432u);
		// > the 16384-byte (16 KB) minimum Vulkan 1.1 guarantees -> unsupportable.
		CHECK(bytes > VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES);
		CHECK(VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES == 16384u);
	}

	SUBCASE("Supported combos fit within the guaranteed 16 KB floor") {
		// (8, 256): 256 * (2 + ceil(256/32)=8) * 4 = 256 * 10 * 4 = 10240.
		CHECK(radix_scatter_shared_memory_bytes(8, 256) == 10240u);
		CHECK(radix_scatter_shared_memory_bytes(8, 256) <= VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES);
		// (4, 512): 16 * (2 + 16) * 4 = 1152.
		CHECK(radix_scatter_shared_memory_bytes(4, 512) == 1152u);
		CHECK(radix_scatter_shared_memory_bytes(4, 512) <= VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES);
		// The shipped default (radix_bits=4, workgroup_size=256): 16 * 10 * 4 = 640.
		CHECK(radix_scatter_shared_memory_bytes(4, 256) == 640u);
		CHECK(radix_scatter_shared_memory_bytes(4, 256) <= VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES);
	}

	SUBCASE("An unsupported radix width fails closed with the sentinel, never a small footprint") {
		const uint32_t unsupported_radix[] = {0, 1, 2, 3, 5, 6, 7, 9, 16, 32, 64, UINT32_MAX};
		for (uint32_t radix : unsupported_radix) {
			CHECK_FALSE(is_supported_radix_bits(radix));
			CHECK(radix_scatter_shared_memory_bytes(radix, 256) == RADIX_SCATTER_SHARED_MEMORY_UNSUPPORTED);
			// The sentinel is UINT32_MAX, so it can never be mistaken for "fits".
			CHECK(radix_scatter_shared_memory_bytes(radix, 256) > VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES);
		}
	}
}

// #634 (Codex P2): (radix_bits=8, workgroup_size=512) is a VALID config -- validate()
// accepts it -- but non-portable (18432 B scatter shared memory > the 16 KB Vulkan-1.1
// floor). Because both get_validation_errors() consumers only read it on the validate()
// FAILURE path, the diagnostic for a valid-but-non-portable config must live on a
// separate, non-fatal surface: get_portability_warnings(). It must NOT gate validate()
// (that would silently reset the config and change sort selection). Same single-source
// helper as the formula test above and the runtime probe.
TEST_CASE("[GaussianSplatting][Config] get_portability_warnings flags the valid-but-nonportable radix x workgroup product (#634)") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	const char *kSharedMemFragment = "compute shared memory";

	SUBCASE("(8, 512) passes validate() yet yields a portability warning naming both knobs and the byte total") {
		config.radix_bits = 8;
		config.workgroup_size = 512;
		// The crux of the P2: validate() ACCEPTS it, so it is never reset -- which is
		// exactly why the diagnostic cannot live behind the validate() failure path.
		CHECK(config.validate());
		const String warnings = config.get_portability_warnings();
		CHECK_FALSE(warnings.is_empty());
		CHECK(warnings.contains(kSharedMemFragment));
		CHECK(warnings.contains(String::num_uint64(
				GPUSortingConstants::radix_scatter_shared_memory_bytes(8, 512))));
		CHECK(warnings.contains(String::num_uint64(
				GPUSortingConstants::VULKAN_MIN_COMPUTE_SHARED_MEMORY_BYTES)));
		// The non-fatal diagnostic must NOT leak into the hard-error surface, or
		// get_validation_errors() would be non-empty while validate() is true.
		CHECK_FALSE(config.get_validation_errors().contains(kSharedMemFragment));
	}

	SUBCASE("Supported (radix, workgroup) products yield no portability warning") {
		CHECK(config.get_portability_warnings().is_empty()); // (4, 256) shipped default
		config.radix_bits = 8;
		config.workgroup_size = 256;
		CHECK(config.get_portability_warnings().is_empty());
		config.radix_bits = 4;
		config.workgroup_size = 512;
		CHECK(config.get_portability_warnings().is_empty());
	}

	SUBCASE("An unsupported radix_bits is a hard error, not a portability warning") {
		// The helper returns the fail-closed sentinel; get_portability_warnings() must
		// stay empty (get_validation_errors() names the real problem and the config is
		// reset to defaults), and must never print the sentinel as a byte count.
		config.radix_bits = 64;
		config.workgroup_size = 512;
		CHECK_FALSE(config.validate());
		const String warnings = config.get_portability_warnings();
		CHECK(warnings.is_empty());
		CHECK(config.get_validation_errors().contains("Radix bits must be 4 or 8"));
	}
}

// #634 (Codex P2, round 2): a reachability test that only re-derives
// get_portability_warnings() after init is VACUOUS -- deleting the
// GS_LOG_GPU_SORT_WARN call from initialize_gpu_sorting_config() would leave it green,
// which is exactly the "diagnostic exists but is never surfaced" defect. So this
// captures the EMISSION itself. GS_LOG_GPU_SORT_WARN -> WARN_PRINT -> Godot's
// ErrorHandlerList, so a scoped error handler installed around the real config-load
// call observes the actually-emitted warning text (WARN is never rate-limited --
// gs_logger::should_rate_limit() covers only INFO/DEBUG/TRACE).
struct ScopedPortabilityWarningCapture : public ErrorHandlerList {
	Vector<String> messages;

	static void _handler(void *p_userdata, const char *, const char *, int, const char *p_error,
			const char *p_message, bool, ErrorHandlerType) {
		ScopedPortabilityWarningCapture *self = static_cast<ScopedPortabilityWarningCapture *>(p_userdata);
		String message;
		if (p_message && p_message[0]) {
			message = String::utf8(p_message);
		} else if (p_error) {
			message = String::utf8(p_error);
		}
		if (!message.is_empty()) {
			self->messages.push_back(message);
		}
	}

	ScopedPortabilityWarningCapture() {
		errfunc = _handler;
		userdata = this;
		add_error_handler(this);
	}
	~ScopedPortabilityWarningCapture() {
		remove_error_handler(this);
	}

	bool captured_containing(const String &p_text) const {
		for (int i = 0; i < messages.size(); i++) {
			if (messages[i].find(p_text) != -1) {
				return true;
			}
		}
		return false;
	}
};

TEST_CASE("[GaussianSplatting][Config] initialize_gpu_sorting_config EMITS the nonportable (8,512) warning and stays silent for supported configs (#634)") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const GPUSortingConfig previous_global_config = g_gpu_sorting_config;
	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	const String radix_bits_path = GPUSortingConfig::RADIX_BITS_PATH;
	const String workgroup_size_path = GPUSortingConfig::WORKGROUP_SIZE_PATH;
	const String max_elements_path = GPUSortingConfig::MAX_ELEMENTS_PATH;
	const String key_bits_path = GPUSortingConfig::KEY_BITS_PATH;
	const String tile_bits_path = GPUSortingConfig::TILE_BITS_PATH;
	const String depth_bits_path = GPUSortingConfig::DEPTH_BITS_PATH;
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	ProjectSettingGuard radix_bits_guard(project_settings, radix_bits_path);
	ProjectSettingGuard workgroup_size_guard(project_settings, workgroup_size_path);
	ProjectSettingGuard max_elements_guard(project_settings, max_elements_path);
	ProjectSettingGuard key_bits_guard(project_settings, key_bits_path);
	ProjectSettingGuard tile_bits_guard(project_settings, tile_bits_path);
	ProjectSettingGuard depth_bits_guard(project_settings, depth_bits_path);

	// Custom preset so the individual knobs are honored (named presets impose their own
	// layout). Pin the validate()-relevant knobs to a known-good state so leftover
	// global settings cannot make validate() fail (which would reset the config and mask
	// the case under test). Only radix_bits/workgroup_size differ between subcases.
	project_settings->set_setting(preset_path, "custom");
	project_settings->set_setting(max_elements_path, 50000000); // (50M,8,512,64) fits the device size type
	project_settings->set_setting(key_bits_path, 64);
	project_settings->set_setting(tile_bits_path, 32);
	project_settings->set_setting(depth_bits_path, 32);

	const char *kSharedMemFragment = "compute shared memory";

	SUBCASE("(8, 512) valid-but-nonportable -> init EMITS the portability WARN") {
		project_settings->set_setting(radix_bits_path, 8);
		project_settings->set_setting(workgroup_size_path, 512);
		ScopedPortabilityWarningCapture capture;
		initialize_gpu_sorting_config();
		// The emission ITSELF, not a re-derivation: deleting the WARN call from
		// initialize_gpu_sorting_config() makes this fail (mutation-proven).
		CHECK(capture.captured_containing(kSharedMemFragment));
		// And the load path accepted it (validate() true -> NOT reset), which is why the
		// diagnostic cannot live behind the validate() failure path.
		CHECK(g_gpu_sorting_config.radix_bits == 8u);
		CHECK(g_gpu_sorting_config.workgroup_size == 512u);
		CHECK(g_gpu_sorting_config.validate());
	}

	SUBCASE("(8, 256) supported -> init emits NO portability WARN") {
		project_settings->set_setting(radix_bits_path, 8);
		project_settings->set_setting(workgroup_size_path, 256);
		ScopedPortabilityWarningCapture capture;
		initialize_gpu_sorting_config();
		CHECK_FALSE(capture.captured_containing(kSharedMemFragment));
		CHECK(g_gpu_sorting_config.radix_bits == 8u);
		CHECK(g_gpu_sorting_config.workgroup_size == 256u);
		CHECK(g_gpu_sorting_config.validate());
	}

	g_gpu_sorting_config = previous_global_config;
}

TEST_CASE("[GaussianSplatting][Config] get_validation_errors survives an out-of-range radix_bits") {
	// REGRESSION: get_validation_errors() calls sort_path_max_buffer_bytes()
	// UNCONDITIONALLY — unlike validate(), it does NOT short-circuit through the
	// `radix_bits == 4 || radix_bits == 8` check first. So a setting like
	// radix_bits = 64 hit the undefined shift *while producing the error message that
	// exists to report it*. A validation path must never be unsafe on exactly the
	// malformed input it is there to describe.
	GPUSortingConfig config;
	config.reset_to_defaults();

	// 64 == the width of the uint64 shifted by the helper; 32 == the width of a
	// uint32 (the sibling shift in gpu_sorter.cpp); 0 was the only value guarded.
	const uint32_t bad_radix[] = {0, 32, 64, 65, UINT32_MAX};
	for (uint32_t radix : bad_radix) {
		config.radix_bits = radix;
		CHECK_FALSE(config.validate());
		// Must return a real, specific error rather than crashing or misbehaving.
		const String errors = config.get_validation_errors();
		CHECK_FALSE(errors.is_empty());
		CHECK(errors.contains("Radix bits must be 4 or 8"));
		// And it must NOT report the fail-closed sentinel as if it were a byte count.
		CHECK_FALSE(errors.contains(String::num_uint64(GPUSortingConstants::SORT_PATH_SIZE_UNSUPPORTED)));
		// Nor may it claim an allocation-size overflow derived from a radix width the
		// sort path cannot build: that byte count is meaningless (and, before the fix,
		// was the product of an undefined shift). The radix error above is the truth.
		CHECK_FALSE(errors.contains("overflows the RenderingDevice buffer size type"));
	}

	// A supported radix with a genuinely oversized allocation still reports the
	// allocation error — the sentinel path must not have swallowed it.
	config.radix_bits = 8;
	config.workgroup_size = 64;
	config.max_sort_elements = 50000000;
	CHECK_FALSE(config.validate());
	CHECK(config.get_validation_errors().contains("overflows the RenderingDevice buffer size type"));
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid radix_bits") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("4-bit radix is valid") {
		config.radix_bits = 4;
		CHECK(config.validate());
	}

	SUBCASE("8-bit radix is valid") {
		config.radix_bits = 8;
		CHECK(config.validate());
	}

	SUBCASE("Other radix values are invalid") {
		uint32_t invalid_values[] = {0, 1, 2, 3, 5, 6, 7, 9, 16, 32};
		for (uint32_t value : invalid_values) {
			config.radix_bits = value;
			CHECK_FALSE(config.validate());
			CHECK(config.get_validation_errors().contains("Radix bits must be 4 or 8"));
		}
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid workgroup_size") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Valid workgroup sizes") {
		uint32_t valid_sizes[] = {64, 128, 256, 512};
		for (uint32_t size : valid_sizes) {
			config.workgroup_size = size;
			CHECK(config.validate());
		}
	}

	SUBCASE("Invalid workgroup sizes") {
		uint32_t invalid_sizes[] = {0, 1, 32, 63, 65, 100, 255, 257, 1024};
		for (uint32_t size : invalid_sizes) {
			config.workgroup_size = size;
			CHECK_FALSE(config.validate());
			CHECK(config.get_validation_errors().contains("Workgroup size must be 64, 128, 256, or 512"));
		}
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid key_bits") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("32-bit keys are valid") {
		config.key_bits = 32;
		config.tile_bits = 16;
		config.depth_bits = 16;
		CHECK(config.validate());
	}

	SUBCASE("64-bit keys are valid") {
		config.key_bits = 64;
		CHECK(config.validate());
	}

	SUBCASE("Other key widths are invalid") {
		uint32_t invalid_widths[] = {0, 8, 16, 24, 48, 128};
		for (uint32_t width : invalid_widths) {
			config.key_bits = width;
			// Ensure tile+depth fits for valid test
			config.tile_bits = 1;
			config.depth_bits = 1;
			CHECK_FALSE(config.validate());
			CHECK(config.get_validation_errors().contains("Key bits must be 32 or 64"));
		}
	}
}

TEST_CASE("[GaussianSplatting][Config] Project settings apply preset layouts unless preset is custom") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const GPUSortingConfig previous_global_config = g_gpu_sorting_config;
	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	const String key_bits_path = GPUSortingConfig::KEY_BITS_PATH;
	const String tile_bits_path = GPUSortingConfig::TILE_BITS_PATH;
	const String depth_bits_path = GPUSortingConfig::DEPTH_BITS_PATH;
	const String tie_breaker_path = GPUSortingConfig::ENABLE_TIE_BREAKER_PATH;
	const String stale_use_32bit_keys_path = GPUSortingConfig::SECTION_PATH + "use_32bit_keys";
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	ProjectSettingGuard key_bits_guard(project_settings, key_bits_path);
	ProjectSettingGuard tile_bits_guard(project_settings, tile_bits_path);
	ProjectSettingGuard depth_bits_guard(project_settings, depth_bits_path);
	ProjectSettingGuard tie_breaker_guard(project_settings, tie_breaker_path);
	ProjectSettingGuard stale_use_32bit_keys_guard(project_settings, stale_use_32bit_keys_path);

	auto apply_explicit_32bit_layout = [&]() {
		project_settings->set_setting(key_bits_path, 32);
		project_settings->set_setting(tile_bits_path, 16);
		project_settings->set_setting(depth_bits_path, 16);
		project_settings->set_setting(tie_breaker_path, true);
	};

	auto check_loaded_32bit_layout = [&]() {
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 32);
		CHECK(g_gpu_sorting_config.tile_bits == 16);
		CHECK(g_gpu_sorting_config.depth_bits == 16);
		CHECK(g_gpu_sorting_config.enable_tie_breaker);

		const SortKeyConfig sort_key_config = SortKeyConfig::from_settings();
		CHECK(sort_key_config.key_bits == 32);
		CHECK(sort_key_config.tile_bits == 16);
		CHECK(sort_key_config.depth_bits == 16);
		CHECK(sort_key_config.enable_tie_breaker);
	};

	SUBCASE("Named presets keep their own key layout") {
		struct PresetExpectation {
			const char *name;
			uint32_t key_bits;
			uint32_t tile_bits;
			uint32_t depth_bits;
			bool tie_breaker;
		};
		const PresetExpectation preset_expectations[] = {
			// "low" deliberately uses 64-bit keys too: 32-bit depth keys band and
			// flicker on real-scan content, so NO preset is allowed to silently
			// select them (see GS-298 / project memory "64-bit keys are the only
			// shippable config"). 32-bit keys are an explicit "custom" opt-in only.
			{ "low", 64, 32, 32, false },
			{ "medium", 64, 32, 32, false },
			{ "high", 64, 32, 32, false },
			{ "ultra", 64, 32, 32, true },
		};
		for (const PresetExpectation &preset : preset_expectations) {
			project_settings->set_setting(preset_path, preset.name);
			apply_explicit_32bit_layout();

			g_gpu_sorting_config.load_from_project_settings();
			CHECK(g_gpu_sorting_config.key_bits == preset.key_bits);
			CHECK(g_gpu_sorting_config.tile_bits == preset.tile_bits);
			CHECK(g_gpu_sorting_config.depth_bits == preset.depth_bits);
			CHECK(g_gpu_sorting_config.enable_tie_breaker == preset.tie_breaker);

			const SortKeyConfig sort_key_config = SortKeyConfig::from_settings();
			CHECK(sort_key_config.key_bits == preset.key_bits);
			CHECK(sort_key_config.tile_bits == preset.tile_bits);
			CHECK(sort_key_config.depth_bits == preset.depth_bits);
			CHECK(sort_key_config.enable_tie_breaker == preset.tie_breaker);
		}

		project_settings->set_setting(preset_path, "ultra");
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 64);
		CHECK(g_gpu_sorting_config.tile_bits == 32);
		CHECK(g_gpu_sorting_config.depth_bits == 32);
		CHECK(g_gpu_sorting_config.enable_tie_breaker);
	}

	SUBCASE("Custom loading honors explicit key-layout settings") {
		project_settings->set_setting(preset_path, "custom");
		apply_explicit_32bit_layout();
		check_loaded_32bit_layout();
	}

	SUBCASE("Stale boolean key-width setting is ignored in favor of canonical key_bits") {
		project_settings->set_setting(preset_path, "custom");

		project_settings->set_setting(key_bits_path, 64);
		project_settings->set_setting(tile_bits_path, 32);
		project_settings->set_setting(depth_bits_path, 32);
		project_settings->set_setting(tie_breaker_path, false);
		project_settings->set_setting(stale_use_32bit_keys_path, true);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 64);
		CHECK(g_gpu_sorting_config.tile_bits == 32);
		CHECK(g_gpu_sorting_config.depth_bits == 32);
		CHECK_FALSE(g_gpu_sorting_config.enable_tie_breaker);
		CHECK(SortKeyConfig::from_settings().key_bits == 64);

		project_settings->set_setting(key_bits_path, 32);
		project_settings->set_setting(tile_bits_path, 16);
		project_settings->set_setting(depth_bits_path, 16);
		project_settings->set_setting(tie_breaker_path, true);
		project_settings->set_setting(stale_use_32bit_keys_path, false);
		check_loaded_32bit_layout();
	}

	g_gpu_sorting_config = previous_global_config;
}

TEST_CASE("[GaussianSplatting][Config] No preset or default path silently yields 32-bit sort keys") {
	// GS-298 regression guard. 32-bit quantized depth keys give only 16 depth bits,
	// which band and flicker on real-scan content; 64-bit keys are the only shippable
	// layout. A user must never end up on the 32-bit path WITHOUT an explicit opt-in.
	// This guards every selectable preset AND the documented default preset.

	SUBCASE("Every named preset uses 64-bit keys") {
		CHECK(GPUSortingConfig::preset_low().key_bits == 64u);
		CHECK(GPUSortingConfig::preset_medium().key_bits == 64u);
		CHECK(GPUSortingConfig::preset_high().key_bits == 64u);
		CHECK(GPUSortingConfig::preset_ultra().key_bits == 64u);
	}

	SUBCASE("struct defaults and reset_to_defaults() (the recovery path) yield 64-bit keys") {
		// initialize_gpu_sorting_config() falls back to reset_to_defaults() after a validation
		// failure, so a project with an invalid GPU sorting setting must NOT silently run on the
		// 32-bit layout. Guards both the struct member default and the explicit reset.
		GPUSortingConfig fresh_defaults;
		CHECK(fresh_defaults.key_bits == 64u);
		CHECK(fresh_defaults.tile_bits == 32u);
		CHECK(fresh_defaults.depth_bits == 32u);

		GPUSortingConfig recovered;
		recovered.key_bits = 32; // simulate a bad/legacy value before recovery
		recovered.reset_to_defaults();
		CHECK(recovered.key_bits == 64u);
		CHECK(recovered.tile_bits == 32u);
		CHECK(recovered.depth_bits == 32u);
	}

	SUBCASE("apply_preset by name never installs 32-bit keys") {
		const char *names[] = { "low", "performance", "medium", "balanced",
				"high", "quality", "ultra", "maximum" };
		for (const char *name : names) {
			GPUSortingConfig config;
			REQUIRE(config.apply_preset(name));
			CHECK_MESSAGE(config.key_bits == 64u,
					vformat("Preset '%s' silently selected %d-bit keys", name, config.key_bits));
		}
	}

	SUBCASE("The documented default gpu_preset loads 64-bit keys") {
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		REQUIRE(project_settings != nullptr);

		const GPUSortingConfig previous_global_config = g_gpu_sorting_config;
		const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
		ProjectSettingGuard preset_guard(project_settings, preset_path);

		// "high" is the GLOBAL_DEF default (see initialize_gpu_sorting_config).
		project_settings->set_setting(preset_path, "high");
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 64u);

		g_gpu_sorting_config = previous_global_config;
	}

	SUBCASE("32-bit keys remain reachable ONLY via an explicit custom opt-in") {
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		REQUIRE(project_settings != nullptr);

		const GPUSortingConfig previous_global_config = g_gpu_sorting_config;
		const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
		const String key_bits_path = GPUSortingConfig::KEY_BITS_PATH;
		const String tile_bits_path = GPUSortingConfig::TILE_BITS_PATH;
		const String depth_bits_path = GPUSortingConfig::DEPTH_BITS_PATH;
		ProjectSettingGuard preset_guard(project_settings, preset_path);
		ProjectSettingGuard key_bits_guard(project_settings, key_bits_path);
		ProjectSettingGuard tile_bits_guard(project_settings, tile_bits_path);
		ProjectSettingGuard depth_bits_guard(project_settings, depth_bits_path);

		// Selecting a named preset ignores an explicit key_bits=32: the layout is
		// the preset's own (64-bit), so the 32-bit request cannot silently apply.
		project_settings->set_setting(preset_path, "low");
		project_settings->set_setting(key_bits_path, 32);
		project_settings->set_setting(tile_bits_path, 16);
		project_settings->set_setting(depth_bits_path, 16);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 64u);

		// Only the explicit "custom" path honors key_bits=32 — an intentional opt-in.
		project_settings->set_setting(preset_path, "custom");
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.key_bits == 32u);

		g_gpu_sorting_config = previous_global_config;
	}
}

TEST_CASE("[GaussianSplatting][Config] Explicit max_overlap_records overrides a named preset's budget") {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);

	const GPUSortingConfig previous_global_config = g_gpu_sorting_config;
	const String preset_path = GPUSortingConfig::GPU_PRESET_PATH;
	const String overlap_path = GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH;
	ProjectSettingGuard preset_guard(project_settings, preset_path);
	ProjectSettingGuard overlap_guard(project_settings, overlap_path);

	// preset_low() pins a 10M overlap budget; the legacy/"high" budget is 100M.
	const uint32_t kLowPresetOverlap = 10000000u;
	const uint32_t kHighOverlap = 100000000u;

	SUBCASE("0 sentinel keeps the preset's own overlap budget") {
		project_settings->set_setting(preset_path, "low");
		project_settings->set_setting(overlap_path, 0);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.max_overlap_records == kLowPresetOverlap);
	}

	SUBCASE("Explicit 100M wins over a preset whose budget is lower") {
		// Regression for Codex P2 on #397: detecting explicitness by value-equality
		// against the 100M default dropped a project that intentionally pinned
		// exactly the legacy budget on top of the low-VRAM preset.
		project_settings->set_setting(preset_path, "low");
		project_settings->set_setting(overlap_path, 100000000);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.max_overlap_records == kHighOverlap);
	}

	SUBCASE("Explicit 100M wins over a preset whose budget is higher") {
		project_settings->set_setting(preset_path, "ultra"); // preset_ultra() pins 150M
		project_settings->set_setting(overlap_path, 100000000);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.max_overlap_records == kHighOverlap);
	}

	SUBCASE("Custom preset with the 0 sentinel falls back to the 100M default") {
		project_settings->set_setting(preset_path, "custom");
		project_settings->set_setting(overlap_path, 0);
		g_gpu_sorting_config.load_from_project_settings();
		CHECK(g_gpu_sorting_config.max_overlap_records == kHighOverlap);
	}

	g_gpu_sorting_config = previous_global_config;
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig validates tile/depth bit allocation") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Tile and depth bits must not exceed key_bits") {
		config.key_bits = 32;
		config.tile_bits = 20;
		config.depth_bits = 20; // 40 > 32
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Tile/depth bit split must fit within key_bits"));
	}

	SUBCASE("Tile and depth bits must allocate at least one bit") {
		config.tile_bits = 0;
		config.depth_bits = 0;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Tile/depth bit split must allocate at least one bit"));
	}

	SUBCASE("Valid 32-bit allocation") {
		config.key_bits = 32;
		config.tile_bits = 16;
		config.depth_bits = 16;
		CHECK(config.validate());
	}

	SUBCASE("Valid 64-bit allocation") {
		config.key_bits = 64;
		config.tile_bits = 32;
		config.depth_bits = 32;
		CHECK(config.validate());
	}

	SUBCASE("Partial allocation is valid") {
		config.key_bits = 64;
		config.tile_bits = 24;
		config.depth_bits = 24; // Only 48 bits used of 64
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig rejects invalid performance_log_interval") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	SUBCASE("Zero interval is invalid") {
		config.performance_log_interval = 0;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("Performance log interval must be > 0"));
	}

	SUBCASE("Positive interval is valid") {
		config.performance_log_interval = 1;
		CHECK(config.validate());

		config.performance_log_interval = 1000;
		CHECK(config.validate());
	}
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig accumulates multiple errors") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	// Set multiple invalid values
	config.target_sort_time_ms = 0.0f;
	config.max_sort_elements = 0;
	config.radix_bits = 3;
	config.workgroup_size = 100;
	config.key_bits = 16;
	config.performance_log_interval = 0;

	CHECK_FALSE(config.validate());

	String errors = config.get_validation_errors();
	CHECK(errors.contains("Target sort time must be > 0.1ms"));
	CHECK(errors.contains("Max sort elements must be > 1000"));
	CHECK(errors.contains("Radix bits must be 4 or 8"));
	CHECK(errors.contains("Workgroup size must be 64, 128, 256, or 512"));
	CHECK(errors.contains("Key bits must be 32 or 64"));
	CHECK(errors.contains("Performance log interval must be > 0"));
}

TEST_CASE("[GaussianSplatting][Config] PipelineFeatureSet default values pass validation") {
	PipelineFeatureSet config;
	config.reset_to_defaults();

	CHECK(config.validate());
	CHECK(config.get_validation_errors().is_empty());
}

TEST_CASE("[GaussianSplatting][Config] PipelineFeatureSet validates SH amortization settings only when active") {
	PipelineFeatureSet config;
	config.reset_to_defaults();

	SUBCASE("Inactive SH amortization tolerates stale divisor values") {
		config.sh_amortization_divisor = 0;
		CHECK(config.validate());
	}

	SUBCASE("Divisor must be greater than one when the feature is active") {
		config.enable_sh_amortization = true;
		config.sh_amortization_divisor = 1;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("SH amortization divisor must be > 1."));
	}

	SUBCASE("Experimental bundle inherits SH amortization validation") {
		config.enable_all_pipeline_experimental = true;
		config.sh_amortization_divisor = 1;
		CHECK_FALSE(config.validate());
		CHECK(config.get_validation_errors().contains("SH amortization divisor must be > 1."));
	}
}

TEST_CASE("[GaussianSplatting][Config] PipelineFeatureSet validates packed stage limits when scene size is known") {
	PipelineFeatureSet config;
	config.reset_to_defaults();

	SUBCASE("Packed stage accepts unknown scene size") {
		config.enable_packed_stage_data = true;
		CHECK(config.validate());
	}

	SUBCASE("Packed stage accepts scenes within the 16-bit index budget") {
		config.enable_packed_stage_data = true;
		CHECK(config.validate(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS));
	}

	SUBCASE("Packed stage rejects oversized scenes") {
		config.enable_packed_stage_data = true;
		CHECK_FALSE(config.validate(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1));
		CHECK(config.get_validation_errors(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1)
				.contains("Packed stage data requires <="));
	}

	SUBCASE("Experimental bundle inherits packed stage limits") {
		config.enable_all_pipeline_experimental = true;
		CHECK_FALSE(config.validate(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1));
		CHECK(config.get_validation_errors(PipelineFeatureSet::PACKED_STAGE_MAX_TOTAL_SPLATS + 1)
				.contains("Packed stage data requires <="));
	}
}

// =============================================================================
// SortingStrategyConfig Sanitize Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] SortingStrategyConfig sanitize corrects invalid values") {
	SortingStrategyConfig config;

	SUBCASE("Zero bitonic_max_elements corrected to 1") {
		config.bitonic_max_elements = 0;
		config.sanitize();
		CHECK(config.bitonic_max_elements == 1);
	}

	SUBCASE("radix_max_elements enforced >= bitonic_max_elements") {
		config.bitonic_max_elements = 10000;
		config.radix_max_elements = 5000; // Less than bitonic
		config.sanitize();
		CHECK(config.radix_max_elements >= config.bitonic_max_elements);
	}

	SUBCASE("Zero history_size defaults to 120") {
		config.history_size = 0;
		config.sanitize();
		CHECK(config.history_size == 120);
	}

	SUBCASE("Zero log_interval_frames defaults to 60") {
		config.log_interval_frames = 0;
		config.sanitize();
		CHECK(config.log_interval_frames == 60);
	}

	SUBCASE("Negative target_sort_time_ms clamped to 0") {
		config.target_sort_time_ms = -5.0f;
		config.sanitize();
		CHECK(config.target_sort_time_ms == 0.0f);
	}
}

TEST_CASE("[GaussianSplatting][Config] SortingStrategyConfig describe_thresholds format") {
	SortingStrategyConfig config;
	config.bitonic_max_elements = 131072;
	config.radix_max_elements = 1500000;

	String description = config.describe_thresholds();

	CHECK(description.contains("131072"));
	CHECK(description.contains("1500000"));
}

// =============================================================================
// SortKeyConfig Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] SortKeyConfig default values") {
	SortKeyConfig config;

	CHECK(config.key_bits == 64);
	CHECK(config.tile_bits == 32);
	CHECK(config.depth_bits == 32);
	CHECK(config.enable_tie_breaker == false);
}

TEST_CASE("[GaussianSplatting][Config] SortKeyConfig bit allocation consistency") {
	SortKeyConfig config;

	SUBCASE("Default allocation fits in key") {
		CHECK(config.tile_bits + config.depth_bits <= config.key_bits);
	}

	SUBCASE("32-bit key allocation") {
		config.key_bits = 32;
		config.tile_bits = 16;
		config.depth_bits = 16;
		CHECK(config.tile_bits + config.depth_bits == config.key_bits);
	}
}

// =============================================================================
// Live LODConfig validation
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] LODConfig calculate_lod_level handles disabled and near-zero distances") {
	LODConfig config;
	config.reset_to_defaults();
	config.enabled = false;

	CHECK(config.calculate_lod_level(0.0f) == 0);
	CHECK(config.calculate_lod_level(25.0f) == 0);
	CHECK(config.calculate_lod_level(100.0f) == 0);

	config.enabled = true;

	CHECK(config.calculate_lod_level(0.0f) == 0);
	CHECK(config.calculate_lod_level(0.0001f) == 0);
	CHECK(config.calculate_lod_level(0.001f) == 0);
	CHECK(config.calculate_lod_level(12.5f) == 0);
}

TEST_CASE("[GaussianSplatting][Config] LODConfig calculate_lod_level boundary mapping is explicit") {
	LODConfig config;
	config.reset_to_defaults();
	config.enabled = true;
	config.num_levels = 4;
	config.max_distance = 100.0f;
	config.base_threshold = 10.0f;

	CHECK(config.calculate_lod_level(24.9999f) == 0);
	CHECK_MESSAGE(config.calculate_lod_level(25.0f) == 1,
			"Exact 25.0 enters LOD 1 because calculate_lod_level() is driven by max_distance/num_levels.");
	CHECK(config.calculate_lod_level(25.0001f) == 1);
	CHECK(config.calculate_lod_level(49.9999f) == 1);
	CHECK_MESSAGE(config.calculate_lod_level(50.0f) == 2,
			"Exact 50.0 enters LOD 2 under the current live floor/clamp mapping.");
	CHECK(config.calculate_lod_level(50.0001f) == 2);
	CHECK(config.calculate_lod_level(99.9999f) == 2);
	CHECK_MESSAGE(config.calculate_lod_level(100.0f) == 3,
			"Exact max_distance lands on the farthest LOD level in the live implementation.");
	CHECK(config.calculate_lod_level(100.0001f) == 3);
	CHECK(config.calculate_lod_level(1000.0f) == 3);
}

TEST_CASE("[GaussianSplatting][Config] LODConfig distance thresholds follow base-threshold progression") {
	LODConfig config;
	config.reset_to_defaults();
	config.base_threshold = 10.0f;
	config.max_distance = 100.0f;

	CHECK(config.get_distance_threshold(-1) == doctest::Approx(10.0f));
	CHECK(config.get_distance_threshold(0) == doctest::Approx(10.0f));
	CHECK(config.get_distance_threshold(1) == doctest::Approx(20.0f));
	CHECK(config.get_distance_threshold(2) == doctest::Approx(40.0f));
	CHECK(config.get_distance_threshold(3) == doctest::Approx(80.0f));
	CHECK_MESSAGE(config.get_distance_threshold(4) == doctest::Approx(100.0f),
			"Distance thresholds double from base_threshold and clamp at max_distance.");
}

TEST_CASE("[GaussianSplatting][Config] LODConfig helper mappings match the live implementation") {
	LODConfig config;
	config.reset_to_defaults();
	config.base_threshold = 10.0f;
	config.max_distance = 100.0f;

	CHECK(config.get_splat_skip_factor(0) == 1);
	CHECK(config.get_splat_skip_factor(1) == 2);
	CHECK(config.get_splat_skip_factor(2) == 4);
	CHECK(config.get_splat_skip_factor(3) == 8);

	config.splat_skip_enabled = false;
	CHECK(config.get_splat_skip_factor(3) == 1);
	config.splat_skip_enabled = true;

	CHECK(config.get_sh_band_for_lod(0) == 3);
	CHECK(config.get_sh_band_for_lod(1) == 2);
	CHECK(config.get_sh_band_for_lod(2) == 1);
	CHECK(config.get_sh_band_for_lod(3) == 0);
	CHECK(config.get_sh_band_for_lod(4) == 0);

	config.sh_reduction_enabled = false;
	CHECK(config.get_sh_band_for_lod(3) == 3);
	config.sh_reduction_enabled = true;

	CHECK(config.get_opacity_multiplier(0.0f) == doctest::Approx(1.0f));
	CHECK(config.get_opacity_multiplier(10.0f) == doctest::Approx(1.0f));
	CHECK(config.get_opacity_multiplier(55.0f) == doctest::Approx(0.5f));
	CHECK(config.get_opacity_multiplier(100.0f) == doctest::Approx(0.0f));
	CHECK(config.get_opacity_multiplier(120.0f) == doctest::Approx(0.0f));

	config.opacity_fade_enabled = false;
	CHECK(config.get_opacity_multiplier(55.0f) == doctest::Approx(1.0f));
}

// =============================================================================
// Node-facing LOD/Streaming config validation
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] GaussianSplatLODConfig defaults match live node expectations") {
	using namespace GaussianSplatting;
	GaussianSplatLODConfig config;

	CHECK(config.lod0_distance < config.lod1_distance);
	CHECK(config.lod1_distance < config.lod2_distance);
	CHECK(config.lod2_distance < config.lod3_distance);
	CHECK(config.lod3_distance < config.cull_distance);
	CHECK(config.min_splats_per_frame < config.max_splats_per_frame);
	CHECK(config.importance_threshold >= 0.0f);
	CHECK(config.importance_threshold <= 1.0f);
	CHECK(config.size_cull_threshold > 0.0f);
	CHECK(config.lod_bias > 0.0f);
}

TEST_CASE("[GaussianSplatting][Config] GaussianSplatStreamingConfig defaults match live node expectations") {
	using namespace GaussianSplatting;
	GaussianSplatStreamingConfig config;

	CHECK(config.max_gpu_memory > 0);
	CHECK(config.target_gpu_memory > 0);
	CHECK(config.target_gpu_memory <= config.max_gpu_memory);
	CHECK(config.max_cpu_memory >= config.max_gpu_memory);
	CHECK(config.load_ahead_distance > 0.0f);
	CHECK(config.unload_distance > config.load_ahead_distance);
	CHECK(config.max_concurrent_loads > 0);
	CHECK(config.num_lod_levels >= 2);
	CHECK(config.stream_budget_ms > 0);
}

// =============================================================================
// CullingConfig Validation Tests (GPUCuller::CullingConfig)
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] CullingConfig default values are sensible") {
	GPUCuller::CullingConfig config;

	// Boolean defaults
	CHECK(config.lod_enabled == true);
	CHECK(config.frustum_culling == true);
	CHECK(config.gpu_culling_enabled == true);
	CHECK(config.temporal_coherence == true);

	// Numeric defaults should be positive where expected
	CHECK(config.lod_bias > 0.0f);
	CHECK(config.lod_min_screen_size > 0.0f);
	CHECK(config.lod_max_distance > 0.0f);
	CHECK(config.cull_radius_multiplier > 0.0f);
	CHECK(config.cull_frustum_plane_slack >= 0.0f);
}

TEST_CASE("[GaussianSplatting][Config] CullingConfig tolerance values") {
	GPUCuller::CullingConfig config;

	// Tolerances should be small positive values
	CHECK(config.cull_near_tolerance >= 0.0f);
	CHECK(config.cull_near_tolerance <= 1.0f);
	CHECK(config.cull_far_tolerance >= 0.0f);
	CHECK(config.cull_far_tolerance <= 1.0f);
}

TEST_CASE("[GaussianSplatting][Config] CullingConfig viewport size") {
	GPUCuller::CullingConfig config;

	// Default viewport size should be valid
	CHECK(config.last_cull_viewport_size.x > 0);
	CHECK(config.last_cull_viewport_size.y > 0);
}

// Issue #167 (settings-hygiene slice 3): three culling globals that were
// registered but never read now act as the PROJECT-WIDE DEFAULT behind their
// live per-renderer cull/* properties. update_culling_settings() seeds the
// member from the global unless an explicit per-node override was set (which
// still wins). These tests prove neutrality, the global-as-default flow, the
// override precedence, and that enabling the runtime overflow auto-tuner via
// the global does not reset the tuned importance threshold on later reloads.
TEST_CASE("[GaussianSplatting][Config][Cull] Misowned culling globals default the per-renderer cull properties (#167)") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	const String opacity_key = "rendering/gaussian_splatting/culling/opacity_aware_bounds";
	const String visibility_key = "rendering/gaussian_splatting/culling/visibility_threshold";
	const String autotune_key = "rendering/gaussian_splatting/cull/overflow_autotune_enabled";
	const String importance_key = "rendering/gaussian_splatting/lod/importance_threshold";
	ProjectSettingGuard opacity_guard(ps, opacity_key);
	ProjectSettingGuard visibility_guard(ps, visibility_key);
	ProjectSettingGuard autotune_guard(ps, autotune_key);
	ProjectSettingGuard importance_guard(ps, importance_key);

	// The construction defaults the wiring must stay neutral against.
	const GPUCuller::CullingConfig defaults;

	SUBCASE("Neutrality: registered global defaults resolve to today's construction defaults") {
		ps->set_setting(opacity_key, true);
		ps->set_setting(visibility_key, gs::RASTER_ALPHA_THRESHOLD);
		ps->set_setting(autotune_key, false);

		Ref<GPUCuller> culler;
		culler.instantiate();
		culler->update_culling_settings();

		CHECK(culler->get_config().opacity_aware_culling == defaults.opacity_aware_culling); // true
		CHECK(culler->get_config().visibility_threshold == doctest::Approx(defaults.visibility_threshold)); // 1/255
		CHECK(culler->get_state().overflow_autotune_enabled == false);
	}

	SUBCASE("A project-wide global seeds a fresh renderer's default") {
		ps->set_setting(opacity_key, false);
		ps->set_setting(visibility_key, 0.05f);
		ps->set_setting(autotune_key, true);

		Ref<GPUCuller> culler;
		culler.instantiate();
		culler->update_culling_settings();

		CHECK(culler->get_config().opacity_aware_culling == false);
		CHECK(culler->get_config().visibility_threshold == doctest::Approx(0.05f));
		CHECK(culler->get_state().overflow_autotune_enabled == true);
	}

	SUBCASE("An explicit per-renderer override wins over the global default") {
		ps->set_setting(opacity_key, false);
		ps->set_setting(visibility_key, 0.05f);
		ps->set_setting(autotune_key, true);

		Ref<GPUCuller> culler;
		culler.instantiate();
		// Simulate explicit per-node property assignments: the setters
		// (set_opacity_aware_culling / set_visibility_threshold /
		// set_overflow_autotune_enabled) mark exactly these override flags.
		culler->get_config().opacity_aware_culling = true;
		culler->get_config().opacity_aware_culling_override = true;
		culler->get_config().visibility_threshold = 0.02f;
		culler->get_config().visibility_threshold_override = true;
		culler->get_state().overflow_autotune_enabled = false;
		culler->get_state().overflow_autotune_override = true;

		culler->update_culling_settings();

		CHECK(culler->get_config().opacity_aware_culling == true); // override, not the false global
		CHECK(culler->get_config().visibility_threshold == doctest::Approx(0.02f));
		CHECK(culler->get_state().overflow_autotune_enabled == false); // override, not the true global
	}

	SUBCASE("Overflow autotune enabled via the global does not reset the runtime-tuned importance threshold") {
		// overflow_autotune gates a runtime auto-tuner that mutates
		// importance_cull_threshold. With autotune enabled project-wide (no
		// per-node override) and lod/importance_threshold at its -1 "auto"
		// sentinel, a later settings reload must leave the tuned value intact.
		ps->set_setting(autotune_key, true);
		ps->set_setting(importance_key, -1.0f); // slice-1 sentinel = auto

		Ref<GPUCuller> culler;
		culler.instantiate();
		culler->update_culling_settings();
		REQUIRE(culler->get_state().overflow_autotune_enabled == true);

		// The auto-tuner raises the threshold at runtime; a subsequent reload
		// must NOT clobber it back to a fixed default.
		culler->get_config().importance_cull_threshold = 0.017f;
		culler->update_culling_settings();
		CHECK(culler->get_config().importance_cull_threshold == doctest::Approx(0.017f));
		CHECK(culler->get_state().overflow_autotune_enabled == true);
	}
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig edge case: maximum valid values") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	// Test maximum reasonable values
	config.target_sort_time_ms = 1000.0f; // 1 second
	config.max_sort_elements = 500000000;
	config.performance_log_interval = UINT32_MAX;

	CHECK(config.validate());
}

TEST_CASE("[GaussianSplatting][Config] GPUSortingConfig edge case: minimum valid values") {
	GPUSortingConfig config;
	config.reset_to_defaults();

	// Test minimum valid values
	config.target_sort_time_ms = 0.11f; // Just above 0.1
	config.max_sort_elements = 1001; // Just above 1000
	config.radix_bits = 4;
	config.workgroup_size = 64; // Smallest valid
	config.key_bits = 32; // Smallest valid
	config.tile_bits = 1;
	config.depth_bits = 1;
	config.performance_log_interval = 1;

	CHECK(config.validate());
}

TEST_CASE("[GaussianSplatting][Config] SortingStrategyConfig cascading sanitization") {
	SortingStrategyConfig config;

	// Set unreasonable ordering that should be corrected
	config.bitonic_max_elements = 1000000; // Very large bitonic
	config.radix_max_elements = 100;       // Small radix

	config.sanitize();

	// After sanitization, ordering should be enforced
	CHECK(config.radix_max_elements >= config.bitonic_max_elements);
}

// =============================================================================
// AUTO sorting-algorithm threshold wiring (#168)
//
// The AUTO band boundaries used to be hard-coded constants in gpu_sorter.cpp
// (32768 / 1048576). They are now resolved from the sorting/{bitonic,radix}
// _max_elements project settings via GPUSorterFactory::AutoThresholds. These
// tests prove (a) the defaults reproduce the historical selection EXACTLY
// (neutrality guard) and (b) tuning a setting actually changes the selected
// algorithm (the knob is live). evaluate_auto_policy is pure given the probes
// and thresholds, so no RenderingDevice is required.
// =============================================================================

namespace {
// All algorithms reported fully capable so that the AUTO decision's preferred
// algorithm is also the selected algorithm (no capability fallback), isolating
// the element-count band logic under test.
static GPUSorterFactory::PolicyProbe _fully_supported_probe() {
	GPUSorterFactory::PolicyProbe probe;
	probe.supported = true;
	probe.supports_indirect = true;
	probe.supports_64bit_keys = true;
	return probe;
}

// A 32-bit, non-stable, no-tie-break key so the "force RADIX" early-out does not
// fire and the pure element-count bands are exercised.
static SortKeyConfig _band_test_key_config() {
	SortKeyConfig key_cfg;
	key_cfg.key_bits = 32;
	key_cfg.tile_bits = 16;
	key_cfg.depth_bits = 16;
	key_cfg.enable_tie_breaker = false;
	key_cfg.require_stable = false;
	return key_cfg;
}
} // namespace

TEST_CASE("[GaussianSplatting][Config] AUTO sorting thresholds default mapping reproduces historical bands") {
	// The SortingStrategyConfig defaults map onto the historical hard-coded AUTO
	// boundaries exactly: bitonic_max_elements -> bitonic->radix boundary,
	// radix_max_elements -> radix->onesweep boundary.
	SortingStrategyConfig config;
	config.sanitize();
	CHECK(config.bitonic_max_elements == GPUSorterFactory::AUTO_BITONIC_MAX_ELEMENTS);
	CHECK(config.radix_max_elements == GPUSorterFactory::AUTO_ONESWEEP_MIN_ELEMENTS);

	// The AutoThresholds struct defaults are the historical constants.
	GPUSorterFactory::AutoThresholds thresholds;
	CHECK(thresholds.bitonic_max_elements == 32768u);
	CHECK(thresholds.onesweep_min_elements == 1048576u);

	const GPUSorterFactory::PolicyProbe probe = _fully_supported_probe();
	const SortKeyConfig key_cfg = _band_test_key_config();
	auto selected = [&](uint32_t count) {
		return GPUSorterFactory::evaluate_auto_policy(count, key_cfg, probe, probe, probe, false, false, thresholds)
				.selected_algorithm;
	};

	// Representative counts select the SAME algorithm as the pre-#168 constants.
	CHECK(selected(10000) == GPUSorterFactory::ALGORITHM_BITONIC);   // small -> bitonic
	CHECK(selected(100000) == GPUSorterFactory::ALGORITHM_RADIX);    // mid   -> radix
	CHECK(selected(5000000) == GPUSorterFactory::ALGORITHM_ONESWEEP); // large -> onesweep

	// Exact boundary behavior (<= bitonic_max -> BITONIC; >= onesweep_min -> ONESWEEP).
	CHECK(selected(32768) == GPUSorterFactory::ALGORITHM_BITONIC);
	CHECK(selected(32769) == GPUSorterFactory::ALGORITHM_RADIX);
	CHECK(selected(1048575) == GPUSorterFactory::ALGORITHM_RADIX);
	CHECK(selected(1048576) == GPUSorterFactory::ALGORITHM_ONESWEEP);

	// The default-argument overload (callers that pass no thresholds) must match
	// the explicit historical-default thresholds: the built-in fallback is neutral.
	CHECK(GPUSorterFactory::evaluate_auto_policy(10000, key_cfg, probe, probe, probe, false, false).selected_algorithm ==
			GPUSorterFactory::ALGORITHM_BITONIC);
	CHECK(GPUSorterFactory::evaluate_auto_policy(100000, key_cfg, probe, probe, probe, false, false).selected_algorithm ==
			GPUSorterFactory::ALGORITHM_RADIX);
	CHECK(GPUSorterFactory::evaluate_auto_policy(5000000, key_cfg, probe, probe, probe, false, false).selected_algorithm ==
			GPUSorterFactory::ALGORITHM_ONESWEEP);
}

TEST_CASE("[GaussianSplatting][Config] AUTO sorting thresholds are tunable and resolve from live project settings") {
	const GPUSorterFactory::PolicyProbe probe = _fully_supported_probe();
	const SortKeyConfig key_cfg = _band_test_key_config();

	SUBCASE("Explicit thresholds drive selection (knob is live)") {
		const uint32_t count = 20000; // BITONIC under defaults (<= 32768)
		GPUSorterFactory::AutoThresholds defaults;
		CHECK(GPUSorterFactory::evaluate_auto_policy(count, key_cfg, probe, probe, probe, false, false, defaults)
						.selected_algorithm == GPUSorterFactory::ALGORITHM_BITONIC);

		// Lowering the bitonic boundary below the count flips BITONIC -> RADIX.
		GPUSorterFactory::AutoThresholds tuned = defaults;
		tuned.bitonic_max_elements = 10000;
		CHECK(GPUSorterFactory::evaluate_auto_policy(count, key_cfg, probe, probe, probe, false, false, tuned)
						.selected_algorithm == GPUSorterFactory::ALGORITHM_RADIX);

		// Additionally lowering the onesweep boundary below the count flips to ONESWEEP.
		tuned.onesweep_min_elements = 15000;
		CHECK(GPUSorterFactory::evaluate_auto_policy(count, key_cfg, probe, probe, probe, false, false, tuned)
						.selected_algorithm == GPUSorterFactory::ALGORITHM_ONESWEEP);
	}

	SUBCASE("from_project_settings maps the two boundary settings end-to-end") {
		ProjectSettings *project_settings = ProjectSettings::get_singleton();
		REQUIRE(project_settings != nullptr);

		const String bitonic_path = "rendering/gaussian_splatting/sorting/bitonic_max_elements";
		const String radix_path = "rendering/gaussian_splatting/sorting/radix_max_elements";
		ProjectSettingGuard bitonic_guard(project_settings, bitonic_path);
		ProjectSettingGuard radix_guard(project_settings, radix_path);

		// Ensure the SortingStrategyConfig cache-invalidation callback is connected
		// (it connects lazily on the first load); otherwise the settings_changed
		// emit below is a no-op and the cached config stays stale.
		SortingStrategyConfig::load_from_project_settings();

		// Non-default, well-ordered boundaries (survive SortingStrategyConfig::sanitize).
		project_settings->set_setting(bitonic_path, 10000);
		project_settings->set_setting(radix_path, 40000);
		project_settings->save();
		// set_setting alone does not invalidate the cached sort config in the test
		// harness; emit settings_changed so from_project_settings re-reads (matches
		// the idiom used by the target_sort_time round-trip tests above).
		project_settings->emit_signal("settings_changed");

		// bitonic_max_elements -> bitonic band; radix_max_elements -> onesweep band.
		const GPUSorterFactory::AutoThresholds resolved = GPUSorterFactory::AutoThresholds::from_project_settings();
		CHECK(resolved.bitonic_max_elements == 10000u);
		CHECK(resolved.onesweep_min_elements == 40000u);

		// The same count selects a DIFFERENT algorithm under the tuned settings than
		// under the historical defaults, proving the setting is now live end-to-end.
		const uint32_t count = 60000; // RADIX under defaults (32768 < 60000 < 1048576)
		CHECK(GPUSorterFactory::evaluate_auto_policy(count, key_cfg, probe, probe, probe, false, false,
						GPUSorterFactory::AutoThresholds())
						.selected_algorithm == GPUSorterFactory::ALGORITHM_RADIX);
		CHECK(GPUSorterFactory::evaluate_auto_policy(count, key_cfg, probe, probe, probe, false, false, resolved)
						.selected_algorithm == GPUSorterFactory::ALGORITHM_ONESWEEP);
	}
}

// C4a ("no silent degradation"): the opt-in 32-bit sort-key path must be counted
// when it executes. SortingMetricsCollector::record_sort increments total_32bit_sorts
// only for key_bits == 32; the 64-bit shippable path must never move the counter.
// (The WARN_PRINT_ONCE fires once and is not the assertable signal — the counter is.)
TEST_CASE("[GaussianSplatting][Config][Sort] 32-bit engage increments counter") {
	SortingMetricsCollector collector;

	// key_bits == 32 -> degraded opt-in path executed: counter increments.
	collector.record_sort(1000u, 0.5f, true, 32u, 4u, 16u, false, false, true);
	CHECK_EQ(collector.get_metrics().total_32bit_sorts, 1u);

	// key_bits == 64 -> shippable path: counter must stay put.
	collector.record_sort(1000u, 0.5f, true, 64u, 4u, 16u, false, false, true);
	CHECK_EQ(collector.get_metrics().total_32bit_sorts, 1u);
	// total_sorts still tracks every sort regardless of width.
	CHECK_EQ(collector.get_metrics().total_sorts, 2u);
}

} // namespace TestConfigValidation
