#pragma once

// Wave 3 node-surface cleanup pins:
//   - GaussianSplatNode3D no longer exposes ply_file_path / auto_load.
//   - rendering/gaussian_splatting/streaming/route_policy is world-only. The
//     settings accessor still returns the configured value; the node-side
//     invariance contract (direct GaussianSplatNode3D always pins
//     SUBMISSION_RESIDENCY_HINT_RESIDENT) is already pinned in
//     test_scene_director_submission_scaffolding.h.
//   - A strict identity-transform gate (project setting
//     rendering/gaussian_splatting/world/strict_identity_transform) is
//     registered and round-trips through gs::settings::get_bool.
//
#include "test_macros.h"
#include "gs_test_setting_guard.h"

#include "../core/gs_project_settings.h"
#include "../nodes/gaussian_splat_node_3d.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/variant/variant.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

namespace {

// Single-splat ASCII PLY, enough for GaussianSplatAsset::load_from_file() to
// produce a non-empty asset. Named distinctly from the importer test's
// equivalent helper because both headers are pulled into the same translation
// unit by test_gaussian_splatting.h.
Error _write_node_surface_legacy_ply(const String &p_path) {
	static const char *k_legacy_ply = R"(ply
format ascii 1.0
element vertex 1
property float x
property float y
property float z
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
property float opacity
property float f_dc_0
property float f_dc_1
property float f_dc_2
end_header
0 0 0 0 0 0 1 0 0 0 0 0.25 0.5 0.75
)";

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_CREATE;
	}
	file->store_string(k_legacy_ply);
	file.unref();
	return OK;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1) GaussianSplatNode3D no longer *exposes* the raw-file compatibility
//    surface.
//
//    "Exposed" means published to the inspector / scene serializer, i.e.
//    present in get_property_list(). That -- not settability -- is the real
//    removal contract, and it is what makes a re-saved scene drop the legacy
//    field. `ply_file_path` remains deliberately *settable* through the
//    `#ifndef DISABLE_DEPRECATED` `_set()` shim in
//    gaussian_splat_node_3d.cpp:586-617 so that old `.tscn` files still load
//    and migrate; that shim is pinned by case (2) below. Asserting
//    `set()` fails here would contradict the documented design, which is why
//    this case checks the property list and the get() axis instead.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][Cleanup] GaussianSplatNode3D no longer exposes ply_file_path or auto_load") {
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);

	List<PropertyInfo> props;
	node->get_property_list(&props);
	bool exposes_ply_file_path = false;
	bool exposes_auto_load = false;
	bool exposes_splat_asset = false;
	for (const PropertyInfo &pi : props) {
		if (pi.name == StringName("ply_file_path")) {
			exposes_ply_file_path = true;
		}
		if (pi.name == StringName("auto_load")) {
			exposes_auto_load = true;
		}
		if (pi.name == StringName("splat_asset")) {
			exposes_splat_asset = true;
		}
	}

	// Positive control. Without it the two absence checks below would also pass
	// on an empty/unpopulated property list, i.e. they could go vacuous without
	// anyone noticing. `splat_asset` is the replacement surface and must be
	// present, which proves the list really was enumerated.
	REQUIRE_MESSAGE(exposes_splat_asset,
			"splat_asset must be exposed; otherwise the absence checks below prove nothing.");

	CHECK_FALSE_MESSAGE(exposes_ply_file_path,
			"ply_file_path must not be re-bound; a re-saved scene has to drop the legacy field.");
	CHECK_FALSE_MESSAGE(exposes_auto_load, "auto_load must be fully removed from the node surface.");

	// Neither property is readable.
	bool get_valid = false;
	Variant legacy_path = node->get(StringName("ply_file_path"), &get_valid);
	CHECK_FALSE(get_valid);
	CHECK_EQ(legacy_path.get_type(), Variant::NIL);

	Variant legacy_auto_load = node->get(StringName("auto_load"), &get_valid);
	CHECK_FALSE(get_valid);
	CHECK_EQ(legacy_auto_load.get_type(), Variant::NIL);

	// auto_load is gone on the write axis too -- unlike ply_file_path it has no
	// migration shim, so nothing should consume it.
	bool set_valid = false;
	node->set(StringName("auto_load"), true, &set_valid);
	CHECK_FALSE(set_valid);

	memdelete(node);
}

#ifndef DISABLE_DEPRECATED
// ─────────────────────────────────────────────────────────────────────────────
// 1b) The `ply_file_path` deprecation shim still does its job: it accepts the
//     legacy field from an old `.tscn` and migrates the path into a runtime
//     GaussianSplatAsset. Nothing previously covered the shim's actual value --
//     only that `set()` reported success -- so a silently broken migration
//     (e.g. the load failing and the node coming up empty) would not have been
//     caught. Guarded by DISABLE_DEPRECATED because the shim itself is.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][Cleanup] ply_file_path deprecation shim migrates legacy scenes into splat_asset") {
	const String fixture_path = "user://node_surface_cleanup_legacy.ply";
	REQUIRE_MESSAGE(_write_node_surface_legacy_ply(fixture_path) == OK,
			"Failed to create synthetic legacy PLY fixture.");

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);
	REQUIRE_MESSAGE(node->get_splat_asset().is_null(), "A fresh node must start without an asset.");

	bool set_valid = false;
	node->set(StringName("ply_file_path"), fixture_path, &set_valid);

	// Settable-but-unbound is the design: _set() consumes the legacy field so
	// old scenes still load (case 1 pins that it is never re-published).
	CHECK_MESSAGE(set_valid, "The deprecation shim must consume ply_file_path so old .tscn files still load.");

	// The migration actually happened -- this is the shim's real contract.
	//
	// NOTE: a failed REQUIRE does NOT abort the test case in this build
	// (doctest runs with exceptions disabled here, so it reports FATAL and
	// keeps going). The `if` below therefore exists purely to keep a failure
	// from dereferencing a null Ref and crashing the whole run -- the
	// CHECK_MESSAGE above it has already failed loudly, so this is not a
	// skip-on-missing-fixture guard.
	Ref<GaussianSplatAsset> migrated = node->get_splat_asset();
	CHECK_MESSAGE(migrated.is_valid(), "ply_file_path must migrate into a runtime GaussianSplatAsset.");
	if (migrated.is_valid()) {
		CHECK_EQ(migrated->get_splat_count(), 1);
	}

	memdelete(node);
}
#endif // DISABLE_DEPRECATED

// ─────────────────────────────────────────────────────────────────────────────
// 2) streaming/route_policy is a real ProjectSettings key and its accessor
//    returns the configured enum. The direct-node invariance contract is
//    pinned in test_scene_director_submission_scaffolding.h's explicit
//    instance submission round-trip; here we only validate the settings
//    surface so the world-only scope statement in gs_project_settings.h does
//    not drift.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][RoutePolicy] streaming/route_policy settings accessor round-trips world-only enum") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	ProjectSettingGuard guard(ps, "rendering/gaussian_splatting/streaming/route_policy");

	ps->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			(int)gs::settings::GS_ROUTE_RESIDENT);
	CHECK_EQ(gs::settings::get_streaming_route_policy(ps), (int)gs::settings::GS_ROUTE_RESIDENT);

	ps->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			(int)gs::settings::GS_ROUTE_STREAMING);
	CHECK_EQ(gs::settings::get_streaming_route_policy(ps), (int)gs::settings::GS_ROUTE_STREAMING);

	// Token and source strings are part of the diagnostics contract that the
	// world path consumes when publishing its submission hint.
	CHECK_EQ(String(gs::settings::get_streaming_route_policy_token(
						 (int)gs::settings::GS_ROUTE_RESIDENT)),
			String("resident"));
	CHECK_EQ(String(gs::settings::get_streaming_route_policy_token(
						 (int)gs::settings::GS_ROUTE_STREAMING)),
			String("streaming"));
	CHECK_EQ(String(gs::settings::get_streaming_route_policy_source(ps)),
			String("route_policy"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 3) The strict identity-transform gate is registered as a ProjectSettings key
//    with a default of false (warn-only legacy behavior preserved). Setting it
//    true and reading back via gs::settings::get_bool pins the wiring so the
//    GaussianSplatWorld3D::_apply_world_internal hard-fail branch cannot be
//    accidentally disabled by a rename.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("[GaussianSplatting][NodeSurface][World] strict_identity_transform setting round-trips") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	ProjectSettingGuard guard(ps, "rendering/gaussian_splatting/world/strict_identity_transform");

	// Default is false (compat). The setting is registered by
	// GaussianSplatManager::initialize_module().
	CHECK_FALSE(gs::settings::get_bool(ps,
			"rendering/gaussian_splatting/world/strict_identity_transform", false));

	ps->set_setting("rendering/gaussian_splatting/world/strict_identity_transform", true);
	CHECK(gs::settings::get_bool(ps,
			"rendering/gaussian_splatting/world/strict_identity_transform", false));

	ps->set_setting("rendering/gaussian_splatting/world/strict_identity_transform", false);
	CHECK_FALSE(gs::settings::get_bool(ps,
			"rendering/gaussian_splatting/world/strict_identity_transform", true));
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
