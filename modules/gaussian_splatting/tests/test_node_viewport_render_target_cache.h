/**
 * @file test_node_viewport_render_target_cache.h
 * @brief #1092 regression: a bound viewport must not cost a RenderingServer
 *        viewport query on every frame.
 *
 * `viewport_get_render_target()` and `viewport_get_texture()` are FUNC1RC
 * getters (servers/rendering/rendering_server_default.h). Under
 * `rendering/driver/threads/thread_model=2` each call blocks the main thread
 * until the render thread has drained its queue. GaussianSplatNode3D used to
 * issue both every frame, which serialised the two threads (the starter template
 * ran at 43 FPS instead of vsync 60).
 *
 * The node counts every call it makes to those two getters (TESTS_ENABLED
 * only). The case drives the per-frame binding step directly: the headless test
 * RenderingServer uses the dummy rasterizer, whose render targets are always
 * RID(), so the READY state a real renderer reaches after its first frame is
 * seeded through the same commit path a successful query takes.
 */

#pragma once

#include "tests/test_macros.h"

#include "../nodes/gaussian_splat_node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"

TEST_CASE("[GaussianSplatting][Node][SceneTree] A bound viewport issues no per-frame RenderingServer viewport queries (#1092)") {
    SceneTree *tree = SceneTree::get_singleton();
    if (tree == nullptr || tree->get_root() == nullptr) {
        FAIL("SceneTree with a root window is required");
        return;
    }
    Window *root = tree->get_root();

    SubViewport *viewport = memnew(SubViewport);
    viewport->set_size(Size2i(256, 256));
    root->add_child(viewport);

    GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
    viewport->add_child(node);
    if (!node->is_inside_tree() || node->get_viewport() != viewport) {
        FAIL("node must be inside the SubViewport");
        root->remove_child(viewport);
        memdelete(viewport);
        return;
    }

    // Wiring: while the render target is unresolved (dummy rasterizer), every
    // binding step queries the RenderingServer. A counter that no longer counts
    // the real call site would read 0 here and fail the case.
    const uint64_t before_waiting = node->test_get_viewport_render_target_query_count();
    node->test_update_viewport_render_target();
    CHECK_FALSE(node->test_is_viewport_render_target_ready());
    CHECK(node->test_get_viewport_render_target_query_count() > before_waiting);

    // Steady state: the render target is bound to this viewport at this size.
    node->test_mark_viewport_render_target_acquired(RID::from_uint64(0x1092), RID::from_uint64(0x1093));
    CHECK(node->test_is_viewport_render_target_ready());

    const uint64_t steady_start = node->test_get_viewport_render_target_query_count();
    constexpr int STEADY_FRAMES = 120;
    for (int frame = 0; frame < STEADY_FRAMES; frame++) {
        node->test_update_viewport_render_target();
    }
    CHECK_MESSAGE(node->test_get_viewport_render_target_query_count() == steady_start,
            "an unchanged, bound viewport must not query the RenderingServer per frame "
            "(each query is a main/render-thread sync under thread_model=2)");
    CHECK(node->test_is_viewport_render_target_ready());

    // A resize must still refresh the binding: the next step queries again.
    viewport->set_size(Size2i(320, 200));
    const uint64_t before_resize_step = node->test_get_viewport_render_target_query_count();
    node->test_update_viewport_render_target();
    CHECK_MESSAGE(node->test_get_viewport_render_target_query_count() > before_resize_step,
            "a viewport resize must re-query the render target");
    // The dummy rasterizer returns no render target, so the re-query cannot
    // re-enter READY; a stale cached RID would.
    CHECK_FALSE(node->test_is_viewport_render_target_ready());

    root->remove_child(viewport);
    memdelete(viewport);
}
