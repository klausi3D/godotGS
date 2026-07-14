/**************************************************************************/
/*  module_string_names.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "module_string_names.h"

namespace gs {

namespace {
ModuleStringNames *g_module_string_names = nullptr;
} // namespace

void ModuleStringNames::initialize() {
	renderdoc_compatibility_path = StringName("rendering/gaussian_splatting/renderdoc_compatibility");
	diagnostics_sort_target_time_path = StringName("rendering/gaussian_splatting/diagnostics/sort_target_time_ms");
	sorting_target_sort_time_path = StringName("rendering/gaussian_splatting/sorting/target_sort_time_ms");
	legacy_gpu_sorting_target_sort_time_path = StringName("rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms");
}

void ModuleStringNames::release() {
	// Assigning a default-constructed StringName drops the ref so the
	// underlying slot leaves the StringName table when no other holder
	// remains. Without this the cached StringNames would live in static
	// storage until program exit and surface as orphans at cleanup time.
	renderdoc_compatibility_path = StringName();
	diagnostics_sort_target_time_path = StringName();
	sorting_target_sort_time_path = StringName();
	legacy_gpu_sorting_target_sort_time_path = StringName();
}

ModuleStringNames *get_module_string_names() {
	return g_module_string_names;
}

const StringName &get_module_string_name_or_construct(
		StringName ModuleStringNames::*member, const char *p_literal) {
	if (g_module_string_names) {
		return g_module_string_names->*member;
	}
	// Pre-initialize / post-release fallback. Returns a function-local
	// static so the StringName has stable storage across the call. This
	// path runs only before initialize_gaussian_splatting_module() or
	// after uninitialize, neither of which should hit hot rendering code.
	static StringName fallback;
	fallback = StringName(p_literal);
	return fallback;
}

void initialize_module_string_names() {
	if (g_module_string_names != nullptr) {
		return;
	}
	g_module_string_names = new ModuleStringNames();
	g_module_string_names->initialize();
}

void release_module_string_names() {
	if (g_module_string_names == nullptr) {
		return;
	}
	g_module_string_names->release();
	delete g_module_string_names;
	g_module_string_names = nullptr;
}

} // namespace gs
