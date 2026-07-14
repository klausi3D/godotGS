/**************************************************************************/
/*  module_string_names.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

/*
 * Module-owned StringNames whose lifetime must straddle module
 * register/unregister so the engine's exit-time orphan StringName report
 * (StringName::cleanup) does not surface them.
 *
 * Function-local `static const StringName s("...")` cache sites are easy
 * to write but cannot be released — the StringName lives in static
 * storage until program exit, which means its slot in the StringName
 * table is occupied past the module's unregister window. Anything that
 * references those slots from the other side (e.g. a Dictionary that has
 * already been freed) shows up as `Orphan StringName: ... (total: N)`.
 *
 * This struct holds the small set of paths that were previously cached
 * via function-local statics. register_types.cpp::
 * initialize_gaussian_splatting_module() constructs the struct; the
 * matching uninitialize() call drops it, releasing the StringNames
 * cleanly so they leave the StringName table at unregister.
 *
 * See tests/ci/run_module_tests.py::_run_stringname_orphan_guard_step
 * for the CI guard that asserts the orphan count stays bounded.
 */

#ifndef GAUSSIAN_SPLATTING_MODULE_STRING_NAMES_H
#define GAUSSIAN_SPLATTING_MODULE_STRING_NAMES_H

#include "core/string/string_name.h"

namespace gs {

struct ModuleStringNames {
	// gaussian_splat_manager.cpp — project-setting key for RenderDoc compat.
	StringName renderdoc_compatibility_path;

	// renderer/sorting_settings_utils.h — canonical and deprecated-alias
	// sort-target project-setting keys. The canonical key is
	// diagnostics/sort_target_time_ms (#168); the two older spellings
	// (sorting/target_sort_time_ms and gpu_sorting/target_sort_time_ms) are
	// retained as read-only deprecated aliases for project compatibility.
	StringName diagnostics_sort_target_time_path;
	StringName sorting_target_sort_time_path;
	StringName legacy_gpu_sorting_target_sort_time_path;

	void initialize();
	void release();
};

// Returns the module-owned StringName cache. Returns nullptr before
// initialize_gaussian_splatting_module() runs and after
// uninitialize_gaussian_splatting_module() runs; callers must check or
// fall back to constructing a StringName on demand.
ModuleStringNames *get_module_string_names();

// Returns a const reference to a StringName held by the module-owned
// cache, or constructs one on demand if the cache is not yet
// initialized. Used by inline accessors in header files where pulling
// the cache singleton dependency in is undesirable.
const StringName &get_module_string_name_or_construct(
		StringName ModuleStringNames::*member, const char *p_literal);

// initialize_module_string_names()/release_module_string_names() are
// invoked from register_types.cpp around the module lifecycle. Safe to
// call release before any initialize (no-op).
void initialize_module_string_names();
void release_module_string_names();

} // namespace gs

#endif // GAUSSIAN_SPLATTING_MODULE_STRING_NAMES_H
