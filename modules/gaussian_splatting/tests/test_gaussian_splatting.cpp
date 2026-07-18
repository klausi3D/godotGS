/**************************************************************************/
/*  test_gaussian_splatting.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

// NOTE (#178): this file must NOT #include "test_gaussian_splatting.h" (the
// aggregator) or any other tests/*.h TEST_CASE header. Every tests/*.h is
// already glob-included once, always, into modules_tests.gen.h's single
// translation unit (tests/test_main.cpp); #pragma once only dedupes within one
// TU, so re-including those headers here would compile a SECOND copy of every
// TEST_CASE they declare into this .cpp's own object file. As long as this
// object was silently linker-dropped that extra copy was harmless - but this
// file now carries a force_link anchor (see test_gaussian_splatting.h) so its
// object always links, and a re-included copy would double-register (and
// double-run) every affected TEST_CASE. Include only what this file's own
// code below actually needs.
#include "test_macros.h"
#include "synthetic_splat_generators.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "../renderer/gaussian_splat_renderer.h"

#ifdef TESTS_ENABLED

namespace TestGaussianSplatting {

// This function is called when tests are run
// The actual test cases are defined in the header files using TEST_CASE macros
void test() {
        print_line("[GaussianSplatting] Test suite initialized");
}

TEST_CASE("[GaussianSplatting] Tile renderer fallback without streaming") {
        // Ensure we have a rendering device available for GPU resources.
        REQUIRE_GPU_DEVICE();

        GaussianSplatManager *manager = memnew(GaussianSplatManager);
        CHECK(manager != nullptr);
        if (manager == nullptr) {
                return;
        }

        Ref<GaussianSplatRenderer> renderer;
        renderer.instantiate(manager->get_primary_rendering_device());
        CHECK(renderer.is_valid());
        if (!renderer.is_valid()) {
                memdelete(manager);
                return;
        }

        Ref<::GaussianData> data;
        data.instantiate();

        UniformSplatGenerator::Config synthetic_config;
        synthetic_config.splat_count = 64;
        synthetic_config.seed = 7;
        synthetic_config.position_min = Vector3(-0.25f, -0.25f, -6.0f);
        synthetic_config.position_max = Vector3(0.25f, 0.25f, -4.0f);
        synthetic_config.min_scale = 0.5f;
        synthetic_config.max_scale = 0.5f;
        synthetic_config.min_opacity = 1.0f;
        synthetic_config.max_opacity = 1.0f;
        synthetic_config.normal_tilt = 0.0f;
        synthetic_config.random_rotation = false;
        synthetic_config.random_colors = false;
        synthetic_config.base_color = Color(1.0f, 1.0f, 1.0f, 1.0f);

        SyntheticSceneSummary synthetic_summary;
        LocalVector<Gaussian> splats = UniformSplatGenerator::generate(synthetic_config, &synthetic_summary);
        CHECK_EQ(splats.size(), static_cast<int>(synthetic_config.splat_count));
        CHECK_EQ(synthetic_summary.seed, synthetic_config.seed);
        CHECK_EQ(synthetic_summary.splat_count, synthetic_config.splat_count);
        CHECK(synthetic_summary.scene_hash != 0);
        data->set_gaussians(splats);

        Error set_err = renderer->set_gaussian_data(data);
        CHECK((set_err == OK || set_err == ERR_UNCONFIGURED));
        renderer->set_max_splats(2000);

        renderer->test_force_disable_streaming();

        renderer->render_scene_instance(nullptr);

        CHECK(renderer->get_visible_splat_count() > 0);

        Dictionary stats = renderer->get_render_stats();
        CHECK(stats.has("visible_splats"));
        CHECK(int(stats["visible_splats"]) > 0);

        CHECK(renderer->has_rendered_content());

        renderer.unref();
        memdelete(manager);
}

} // namespace TestGaussianSplatting

#endif // TESTS_ENABLED

// Force-link anchor (#178): a doctest TEST_CASE registers via a file-scope static
// initializer; MSVC drops this whole object from the module static library when
// nothing references it, silently discarding the cases. test_gaussian_splatting.h
// calls this symbol so the linker keeps the object and the cases actually run.
extern "C" int test_gaussian_splatting_cpp_force_link() {
    return 0;
}
