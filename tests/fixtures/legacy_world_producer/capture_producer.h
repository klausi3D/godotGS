#pragma once

// Evidence-only helper for an ISOLATED immutable old-producer tree. Do not add
// it to the candidate or mutate an existing verified producer checkout.
// Rebuild only after root grants a slot, then run this exact case and hash the
// fixture files, binary, helper and unchanged writer/bake/ABI source set.
#include "tests/test_macros.h"
#include "modules/gaussian_splatting/core/gaussian_data.h"
#include "modules/gaussian_splatting/core/gaussian_splat_world.h"
#include "modules/gaussian_splatting/io/gaussian_splat_world_io.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

TEST_CASE("[LegacyWorldCapture] actual released v1 and HLOD v2 producer fixtures") {
	String directory = OS::get_singleton()->get_environment("GS_LEGACY_WORLD_CAPTURE_DIR").replace("\\", "/");
	if ((!directory.begins_with("Z:/") && !directory.begins_with("C:/")) || DirAccess::make_dir_recursive_absolute(directory) != OK) {
		FAIL("a root-authorized C:/Z: output directory is required");
		return;
	}
	Vector<Gaussian> gaussians;
	for (int i = 0; i < 2; i++) {
		Gaussian g;
		g.position = Vector3(4.0f + i, 5.0f, 6.0f);
		g.opacity = 0.75f;
		g.scale = Vector3(0.25f, 0.5f, 0.75f);
		g.rotation = Quaternion();
		g.sh_dc = Color(0.25f, 0.5f, 0.75f, 1.0f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.brush_axes = Vector2(1.0f, 1.0f);
		g.painterly_meta = gaussian_pack_painterly_meta(uint16_t(23 + i), 5);
		gaussians.push_back(g);
	}
	Ref<GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (bool hlod : {false, true}) {
		Ref<GaussianSplatWorld> world;
		world.instantiate();
		world->set_gaussian_data(data);
		world->set_bounds(data->get_aabb());
		Dictionary metadata;
		metadata["fixture"] = "released legacy producer, two distinct splats";
		world->set_metadata(metadata);
		if (hlod && world->bake_hlod() != OK) {
			FAIL("the historical HLOD producer must bake its own tree");
			return;
		}
		for (int mode = 0; mode < 3; mode++) {
			const String prefix = hlod ? "hlod_v2" : "plain_v1";
			const String suffix = mode == 0 ? "streamable" : mode == 1 ? "resident" : "compressed";
			const String path = directory.path_join(prefix + "_" + suffix + ".gsplatworld");
			const Error saved = mode == 0 ? saver.save(world, path)
					: mode == 1 ? saver.save_resident_uncompressed(world, path)
								: saver.save_resident_compressed(world, path);
			if (saved != OK) {
				FAIL("old production saver failed for ", path.utf8().get_data());
				return;
			}
			{
				Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
				if (file.is_null() || file->get_length() < (hlod ? 184u : 104u)) {
					FAIL("complete old producer header required");
					return;
				}
				file->seek(4u);
				CHECK_EQ(file->get_32(), hlod ? 2u : 1u);
				const uint32_t flags = file->get_32();
				CHECK_EQ((flags & (1u << 6u)) != 0u, hlod);
				CHECK_EQ((flags & (1u << 4u)) != 0u, mode == 2);
				CHECK_EQ((flags & (1u << 5u)) != 0u, mode != 0);
				file->seek(56u);
				CHECK_EQ(file->get_64(), hlod ? 184u : 104u);
			}
			Error err = ERR_BUG;
			Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &err);
			if (loaded.is_null() || err != OK) {
				FAIL("historical producer's own reader rejected ", path.utf8().get_data());
				return;
			}
			CHECK_EQ(loaded->get_splat_count(), 2);
			CHECK_EQ(loaded->has_hlod_tree(), hlod);
			Ref<GaussianData> decoded = loaded->get_gaussian_data();
			if (decoded.is_null() || decoded->get_count() != 2) {
				FAIL("historical reader must decode both original leaves");
				return;
			}
			bool seen[2] = {false, false};
			for (int i = 0; i < 2; i++) {
				const Gaussian got = decoded->get_gaussian(i);
				const int original = got.painterly_meta == gaussian_pack_painterly_meta(23u, 5u) ? 0
						: got.painterly_meta == gaussian_pack_painterly_meta(24u, 5u) ? 1 : -1;
				if (original < 0) {
					FAIL("historical reader changed leaf identity");
					return;
				}
				CHECK_FALSE(seen[original]);
				seen[original] = true;
				CHECK(got.position.is_equal_approx(Vector3(4.0f + original, 5.0f, 6.0f)));
				CHECK(got.scale.is_equal_approx(Vector3(0.25f, 0.5f, 0.75f)));
				CHECK(got.sh_dc.is_equal_approx(Color(0.25f, 0.5f, 0.75f, 1.0f)));
				CHECK_EQ(got.opacity, 0.75f);
				CHECK(got.normal.is_equal_approx(Vector3(0.0f, 1.0f, 0.0f)));
			}
			CHECK(seen[0]);
			CHECK(seen[1]);
		}
	}
}
