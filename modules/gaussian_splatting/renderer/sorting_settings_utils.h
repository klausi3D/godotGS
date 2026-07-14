#ifndef GS_SORTING_SETTINGS_UTILS_H
#define GS_SORTING_SETTINGS_UTILS_H

#include "core/config/project_settings.h"
#include "../core/gs_project_settings.h"
#include "../core/module_string_names.h"

#include "core/error/error_macros.h"
#include "core/string/string_name.h"

namespace gs {
namespace sorting_settings {

// The setting paths are cached in the module-owned StringName struct so
// they can be released at module unregister; without this the
// function-local statics would surface in the engine's exit-time orphan
// StringName report. The fallback path constructs an on-demand
// StringName for early callers (test fixtures) before
// initialize_gaussian_splatting_module() runs.
//
// The sort-time target is a telemetry-only logging target (#168): it is the
// `meets_target` reference in the GPU-sort perf log and is reported as
// `target_ms`; it is NOT adaptive and does not steer any sort decision. The
// canonical key therefore lives under diagnostics/. Two older spellings are
// accepted as read-only DEPRECATED ALIASES so existing project.godot files keep
// working: precedence is diagnostics/ (canonical) > sorting/ > gpu_sorting/.
static inline const StringName &canonical_sort_target_time_path() {
	return gs::get_module_string_name_or_construct(
			&gs::ModuleStringNames::diagnostics_sort_target_time_path,
			"rendering/gaussian_splatting/diagnostics/sort_target_time_ms");
}

// Deprecated alias (renamed to diagnostics/sort_target_time_ms in #168). Read
// as a fallback only; kept for project compatibility.
static inline const StringName &deprecated_sorting_target_sort_time_path() {
	return gs::get_module_string_name_or_construct(
			&gs::ModuleStringNames::sorting_target_sort_time_path,
			"rendering/gaussian_splatting/sorting/target_sort_time_ms");
}

// Older deprecated alias (predates the sorting/ spelling). Read as a fallback
// only; kept for project compatibility.
static inline const StringName &legacy_gpu_target_sort_time_path() {
	return gs::get_module_string_name_or_construct(
			&gs::ModuleStringNames::legacy_gpu_sorting_target_sort_time_path,
			"rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms");
}

static inline bool has_explicit_target_sort_time_override(ProjectSettings *p_ps) {
	if (!p_ps) {
		return false;
	}
	if (!p_ps->has_setting(String(canonical_sort_target_time_path()))) {
		return false;
	}
	if (!p_ps->is_builtin_setting(String(canonical_sort_target_time_path()))) {
		return true;
	}
	if (!p_ps->property_can_revert(canonical_sort_target_time_path())) {
		return true;
	}
	return p_ps->get_setting_with_override(canonical_sort_target_time_path()) != p_ps->property_get_revert(canonical_sort_target_time_path());
}

static inline void register_canonical_target_sort_time_setting(ProjectSettings *p_ps, float p_default_value) {
	const bool had_project_target_override = p_ps && p_ps->has_setting(String(canonical_sort_target_time_path())) &&
			!p_ps->is_builtin_setting(String(canonical_sort_target_time_path()));
	const int prior_target_order = had_project_target_override ? p_ps->get_order(canonical_sort_target_time_path()) : -1;
	GLOBAL_DEF(String(canonical_sort_target_time_path()), p_default_value);
	if (had_project_target_override) {
		p_ps->set_order(canonical_sort_target_time_path(), prior_target_order);
	}
}

static inline float get_target_sort_time_ms(ProjectSettings *p_ps, float p_fallback) {
	if (!p_ps) {
		return p_fallback;
	}
	// 1. Explicit canonical (diagnostics/) override always wins.
	if (has_explicit_target_sort_time_override(p_ps)) {
		return gs::settings::get_float(p_ps, canonical_sort_target_time_path(), p_fallback);
	}
	// 2. Deprecated alias sorting/target_sort_time_ms (renamed in #168).
	if (p_ps->has_setting(deprecated_sorting_target_sort_time_path())) {
		WARN_PRINT_ONCE(vformat("[GaussianSplatting] Project setting '%s' is deprecated; use '%s' instead. Deprecated alias support is read-only compatibility and will be removed after project migration.",
				String(deprecated_sorting_target_sort_time_path()),
				String(canonical_sort_target_time_path())));
		return gs::settings::get_float(p_ps, deprecated_sorting_target_sort_time_path(), p_fallback);
	}
	// 3. Older deprecated alias gpu_sorting/target_sort_time_ms.
	if (p_ps->has_setting(legacy_gpu_target_sort_time_path())) {
		WARN_PRINT_ONCE(vformat("[GaussianSplatting] Project setting '%s' is deprecated; use '%s' instead. Deprecated alias support is read-only compatibility and will be removed after project migration.",
				String(legacy_gpu_target_sort_time_path()),
				String(canonical_sort_target_time_path())));
		return gs::settings::get_float(p_ps, legacy_gpu_target_sort_time_path(), p_fallback);
	}
	// 4. Canonical registered default (builtin) or caller fallback.
	if (p_ps->has_setting(String(canonical_sort_target_time_path()))) {
		return gs::settings::get_float(p_ps, canonical_sort_target_time_path(), p_fallback);
	}
	return p_fallback;
}

} // namespace sorting_settings
} // namespace gs

#endif // GS_SORTING_SETTINGS_UTILS_H
