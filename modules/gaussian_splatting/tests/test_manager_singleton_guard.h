#pragma once

// Regression test for issue #514: GaussianSplatManager::~GaussianSplatManager()
// used to do `singleton = nullptr;` unconditionally. GaussianSplatManager is
// script-instantiable (GDREGISTER_CLASS, register_types.cpp), and its
// constructor already guards against a second instance stealing the
// singleton slot (ERR_FAIL_COND_MSG(singleton != nullptr, ...) at
// gaussian_splat_manager.cpp:230 leaves `singleton` pointing at the first
// instance and returns early from the second instance's constructor body).
// Without a matching guard in the destructor, freeing that second,
// never-really-initialized instance would still null out the *real* engine
// singleton, breaking every other module subsystem that calls
// GaussianSplatManager::get_singleton() (e.g. GaussianSplatSceneDirector).
//
// The fix mirrors GaussianSplatSceneDirector's destructor
// (`if (singleton == this) { singleton = nullptr; }`,
// gaussian_splat_scene_director.cpp:331-333).

#include "test_macros.h"

#include "../core/gaussian_splat_manager.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

// Headless: no RenderingDevice/SceneTree required. Constructing a second
// GaussianSplatManager while one already exists only runs
// GS_STARTUP_SCOPE + the ERR_FAIL_COND_MSG guard before returning, so no GPU
// device/resource setup happens on the throwaway instance.
TEST_CASE("[GaussianSplatting][Manager] freeing a second instance does not clobber the live singleton") {
	GaussianSplatManager *original = GaussianSplatManager::get_singleton();
	const bool owns_original = (original == nullptr);
	if (!original) {
		original = memnew(GaussianSplatManager);
	}
	REQUIRE(original != nullptr);
	REQUIRE(GaussianSplatManager::get_singleton() == original);

	// Constructing a second instance while `original` is the singleton hits
	// the constructor's ERR_FAIL_COND_MSG guard (expected error print,
	// suppressed here) and must not change which instance is the singleton.
	ERR_PRINT_OFF;
	GaussianSplatManager *second = memnew(GaussianSplatManager);
	ERR_PRINT_ON;
	REQUIRE(second != nullptr);
	CHECK(GaussianSplatManager::get_singleton() == original);

	// The regression: destroying the second (non-singleton) instance must
	// leave the real singleton untouched.
	memdelete(second);
	CHECK(GaussianSplatManager::get_singleton() == original);

	if (owns_original) {
		memdelete(original);
	}
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
