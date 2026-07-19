/**************************************************************************/
/*  test_macros.h                                                         */
/*  Gaussian Splatting Module Test Framework                              */
/**************************************************************************/

#ifndef GAUSSIAN_SPLATTING_TEST_MACROS_H
#define GAUSSIAN_SPLATTING_TEST_MACROS_H

// Include Godot's test framework - doctest is built into Godot
#include "tests/test_macros.h"

// Additional test utilities specific to Gaussian Splatting
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "servers/rendering_server.h"
#include "servers/rendering/rendering_device.h"

#include "../core/gaussian_splat_manager.h"

// Performance testing utilities
class PerformanceTimer {
private:
    uint64_t start_time;
    String name;

public:
    PerformanceTimer(const String &p_name) : name(p_name) {
        start_time = OS::get_singleton()->get_ticks_usec();
    }

    float elapsed_ms() const {
        uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start_time;
        return elapsed / 1000.0f;
    }

    void print_elapsed() const {
        print_line(vformat("[%s] Elapsed: %.2f ms", name, elapsed_ms()));
    }
};

// Simple GPU memory tracker for tests
// NOTE: Named TestGPUMemoryTracker to avoid conflict with memory_validator.h's GPUMemoryTracker
class TestGPUMemoryTracker {
private:
    RenderingDevice *rd;
    uint64_t initial_memory;

public:
    TestGPUMemoryTracker(RenderingDevice *p_rd) : rd(p_rd), initial_memory(0) {
        if (rd) {
            initial_memory = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
        }
    }

    float get_memory_usage_mb() const {
        if (!rd) return 0.0f;
        uint64_t current = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
        return (current - initial_memory) / (1024.0f * 1024.0f);
    }

    void print_usage() const {
        print_line(vformat("GPU Memory Usage: %.2f MB", get_memory_usage_mb()));
    }
};

// RAII guard for the optional locally-allocated fallback RenderingDevice that
// REQUIRE_GPU_DEVICE() may construct when RenderingDevice::get_singleton()
// returns null. Frees the device on scope exit if-and-only-if the macro owned
// it. `rd` is exposed as a public field so the macro can republish it under
// that name to the surrounding test function — the call-site contract from
// pre-refactor REQUIRE_GPU_DEVICE() is unchanged.
class ScopedFallbackRD {
public:
    RenderingDevice *rd = nullptr;
    bool owns_rd = false;

    ScopedFallbackRD() {
        rd = RenderingDevice::get_singleton();
        if (!rd) {
            if (RenderingServer *rs = RenderingServer::get_singleton()) {
                rd = rs->create_local_rendering_device();
                owns_rd = (rd != nullptr);
            }
        }
    }
    ~ScopedFallbackRD() {
        if (owns_rd && rd) {
            memdelete(rd);
        }
    }
    ScopedFallbackRD(const ScopedFallbackRD &) = delete;
    ScopedFallbackRD &operator=(const ScopedFallbackRD &) = delete;
};

// Helper macro for GPU tests that require a rendering device.
// Publishes a `RenderingDevice *rd` into the calling test's scope. If the
// engine singleton is null, a local fallback RD is created and OWNED by an
// inline ScopedFallbackRD guard — the guard's destructor frees the device on
// every scope exit (normal return, early return below, doctest REQUIRE
// exception). Pre-refactor callers leaked the fallback RD because the macro
// had no paired teardown; #334.
#define REQUIRE_GPU_DEVICE()                                              \
    ScopedFallbackRD _gs_rd_scope;                                        \
    RenderingDevice *rd = _gs_rd_scope.rd;                                \
    if (rd == nullptr) {                                                  \
        MESSAGE("Skipping test - RenderingDevice unavailable");           \
        return;                                                           \
    }

// Issue #641 helper: acquire a *local* (non-main) RenderingDevice for tests
// that legitimately need to own and submit/sync their own device (TileRenderer
// pipeline creation, the sync-policy contract tests). Two bootstraps can supply
// one, and before #641 these tests only knew about the first:
//
//   * plain `--test` WITH a real RenderingServer (i.e. a `[SceneTree]`-tagged
//     case, tests/test_main.cpp:333) -> `rs->create_local_rendering_device()`.
//     Under `--headless` this still returns nullptr: the mock DisplayServer has
//     no RenderingDevice behind it.
//   * `--gs-gpu-test` (modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp)
//     -> there is NO RenderingServer at all, but `_bootstrap_rd()` creates an
//     offscreen `RenderingDevice` that becomes `RenderingDevice::get_singleton()`.
//     `RenderingDevice::create_local_device()` (servers/rendering/rendering_device.cpp:7350)
//     spawns a fresh local device from its context.
//
// Returns nullptr when neither bootstrap is present, so the caller skips instead
// of failing a precondition it cannot satisfy.
inline RenderingDevice *gs_test_create_local_rendering_device() {
    if (RenderingServer *rs = RenderingServer::get_singleton()) {
        if (RenderingDevice *device = rs->create_local_rendering_device()) {
            return device;
        }
    }
    if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
        return rd->create_local_device();
    }
    return nullptr;
}

// RAII owner for the device above, mirroring ScopedFallbackRD. The local device
// is ALWAYS owned by the test (both bootstraps hand back a fresh device), so the
// guard frees it unconditionally on every scope exit — normal return, early
// return, or a doctest REQUIRE throw. This is the #334 leak pattern applied to
// the local-device call sites, which previously relied on a hand-written
// `memdelete(local_device)` on each return path.
class ScopedLocalRD {
public:
    RenderingDevice *rd = nullptr;

    ScopedLocalRD() {
        rd = gs_test_create_local_rendering_device();
    }
    ~ScopedLocalRD() {
        if (rd) {
            memdelete(rd);
        }
    }
    ScopedLocalRD(const ScopedLocalRD &) = delete;
    ScopedLocalRD &operator=(const ScopedLocalRD &) = delete;
};

// Publishes an owned `RenderingDevice *local_device` into the calling test's
// scope, or skips the test when no lane-provided bootstrap can supply one.
// Tag any TEST_CASE using this `[RequiresGPU]` so the `--gs-gpu-test` harness
// (tests/ci/run_gpu_harness.py) actually executes it; without that tag it is
// unreachable by every CI lane, which is exactly the #641 defect.
#define REQUIRE_LOCAL_GPU_DEVICE()                                        \
    ScopedLocalRD _gs_local_rd_scope;                                     \
    RenderingDevice *local_device = _gs_local_rd_scope.rd;                \
    if (local_device == nullptr) {                                        \
        MESSAGE("Skipping test - local RenderingDevice unavailable");     \
        return;                                                           \
    }

// #637 helper: assert the GLOBAL `RenderingDevice::get_singleton()` exists.
//
// This is NOT interchangeable with REQUIRE_LOCAL_GPU_DEVICE(). That macro
// hands you a *local* device you own; this one asserts the *singleton*, which
// is what upstream `ShaderRD::ShaderRD()` reads unguarded:
//
//     servers/rendering/renderer_rd/shader_rd.cpp:791
//         if (RD::get_singleton()->get_device_vendor_name() == "NVIDIA") {
//
// Any type that owns a ShaderRD subclass therefore cannot be constructed at
// all without the singleton. `TileRenderer` is such a type: its constructor's
// member-initializer list builds `shader_resources(*this)`
// (renderer/tile_renderer.cpp:1384), whose own member-initializer list does
// `std::make_unique<TileBinningShaderRD>()`
// (renderer/tile_render_resources.cpp:286), and `TileBinningShaderRD` derives
// from `ShaderRD` (shaders/tile_binning.glsl.gen.h:37). All of that runs
// BEFORE the first statement of TileRenderer's constructor body, so the
// null-guards in that body cannot help.
//
// Production honours this invariant (every TileRenderer construction site sits
// behind a device check), so a test that trips it has an unmet precondition —
// hence FAIL, not a silent skip: a skip here would be the vacuous-pass shape
// this module keeps eliminating, and #656 established that REQUIRE does not
// abort the case in this build. Tag such a TEST_CASE `[RequiresGPU]` so the
// `--gs-gpu-test` harness (whose _bootstrap_rd() memnew(RenderingDevice)
// installs the singleton, servers/rendering/rendering_device.cpp:8144) runs it.
#define REQUIRE_RENDERING_DEVICE_SINGLETON()                                   \
    if (RenderingDevice::get_singleton() == nullptr) {                         \
        FAIL("RenderingDevice::get_singleton() is null. This [RequiresGPU] "   \
             "case must run under the --gs-gpu-test harness "                  \
             "(tests/ci/run_gpu_harness.py), which bootstraps the singleton "  \
             "that upstream ShaderRD's constructor dereferences.");            \
        return;                                                                \
    }

// PR #352 helper for streaming tests: cheap probe symmetric to
// REQUIRE_GPU_DEVICE(). Skips the calling test when the streaming pipeline
// has no usable RenderingDevice — i.e. when
// GaussianStreamingSystem::initialize() would fail with "runtime not
// loadable". Use this in tests that exercise the streaming runtime end-to-
// end (initialize -> update_streaming -> chunk uploads) so they degrade to
// a skip on headless lanes instead of cascading into the failed-init crash
// path the rest of this PR closes.
//
// Probes all three device paths that GaussianStreamingSystem::initialize()
// can use: RenderingDevice::get_singleton(),
// RenderingServer::get_rendering_device(), and
// GaussianSplatManager::get_primary_rendering_device() (which may construct
// a local-device fallback). Only skips when ALL THREE report no device,
// matching the retry fix pattern in test_gaussian_streaming_lifecycle.cpp
// (1c447b99e3) and test_renderer_lifetime_proof.h (97f295e57a, PR #386).
#define REQUIRE_STREAMING_CAPABLE()                                          \
    do {                                                                     \
        RenderingDevice *_gs_streaming_probe = RenderingDevice::get_singleton(); \
        if (_gs_streaming_probe == nullptr) {                                \
            if (RenderingServer *_gs_streaming_rs = RenderingServer::get_singleton()) { \
                _gs_streaming_probe = _gs_streaming_rs->get_rendering_device(); \
            }                                                                \
        }                                                                    \
        if (_gs_streaming_probe == nullptr) {                                \
            if (GaussianSplatManager *_gs_streaming_mgr = GaussianSplatManager::get_singleton()) { \
                _gs_streaming_probe = _gs_streaming_mgr->get_primary_rendering_device(); \
            }                                                                \
        }                                                                    \
        if (_gs_streaming_probe == nullptr) {                                \
            MESSAGE("Skipping test - streaming unavailable");                \
            return;                                                          \
        }                                                                    \
    } while (0)

// Issue #392: helper for tests that exercise renderer/manager paths which
// transitively dispatch work through WorkerThreadPool (e.g. the Lifetime
// scenarios A/D in test_renderer_lifetime_proof.h, which call into
// GaussianSplatManager::register_gaussian_buffer /
// _request_primary_local_device). The `--gs-gpu-test` runner
// (gs_gpu_test_runner.cpp) provisions a RenderingDevice via Main::test_setup()
// but does NOT call WorkerThreadPool::get_singleton()->init(), so the pool's
// `TightLocalVector<ThreadData> threads` is empty (size 0). Any code path that
// indexes that vector (`threads[i]`) by a thread index derived from
// thread-local state therefore OOBs the LocalVector — STATUS_STACK_BUFFER_OVERRUN
// (#392 scenarios A/D crash signature).
//
// The probe is `get_singleton()` non-null AND `get_thread_count() > 0`. The
// singleton itself is created by register_core_types() (which test_setup()
// calls) so the null check is a belt-and-braces guard — the load-bearing
// gate is the thread-count check. In a normally bootstrapped runner
// (tests/test_main.cpp:242 calls `init()`) this passes cleanly; in the
// `--gs-gpu-test` runner it skips with a clear reason instead of crashing.
//
// Use this AFTER REQUIRE_GPU_DEVICE() in scenarios that need both a device
// and a usable pool: device first (so headless lanes skip first with the
// existing "RenderingDevice unavailable" reason), pool second.
#define REQUIRE_WORKER_THREAD_POOL()                                          \
    do {                                                                      \
        WorkerThreadPool *_gs_wtp_probe = WorkerThreadPool::get_singleton();  \
        if (_gs_wtp_probe == nullptr || _gs_wtp_probe->get_thread_count() <= 0) { \
            MESSAGE("Skipping test - worker thread pool unavailable");        \
            return;                                                           \
        }                                                                     \
    } while (0)

// Performance baseline checking
#define CHECK_PERFORMANCE(timer, max_ms, operation)                      \
    CHECK_MESSAGE(timer.elapsed_ms() < max_ms,                          \
        vformat(operation " took %.2fms, expected < %.2fms",            \
            timer.elapsed_ms(), max_ms))

#endif // GAUSSIAN_SPLATTING_TEST_MACROS_H
